/* swc: libswc/window.c
 *
 * Copyright (c) 2013 Michael Forney
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

#include "window.h"
#include "compositor.h"
#include "event.h"
#include "foreign_toplevel.h"
#include "internal.h"
#include "keyboard.h"
#include "seat.h"
#include "surface.h"
#include "swc.h"
#include "util.h"
#include "view.h"
#include "xdg_shell.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define INTERNAL(w) ((struct window *)(w))

static const uint32_t def_motion_throttle_ms = 16;

static const struct swc_window_handler null_handler;

static bool
should_throttle_motion(uint32_t throttle_ms, uint32_t *last_time, uint32_t time)
{
	if (!throttle_ms) {
		return false;
	}

	if (*last_time && time - *last_time < throttle_ms) {
		return true;
	}

	*last_time = time;
	return false;
}

static uint32_t
clamp_dimension(int32_t value, uint32_t min, uint32_t max)
{
	if (value < 0) {
		value = 0;
	}

	if (min && value < min) {
		value = min;
	}

	if (max) {
		if (min && max < min) {
			max = min;
		}

		if (value > max) {
			value = max;
		}
	}

	if (value > UINT32_MAX) {
		value = UINT32_MAX;
	}

	return value;
}

static void
clamp_window_size(const struct window *window, uint32_t *width,
                  uint32_t *height)
{
	*width =
	    clamp_dimension(*width, window->base.min_width, window->base.max_width);
	*height = clamp_dimension(*height, window->base.min_height,
	                          window->base.max_height);
}

static void
handle_window_enter(struct wl_listener *listener, void *data)
{
	struct event *event = data;
	struct input_focus_event_data *event_data = event->data;
	struct window *window;

	if (event->type != INPUT_FOCUS_EVENT_CHANGED) {
		return;
	}

	if (!event_data->new || !(window = event_data->new->window)) {
		return;
	}

	if (window->handler->entered) {
		window->handler->entered(window->handler_data);
	}
}

struct wl_listener window_enter_listener = {
    .notify = handle_window_enter,
};

static void
begin_interaction(struct window_pointer_interaction *interaction,
                  struct button *button)
{
	if (button) {
		/* Store the serial of the button press so we are able to cancel the
		 * interaction if the window changes from stacked mode. */
		interaction->serial = button->press.serial;
		interaction->original_handler = button->handler;
		button->handler = &interaction->handler;
	} else {
		interaction->original_handler = NULL;
	}

	interaction->active = true;
	wl_list_insert(&swc.seat->pointer->handlers, &interaction->handler.link);
}

static void
end_interaction(struct window_pointer_interaction *interaction,
                struct button *button)
{
	if (!interaction->active) {
		return;
	}

	if (interaction->original_handler) {
		if (!button) {
			button = pointer_get_button(swc.seat->pointer, interaction->serial);

			if (!button) {
				WARNING("No button with serial %u\n", interaction->serial);
				goto remove;
			}
		}

		interaction->original_handler->button(interaction->original_handler,
		                                      get_time(), button,
		                                      WL_POINTER_BUTTON_STATE_RELEASED);
	}

remove:
	interaction->active = false;
	wl_list_remove(&interaction->handler.link);
}

/*
 * Drop an interaction whose window is going away. Unlike end_interaction()
 * this does not replay the release to the handler the press came from: there
 * is no window left for it to act on. Both places the handler is reachable
 * from have to be cleared -- the seat's handler list, and the held button,
 * which keeps a bare pointer to whichever handler claimed it -- or the next
 * pointer event walks into the freed window.
 */
static void
cancel_interaction(struct window_pointer_interaction *interaction)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;
	struct button *button;

	if (!interaction->active) {
		return;
	}

	if (pointer) {
		wl_array_for_each(button, &pointer->buttons)
		{
			if (button->handler == &interaction->handler) {
				button->handler = interaction->original_handler;
			}
		}
	}

	interaction->active = false;
	interaction->original_handler = NULL;
	wl_list_remove(&interaction->handler.link);
}

static void
flush(struct window *window)
{
	if (window->move.pending) {
		if (window->impl->move) {
			window->impl->move(window, window->move.x, window->move.y);
		}

		view_move(&window->view->base, window->move.x, window->move.y);
		window->move.pending = false;
	}
}

EXPORT void
swc_window_set_handler(struct swc_window *base,
                       const struct swc_window_handler *handler, void *data)
{
	struct window *window = INTERNAL(base);

	window->handler = handler;
	window->handler_data = data;
}

EXPORT void
swc_window_set_raise_on_click(struct swc_window *base, bool enabled)
{
	struct window *window = INTERNAL(base);
	if (window)
		window->raise_on_click = enabled;
}

EXPORT void
swc_window_set_movable(struct swc_window *base, bool enabled)
{
	struct window *window = INTERNAL(base);
	if (!window)
		return;
	if (!enabled)
		end_interaction(&window->move.interaction, NULL);
	window->movable = enabled;
}

EXPORT void
swc_window_set_resizable(struct swc_window *base, bool enabled)
{
	struct window *window = INTERNAL(base);
	if (!window)
		return;
	if (!enabled)
		end_interaction(&window->resize.interaction, NULL);
	window->resizable = enabled;
}

EXPORT void
swc_window_raise(struct swc_window *base)
{
	struct window *window = INTERNAL(base);
	if (window)
		raise_window(window->view);
}

EXPORT void
swc_window_close(struct swc_window *base)
{
	struct window *window = INTERNAL(base);

	if (window->impl->close) {
		window->impl->close(window);
	}
}

EXPORT void
swc_window_show(struct swc_window *window)
{
	compositor_view_show(INTERNAL(window)->view);
}

EXPORT void
swc_window_show_in_place(struct swc_window *window)
{
	compositor_view_show_in_place(INTERNAL(window)->view);
}

EXPORT void
swc_window_hide(struct swc_window *window)
{
	compositor_view_hide(INTERNAL(window)->view);
}

EXPORT void
swc_window_focus(struct swc_window *base)
{
	if (base && input_mode_active()) return;
	struct window *window = INTERNAL(base);
	struct compositor_view *new = window ? window->view : NULL,
	                       *old;

	/* A menu holding a popup grab keeps the keyboard; the window gets it
	 * when the menu is done. */
	if (xdg_popup_grab_defer_window_focus(new)) {
		return;
	}
	old = swc.seat->keyboard->focus.view;
	if (new == old) {
		return;
	}

	/* Focus the new window before unfocusing the old one in case both are X11
	 * windows so the xwl_window implementation can handle this transition
	 * correctly. */
	if (window && window->impl->focus) {
		window->impl->focus(window);
	}
	if (old && old->window && old->window->impl->unfocus) {
		old->window->impl->unfocus(old->window);
	}

	keyboard_set_focus(swc.seat->keyboard, new);
	if (old && old->window)
		foreign_toplevel_window_state(old->window);
	if (window)
		foreign_toplevel_window_state(window);
}

EXPORT void swc_window_set_minimized(struct swc_window *base, bool minimized)
{
	struct window *window = INTERNAL(base);
	if (!window || window->minimized == minimized)
		return;
	window->minimized = minimized;
	foreign_toplevel_window_state(window);
}

EXPORT void swc_window_set_workspace(struct swc_window *base, uint32_t workspace)
{
	struct window *window = INTERNAL(base);
	if (!window || window->workspace == workspace)
		return;
	window->workspace = workspace;
	foreign_toplevel_window_workspace(window);
}

/* Pinning outranks the mode: a pinned window stays above a fullscreen one,
 * and a pinned window that goes fullscreen itself stays above the rest. */
static uint32_t
window_stack_layer(const struct window *window)
{
	if (window->pinned)
		return STACK_LAYER_PINNED;
	return window->mode == WINDOW_MODE_FULLSCREEN ? STACK_LAYER_FULLSCREEN
	                                              : STACK_LAYER_NORMAL;
}

EXPORT void
swc_window_set_pinned(struct swc_window *base, bool pinned)
{
	struct window *window = INTERNAL(base);

	if (!window || window->pinned == pinned)
		return;
	window->pinned = pinned;
	/* Raise as well: pinning something buried should bring it out, and
	 * unpinning should leave it on top of the layer it drops back into
	 * rather than at the bottom of it. */
	compositor_view_set_stack_layer(window->view, window_stack_layer(window),
	                                true);
	foreign_toplevel_window_state(window);
}

EXPORT void
swc_window_set_stacked(struct swc_window *base)
{
	struct window *window = INTERNAL(base);

	flush(window);
	window->configure.pending = false;
	window->configure.acknowledged = false;
	window->configure.width = 0;
	window->configure.height = 0;
	if (window->impl->set_mode) {
		window->impl->set_mode(window, WINDOW_MODE_STACKED);
	}
	window->mode = WINDOW_MODE_STACKED;
	if (window->view->stack_layer != window_stack_layer(window))
		compositor_view_set_stack_layer(window->view,
		                                window_stack_layer(window), true);
	foreign_toplevel_window_state(window);
}

EXPORT void
swc_window_set_tiled(struct swc_window *base)
{
	struct window *window = INTERNAL(base);

	end_interaction(&window->move.interaction, NULL);
	end_interaction(&window->resize.interaction, NULL);
	if (window->impl->set_mode) {
		window->impl->set_mode(window, WINDOW_MODE_TILED);
	}
	window->mode = WINDOW_MODE_TILED;
	if (window->view->stack_layer != window_stack_layer(window))
		compositor_view_set_stack_layer(window->view,
		                                window_stack_layer(window), true);
	foreign_toplevel_window_state(window);
}

EXPORT void
swc_window_set_tiled_edges(struct swc_window *base, uint32_t edges)
{
	struct window *window = INTERNAL(base);

	if (!window || window->tiled_edges == edges) {
		return;
	}
	window->tiled_edges = edges;
	if (window->mode == WINDOW_MODE_TILED && window->impl->set_tiled_edges) {
		window->impl->set_tiled_edges(window, edges);
	}
}

EXPORT void
swc_window_set_fullscreen(struct swc_window *base, struct swc_screen *screen)
{
	struct window *window = INTERNAL(base);

	if (!screen) return;
	end_interaction(&window->move.interaction, NULL);
	end_interaction(&window->resize.interaction, NULL);
	if (window->mode != WINDOW_MODE_FULLSCREEN && window->impl->set_mode)
		window->impl->set_mode(window, WINDOW_MODE_FULLSCREEN);
	window->mode = WINDOW_MODE_FULLSCREEN;
	/* Set the mode before sizing: fullscreen must ignore normal size hints. */
	swc_window_set_geometry(base, &screen->geometry);
	compositor_view_set_stack_layer(window->view, window_stack_layer(window),
	                                true);
	foreign_toplevel_window_state(window);
}

EXPORT void
swc_window_set_position(struct swc_window *base, int32_t x, int32_t y)
{
	struct window *window = INTERNAL(base);
	struct swc_rectangle *geometry = &window->view->base.geometry;

	if (x == geometry->x && y == geometry->y) {
		window->move.pending = false;
		return;
	}

	window->move.x = x;
	window->move.y = y;
	window->move.pending = true;

	/* If we don't have a configure pending, perform the move now. */
	if (!window->configure.pending) {
		flush(window);
	}
}

EXPORT void
swc_window_set_size(struct swc_window *base, uint32_t width, uint32_t height)
{
	struct window *window = INTERNAL(base);
	struct swc_rectangle *geom = &window->view->base.geometry;

	if (window->mode == WINDOW_MODE_STACKED)
		clamp_window_size(window, &width, &height);

	if ((window->configure.pending && width == window->configure.width &&
	     height == window->configure.height) ||
	    (!window->configure.pending && width == geom->width &&
	     height == geom->height)) {
		return;
	}

	window->configure.width = width;
	window->configure.height = height;
	window->configure.pending = true;
	window->impl->configure(window, width, height);
}

EXPORT void
swc_window_set_geometry(struct swc_window *window,
                        const struct swc_rectangle *geometry)
{
	swc_window_set_size(window, geometry->width, geometry->height);
	swc_window_set_position(window, geometry->x, geometry->y);
}

EXPORT bool
swc_window_get_geometry(const struct swc_window *base,
                        struct swc_rectangle *geometry)
{
	struct window *window = INTERNAL((struct swc_window *)base);

	if (!window || !geometry) {
		return false;
	}

	*geometry = window->view->base.geometry;
	return true;
}

EXPORT void
swc_window_set_border(struct swc_window *window, uint32_t inner_border_color,
                      uint32_t inner_border_width, uint32_t outer_border_color,
                      uint32_t outer_border_width)
{
	struct compositor_view *view = INTERNAL(window)->view;

	compositor_view_set_border_color(view, outer_border_color,
	                                 inner_border_color);
	compositor_view_set_border_width(view, outer_border_width,
	                                 inner_border_width);
}

EXPORT void
swc_window_set_decor(struct swc_window *window, const struct swc_decor *decor)
{
	struct compositor_view *view = INTERNAL(window)->view;

	compositor_view_set_decor(view, decor);
}

EXPORT void
swc_window_begin_move(struct swc_window *window)
{
	window_begin_move(INTERNAL(window), NULL);
}

EXPORT void
swc_window_apply_decor(struct swc_window *window, struct swc_prepared_decor *prepared)
{
	compositor_view_apply_decor(INTERNAL(window)->view, prepared);
}

EXPORT void
swc_window_end_move(struct swc_window *window)
{
	end_interaction(&INTERNAL(window)->move.interaction, NULL);
}

EXPORT void
swc_window_begin_resize(struct swc_window *window, uint32_t edges)
{
	window_begin_resize(INTERNAL(window), edges, NULL);
}

EXPORT void
swc_window_end_resize(struct swc_window *window)
{
	end_interaction(&INTERNAL(window)->resize.interaction, NULL);
}

static bool
move_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx,
            wl_fixed_t fy)
{
	struct window *window =
	    wl_container_of(handler, window, move.interaction.handler);

	if (should_throttle_motion(window->base.motion_throttle_ms,
	                           &window->move.last_time, time)) {
		return true;
	}

	int32_t x = wl_fixed_to_int(fx) + window->move.offset.x,
	        y = wl_fixed_to_int(fy) + window->move.offset.y;

	view_move(&window->view->base, x, y);
	return true;
}

static bool
resize_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx,
              wl_fixed_t fy)
{
	struct window *window =
	    wl_container_of(handler, window, resize.interaction.handler);
	const struct swc_rectangle *geometry = &window->view->base.geometry;
	int64_t width = geometry->width, height = geometry->height;

	if (should_throttle_motion(window->base.motion_throttle_ms,
	                           &window->resize.last_time, time)) {
		return true;
	}

	if (window->resize.edges & SWC_WINDOW_EDGE_LEFT) {
		width -= wl_fixed_to_int(fx) + window->resize.offset.x - geometry->x;
	} else if (window->resize.edges & SWC_WINDOW_EDGE_RIGHT) {
		width = wl_fixed_to_int(fx) + window->resize.offset.x - geometry->x;
	}

	if (window->resize.edges & SWC_WINDOW_EDGE_TOP) {
		height -= wl_fixed_to_int(fy) + window->resize.offset.y - geometry->y;
	} else if (window->resize.edges & SWC_WINDOW_EDGE_BOTTOM) {
		height = wl_fixed_to_int(fy) + window->resize.offset.y - geometry->y;
	}

	/* Crossing the opposite edge must not wrap into a multi-gigapixel size. */
	width = width < 1 ? 1 : width > INT32_MAX ? INT32_MAX : width;
	height = height < 1 ? 1 : height > INT32_MAX ? INT32_MAX : height;
	swc_window_set_size(&window->base, width, height);

	return true;
}

static bool
handle_button(struct pointer_handler *handler, uint32_t time,
              struct button *button, uint32_t state)
{
	struct window_pointer_interaction *interaction =
	    wl_container_of(handler, interaction, handler);

	if (state != WL_POINTER_BUTTON_STATE_RELEASED ||
	    !interaction->original_handler) {
		return false;
	}

	end_interaction(interaction, button);
	return true;
}

void
window_commit(struct window *window)
{
	if (window->configure.acknowledged) {
		flush(window);
		window->configure.pending = false;
		window->configure.acknowledged = false;
	}
}

static void
handle_attach(struct view_handler *handler)
{
	struct window *window = wl_container_of(handler, window, view_handler);
	window_commit(window);
}

static void
handle_resize(struct view_handler *handler, uint32_t old_width,
              uint32_t old_height)
{
	struct window *window = wl_container_of(handler, window, view_handler);

	if (window->resize.interaction.active &&
	    window->resize.edges & (SWC_WINDOW_EDGE_TOP | SWC_WINDOW_EDGE_LEFT)) {
		const struct swc_rectangle *geometry = &window->view->base.geometry;
		int32_t x = geometry->x, y = geometry->y;

		if (window->resize.edges & SWC_WINDOW_EDGE_LEFT) {
			x += old_width - geometry->width;
		}
		if (window->resize.edges & SWC_WINDOW_EDGE_TOP) {
			y += old_height - geometry->height;
		}

		view_move(&window->view->base, x, y);
	}
	if (window->managed && window->handler->geometry_changed)
		window->handler->geometry_changed(window->handler_data);
}

static void handle_screens(struct view_handler *handler, uint32_t entered,
		uint32_t left)
{
	struct window *window = wl_container_of(handler, window, view_handler);
	foreign_toplevel_window_screens(window, entered, left);
}

static const struct view_handler_impl view_handler_impl = {
    .attach = handle_attach,
    .resize = handle_resize,
    .screens = handle_screens,
};

bool
window_initialize(struct window *window, const struct window_impl *impl,
                  struct surface *surface)
{
	DEBUG("Initializing window, %p\n", window);

	window->base.title = NULL;
	window->base.app_id = NULL;
	window->base.parent = NULL;

	if (surface->view) {
		window->view = compositor_view(surface->view);
		if (!window->view || window->view->window) {
			return false;
		}
	} else {
		if (!(window->view = compositor_create_view(surface))) {
			return false;
		}
	}

	window->impl = impl;
	window->handler = &null_handler;
	window->view_handler.impl = &view_handler_impl;
	window->view->window = window;
	window->base.motion_throttle_ms = def_motion_throttle_ms;
	window->base.min_width = 0;
	window->base.min_height = 0;
	window->base.max_width = 0;
	window->base.max_height = 0;
	window->managed = false;
	window->minimized = false;
	window->raise_on_click = true;
	window->movable = true;
	window->resizable = true;
	window->pinned = false;
	window->foreign_toplevel = NULL;
	window->mode = WINDOW_MODE_STACKED;
	window->move.pending = false;
	window->move.last_time = 0;
	window->move.interaction.active = false;
	window->move.interaction.handler = (struct pointer_handler){
	    .motion = move_motion,
	    .button = handle_button,
	};
	window->configure.pending = false;
	window->configure.acknowledged = false;
	window->configure.width = 0;
	window->configure.height = 0;
	window->resize.interaction.active = false;
	window->resize.interaction.handler = (struct pointer_handler){
	    .motion = resize_motion,
		.button = handle_button,
	};
	window->resize.last_time = 0;

	wl_list_insert(&window->view->base.handlers, &window->view_handler.link);

	return true;
}

void
window_finalize(struct window *window)
{
	DEBUG("Finalizing window, %p\n", window);

	window_unmanage(window);
	/* Before the view goes: a window destroyed mid-move or mid-resize leaves
	 * its interaction handler linked into the seat, and the button held on
	 * it pointing at memory that is about to be freed. */
	cancel_interaction(&window->move.interaction);
	cancel_interaction(&window->resize.interaction);
	compositor_view_destroy(window->view);
	free(window->base.title);
	free(window->base.app_id);
}

void
window_manage(struct window *window)
{
	if (window->managed) {
		return;
	}

	swc.manager->new_window(&window->base);
	window->managed = true;
	foreign_toplevel_window_manage(window);
}

void
window_unmanage(struct window *window)
{
	if (!window->managed) {
		return;
	}

	foreign_toplevel_window_unmanage(window);
	if (window->handler->destroy) {
		window->handler->destroy(window->handler_data);
	}
	window->handler = &null_handler;
	window->managed = false;
}

void
window_set_title(struct window *window, const char *title, size_t length)
{
	char *copy = title ? strndup(title, length) : NULL;

	/* Said again rather than changed: nothing downstream has anything to
	 * do, and a taskbar would repaint every panel for it. A shell that
	 * sets its title on every prompt does this constantly. */
	if (!copy == !window->base.title &&
	    (!copy || strcmp(copy, window->base.title) == 0)) {
		free(copy);
		return;
	}

	free(window->base.title);
	window->base.title = copy;

	if (window->handler->title_changed) {
		window->handler->title_changed(window->handler_data);
	}
	foreign_toplevel_window_title(window);
}

void
window_set_app_id(struct window *window, const char *app_id)
{
	char *copy = app_id ? strdup(app_id) : NULL;

	if (!copy == !window->base.app_id &&
	    (!copy || strcmp(copy, window->base.app_id) == 0)) {
		free(copy);
		return;
	}

	free(window->base.app_id);
	window->base.app_id = copy;

	if (window->handler->app_id_changed) {
		window->handler->app_id_changed(window->handler_data);
	}
	foreign_toplevel_window_app_id(window);
}

void
window_set_parent(struct window *window, struct window *parent)
{
	struct swc_window *base_parent = parent ? &parent->base : NULL;
	if (window->base.parent == base_parent) {
		return;
	}

	compositor_view_set_parent(window->view, parent ? parent->view : NULL);
	window->base.parent = base_parent;

	if (window->handler->parent_changed) {
		window->handler->parent_changed(window->handler_data);
	}
	foreign_toplevel_window_parent(window);
}

void
window_begin_move(struct window *window, struct button *button)
{
	if (!window->movable)
		return;

	if (window->mode != WINDOW_MODE_STACKED && window->handler->move) {
		window->handler->move(window->handler_data);
	}

	if (window->mode != WINDOW_MODE_STACKED ||
	    window->move.interaction.active) {
		return;
	}

	struct swc_rectangle *geometry = &window->view->base.geometry;
	int32_t px = wl_fixed_to_int(swc.seat->pointer->x),
	        py = wl_fixed_to_int(swc.seat->pointer->y);

	begin_interaction(&window->move.interaction, button);
	window->move.last_time = 0;
	window->move.offset.x = geometry->x - px;
	window->move.offset.y = geometry->y - py;
}

void
window_begin_resize(struct window *window, uint32_t edges,
                    struct button *button)
{
	if (!window->resizable)
		return;

	if (window->mode != WINDOW_MODE_STACKED && window->handler->resize) {
		window->handler->resize(window->handler_data);
	}

	if (window->mode != WINDOW_MODE_STACKED ||
	    window->resize.interaction.active) {
		return;
	}

	struct swc_rectangle *geometry = &window->view->base.geometry;
	int32_t px = wl_fixed_to_int(swc.seat->pointer->x),
	        py = wl_fixed_to_int(swc.seat->pointer->y);

	begin_interaction(&window->resize.interaction, button);
	window->resize.last_time = 0;

	if (!edges) {
		edges |= (px < geometry->x + geometry->width / 2)
		             ? SWC_WINDOW_EDGE_LEFT
		             : SWC_WINDOW_EDGE_RIGHT;
		edges |= (py < geometry->y + geometry->height / 2)
		             ? SWC_WINDOW_EDGE_TOP
		             : SWC_WINDOW_EDGE_BOTTOM;
	}

	window->resize.offset.x =
	    geometry->x - px +
	    ((edges & SWC_WINDOW_EDGE_RIGHT) ? geometry->width : 0);
	window->resize.offset.y =
	    geometry->y - py +
	    ((edges & SWC_WINDOW_EDGE_BOTTOM) ? geometry->height : 0);
	window->resize.edges = edges;
}

EXPORT pid_t
swc_window_get_pid(struct swc_window *base)
{
	struct window *window = INTERNAL(base);
	struct surface *surface;
	struct wl_client *client;
	pid_t pid;
	uid_t uid;
	gid_t gid;

	if (!window || !window->view || !window->view->surface) {
		return 0;
	}

	surface = window->view->surface;
	if (!surface->resource) {
		return 0;
	}

	client = wl_resource_get_client(surface->resource);
	wl_client_get_credentials(client, &pid, &uid, &gid);

	return pid;
}
