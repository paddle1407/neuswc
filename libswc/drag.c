/* swc: drag.c
 *
 * Copyright (c) 2013-2020 Michael Forney
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

#include "drag.h"
#include "compositor.h"
#include "data.h"
#include "data_device.h"
#include "internal.h"
#include "keyboard.h"
#include "pointer.h"
#include "seat.h"
#include "surface.h"
#include "titlebar.h"
#include "util.h"

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <wayland-server.h>

/*
 * There is one seat, so there is at most one drag, and its state lives here
 * rather than on the heap.
 *
 * That is not only for convenience. A pointer button release is dispatched
 * straight to the handler that took the press (see pointer_handle_button),
 * which then writes handler->pending once the callback returns -- so the
 * handler must outlive the drag it ends. A static struct always does.
 */
static struct {
	/* Whether the pointer grab is still held. The drop itself is not the end
	 * of it: a version 3 source hears dnd_finished later, so source and offer
	 * outlive the grab. */
	bool active;
	/* Drop delivered, waiting for wl_data_offer.finish. */
	bool dropped;

	struct data_device *data_device;
	uint32_t button;

	struct wl_resource *source; /* wl_data_source, NULL for an internal drag */
	struct wl_listener source_destroy;

	/* The view under the pointer, and what we have told it. */
	struct compositor_view *focus;
	struct wl_listener focus_destroy;
	struct wl_resource *device; /* the focused client's wl_data_device */
	struct wl_listener device_destroy;
	struct wl_resource *offer;
	struct wl_listener offer_destroy;
	bool accepted;
	uint32_t offer_actions, preferred_action, action;

	struct surface *icon;
	struct compositor_view *icon_view;
	struct wl_listener icon_destroy, icon_commit;
	int32_t icon_x, icon_y;

	struct pointer_handler pointer_handler;
	struct keyboard_handler keyboard_handler;
} drag;

/* Defined below, next to the grab it releases. */
static void
restore_pointer(void);

static void
send_action(void)
{
	if (drag.offer
	    && wl_resource_get_version(drag.offer) >= WL_DATA_OFFER_ACTION_SINCE_VERSION) {
		wl_data_offer_send_action(drag.offer, drag.action);
	}
	if (drag.source
	    && wl_resource_get_version(drag.source) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
		wl_data_source_send_action(drag.source, drag.action);
	}
}

/*
 * The action the drop would carry out, from what the source offers and what
 * the destination is willing to take.
 *
 * Versions 1 and 2 have no say in this -- neither side can name an action, so
 * the intersection would always be empty and no drop would ever be delivered.
 * For them the drag is a copy, which is what those versions always meant.
 */
static uint32_t
compute_action(void)
{
	uint32_t source_actions, matched;

	if (!drag.offer) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	}
	if (wl_resource_get_version(drag.offer) < 3
	    || (drag.source && wl_resource_get_version(drag.source) < 3)) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	}

	source_actions =
	    drag.source ? data_source_actions(drag.source) : DATA_DND_ACTION_ALL;
	matched = source_actions & drag.offer_actions;

	if (!matched) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	}
	if (drag.preferred_action & matched) {
		return drag.preferred_action;
	}
	if (matched & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	}
	if (matched & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	}
	return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
}

static void
update_action(void)
{
	uint32_t action = compute_action();

	if (action == drag.action) {
		return;
	}
	drag.action = action;
	send_action();
}

static void
release_source(void)
{
	if (drag.source) {
		wl_list_remove(&drag.source_destroy.link);
		drag.source = NULL;
	}
}

/* Drop our references to the target without telling it anything. */
static void
forget_focus(void)
{
	if (drag.offer) {
		wl_list_remove(&drag.offer_destroy.link);
		drag.offer = NULL;
	}
	if (drag.focus) {
		wl_list_remove(&drag.focus_destroy.link);
		drag.focus = NULL;
	}
	if (drag.device) {
		wl_list_remove(&drag.device_destroy.link);
		drag.device = NULL;
	}
	drag.accepted = false;
	drag.offer_actions = 0;
	drag.preferred_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	drag.action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

/* Tell the target the drag has left it, then forget it. */
static void
leave_focus(void)
{
	if (drag.device) {
		wl_data_device_send_leave(drag.device);
	}
	forget_focus();
	/* Nothing is targeted any more, so nothing would be carried out. */
	send_action();
}

static void
handle_offer_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	wl_list_remove(&drag.offer_destroy.link);
	drag.offer = NULL;
	drag.accepted = false;
	/* A destination that drops the offer instead of finishing it is not going
	 * to; the source has been waiting on a transfer that will never come. */
	if (drag.dropped) {
		drag.dropped = false;
		if (drag.source) {
			wl_data_source_send_cancelled(drag.source);
			release_source();
		}
		restore_pointer();
	}
}

static void
handle_device_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	/* The client is going away in the middle of the drag. Drop every
	 * reference to it before its surfaces and offers are destroyed too. */
	wl_list_remove(&drag.device_destroy.link);
	drag.device = NULL;
	forget_focus();
	if (drag.dropped) {
		drag.dropped = false;
		if (drag.source) {
			wl_data_source_send_cancelled(drag.source);
			release_source();
		}
	}
	restore_pointer();
}

static void
handle_focus_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	/* The view is going away under the pointer. The client is not: it still
	 * has a data device, and is owed the leave for the enter it was sent. */
	leave_focus();
}

static void
surface_coords(struct compositor_view *view, wl_fixed_t *sx, wl_fixed_t *sy)
{
	struct pointer *pointer = swc.seat->pointer;
	int32_t origin_x = view->base.geometry.x - view->buffer_offset_x;
	int32_t origin_y = view->base.geometry.y - view->buffer_offset_y;

	*sx = pointer->x - wl_fixed_from_int(origin_x);
	*sy = pointer->y - wl_fixed_from_int(origin_y);
}

static void
set_focus(struct compositor_view *view)
{
	struct wl_client *client;
	struct wl_resource *device, *offer = NULL;
	wl_fixed_t sx, sy;

	if (view == drag.focus) {
		return;
	}
	leave_focus();

	if (!view || !view->surface) {
		return;
	}
	/* Track the view either way, so a client without a data device does not
	 * get looked up again on every motion event. */
	drag.focus = view;
	drag.focus_destroy.notify = &handle_focus_destroy;
	wl_signal_add(&view->destroy_signal, &drag.focus_destroy);

	client = wl_resource_get_client(view->surface->resource);
	device = wl_resource_find_for_client(&drag.data_device->resources, client);
	if (!device) {
		return;
	}

	if (drag.source) {
		offer =
		    data_offer_new(client, drag.source, wl_resource_get_version(device));
		if (!offer) {
			wl_client_post_no_memory(client);
			return;
		}
		wl_data_device_send_data_offer(device, offer);
		data_send_mime_types(drag.source, offer);
		if (wl_resource_get_version(offer)
		    >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
			wl_data_offer_send_source_actions(offer,
			                                  data_source_actions(drag.source));
		}
		drag.offer = offer;
		drag.offer_destroy.notify = &handle_offer_destroy;
		wl_resource_add_destroy_listener(offer, &drag.offer_destroy);
	}

	drag.device = device;
	drag.device_destroy.notify = &handle_device_destroy;
	wl_resource_add_destroy_listener(device, &drag.device_destroy);
	surface_coords(view, &sx, &sy);
	wl_data_device_send_enter(device, wl_display_next_serial(swc.display),
	                          view->surface->resource, sx, sy, offer);
	update_action();
}

/* Drag icon {{{ */

static void
move_icon(void)
{
	struct pointer *pointer = swc.seat->pointer;

	if (!drag.icon_view) {
		return;
	}
	view_move(&drag.icon_view->base,
	          wl_fixed_to_int(pointer->x) + drag.icon_x,
	          wl_fixed_to_int(pointer->y) + drag.icon_y);
}

static void
destroy_icon(void)
{
	if (!drag.icon) {
		return;
	}
	wl_list_remove(&drag.icon_destroy.link);
	wl_list_remove(&drag.icon_commit.link);
	compositor_view_destroy(drag.icon_view);
	drag.icon = NULL;
	drag.icon_view = NULL;
}

static void
handle_icon_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	/* The surface is still alive during this signal, and its view must go
	 * before it does. */
	wl_list_remove(&drag.icon_destroy.link);
	wl_list_remove(&drag.icon_commit.link);
	compositor_view_destroy(drag.icon_view);
	drag.icon = NULL;
	drag.icon_view = NULL;
}

static void
handle_icon_commit(struct wl_listener *listener, void *data)
{
	struct surface *surface = data;

	(void)listener;

	/*
	 * wl_surface.attach offsets move a drag icon relative to the pointer.
	 * swc ignores them everywhere else -- nothing reads surface->pending.x --
	 * so take them here and clear them, which also keeps the next commit from
	 * applying the same offset again.
	 */
	drag.icon_x += surface->pending.x;
	drag.icon_y += surface->pending.y;
	surface->pending.x = 0;
	surface->pending.y = 0;
	pixman_region32_clear(&surface->state.input);
	move_icon();

	/* An icon with nothing in it yet has nothing to show. */
	if (surface_has_buffer(surface)) {
		compositor_view_show(drag.icon_view);
	}
}

static bool
create_icon(struct surface *surface)
{
	drag.icon_view = compositor_create_view(surface);
	if (!drag.icon_view) {
		return false;
	}
	drag.icon = surface;
	drag.icon_x = 0;
	drag.icon_y = 0;
	/*
	 * A drag icon takes no input -- the protocol says its input region is
	 * ignored, and it has to be, because the icon follows the pointer and
	 * would otherwise be the only thing ever found under it.
	 */
	pixman_region32_clear(&surface->state.input);
	compositor_view_set_stack_layer(drag.icon_view, STACK_LAYER_OVERLAY, true);
	drag.icon_destroy.notify = &handle_icon_destroy;
	wl_signal_add(&surface->signal.destroy, &drag.icon_destroy);
	drag.icon_commit.notify = &handle_icon_commit;
	wl_signal_add(&surface->signal.commit, &drag.icon_commit);
	move_icon();
	if (surface_has_buffer(surface)) {
		compositor_view_show(drag.icon_view);
	}
	return true;
}

/* }}} */

static void
end_grab(void)
{
	if (!drag.active) {
		return;
	}
	drag.active = false;
	wl_list_remove(&drag.pointer_handler.link);
	wl_list_remove(&drag.keyboard_handler.link);
	destroy_icon();
}

/*
 * Give the pointer back to the clients, once the drag is over for them too.
 *
 * Not a moment earlier. A client that is told the pointer has entered while it
 * still believes it is dragging discards the enter, and nothing generates
 * another one: input_focus_set() returns early when the view has not changed,
 * so the compositor goes on thinking that client is focused while the client
 * thinks the pointer is somewhere else entirely. Everything it is sent from
 * then on is ignored, and only focusing another window and coming back breaks
 * the tie. It takes a drag that begins and ends in the same client to hit --
 * anywhere else the client hearing the enter is not the one doing the drag.
 *
 * Leaving the focus clear until then is also what makes this safe: if a
 * destination never finishes the drop, the next motion or click restores the
 * focus through the ordinary path, because the compositor still has none.
 */
static void
restore_pointer(void)
{
	if (drag.active || drag.dropped) {
		return;
	}
	compositor_refocus_pointer();
}

static void
cancel_drag(void)
{
	end_grab();
	leave_focus();
	if (drag.source) {
		wl_data_source_send_cancelled(drag.source);
		release_source();
	}
	drag.dropped = false;
	restore_pointer();
}

static void
drop(void)
{
	bool deliver = drag.device && drag.offer && drag.accepted
	               && drag.action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;

	end_grab();

	if (!deliver) {
		cancel_drag();
		return;
	}

	wl_data_device_send_drop(drag.device);
	if (drag.source
	    && wl_resource_get_version(drag.source)
	           >= WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION) {
		wl_data_source_send_dnd_drop_performed(drag.source);
		/* The source is not done until the destination says so, so keep both
		 * ends around for wl_data_offer.finish. */
		drag.dropped = true;
		return;
	}

	/* Version 1 and 2 have no finish; the transfer is between the two clients
	 * from here on, and the drag session is over. */
	release_source();
	leave_focus();
	restore_pointer();
}

static void
handle_source_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	wl_list_remove(&drag.source_destroy.link);
	drag.source = NULL;
	if (drag.active || drag.dropped) {
		end_grab();
		drag.dropped = false;
		leave_focus();
		restore_pointer();
	}
}

static bool
handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t x,
              wl_fixed_t y)
{
	struct compositor_view *view;
	int32_t ix = wl_fixed_to_int(x), iy = wl_fixed_to_int(y);
	wl_fixed_t sx, sy;

	(void)handler;

	if (!drag.active) {
		return false;
	}
	move_icon();

	view = compositor_view_at(ix, iy);
	/* Decorations are the compositor's, not the client's: a window is not a
	 * drop target by its titlebar. */
	if (view && titlebar_hit(view, ix, iy) != -2) {
		view = NULL;
	}

	if (view != drag.focus) {
		set_focus(view);
	} else if (drag.device) {
		surface_coords(drag.focus, &sx, &sy);
		wl_data_device_send_motion(drag.device, time, sx, sy);
	}
	return true;
}

static bool
handle_button(struct pointer_handler *handler, uint32_t time,
              struct button *button, uint32_t state)
{
	(void)handler;
	(void)time;

	/* The button that started the drag still names this handler even after the
	 * drag has ended, so its release arrives here either way. */
	if (drag.active && state == WL_POINTER_BUTTON_STATE_RELEASED
	    && button->press.value == drag.button) {
		drop();
	}
	return true;
}

static bool
handle_key(struct keyboard *keyboard, uint32_t time, struct key *key,
           uint32_t state)
{
	(void)keyboard;
	(void)time;

	if (key->press.value != KEY_ESC) {
		return false;
	}
	if (drag.active && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		cancel_drag();
	}
	/* Swallow the release of an escape that ended a drag as well. */
	return true;
}

void
drag_start(struct data_device *data_device, struct wl_resource *device_resource,
           struct wl_resource *source_resource,
           struct wl_resource *origin_resource,
           struct wl_resource *icon_resource, uint32_t serial)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;
	struct keyboard *keyboard = swc.seat ? swc.seat->keyboard : NULL;
	struct surface *origin, *icon = NULL;
	struct button *button;

	if (!pointer || !keyboard || drag.active) {
		return;
	}
	origin = surface_from_resource(origin_resource);
	if (!origin) {
		return;
	}
	/* The drag has to come out of a button the client is holding, on the
	 * surface the pointer is actually over. Anything else lost the race and is
	 * ignored rather than killed. */
	button = pointer_get_button(pointer, serial);
	if (!button || !pointer->focus.view || pointer->focus.view->surface != origin) {
		return;
	}

	if (icon_resource) {
		icon = surface_from_resource(icon_resource);
		if (!icon) {
			return;
		}
		if (icon->role != device_resource
		    && !surface_set_role(icon, device_resource)) {
			wl_resource_post_error(device_resource, WL_DATA_DEVICE_ERROR_ROLE,
			                       "surface already has a role");
			return;
		}
	}

	/* Whatever the last drag left behind is over now. */
	forget_focus();
	release_source();
	drag.dropped = false;

	drag.data_device = data_device;
	drag.button = button->press.value;
	drag.source = source_resource;
	if (drag.source) {
		drag.source_destroy.notify = &handle_source_destroy;
		wl_resource_add_destroy_listener(drag.source, &drag.source_destroy);
	}

	if (icon && !create_icon(icon)) {
		wl_client_post_no_memory(wl_resource_get_client(device_resource));
		release_source();
		return;
	}

	drag.pointer_handler.motion = &handle_motion;
	drag.pointer_handler.button = &handle_button;
	drag.pointer_handler.axis = NULL;
	drag.pointer_handler.frame = NULL;
	drag.pointer_handler.pending = 0;
	drag.keyboard_handler.key = &handle_key;
	drag.keyboard_handler.modifiers = NULL;
	wl_list_insert(&pointer->handlers, &drag.pointer_handler.link);
	wl_list_insert(&keyboard->handlers, &drag.keyboard_handler.link);
	drag.active = true;

	/*
	 * A release is dispatched to the handler that took the press, not down the
	 * handler list, so the drag has to inherit the implicit grab to hear the
	 * drop at all. The origin gets no more pointer events either way: it is
	 * dragging, not clicking.
	 */
	button->handler = &drag.pointer_handler;
	pointer_set_focus(pointer, NULL);

	set_focus(compositor_view_at(wl_fixed_to_int(pointer->x),
	                             wl_fixed_to_int(pointer->y)));
}

void
drag_offer_accept(struct wl_resource *offer, const char *mime_type)
{
	if (offer != drag.offer) {
		return;
	}
	drag.accepted = mime_type != NULL;
	update_action();
}

void
drag_offer_set_actions(struct wl_resource *offer, uint32_t actions,
                       uint32_t preferred)
{
	if (offer != drag.offer) {
		return;
	}
	drag.offer_actions = actions & DATA_DND_ACTION_ALL;
	drag.preferred_action = preferred & DATA_DND_ACTION_ALL;
	update_action();
}

void
drag_offer_finish(struct wl_resource *offer)
{
	if (offer != drag.offer || !drag.dropped) {
		return;
	}
	drag.dropped = false;
	if (drag.source
	    && wl_resource_get_version(drag.source)
	           >= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
		wl_data_source_send_dnd_finished(drag.source);
	}
	/* Released first so the leave does not also report an action to a source
	 * that has just been told the drag finished. */
	release_source();
	leave_focus();
	restore_pointer();
}

void
drag_source_actions_changed(struct wl_resource *source)
{
	if (source != drag.source) {
		return;
	}
	if (drag.offer
	    && wl_resource_get_version(drag.offer)
	           >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
		wl_data_offer_send_source_actions(drag.offer,
		                                  data_source_actions(source));
	}
	update_action();
}
