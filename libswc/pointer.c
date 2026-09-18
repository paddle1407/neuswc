/* swc: libswc/pointer.c
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

#include "pointer.h"
#include "backend.h"
#include "compositor.h"
#include "screencopy.h"
#include "cursor/cursor_data.h"
#include "event.h"
#include "internal.h"
#ifdef ENABLE_DRM
#include "plane.h"
#endif
#include "screen.h"
#include "output.h"
#include "mode.h"
#include "seat.h"
#include "shm.h"
#include "surface.h"
#include "pointer_constraints.h"
#include "relative_pointer.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wld/wld.h>

static enum swc_cursor_kind cursor_override = SWC_CURSOR_DEFAULT;
static enum swc_cursor_mode cursor_mode = SWC_CURSOR_MODE_CLIENT;

/*
 * A shape the focused client asked for through cursor-shape-v1. It behaves
 * like a client cursor surface -- it belongs to whichever client the pointer
 * is over, and is dropped when the pointer leaves -- but the pixels come from
 * the window manager's theme rather than from the client.
 */
static enum swc_cursor_kind client_shape = SWC_CURSOR_DEFAULT;
static bool client_shape_set;

static bool profile_input;
static struct input_profile {
	uint64_t total;
	uint32_t count, worst;
} motion_profile, cursor_profile;

static void
record_input_time(struct input_profile *profile, const char *label, uint32_t start)
{
	if (!profile_input) return;
	uint32_t elapsed = get_time() - start;
	profile->total += elapsed;
	profile->worst = MAX(profile->worst, elapsed);
	if (++profile->count == 120) {
		fprintf(stderr, "input-profile: %s: 120 calls, average %.3f ms, worst %u ms\n",
		        label, (double)profile->total / profile->count, profile->worst);
		memset(profile, 0, sizeof(*profile));
	}
}

static struct {
	struct wld_buffer *buffer;
	uint32_t width, height;
	int32_t hotspot_x, hotspot_y;
	bool active;
} cursor_images[SWC_CURSOR_KIND_COUNT];

EXPORT void
swc_pointer_send_button(uint32_t time, uint32_t button, uint32_t state)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;
	struct wl_resource *resource;
	uint32_t serial;

	if (!pointer || wl_list_empty(&pointer->focus.active)) {
		return;
	}

	serial = wl_display_next_serial(swc.display);
	wl_resource_for_each(resource, &pointer->focus.active)
	    wl_pointer_send_button(resource, serial, time, button, state);
	wl_resource_for_each(resource, &pointer->focus.active)
	{
		if (wl_resource_get_version(resource) >=
		    WL_POINTER_FRAME_SINCE_VERSION) {
			wl_pointer_send_frame(resource);
		}
	}
	pointer->client_axis_source = -1;
}

EXPORT void
swc_pointer_send_axis(uint32_t time, uint32_t axis, int32_t value120)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;
	struct wl_resource *resource;
	wl_fixed_t value;

	if (!pointer || wl_list_empty(&pointer->focus.active)) {
		return;
	}

	value = wl_fixed_from_double((double)value120 / 120.0);

	wl_resource_for_each(resource, &pointer->focus.active)
	{
		int ver = wl_resource_get_version(resource);

		if (ver >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
			wl_pointer_send_axis_source(resource, WL_POINTER_AXIS_SOURCE_WHEEL);
		}
		if (value120) {
			if (ver >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
				wl_pointer_send_axis_value120(resource, axis, value120);
			} else if (ver >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
				wl_pointer_send_axis_discrete(resource, axis, value120 / 120);
			}
		}

		if (value) {
			wl_pointer_send_axis(resource, time, axis, value);
		} else if (ver >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
			wl_pointer_send_axis_stop(resource, time, axis);
		}
	}

	wl_resource_for_each(resource, &pointer->focus.active)
	{
		if (wl_resource_get_version(resource) >=
		    WL_POINTER_FRAME_SINCE_VERSION) {
			wl_pointer_send_frame(resource);
		}
	}
	pointer->client_axis_source = -1;
}

static void
enter(struct input_focus_handler *handler, struct wl_list *resources,
      struct compositor_view *view)
{
	struct pointer *pointer = wl_container_of(handler, pointer, focus_handler);
	struct wl_resource *resource;
	uint32_t serial;
	wl_fixed_t surface_x, surface_y;
	int32_t origin_x, origin_y;

	/* Each client starts from the theme default; a shape lasts only as long
	 * as the pointer stays inside the surface that asked for it. */
	pointer_clear_shape(pointer);

	if (wl_list_empty(resources)) {
		pointer_set_cursor(pointer, cursor_left_ptr);
		return;
	}
	serial = wl_display_next_serial(swc.display);
	/* do based on buffer origin, holy fuck */
	origin_x = view->base.geometry.x - view->buffer_offset_x;
	origin_y = view->base.geometry.y - view->buffer_offset_y;
	surface_x = pointer->x - wl_fixed_from_int(origin_x);
	surface_y = pointer->y - wl_fixed_from_int(origin_y);
	wl_resource_for_each(resource, resources) {
		wl_pointer_send_enter(resource, serial, view->surface->resource, surface_x, surface_y);
		if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(resource);
	}
}

static void
leave(struct input_focus_handler *handler, struct wl_list *resources,
      struct compositor_view *view)
{
	struct wl_resource *resource;
	uint32_t serial;

	serial = wl_display_next_serial(swc.display);
	wl_resource_for_each(resource, resources) {
		wl_pointer_send_leave(resource, serial, view->surface->resource);
		if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(resource);
	}
}

static void
handle_cursor_surface_destroy(struct wl_listener *listener, void *data)
{
	struct pointer *pointer =
	    wl_container_of(listener, pointer, cursor.destroy_listener);

	/* The surface is still alive during this signal. Unhook its view handler
	 * before attaching a replacement, so it cannot see our desktop pixels. */
	surface_set_view(pointer->cursor.surface, NULL);
	pointer->cursor.surface = NULL;
	wl_list_remove(&pointer->cursor.destroy_listener.link);
	wl_list_init(&pointer->cursor.destroy_listener.link);
	pointer_set_cursor(pointer, cursor_left_ptr);
}

static int
cursor_frame_ready(void *data)
{
	struct pointer *pointer = data;
	pointer->cursor.frame_pending = false;
	if (swc.active && pointer->cursor.surface &&
	    pointer->cursor.view.buffer && pointer->cursor.view.screens)
		view_frame(&pointer->cursor.view, get_time());
	return 0;
}

static bool
update(struct view *view)
{
	struct pointer *pointer = wl_container_of(view, pointer, cursor.view);
	struct surface *surface = pointer->cursor.surface;

	/* Cursor pixels may change in a damage-only commit, without attach. */
	if (surface && pixman_region32_not_empty(&surface->state.damage))
		view_attach(view, surface->state.buffer);
	/* Cursor-only animations don't cause primary-plane page flips. Pace their
	 * callbacks separately instead of replying inside commit(), which lets a
	 * client request its next frame in an unbounded round-trip loop. */
	if (swc.active && surface && view->buffer && view->screens &&
	    !wl_list_empty(&surface->state.frame_callbacks) &&
	    !pointer->cursor.frame_pending) {
		struct screen *screen;
		struct output *output;
		uint32_t refresh = 0;
		wl_list_for_each(screen, &swc.screens, link) {
			if (!(view->screens & screen_mask(screen))) continue;
			wl_list_for_each(output, &screen->outputs, link)
				if (output->preferred_mode)
					refresh = MAX(refresh, output->preferred_mode->refresh);
		}
		if (!refresh) refresh = 60000; /* millihertz */
		int delay = MAX(1, (int)((1000000ULL + refresh - 1) / refresh));
		if (wl_event_source_timer_update(pointer->cursor.frame_timer, delay) == 0)
			pointer->cursor.frame_pending = true;
	}
	return true;
}

static int
attach(struct view *view, struct wld_buffer *buffer)
{
	struct pointer *pointer = wl_container_of(view, pointer, cursor.view);
	uint32_t old_screens = view->screens;
	struct surface *surface = pointer->cursor.surface;
#ifdef ENABLE_DRM
	struct screen *screen;
#endif

	/* A frame-only commit or a repeated set_cursor needs no pixel upload or
	 * KMS image update. Damage on a reused buffer still refreshes the pixels. */
	if (buffer == view->buffer &&
	    (!surface || !pixman_region32_not_empty(&surface->state.damage)))
		return 0;
	uint32_t started = profile_input ? get_time() : 0;

	wld_set_target_buffer(swc.shm->renderer, pointer->cursor.buffer);
	wld_fill_rectangle(swc.shm->renderer, 0x00000000, 0, 0,
	                   pointer->cursor.buffer->width,
	                   pointer->cursor.buffer->height);

	if (buffer) {
		wld_copy_rectangle(swc.shm->renderer, buffer, 0, 0, 0, 0, buffer->width,
		                   buffer->height);
	}

	wld_flush(swc.shm->renderer);

	if (surface) {
		pixman_region32_clear(&surface->state.damage);
	}

	/* TODO: Send an early release to the buffer */

	if (view_set_size_from_buffer(view, buffer)) {
		view_update_screens(view);
	}

#ifdef ENABLE_DRM
	wl_list_for_each(screen, &swc.screens, link) {
		if (!screen->planes.cursor)
			continue;
		view_attach(&screen->planes.cursor->view,
		            buffer ? pointer->cursor.buffer : NULL);
		view_update(&screen->planes.cursor->view);
	}
#else
	compositor_damage_all();
#endif

	screencopy_cursor_changed(old_screens | view->screens);
	record_input_time(&cursor_profile, "cursor image upload", started);
	return 0;
}

static bool
move(struct view *view, int32_t x, int32_t y)
{
	uint32_t old_screens = view->screens;
#ifdef ENABLE_DRM
	struct screen *screen;
#endif

	if (!view_set_position(view, x, y))
		return true;
	view_update_screens(view);

#ifdef ENABLE_DRM
	wl_list_for_each(screen, &swc.screens, link) {
		if (!screen->planes.cursor)
			continue;
		if (!((old_screens | view->screens) & screen_mask(screen)))
			continue;
		view_move(&screen->planes.cursor->view, view->geometry.x,
		          view->geometry.y);
		view_update(&screen->planes.cursor->view);
	}
#else
	compositor_damage_all();
#endif

	screencopy_cursor_changed(old_screens | view->screens);
	return true;
}

static const struct view_impl view_impl = {
    .update = update,
    .attach = attach,
    .move = move,
};

static inline void
update_cursor(struct pointer *pointer)
{
	int32_t x = wl_fixed_to_int(pointer->x) - pointer->cursor.hotspot.x,
	        y = wl_fixed_to_int(pointer->y) - pointer->cursor.hotspot.y;

	view_move(&pointer->cursor.view, x, y);
}

static void
drop_client_cursor_surface(struct pointer *pointer)
{
	if (!pointer || !pointer->cursor.surface) {
		return;
	}
	surface_set_view(pointer->cursor.surface, NULL);
	wl_list_remove(&pointer->cursor.destroy_listener.link);
	pointer->cursor.surface = NULL;
}

static void
apply_cursor_override(struct pointer *pointer)
{
	if (!pointer || pointer->cursor.surface) {
		return;
	}

	pointer_set_cursor(pointer, cursor_left_ptr);
}

EXPORT void
swc_set_cursor(enum swc_cursor_kind kind)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;

	cursor_override = kind;

	drop_client_cursor_surface(pointer);

	apply_cursor_override(pointer);
}

EXPORT void
swc_set_cursor_mode(enum swc_cursor_mode mode)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;

	cursor_mode = mode;
	if (cursor_mode == SWC_CURSOR_MODE_COMPOSITOR) {
		drop_client_cursor_surface(pointer);
	}
	apply_cursor_override(pointer);
}

EXPORT bool
swc_set_cursor_image(enum swc_cursor_kind kind, const uint32_t *argb8888,
                     uint32_t width, uint32_t height, int32_t hotspot_x,
                     int32_t hotspot_y)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;

	if (kind < 0 || kind >= (int)ARRAY_LENGTH(cursor_images)) {
		return false;
	}
	if (!argb8888 || width == 0 || height == 0) {
		return false;
	}

	/* Own the pixels, including while a client cursor temporarily hides this
	 * image. Prepare the replacement before releasing the current one. */
	if (width > 4096 || height > 4096) return false;
	struct wld_buffer *next = wld_create_buffer(swc.shm->context, width, height,
	                                            WLD_FORMAT_ARGB8888, WLD_FLAG_MAP);
	if (!next) return false;
	if (!wld_map(next)) { wld_buffer_unreference(next); return false; }
	for (uint32_t y = 0; y < height; ++y)
		memcpy((uint8_t *)next->map + (size_t)y * next->pitch,
		       argb8888 + (size_t)y * width, (size_t)width * 4);
	wld_unmap(next);
	if (cursor_images[kind].buffer) wld_buffer_unreference(cursor_images[kind].buffer);
	cursor_images[kind].buffer = next;
	cursor_images[kind].width = width;
	cursor_images[kind].height = height;
	cursor_images[kind].hotspot_x = hotspot_x;
	cursor_images[kind].hotspot_y = hotspot_y;
	cursor_images[kind].active = true;

	if (cursor_mode == SWC_CURSOR_MODE_COMPOSITOR) {
		drop_client_cursor_surface(pointer);
	}
	apply_cursor_override(pointer);
	return true;
}

EXPORT void
swc_clear_cursor_image(enum swc_cursor_kind kind)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;

	if (kind < 0 || kind >= (int)ARRAY_LENGTH(cursor_images)) {
		return;
	}

	cursor_images[kind].active = false;
	if (cursor_images[kind].buffer) wld_buffer_unreference(cursor_images[kind].buffer);
	cursor_images[kind].buffer = NULL;

	apply_cursor_override(pointer);
}

void
pointer_set_shape(struct pointer *pointer, enum swc_cursor_kind kind)
{
	if (!pointer) {
		return;
	}
	if (kind < 0 || kind >= SWC_CURSOR_KIND_COUNT) {
		return;
	}
	/* Asking for a shape replaces any cursor surface the same client set,
	 * which is what wl_pointer.set_cursor would have done. */
	drop_client_cursor_surface(pointer);
	client_shape = kind;
	client_shape_set = true;
	pointer_set_cursor(pointer, cursor_left_ptr);
}

void
pointer_clear_shape(struct pointer *pointer)
{
	if (!client_shape_set) {
		return;
	}
	client_shape_set = false;
	client_shape = SWC_CURSOR_DEFAULT;
	if (pointer && !pointer->cursor.surface) {
		pointer_set_cursor(pointer, cursor_left_ptr);
	}
}

EXPORT bool
swc_pointer_has_buttons(void)
{
	return swc.seat && swc.seat->pointer && swc.seat->pointer->buttons.size != 0;
}

void
pointer_set_cursor(struct pointer *pointer, uint32_t id)
{
	struct cursor *cursor = &cursor_metadata[id];
	union wld_object object = {.ptr = &cursor_data[cursor->offset]};
	struct wld_buffer *buffer;

	if (id == cursor_left_ptr) {
		/* A window manager override is a mode cursor -- move, resize, select
		 * -- and outranks whatever the client underneath would prefer. */
		enum swc_cursor_kind kind =
		    (cursor_override == SWC_CURSOR_DEFAULT && client_shape_set)
		        ? client_shape
		        : cursor_override;
		if (kind < 0 || kind >= (int)ARRAY_LENGTH(cursor_images)) {
			kind = SWC_CURSOR_DEFAULT;
		}

		if (cursor_images[kind].active) {
			static struct cursor custom_cursor;
			custom_cursor.width = (int)cursor_images[kind].width;
			custom_cursor.height = (int)cursor_images[kind].height;
			custom_cursor.hotspot_x = (int)cursor_images[kind].hotspot_x;
			custom_cursor.hotspot_y = (int)cursor_images[kind].hotspot_y;
			custom_cursor.offset = 0;

			cursor = &custom_cursor;
			buffer = cursor_images[kind].buffer;
			wld_buffer_reference(buffer);
			goto attach_buffer;
		}
	}

	buffer = wld_import_buffer(swc.shm->context, WLD_OBJECT_DATA, object,
	                           cursor->width, cursor->height,
	                           WLD_FORMAT_ARGB8888, cursor->width * 4);
	if (!buffer) {
		WARNING("Failed to create cursor buffer\n");
		return;
	}
attach_buffer:
	if (pointer->cursor.internal_buffer) {
		wld_buffer_unreference(pointer->cursor.internal_buffer);
	}
	if (pointer->cursor.surface) {
		surface_set_view(pointer->cursor.surface, NULL);
		wl_list_remove(&pointer->cursor.destroy_listener.link);
		pointer->cursor.surface = NULL;
	}


	pointer->cursor.internal_buffer = buffer;
	pointer->cursor.hotspot.x = cursor->hotspot_x;
	pointer->cursor.hotspot.y = cursor->hotspot_y;
	update_cursor(pointer);
	view_attach(&pointer->cursor.view, buffer);
}

static bool
client_handle_motion(struct pointer_handler *handler, uint32_t time,
                     wl_fixed_t x, wl_fixed_t y)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	struct wl_resource *resource;
	wl_fixed_t sx, sy;
	int32_t origin_x, origin_y;

	if (wl_list_empty(&pointer->focus.active)) {
		return false;
	}
	if (pointer_constraints_pointer_locked())
		return true;

	origin_x = pointer->focus.view->base.geometry.x -
	           pointer->focus.view->buffer_offset_x;
	origin_y = pointer->focus.view->base.geometry.y -
	           pointer->focus.view->buffer_offset_y;
	sx = x - wl_fixed_from_int(origin_x);
	sy = y - wl_fixed_from_int(origin_y);
	wl_resource_for_each(resource, &pointer->focus.active)
	    wl_pointer_send_motion(resource, time, sx, sy);
	return true;
}

static bool
client_handle_button(struct pointer_handler *handler, uint32_t time,
                     struct button *button, uint32_t state)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	struct wl_resource *resource;

	if (wl_list_empty(&pointer->focus.active)) {
		return false;
	}

	wl_resource_for_each(resource, &pointer->focus.active)
	    wl_pointer_send_button(resource, button->press.serial, time,
	                           button->press.value, state);
	return true;
}

static bool
client_handle_axis(struct pointer_handler *handler, uint32_t time,
                   enum wl_pointer_axis axis,
                   enum wl_pointer_axis_source source, wl_fixed_t value,
                   int value120)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	struct wl_resource *resource;
	int ver;

	if (wl_list_empty(&pointer->focus.active)) {
		return false;
	}

	if (pointer->client_axis_source != -1) {
		assert(pointer->client_axis_source == source);
		source = -1;
	} else {
		pointer->client_axis_source = source;
	}

	wl_resource_for_each(resource, &pointer->focus.active)
	{
		ver = wl_resource_get_version(resource);
		if (source != -1 && ver >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
			wl_pointer_send_axis_source(resource, source);
		}
		if (value120) {
			if (ver >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
				wl_pointer_send_axis_value120(resource, axis, value120);
			} else if (ver >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
				wl_pointer_send_axis_discrete(resource, axis, value120 / 120);
			}
		}
		if (value) {
			wl_pointer_send_axis(resource, time, axis, value);
		} else if (ver >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
			wl_pointer_send_axis_stop(resource, time, axis);
		}
	}
	return true;
}

static void
client_handle_frame(struct pointer_handler *handler)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	struct wl_resource *resource;

	wl_resource_for_each(resource, &pointer->focus.active)
	{
		if (wl_resource_get_version(resource) >=
		    WL_POINTER_FRAME_SINCE_VERSION) {
			wl_pointer_send_frame(resource);
		}
	}
	pointer->client_axis_source = -1;
}

static void
handle_focus_changed(struct wl_listener *listener, void *data)
{
	struct pointer *pointer = wl_container_of(listener, pointer, focus_changed);
	pointer_constraints_update_focus(pointer);
	/* Destruction clears input focus without calling enter(NULL). Restore the
	 * desktop cursor here too, including a client that last hid its cursor. */
	if (!pointer->focus.view) {
		pointer_clear_shape(pointer);
		if (pointer->cursor.surface || !pointer->cursor.view.buffer)
			pointer_set_cursor(pointer, cursor_left_ptr);
	}
}

static void
handle_activity_changed(struct wl_listener *listener, void *data)
{
	struct pointer *pointer = wl_container_of(listener, pointer, activity_changed);
	pointer_constraints_update_focus(pointer);
	if (swc.active)
		update(&pointer->cursor.view);
}

bool
pointer_initialize(struct pointer *pointer)
{
	struct screen *screen = wl_container_of(swc.screens.next, screen, link);
	struct swc_rectangle *geom = &screen->base.geometry;

	profile_input = getenv("SWC_INPUT_PROFILE") &&
	                strcmp(getenv("SWC_INPUT_PROFILE"), "1") == 0;
	memset(&motion_profile, 0, sizeof(motion_profile));
	memset(&cursor_profile, 0, sizeof(cursor_profile));
	wl_signal_init(&pointer->destroy_signal);
	/* Center cursor in the geometry of the first screen. */
	pointer->x = wl_fixed_from_int(geom->x + geom->width / 2);
	pointer->y = wl_fixed_from_int(geom->y + geom->height / 2);
	pointer->focus_handler.enter = enter;
	pointer->focus_handler.leave = leave;
	pointer->client_handler.motion = client_handle_motion;
	pointer->client_handler.button = client_handle_button;
	pointer->client_handler.axis = client_handle_axis;
	pointer->client_handler.frame = client_handle_frame;
	pointer->client_handler.pending = false;
	pointer->client_axis_source = -1;
	wl_list_init(&pointer->handlers);
	wl_list_insert(&pointer->handlers, &pointer->client_handler.link);
	wl_array_init(&pointer->buttons);

	view_initialize(&pointer->cursor.view, &view_impl);
	pointer->cursor.surface = NULL;
	pointer->cursor.destroy_listener.notify = &handle_cursor_surface_destroy;
	pointer->cursor.buffer = wld_create_buffer(
	    swc.backend->context, swc.backend->cursor_width,
	    swc.backend->cursor_height,
	    WLD_FORMAT_ARGB8888, WLD_FLAG_MAP | WLD_FLAG_CURSOR);
	pointer->cursor.internal_buffer = NULL;
	pointer->cursor.frame_pending = false;
	pointer->cursor.frame_timer = NULL;

	if (!pointer->cursor.buffer) {
		return false;
	}
	pointer->cursor.frame_timer = wl_event_loop_add_timer(
	    swc.event_loop, cursor_frame_ready, pointer);
	if (!pointer->cursor.frame_timer) {
		wld_buffer_unreference(pointer->cursor.buffer);
		return false;
	}

	pointer_set_cursor(pointer, cursor_left_ptr);

#ifdef ENABLE_DRM
	wl_list_for_each(screen, &swc.screens, link)
		if (screen->planes.cursor)
			view_attach(&screen->planes.cursor->view, pointer->cursor.buffer);
#endif

	input_focus_initialize(&pointer->focus, &pointer->focus_handler);
	pointer->focus_changed.notify = handle_focus_changed;
	wl_signal_add(&pointer->focus.event_signal, &pointer->focus_changed);
	pointer->activity_changed.notify = handle_activity_changed;
	wl_signal_add(&swc.event_signal, &pointer->activity_changed);
	pixman_region32_init(&pointer->region);

	return true;
}

void
pointer_finalize(struct pointer *pointer)
{
	wl_event_source_remove(pointer->cursor.frame_timer);
	for (size_t i = 0; i < ARRAY_LENGTH(cursor_images); ++i) {
		if (cursor_images[i].buffer) wld_buffer_unreference(cursor_images[i].buffer);
	}
	memset(cursor_images, 0, sizeof(cursor_images));
	struct wl_resource *resource, *tmp;

	wl_signal_emit(&pointer->destroy_signal, pointer);
	wl_list_remove(&pointer->focus_changed.link);
	wl_list_remove(&pointer->activity_changed.link);
	if (pointer->focus.view)
		wl_list_remove(&pointer->focus.view_destroy_listener.link);
	/* Make surviving wl_pointer resources inert before the seat is freed. */
	wl_resource_for_each_safe(resource, tmp, &pointer->focus.active) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
		wl_resource_set_user_data(resource, NULL);
	}
	wl_resource_for_each_safe(resource, tmp, &pointer->focus.inactive) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
		wl_resource_set_user_data(resource, NULL);
	}
	drop_client_cursor_surface(pointer);
	view_attach(&pointer->cursor.view, NULL);
	view_finalize(&pointer->cursor.view);
	if (pointer->cursor.internal_buffer)
		wld_buffer_unreference(pointer->cursor.internal_buffer);
	wld_buffer_unreference(pointer->cursor.buffer);
	wl_array_release(&pointer->buttons);
	pixman_region32_fini(&pointer->region);
}

void
pointer_set_focus(struct pointer *pointer, struct compositor_view *view)
{
	input_focus_set(&pointer->focus, view);
	pointer_constraints_update_focus(pointer);
}

static void
clip_position(struct pointer *pointer, wl_fixed_t fx, wl_fixed_t fy)
{
	int32_t x, y, last_x, last_y;
	pixman_box32_t box;

	x = wl_fixed_to_int(fx);
	y = wl_fixed_to_int(fy);
	last_x = wl_fixed_to_int(pointer->x);
	last_y = wl_fixed_to_int(pointer->y);

	if (!pixman_region32_contains_point(&pointer->region, x, y, NULL)) {
		if (!pixman_region32_contains_point(&pointer->region, last_x, last_y,
		                                    &box)) {
			WARNING("cursor is not in the visible screen area\n");
			pointer->x = 0;
			pointer->y = 0;
			return;
		}

		/* Do some clipping. */
		fx = wl_fixed_from_int(MAX(MIN(x, box.x2 - 1), box.x1));
		fy = wl_fixed_from_int(MAX(MIN(y, box.y2 - 1), box.y1));
	}

	pointer->x = fx;
	pointer->y = fy;
}

/* A lock-release hint is a warp, not physical motion. In particular it must
 * not enter focus/constraint selection again or emit relative motion. */
void
pointer_warp(struct pointer *pointer, wl_fixed_t x, wl_fixed_t y)
{
	clip_position(pointer, x, y);
	update_cursor(pointer);
}

void
pointer_set_region(struct pointer *pointer, pixman_region32_t *region)
{
	pixman_region32_copy(&pointer->region, region);
	clip_position(pointer, pointer->x, pointer->y);
}

static void
set_cursor(struct wl_client *client, struct wl_resource *resource,
           uint32_t serial, struct wl_resource *surface_resource,
           int32_t hotspot_x, int32_t hotspot_y)
{
	struct pointer *pointer = wl_resource_get_user_data(resource);
	struct surface *surface;

	(void)serial;

	if (!pointer || client != pointer->focus.client) {
		return;
	}

	/* If forcing compositor cursor, ignore client cursor surfaces. */
	if (cursor_mode == SWC_CURSOR_MODE_COMPOSITOR ||
	    cursor_override != SWC_CURSOR_DEFAULT) {
		return;
	}

	surface = surface_resource ? wl_resource_get_user_data(surface_resource) : NULL;
	if (surface && surface == pointer->cursor.surface) {
		/* A client may select the same cursor on every motion event. Its
		 * pixels change on surface commit, not on set_cursor. */
		pointer->cursor.hotspot.x = hotspot_x;
		pointer->cursor.hotspot.y = hotspot_y;
		update_cursor(pointer);
		return;
	}
	if (!surface && !pointer->cursor.surface && !pointer->cursor.view.buffer)
		return;

	if (pointer->cursor.surface) {
		surface_set_view(pointer->cursor.surface, NULL);
		wl_list_remove(&pointer->cursor.destroy_listener.link);
	}

	surface =
	    surface_resource ? wl_resource_get_user_data(surface_resource) : NULL;
	pointer->cursor.surface = surface;
	pointer->cursor.hotspot.x = hotspot_x;
	pointer->cursor.hotspot.y = hotspot_y;

	if (surface) {
		/* Restore position/hotspot before attaching a visible cursor image. */
		update_cursor(pointer);
		surface_set_view(surface, &pointer->cursor.view);
		wl_signal_add(&surface->signal.destroy, &pointer->cursor.destroy_listener);
	} else {
		/*
		 * A null surface means hide the pointer. Dropping the surface alone
		 * leaves the compositor's own cursor attached to the view, so the
		 * client ends up drawing its cursor underneath ours.
		 */
		view_attach(&pointer->cursor.view, NULL);
	}
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = set_cursor,
    .release = destroy_resource,
};

static void
unbind(struct wl_resource *resource)
{
	struct pointer *pointer = wl_resource_get_user_data(resource);
	if (pointer)
		input_focus_remove_resource(&pointer->focus, resource);
}

struct wl_resource *
pointer_bind(struct pointer *pointer, struct wl_client *client,
             uint32_t version, uint32_t id)
{
	struct wl_resource *client_resource;

	client_resource =
	    wl_resource_create(client, &wl_pointer_interface, version, id);
	if (!client_resource) {
		return NULL;
	}
	wl_resource_set_implementation(client_resource, &pointer_impl, pointer,
	                               &unbind);
	input_focus_add_resource(&pointer->focus, client_resource);

	return client_resource;
}

struct button *
pointer_get_button(struct pointer *pointer, uint32_t serial)
{
	struct button *button;

	wl_array_for_each(button, &pointer->buttons)
	{
		if (button->press.serial == serial) {
			return button;
		}
	}

	return NULL;
}

void
pointer_handle_button(struct pointer *pointer, uint32_t time, uint32_t value,
                      uint32_t state)
{
	struct pointer_handler *handler;
	struct button *button;
	uint32_t serial;

	serial = wl_display_next_serial(swc.display);

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		wl_array_for_each(button, &pointer->buttons)
		{
			if (button->press.value == value) {
				if (button->handler) {
					button->press.serial = serial;
					button->handler->button(button->handler, time, button,
					                        state);
					button->handler->pending = true;
				}

				array_remove(&pointer->buttons, button, sizeof(*button));
				break;
			}
		}
	} else {
		button = wl_array_add(&pointer->buttons, sizeof(*button));

		if (!button) {
			return;
		}

		button->press.value = value;
		button->press.serial = serial;
		button->handler = NULL;

		wl_list_for_each(handler, &pointer->handlers, link)
		{
			if (handler->button &&
			    handler->button(handler, time, button, state)) {
				button->handler = handler;
				handler->pending = true;
				break;
			}
		}
	}
}

void
pointer_handle_axis(struct pointer *pointer, uint32_t time,
                    enum wl_pointer_axis axis,
                    enum wl_pointer_axis_source source, wl_fixed_t value,
                    int value120)
{
	struct pointer_handler *handler;

	wl_list_for_each(handler, &pointer->handlers, link)
	{
		if (handler->axis &&
		    handler->axis(handler, time, axis, source, value, value120)) {
			handler->pending = true;
			break;
		}
	}
}

void
pointer_handle_relative_motion(struct pointer *pointer, uint32_t time,
                               wl_fixed_t dx, wl_fixed_t dy)
{
	pointer_handle_absolute_motion(pointer, time, pointer->x + dx,
	                               pointer->y + dy);
}

void
pointer_handle_absolute_motion(struct pointer *pointer, uint32_t time,
                               wl_fixed_t x, wl_fixed_t y)
{
	struct pointer_handler *handler;
	uint32_t started = profile_input ? get_time() : 0;

	/*
	 * A locked pointer must not move. Relative motion is still reported, so
	 * the client can keep driving whatever the pointer was locked for.
	 */
	pointer_constraints_update_focus(pointer);
	if (pointer_constraints_pointer_locked())
		return;

	pointer_constraints_confine(pointer, &x, &y);
	clip_position(pointer, x, y);

	wl_list_for_each(handler, &pointer->handlers, link)
	{
		if (handler->motion &&
		    handler->motion(handler, time, pointer->x, pointer->y)) {
			handler->pending = true;
			break;
		}
	}

	update_cursor(pointer);
	record_input_time(&motion_profile, "pointer motion including KMS move", started);
}

void
pointer_handle_frame(struct pointer *pointer)
{
	struct pointer_handler *handler;

	wl_list_for_each(handler, &pointer->handlers, link)
	{
		if (handler->pending) {
			if (handler->frame)
				handler->frame(handler);
			handler->pending = false;
		}
	}

	update_cursor(pointer);
}
