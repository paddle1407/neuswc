/* swc: libswc/text_input.c
 *
 * Copyright (c) 2025 charaWC contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "text_input.h"
#include "compositor.h"
#include "internal.h"
#include "keyboard.h"
#include "seat.h"
#include "session_lock.h"
#include "view.h"
#include <limits.h>
#include "surface.h"
#include "util.h"

#include "input-method-unstable-v2-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"

#include <stdlib.h>
#include <string.h>

/*
 * Input methods, in two halves that have to be joined here.
 *
 * text-input-v3 is the application side: a text field says "I am focused, the
 * text around the cursor is this, my caret is here". input-method-v2 is the
 * fcitx5/ibus side: it reads that state, takes the keyboard while composing,
 * and hands back preedit and committed text.
 *
 * Neither speaks to the other. The compositor is the relay, and that is all
 * this file is: forward application state to the input method, forward the
 * input method's output back to whichever text input is focused.
 *
 * Both protocols are double-buffered -- state accumulates until a commit --
 * so each side keeps a pending copy that is only published on commit.
 */

struct text_input {
	struct wl_resource *resource;
	struct wl_client *client;
	struct wl_list link;

	/* The surface we last sent enter for, so leave goes to the right one.
	 * The listener drops it when the surface goes, so that a later focus
	 * change cannot send leave through freed memory. */
	struct surface *entered;
	struct wl_listener entered_destroy_listener;

	bool enabled;
	uint32_t serial; /* counts the done events we have sent */

	struct {
		bool enabled, disabled;
		bool surrounding_set, content_set, cursor_rect_set, cause_set;
		char *surrounding_text;
		int32_t surrounding_cursor, surrounding_anchor;
		uint32_t cause;
		uint32_t content_hint, content_purpose;
		int32_t cursor_x, cursor_y, cursor_width, cursor_height;
	} pending;

	struct {
		int32_t x, y, width, height;
	} cursor_rect;
};

struct input_popup {
	struct wl_resource *resource;
	struct surface *surface;
	struct compositor_view *view;
	struct wl_listener surface_destroy_listener, surface_commit_listener;
	struct view_handler anchor_handler;
	struct wl_listener anchor_destroy;
	struct compositor_view *anchor;
	struct wl_list link;
};

struct input_method {
	struct wl_resource *resource;
	struct wl_list popups;
	struct wl_resource *grab;

	bool active;
	pid_t pid;
	uint32_t serial;

	/* Accumulated until commit(serial). */
	struct {
		char *preedit;
		int32_t preedit_begin, preedit_end;
		char *commit_string;
		uint32_t delete_before, delete_after;
	} pending;
};

static struct wl_list text_inputs;
/* The protocol allows one input method per seat, and swc has one seat. */
static struct input_method *input_method;
static struct text_input *focused_input;
static void set_entered(struct text_input *ti, struct surface *surface);
static void update_popups(void);
static void suspend_grab(void);
static void install_grab(void);

static struct keyboard_handler grab_handler;
static bool grab_handler_linked;
static bool initialized;

static bool
input_method_trusted(void)
{
	return input_method && swc.manager && swc.manager->authorize_input_method &&
		    swc.manager->authorize_input_method(input_method->pid);
}

/* ------------------------------------------------------------- helpers */

static void
replace_string(char **slot, const char *value)
{
	free(*slot);
	*slot = value ? strdup(value) : NULL;
}

static struct surface *
view_surface(struct compositor_view *view)
{
	return view ? view->surface : NULL;
}

/* Push the focused text input's whole state at the input method, then done. */
static void
send_state_to_input_method(void)
{
	struct text_input *ti = focused_input;

	if (!input_method_trusted() || !input_method->active || !ti || !ti->enabled || session_lock_active()) {
		return;
	}

	if (ti->pending.surrounding_set && ti->pending.surrounding_text) {
		zwp_input_method_v2_send_surrounding_text(
		    input_method->resource, ti->pending.surrounding_text,
		    (uint32_t)ti->pending.surrounding_cursor,
		    (uint32_t)ti->pending.surrounding_anchor);
	}
	if (ti->pending.cause_set) {
		zwp_input_method_v2_send_text_change_cause(input_method->resource,
		                                           ti->pending.cause);
	}
	if (ti->pending.content_set) {
		zwp_input_method_v2_send_content_type(input_method->resource,
		                                      ti->pending.content_hint,
		                                      ti->pending.content_purpose);
	}
	zwp_input_method_v2_send_done(input_method->resource);
	++input_method->serial;
}

static void
input_method_activate(void)
{
	if (!input_method_trusted() || input_method->active || session_lock_active()) {
		return;
	}
	free(input_method->pending.preedit);
	free(input_method->pending.commit_string);
	memset(&input_method->pending, 0, sizeof(input_method->pending));
	input_method->active = true;
	install_grab();
	zwp_input_method_v2_send_activate(input_method->resource);
	send_state_to_input_method();
}

static void
input_method_deactivate(void)
{
	if (!input_method || !input_method->active) {
		return;
	}
	input_method->active = false;
	free(input_method->pending.preedit);
	free(input_method->pending.commit_string);
	memset(&input_method->pending, 0, sizeof(input_method->pending));
	suspend_grab();
	update_popups();
	zwp_input_method_v2_send_deactivate(input_method->resource);
	zwp_input_method_v2_send_done(input_method->resource);
	++input_method->serial;
}

/* Tell every input popup where the caret is, so a candidate list can sit
 * under it rather than in the corner of the screen. */
static void
popup_changed(struct view_handler *handler)
{
	(void)handler;
	update_popups();
}
static void popup_resized(struct view_handler *handler, uint32_t w, uint32_t h)
{ (void)w; (void)h; popup_changed(handler); }
static const struct view_handler_impl popup_anchor_impl = {
	.move = popup_changed, .attach = popup_changed, .resize = popup_resized,
};
static void
popup_anchor_destroyed(struct wl_listener *listener, void *data)
{
	struct input_popup *popup = wl_container_of(listener, popup, anchor_destroy);
	(void)data;
	wl_list_remove(&popup->anchor_handler.link); wl_list_init(&popup->anchor_handler.link);
	wl_list_remove(&popup->anchor_destroy.link); wl_list_init(&popup->anchor_destroy.link);
	popup->anchor = NULL;
	if (popup->view) compositor_view_hide(popup->view);
}
static void
popup_committed(struct wl_listener *listener, void *data)
{ (void)listener; (void)data; update_popups(); }
static void
update_popups(void)
{
	struct input_popup *popup;
	if (!input_method) return;
	struct compositor_view *anchor = swc.seat && swc.seat->keyboard ?
		    swc.seat->keyboard->focus.view : NULL;
	bool active = input_method_trusted() && input_method->active && focused_input && focused_input->enabled &&
		    anchor && !session_lock_active();
	wl_list_for_each(popup, &input_method->popups, link) {
		if (popup->anchor != anchor) {
			wl_list_remove(&popup->anchor_handler.link); wl_list_init(&popup->anchor_handler.link);
			wl_list_remove(&popup->anchor_destroy.link); wl_list_init(&popup->anchor_destroy.link);
			popup->anchor = anchor;
			if (anchor) {
				popup->anchor_handler.impl = &popup_anchor_impl;
				popup->anchor_destroy.notify = popup_anchor_destroyed;
				wl_list_insert(&anchor->base.handlers, &popup->anchor_handler.link);
				wl_signal_add(&anchor->destroy_signal, &popup->anchor_destroy);
	}
		}
		if (!popup->view) continue;
		if (!active || !popup->view->base.buffer) {
			compositor_view_hide(popup->view);
			continue;
		}
		zwp_input_popup_surface_v2_send_text_input_rectangle(popup->resource,
		    focused_input->cursor_rect.x, focused_input->cursor_rect.y,
		    focused_input->cursor_rect.width, focused_input->cursor_rect.height);
		int64_t x = (int64_t)anchor->base.geometry.x - anchor->buffer_offset_x + focused_input->cursor_rect.x;
		int64_t y = (int64_t)anchor->base.geometry.y - anchor->buffer_offset_y + focused_input->cursor_rect.y + focused_input->cursor_rect.height;
		view_move(&popup->view->base, (int32_t)MAX(INT32_MIN, MIN(x, INT32_MAX)),
		    (int32_t)MAX(INT32_MIN, MIN(y, INT32_MAX)));
		compositor_view_show(popup->view);
	}
}

/* --------------------------------------------------------- text input */

static void
text_input_enable(struct wl_client *client, struct wl_resource *resource)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	(void)client;
	if (!ti) {
		return;
	}
	ti->pending.enabled = true;
	ti->pending.disabled = false;
}

static void
text_input_disable(struct wl_client *client, struct wl_resource *resource)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	(void)client;
	if (!ti) {
		return;
	}
	ti->pending.disabled = true;
	ti->pending.enabled = false;
}

static void
text_input_set_surrounding_text(struct wl_client *client,
                                struct wl_resource *resource, const char *text,
                                int32_t cursor, int32_t anchor)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	(void)client;
	if (!ti) {
		return;
	}
	replace_string(&ti->pending.surrounding_text, text);
	ti->pending.surrounding_cursor = cursor;
	ti->pending.surrounding_anchor = anchor;
	ti->pending.surrounding_set = true;
}

static void
text_input_set_text_change_cause(struct wl_client *client,
                                 struct wl_resource *resource, uint32_t cause)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	(void)client;
	if (!ti) {
		return;
	}
	ti->pending.cause = cause;
	ti->pending.cause_set = true;
}

static void
text_input_set_content_type(struct wl_client *client,
                            struct wl_resource *resource, uint32_t hint,
                            uint32_t purpose)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	(void)client;
	if (!ti) {
		return;
	}
	ti->pending.content_hint = hint;
	ti->pending.content_purpose = purpose;
	ti->pending.content_set = true;
}

static void
text_input_set_cursor_rectangle(struct wl_client *client,
                                struct wl_resource *resource, int32_t x,
                                int32_t y, int32_t width, int32_t height)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	(void)client;
	if (!ti) {
		return;
	}
	ti->pending.cursor_x = x;
	ti->pending.cursor_y = y;
	ti->pending.cursor_width = width;
	ti->pending.cursor_height = height;
	ti->pending.cursor_rect_set = true;
}

static void
text_input_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct text_input *ti = wl_resource_get_user_data(resource);
	bool was_enabled;

	(void)client;
	if (!ti) {
		return;
	}

	was_enabled = ti->enabled;
	if (ti->pending.enabled) {
		ti->enabled = true;
	}
	if (ti->pending.disabled) {
		ti->enabled = false;
	}
	if (ti->pending.cursor_rect_set) {
		ti->cursor_rect.x = ti->pending.cursor_x;
		ti->cursor_rect.y = ti->pending.cursor_y;
		ti->cursor_rect.width = ti->pending.cursor_width;
		ti->cursor_rect.height = ti->pending.cursor_height;
	}
	++ti->serial;

	if (ti == focused_input) {
		if (ti->enabled && !was_enabled) {
			input_method_activate();
		} else if (!ti->enabled && was_enabled) {
			input_method_deactivate();
		} else if (ti->enabled) {
			send_state_to_input_method();
		}
		update_popups();
	}

	/* One commit's worth of state has been forwarded; start accumulating
	 * the next. The enabled flag itself is current state, not pending. */
	ti->pending.enabled = false;
	ti->pending.disabled = false;
	ti->pending.surrounding_set = false;
	ti->pending.content_set = false;
	ti->pending.cursor_rect_set = false;
	ti->pending.cause_set = false;
}

/* Introduced after version 1; charaWC advertises version 1, so these cannot
 * be reached. They exist because the interface struct has slots for them. */
static void
text_input_set_available_actions(struct wl_client *client,
                                 struct wl_resource *resource,
                                 struct wl_array *actions)
{
	(void)client;
	(void)resource;
	(void)actions;
}

static void
text_input_show_input_panel(struct wl_client *client,
                            struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void
text_input_hide_input_panel(struct wl_client *client,
                            struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static const struct zwp_text_input_v3_interface text_input_impl = {
	.destroy = destroy_resource,
	.enable = text_input_enable,
	.disable = text_input_disable,
	.set_surrounding_text = text_input_set_surrounding_text,
	.set_text_change_cause = text_input_set_text_change_cause,
	.set_content_type = text_input_set_content_type,
	.set_cursor_rectangle = text_input_set_cursor_rectangle,
	.commit = text_input_commit,
	.set_available_actions = text_input_set_available_actions,
	.show_input_panel = text_input_show_input_panel,
	.hide_input_panel = text_input_hide_input_panel,
};

static void
destroy_text_input(struct wl_resource *resource)
{
	struct text_input *ti = wl_resource_get_user_data(resource);

	if (!ti) {
		return;
	}
	if (focused_input == ti) {
		if (ti->enabled) {
			input_method_deactivate();
		}
		focused_input = NULL;
	}
	set_entered(ti, NULL);
	free(ti->pending.surrounding_text);
	wl_list_remove(&ti->link);
	free(ti);
}

/* ----------------------------------------------------------- focus */

static void handle_entered_destroy(struct wl_listener *listener, void *data);

/* The entered surface is remembered only so that leave can name it. Nothing
 * else keeps it alive, so the pointer has to be dropped when it dies. */
static void
set_entered(struct text_input *ti, struct surface *surface)
{
	if (ti->entered == surface) {
		return;
	}
	if (ti->entered) {
		wl_list_remove(&ti->entered_destroy_listener.link);
	}
	ti->entered = surface;
	if (surface) {
		ti->entered_destroy_listener.notify = handle_entered_destroy;
		wl_signal_add(&surface->signal.destroy,
		              &ti->entered_destroy_listener);
	} else {
		wl_list_init(&ti->entered_destroy_listener.link);
	}
}

static void
reset_text_input_state(struct text_input *ti)
{
	ti->enabled = false;
	free(ti->pending.surrounding_text);
	memset(&ti->pending, 0, sizeof(ti->pending));
	memset(&ti->cursor_rect, 0, sizeof(ti->cursor_rect));
}

static void
handle_entered_destroy(struct wl_listener *listener, void *data)
{
	struct text_input *ti =
	    wl_container_of(listener, ti, entered_destroy_listener);

	(void)data;
	/* No leave: the surface it would name is going away, and the client
	 * knows that already. */
	wl_list_remove(&ti->entered_destroy_listener.link);
	wl_list_init(&ti->entered_destroy_listener.link);
	ti->entered = NULL;
	if (ti == focused_input && ti->enabled) {
		ti->enabled = false;
		input_method_deactivate();
	}
	reset_text_input_state(ti);
}

void
text_input_handle_focus(struct compositor_view *view)
{
	struct surface *surface = view_surface(view);
	struct wl_client *client =
	    surface ? wl_resource_get_client(surface->resource) : NULL;
	struct text_input *ti, *next_focus = NULL;

	if (!initialized) {
		return;
	}

	/* Leave first: an input method must never see the new focus enabled
	 * before the old one has been taken away. */
	wl_list_for_each(ti, &text_inputs, link)
	{
		if (ti->entered && ti->entered != surface) {
			zwp_text_input_v3_send_leave(ti->resource,
			                             ti->entered->resource);
			set_entered(ti, NULL);
			if (ti == focused_input && ti->enabled) {
				ti->enabled = false;
				input_method_deactivate();
			}
			reset_text_input_state(ti);
		}
	}

	focused_input = NULL;

	if (!client || !surface) {
		update_popups();
		return;
	}

	wl_list_for_each(ti, &text_inputs, link)
	{
		if (ti->client != client) {
			continue;
		}
		if (ti->entered != surface) {
			set_entered(ti, surface);
			zwp_text_input_v3_send_enter(ti->resource, surface->resource);
		}
		if (!next_focus) {
			next_focus = ti;
		}
	}

	focused_input = next_focus;
	update_popups();
}

/* --------------------------------------------------- keyboard grab */

static bool
grab_handle_key(struct keyboard *keyboard, uint32_t time, struct key *key,
                uint32_t state)
{
	(void)keyboard;

	if (!input_method_trusted() || !input_method->grab || !input_method->active ||
	    !focused_input || !focused_input->enabled || session_lock_active()) {
		return false;
	}
	zwp_input_method_keyboard_grab_v2_send_key(
	    input_method->grab, wl_display_next_serial(swc.display), time,
	    key->press.value, state);
	return true;
}

static bool
grab_handle_modifiers(struct keyboard *keyboard,
                      const struct keyboard_modifier_state *state)
{
	if (!input_method_trusted() || !input_method->grab || !input_method->active ||
	    !focused_input || !focused_input->enabled || session_lock_active()) {
		return false;
	}
	zwp_input_method_keyboard_grab_v2_send_modifiers(
	    input_method->grab, wl_display_next_serial(swc.display),
	    state->depressed, state->latched, state->locked, state->group);
	return true;
}

static void
destroy_grab(struct wl_resource *resource)
{
	/* An input method that has gone away can leave its grab object behind.
	 * Destroying that stale object must not take the keyboard away from the
	 * input method holding the grab now. */
	if (!input_method || input_method->grab != resource) {
		return;
	}
	suspend_grab();
	input_method->grab = NULL;
}

static const struct zwp_input_method_keyboard_grab_v2_interface grab_impl = {
	.release = destroy_resource,
};

static void
input_method_grab_keyboard(struct wl_client *client,
                           struct wl_resource *resource, uint32_t id)
{
	struct input_method *im = wl_resource_get_user_data(resource);
	struct keyboard *keyboard = swc.seat ? swc.seat->keyboard : NULL;
	struct wl_resource *grab;

	if (!im || !keyboard || im != input_method) {
		return;
	}

	if (im->grab) {
		suspend_grab();
		wl_resource_set_user_data(im->grab, NULL);
	}
	grab = wl_resource_create(client,
	                          &zwp_input_method_keyboard_grab_v2_interface,
	                          wl_resource_get_version(resource), id);
	if (!grab) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(grab, &grab_impl, im, destroy_grab);
	im->grab = grab;

	/* The input method needs the same keymap the clients get, or the
	 * keycodes it receives mean nothing. */
	zwp_input_method_keyboard_grab_v2_send_keymap(
	    grab, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keyboard->xkb.keymap.fd,
	    keyboard->xkb.keymap.size);

	install_grab();
}

static void
install_grab(void)
{
	if (!input_method_trusted() || !input_method->grab || !input_method->active ||
		    !focused_input || !focused_input->enabled || session_lock_active() || grab_handler_linked) return;
	struct keyboard *keyboard = swc.seat ? swc.seat->keyboard : NULL;
	if (!keyboard) return;
	wl_list_insert(keyboard->client_handler.link.prev, &grab_handler.link);
		grab_handler_linked = true;
	}

/* Release handler ownership too: held key releases must not reach a revoked
	* input method, nor turn into an unpaired release in the newly focused client. */
static void
suspend_grab(void)
{
	struct keyboard *keyboard = swc.seat ? swc.seat->keyboard : NULL;
	struct key *key;
	if (keyboard) wl_array_for_each(key, &keyboard->keys)
		if (key->handler == &grab_handler) key->handler = NULL;
	if (grab_handler_linked) {
		wl_list_remove(&grab_handler.link);
		grab_handler_linked = false;
	}
}

void
text_input_suspend(void)
{
	input_method_deactivate();
	suspend_grab();
	update_popups();
}

/* --------------------------------------------------- input popups */

static void
destroy_popup(struct wl_resource *resource)
{
	struct input_popup *popup = wl_resource_get_user_data(resource);

	if (!popup) {
		return;
	}
	wl_list_remove(&popup->surface_destroy_listener.link);
	wl_list_remove(&popup->surface_commit_listener.link);
	wl_list_remove(&popup->anchor_handler.link);
	wl_list_remove(&popup->anchor_destroy.link);
	wl_list_remove(&popup->link);
	if (popup->view) {
		compositor_view_destroy(popup->view);
	}
	free(popup);
}

static void
handle_popup_surface_destroy(struct wl_listener *listener, void *data)
{
	struct input_popup *popup =
	    wl_container_of(listener, popup, surface_destroy_listener);
	(void)data;
	wl_resource_destroy(popup->resource);
}

static const struct zwp_input_popup_surface_v2_interface popup_impl = {
	.destroy = destroy_resource,
};

static void
input_method_get_popup_surface(struct wl_client *client,
                               struct wl_resource *resource, uint32_t id,
                               struct wl_resource *surface_resource)
{
	struct input_method *im = wl_resource_get_user_data(resource);
	struct input_popup *popup;
	struct surface *surface;

	surface = surface_from_resource(surface_resource);
	if (!im || !surface) {
		return;
	}

	popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wl_client_post_no_memory(client);
		return;
	}
	popup->resource = wl_resource_create(
	    client, &zwp_input_popup_surface_v2_interface,
	    wl_resource_get_version(resource), id);
	if (!popup->resource) {
		free(popup);
		wl_client_post_no_memory(client);
		return;
	}
	if (!surface_set_role(surface, popup->resource)) {
		wl_resource_destroy(popup->resource);
		free(popup);
		wl_resource_post_error(resource, ZWP_INPUT_METHOD_V2_ERROR_ROLE, "surface already has another role");
		return;
	}
	wl_list_init(&popup->anchor_handler.link);
	wl_list_init(&popup->anchor_destroy.link);
	popup->surface = surface;
	popup->view = compositor_create_view(surface);
	popup->surface_commit_listener.notify = popup_committed;
	wl_signal_add(&surface->signal.commit, &popup->surface_commit_listener);
	popup->surface_destroy_listener.notify = handle_popup_surface_destroy;
	wl_resource_add_destroy_listener(surface->resource,
	                                 &popup->surface_destroy_listener);
	wl_list_insert(&im->popups, &popup->link);
	wl_resource_set_implementation(popup->resource, &popup_impl, popup,
	                               destroy_popup);

	if (popup->view) {
		popup->view->always_top = true;
		compositor_view_set_stack_layer(popup->view, STACK_LAYER_OVERLAY,
		                                true);
	}
	update_popups();
}

/* -------------------------------------------------- input method */

static void
input_method_commit_string(struct wl_client *client,
                           struct wl_resource *resource, const char *text)
{
	struct input_method *im = wl_resource_get_user_data(resource);

	(void)client;
	if (im) {
		replace_string(&im->pending.commit_string, text);
	}
}

static void
input_method_set_preedit_string(struct wl_client *client,
                                struct wl_resource *resource, const char *text,
                                int32_t cursor_begin, int32_t cursor_end)
{
	struct input_method *im = wl_resource_get_user_data(resource);

	(void)client;
	if (!im) {
		return;
	}
	replace_string(&im->pending.preedit, text);
	im->pending.preedit_begin = cursor_begin;
	im->pending.preedit_end = cursor_end;
}

static void
input_method_delete_surrounding_text(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t before_length,
                                     uint32_t after_length)
{
	struct input_method *im = wl_resource_get_user_data(resource);

	(void)client;
	if (!im) {
		return;
	}
	im->pending.delete_before = before_length;
	im->pending.delete_after = after_length;
}

static void
input_method_do_commit(struct wl_client *client, struct wl_resource *resource,
                       uint32_t serial)
{
	struct input_method *im = wl_resource_get_user_data(resource);
	struct text_input *ti = focused_input;

	(void)client;
	(void)serial;
	if (!im) {
		return;
	}

	if (input_method_trusted() && im->active && serial == im->serial &&
	    ti && ti->enabled && !session_lock_active()) {
		/*
		 * Order matters and is fixed by the protocol: remove the text being
		 * replaced, put the committed text in its place, then describe what
		 * is still being composed.
		 */
		if (im->pending.delete_before || im->pending.delete_after) {
			zwp_text_input_v3_send_delete_surrounding_text(
			    ti->resource, im->pending.delete_before,
			    im->pending.delete_after);
		}
		if (im->pending.commit_string) {
			zwp_text_input_v3_send_commit_string(ti->resource,
			                                     im->pending.commit_string);
		}
		zwp_text_input_v3_send_preedit_string(
		    ti->resource, im->pending.preedit, im->pending.preedit_begin,
		    im->pending.preedit_end);
		zwp_text_input_v3_send_done(ti->resource, ti->serial);
	}

	replace_string(&im->pending.preedit, NULL);
	replace_string(&im->pending.commit_string, NULL);
	im->pending.preedit_begin = 0;
	im->pending.preedit_end = 0;
	im->pending.delete_before = 0;
	im->pending.delete_after = 0;
}

static const struct zwp_input_method_v2_interface input_method_impl = {
	.commit_string = input_method_commit_string,
	.set_preedit_string = input_method_set_preedit_string,
	.delete_surrounding_text = input_method_delete_surrounding_text,
	.commit = input_method_do_commit,
	.get_input_popup_surface = input_method_get_popup_surface,
	.grab_keyboard = input_method_grab_keyboard,
	.destroy = destroy_resource,
};

static void
destroy_input_method(struct wl_resource *resource)
{
	struct input_method *im = wl_resource_get_user_data(resource);
	struct input_popup *popup, *tmp;

	if (!im || input_method != im) {
		return;
	}

	suspend_grab();
	if (im->grab) wl_resource_set_user_data(im->grab, NULL);
	wl_list_for_each_safe(popup, tmp, &im->popups, link)
	    wl_resource_destroy(popup->resource);
	if (grab_handler_linked) {
		wl_list_remove(&grab_handler.link);
		grab_handler_linked = false;
	}
	free(im->pending.preedit);
	free(im->pending.commit_string);
	free(im);
	input_method = NULL;
}

static void
get_input_method(struct wl_client *client, struct wl_resource *manager,
                 struct wl_resource *seat, uint32_t id)
{
	struct input_method *im;
	struct wl_resource *resource;

	(void)seat;

	resource = wl_resource_create(client, &zwp_input_method_v2_interface,
	                              wl_resource_get_version(manager), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	pid_t pid;
	wl_client_get_credentials(client, &pid, NULL, NULL);
	if (input_method || !swc.manager->authorize_input_method ||
	    !swc.manager->authorize_input_method(pid)) {
		/* Only a compositor-authorized foreground service may access input.
		 * One input method per seat. A second is told so rather than
		 * silently sharing the keyboard with the first. */
		wl_resource_set_implementation(resource, &input_method_impl, NULL,
		                               NULL);
		zwp_input_method_v2_send_unavailable(resource);
		return;
	}

	im = calloc(1, sizeof(*im));
	if (!im) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}
	im->resource = resource;
	im->pid = pid;
	wl_list_init(&im->popups);
	input_method = im;
	wl_resource_set_implementation(resource, &input_method_impl, im,
	                               destroy_input_method);

	/* A text field may already be focused and enabled when the input method
	 * starts, which is the normal case when fcitx5 is launched from exec. */
	if (focused_input && focused_input->enabled) {
		input_method_activate();
	}
}

static const struct zwp_input_method_manager_v2_interface
    input_method_manager_impl = {
        .get_input_method = get_input_method,
        .destroy = destroy_resource,
};

static void
bind_input_method_manager(struct wl_client *client, void *data,
                          uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(
	    client, &zwp_input_method_manager_v2_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &input_method_manager_impl, NULL,
	                               NULL);
}

/* ------------------------------------------------------ managers */

static void
get_text_input(struct wl_client *client, struct wl_resource *manager,
               uint32_t id, struct wl_resource *seat)
{
	struct text_input *ti;
	struct compositor_view *focus;

	(void)seat;

	ti = calloc(1, sizeof(*ti));
	if (!ti) {
		wl_client_post_no_memory(client);
		return;
	}
	ti->resource = wl_resource_create(client, &zwp_text_input_v3_interface,
	                                  wl_resource_get_version(manager), id);
	if (!ti->resource) {
		free(ti);
		wl_client_post_no_memory(client);
		return;
	}
	ti->client = client;
	wl_list_init(&ti->entered_destroy_listener.link);
	wl_list_insert(&text_inputs, &ti->link);
	wl_resource_set_implementation(ti->resource, &text_input_impl, ti,
	                               destroy_text_input);

	/* A client that creates its text input after it already has focus -- the
	 * usual order -- still needs the enter event. */
	focus = swc.seat && swc.seat->keyboard ? swc.seat->keyboard->focus.view
	                                       : NULL;
	if (focus && view_surface(focus) &&
	    wl_resource_get_client(view_surface(focus)->resource) == client) {
		set_entered(ti, view_surface(focus));
		zwp_text_input_v3_send_enter(ti->resource, ti->entered->resource);
		if (!focused_input) {
			focused_input = ti;
		}
	}
}

static const struct zwp_text_input_manager_v3_interface
    text_input_manager_impl = {
        .destroy = destroy_resource,
        .get_text_input = get_text_input,
};

static void
bind_text_input_manager(struct wl_client *client, void *data, uint32_t version,
                        uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &zwp_text_input_manager_v3_interface,
	                              version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &text_input_manager_impl, NULL,
	                               NULL);
}

struct wl_global *
text_input_manager_create(struct wl_display *display)
{
	struct wl_global *global;

	if (!initialized) {
		wl_list_init(&text_inputs);
		grab_handler.key = grab_handle_key;
		grab_handler.modifiers = grab_handle_modifiers;
		wl_list_init(&grab_handler.link);
	}
	global = wl_global_create(display, &zwp_text_input_manager_v3_interface, 1,
	                          NULL, &bind_text_input_manager);
	if (global) {
		initialized = true;
	}
	return global;
}

struct wl_global *
input_method_manager_create(struct wl_display *display)
{
	return wl_global_create(display, &zwp_input_method_manager_v2_interface, 1,
	                        NULL, &bind_input_method_manager);
}

void
text_input_finish(void)
{
	struct text_input *ti, *tmp;

	if (!initialized) {
		return;
	}
	if (input_method) {
		wl_resource_destroy(input_method->resource);
	}
	wl_list_for_each_safe(ti, tmp, &text_inputs, link)
	    wl_resource_destroy(ti->resource);
	focused_input = NULL;
	initialized = false;
}
