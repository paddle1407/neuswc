/* swc: surface.c
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

#include "surface.h"
#include "backend.h"
#include "compositor.h"
#include "drm_syncobj.h"
#include "pointer.h"
#include "seat.h"
#include "event.h"
#include "internal.h"
#include "output.h"
#include "region.h"
#include "screen.h"
#include "subsurface.h"
#include "util.h"
#include "view.h"
#include "wayland_buffer.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wld/wld.h>

/* A wl_buffer.release waiting for the GPU to finish reading the buffer. */
struct pending_release {
	struct wl_resource *resource;
	struct wl_listener destroy_listener;
	struct wl_event_source *source;
	int fd;
};

static void
pending_release_free(struct pending_release *release)
{
	wl_list_remove(&release->destroy_listener.link);
	wl_event_source_remove(release->source);
	close(release->fd);
	free(release);
}

static int
handle_release_fence(int fd, uint32_t mask, void *data)
{
	struct pending_release *release = data;

	(void)fd;
	(void)mask;
	wl_buffer_send_release(release->resource);
	pending_release_free(release);
	return 0;
}

static void
handle_release_destroy(struct wl_listener *listener, void *data)
{
	struct pending_release *release =
	    wl_container_of(listener, release, destroy_listener);

	(void)data;
	pending_release_free(release);
}

/*
 * wl_buffer.release tells the client the compositor is done reading the
 * buffer. Frames are only flushed to the GPU before they are presented, not
 * finished, so one that sampled this buffer can still be running; on a driver
 * without implicit synchronization the client could then draw into it under
 * that read. Send the release once the renderer's fence says the GPU is done,
 * which is nearly always already, from the event loop rather than blocking.
 */
static void
release_buffer(struct wl_resource *resource)
{
	struct wld_renderer *renderer = swc.backend ? swc.backend->renderer : NULL;
	struct wld_buffer *buffer = wayland_buffer_get(resource);
	struct pending_release *release;
	struct pollfd pollfd;
	int fd;

	/* Only buffers the renderer samples directly are read on the GPU. */
	if (!renderer || !buffer ||
	    !(wld_capabilities(renderer, buffer) & WLD_CAPABILITY_READ) ||
	    (fd = wld_export_fence(renderer)) < 0) {
		wl_buffer_send_release(resource);
		return;
	}

	pollfd.fd = fd;
	pollfd.events = POLLIN;
	if (poll(&pollfd, 1, 0) == 0 && (release = malloc(sizeof(*release)))) {
		release->source = wl_event_loop_add_fd(
		    swc.event_loop, fd, WL_EVENT_READABLE, &handle_release_fence, release);
		if (release->source) {
			release->resource = resource;
			release->fd = fd;
			release->destroy_listener.notify = &handle_release_destroy;
			wl_resource_add_destroy_listener(resource,
			                                 &release->destroy_listener);
			return;
		}
		free(release);
	}

	close(fd);
	wl_buffer_send_release(resource);
}

/**
 * Removes a buffer from a surface state.
 */
static void
handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct surface_state *state;

	state = wl_container_of(listener, state, buffer_destroy_listener);
	state->buffer = NULL;
	state->buffer_resource = NULL;
}

static void
handle_role_destroy(struct wl_listener *listener, void *data)
{
	struct surface *surface = wl_container_of(listener, surface, role_destroy_listener);

	(void)data;

	surface->role = NULL;
}

static void
state_initialize(struct surface_state *state)
{
	state->buffer = NULL;
	/*layer-shell rejects surfaces with a pre-existing buffer */
	state->buffer_resource = NULL;
	state->buffer_destroy_listener.notify = &handle_buffer_destroy;

	pixman_region32_init(&state->damage);
	pixman_region32_init(&state->opaque);
	pixman_region32_init_with_extents(&state->input, &infinite_extents);

	wl_list_init(&state->frame_callbacks);
	wl_list_init(&state->subsurfaces_below);
	wl_list_init(&state->subsurfaces_above);
}

static void
state_finalize(struct surface_state *state)
{
	struct wl_resource *resource, *tmp;

	if (state->buffer) {
		wl_list_remove(&state->buffer_destroy_listener.link);
	}

	pixman_region32_fini(&state->damage);
	pixman_region32_fini(&state->opaque);
	pixman_region32_fini(&state->input);

	/* Remove all leftover callbacks. */
	wl_list_for_each_safe(resource, tmp, &state->frame_callbacks, link)
	    wl_resource_destroy(resource);
}

/**
 * In order to set the buffer of a surface state (current or pending), we need
 * to manage the destroy listeners we have for the new and old buffer.
 */
static void
state_set_buffer(struct surface_state *state, struct wl_resource *resource)
{
	struct wld_buffer *buffer = resource ? wayland_buffer_get(resource) : NULL;

	if (state->buffer) {
		wl_list_remove(&state->buffer_destroy_listener.link);
	}

	if (buffer) {
		wl_resource_add_destroy_listener(resource,
		                                 &state->buffer_destroy_listener);
	}

	state->buffer = buffer;
	state->buffer_resource = resource;
}

static void
handle_frame(struct view_handler *handler, uint32_t time)
{
	struct surface *surface = wl_container_of(handler, surface, view_handler);
	struct wl_resource *resource, *tmp;

	wl_list_for_each_safe(resource, tmp, &surface->state.frame_callbacks, link)
	{
		wl_callback_send_done(resource, time);
		wl_resource_destroy(resource);
	}

	wl_list_init(&surface->state.frame_callbacks);
}

static void
handle_screens(struct view_handler *handler, uint32_t entered, uint32_t left)
{
	struct surface *surface = wl_container_of(handler, surface, view_handler);
	struct screen *screen;
	struct output *output;
	struct wl_client *client;
	struct wl_resource *resource;

	client = wl_resource_get_client(surface->resource);

	wl_list_for_each(screen, &swc.screens, link)
	{
		if (!((entered | left) & screen_mask(screen))) {
			continue;
		}

		wl_list_for_each(output, &screen->outputs, link)
		{
			resource = wl_resource_find_for_client(&output->resources, client);

			if (resource) {
				if (entered & screen_mask(screen)) {
					wl_surface_send_enter(surface->resource, resource);
				} else if (left & screen_mask(screen)) {
					wl_surface_send_leave(surface->resource, resource);
				}
			}
		}
	}
}

static const struct view_handler_impl view_handler_impl = {
    .frame = handle_frame,
    .screens = handle_screens,
};

static void
attach(struct wl_client *client, struct wl_resource *resource,
       struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
	struct surface *surface = wl_resource_get_user_data(resource);

	surface->pending.commit |= SURFACE_COMMIT_ATTACH;

	state_set_buffer(&surface->pending.state, buffer_resource);
	surface->pending.x = x;
	surface->pending.y = y;
}

static void
damage(struct wl_client *client, struct wl_resource *resource, int32_t x,
       int32_t y, int32_t width, int32_t height)
{
	struct surface *surface = wl_resource_get_user_data(resource);

	surface->pending.commit |= SURFACE_COMMIT_DAMAGE;
	pixman_region32_union_rect(&surface->pending.state.damage,
	                           &surface->pending.state.damage, x, y, width,
	                           height);
}

static void
frame(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	struct wl_resource *callback_resource;

	callback_resource =
	    wl_resource_create(client, &wl_callback_interface, 1, id);
	if (!callback_resource) {
		wl_resource_post_no_memory(resource);
		return;
	}
	surface->pending.commit |= SURFACE_COMMIT_FRAME;
	wl_resource_set_implementation(callback_resource, NULL, NULL,
	                               &remove_resource);
	wl_list_insert(surface->pending.state.frame_callbacks.prev,
	               wl_resource_get_link(callback_resource));
}

static void
set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *region_resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);

	surface->pending.commit |= SURFACE_COMMIT_OPAQUE;

	if (region_resource) {
		pixman_region32_t *region = wl_resource_get_user_data(region_resource);
		pixman_region32_copy(&surface->pending.state.opaque, region);
	} else {
		pixman_region32_clear(&surface->pending.state.opaque);
	}
}

static void
set_input_region(struct wl_client *client, struct wl_resource *resource,
                 struct wl_resource *region_resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);

	surface->pending.commit |= SURFACE_COMMIT_INPUT;

	if (region_resource) {
		pixman_region32_t *region = wl_resource_get_user_data(region_resource);
		pixman_region32_copy(&surface->pending.state.input, region);
	} else {
		pixman_region32_reset(&surface->pending.state.input, &infinite_extents);
	}
}

static inline void
trim_region(pixman_region32_t *region, struct wld_buffer *buffer)
{
	pixman_region32_intersect_rect(region, region, 0, 0,
	                               buffer ? buffer->width : 0,
	                               buffer ? buffer->height : 0);
}

static void
surface_apply_pending(struct surface *surface, bool flush_children)
{
	struct wld_buffer *buffer;
	struct wl_resource *replaced = NULL;
	bool attached, ready;

	/*
	 * An explicit-synchronization protocol error leaves the client dead, so
	 * nothing of this commit may be applied.
	 */
	if (!drm_syncobj_surface_check_commit(surface)) {
		return;
	}

	/* Attach */
	attached = surface->pending.commit & SURFACE_COMMIT_ATTACH;
	if (attached) {
		if (surface->state.buffer &&
		    surface->state.buffer != surface->pending.state.buffer) {
			replaced = surface->state.buffer_resource;
		}

		state_set_buffer(&surface->state,
		                 surface->pending.state.buffer_resource);
	}

	/*
	 * Signal the release point of the buffer just replaced and order the
	 * renderer after the new buffer's acquire point, before anything can
	 * schedule a repaint that would sample it. A buffer that is not ready
	 * yet leaves the old one on screen until it is.
	 */
	ready = drm_syncobj_surface_apply_commit(surface, attached, &replaced);
	if (replaced) {
		release_buffer(replaced);
	}

	buffer = surface->state.buffer;
	/* Destroying the wl_buffer object does not detach its committed contents.
	 * The view still owns those pixels until an explicit replacement attach. */
	if ((!attached || !ready) && surface->view)
		buffer = surface->view->buffer;
	if (surface->pending.commit & SURFACE_COMMIT_GEOMETRY) {
		const struct swc_rectangle *g = &surface->pending.window_geometry;
		surface->has_window_geometry = true;
		surface->window_x = g->x;
		surface->window_y = g->y;
		surface->window_width = g->width;
		surface->window_height = g->height;
	}

	/* Damage */
	if (surface->pending.commit & SURFACE_COMMIT_DAMAGE) {
		pixman_region32_union(&surface->state.damage, &surface->state.damage,
		                      &surface->pending.state.damage);
		pixman_region32_clear(&surface->pending.state.damage);
	}

	/* Opaque */
	if (surface->pending.commit & SURFACE_COMMIT_OPAQUE) {
		pixman_region32_copy(&surface->state.opaque,
		                     &surface->pending.state.opaque);
	}

	/* Input */
	if (surface->pending.commit & SURFACE_COMMIT_INPUT) {
		pixman_region32_copy(&surface->state.input,
		                     &surface->pending.state.input);
	}

	/* Frame */
	if (surface->pending.commit & SURFACE_COMMIT_FRAME) {
		wl_list_insert_list(&surface->state.frame_callbacks,
		                    &surface->pending.state.frame_callbacks);
		wl_list_init(&surface->pending.state.frame_callbacks);
	}

	trim_region(&surface->state.damage, buffer);
	trim_region(&surface->state.opaque, buffer);

	if (surface->view) {
		if (surface->pending.commit & (SURFACE_COMMIT_ATTACH | SURFACE_COMMIT_GEOMETRY)) {
			view_attach(surface->view, buffer);
		}
		view_update(surface->view);
	}

	surface->pending.commit = 0;

	if (surface->subsurface) {
		surface->subsurface->pending = false;
	}

	if (surface->subsurface) {
		subsurface_update_visibility(surface->subsurface);
	}

	subsurface_parent_commit(surface);
	wl_signal_emit(&surface->signal.commit, surface);

	if (flush_children) {
		struct subsurface *child;
		wl_list_for_each(child, &surface->subsurfaces, link)
		{
			if (!child->pending || !subsurface_is_synchronized(child)) {
				continue;
			}
			if (child->surface) {
				surface_apply_pending(child->surface, true);
			}
		}
	}
}

static void
commit(struct wl_client *client, struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);

	if (surface->subsurface &&
	    subsurface_is_synchronized(surface->subsurface)) {
		surface->subsurface->pending = true;
		return;
	}

	surface_apply_pending(surface, true);
}

/*
 * Buffer transforms and scales other than the defaults are not implemented:
 * the renderer only blits buffers one to one.
 *
 * wl_surface only defines invalid_transform for a value outside the
 * wl_output.transform enumeration and invalid_scale for a non-positive scale,
 * so posting a protocol error for a legal-but-unsupported value would kill a
 * conforming client. Chromium/Electron sends set_buffer_scale for a forced
 * device scale factor and set_buffer_transform for a rotated output, so that
 * error was fatal to those clients. Accept the request and ignore it instead;
 * the surface is composited unscaled and unrotated.
 */
static void
set_buffer_transform(struct wl_client *client, struct wl_resource *surface,
                     int32_t transform)
{
	static bool warned;

	if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
	    transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(surface, WL_SURFACE_ERROR_INVALID_TRANSFORM,
		                       "buffer transform %" PRId32 " is not a valid "
		                       "wl_output.transform value",
		                       transform);
		return;
	}

	if (transform != WL_OUTPUT_TRANSFORM_NORMAL && !warned) {
		warned = true;
		WARNING("Ignoring unsupported buffer transform %" PRId32 "\n",
		        transform);
	}
}

static void
set_buffer_scale(struct wl_client *client, struct wl_resource *surface,
                 int32_t scale)
{
	static bool warned;

	if (scale <= 0) {
		wl_resource_post_error(surface, WL_SURFACE_ERROR_INVALID_SCALE,
		                       "buffer scale %" PRId32 " is not positive",
		                       scale);
		return;
	}

	if (scale != 1 && !warned) {
		warned = true;
		WARNING("Ignoring unsupported buffer scale %" PRId32 "\n", scale);
	}
}

static void
damage_buffer(struct wl_client *client, struct wl_resource *surface, int32_t x,
              int32_t y, int32_t w, int32_t h)
{
	damage(client, surface, x, y, w, h);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = destroy_resource,
    .attach = attach,
    .damage = damage,
    .frame = frame,
    .set_opaque_region = set_opaque_region,
    .set_input_region = set_input_region,
    .commit = commit,
    .set_buffer_transform = set_buffer_transform,
    .set_buffer_scale = set_buffer_scale,
    .damage_buffer = damage_buffer,
};

static void
surface_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);

	drm_syncobj_surface_finish(surface);
	wl_signal_emit(&surface->signal.destroy, surface);
	/* Clear pointer focus while the surface used by wl_pointer.leave exists. */
	if (swc.seat && swc.seat->pointer && swc.seat->pointer->focus.view &&
	    swc.seat->pointer->focus.view->surface == surface)
		pointer_set_focus(swc.seat->pointer, NULL);
	state_finalize(&surface->state);
	state_finalize(&surface->pending.state);

	if (surface->view) {
		wl_list_remove(&surface->view_handler.link);
	}
	if (surface->role) {
		wl_list_remove(&surface->role_destroy_listener.link);
	}

	free(surface);
}

struct surface *
surface_from_resource(struct wl_resource *resource)
{
	if (!resource ||
	    !wl_resource_instance_of(resource, &wl_surface_interface,
	                             &surface_impl)) {
		return NULL;
	}

	return wl_resource_get_user_data(resource);
}

/**
 * Construct a new surface, adding it to the given client as id.
 *
 * The surface will be free'd automatically when its resource is destroyed.
 *
 * @return The newly allocated surface.
 */
struct surface *
surface_new(struct wl_client *client, uint32_t version, uint32_t id)
{
	struct surface *surface;

	surface = malloc(sizeof(*surface));
	if (!surface) {
		goto error0;
	}

	surface->resource =
	    wl_resource_create(client, &wl_surface_interface, version, id);
	if (!surface->resource) {
		goto error1;
	}
	wl_resource_set_implementation(surface->resource, &surface_impl, surface,
	                               &surface_destroy);

	/* Initialize the surface. */
	surface->pending.commit = 0;
	wl_signal_init(&surface->signal.commit);
	wl_signal_init(&surface->signal.destroy);
	surface->view = NULL;
	surface->view_handler.impl = &view_handler_impl;
	surface->role = NULL;
	surface->role_destroy_listener.notify = handle_role_destroy;
	surface->subsurface = NULL;
	wl_list_init(&surface->subsurfaces);
	surface->synced = NULL;
	surface->has_window_geometry = false;
	surface->window_x = 0;
	surface->window_y = 0;
	surface->window_width = 0;
	surface->window_height = 0;

	state_initialize(&surface->state);
	state_initialize(&surface->pending.state);

	return surface;

error1:
	free(surface);
error0:
	return NULL;
}

void
surface_set_view(struct surface *surface, struct view *view)
{
	if (surface->view == view) {
		return;
	}

	/* The new view is given the committed buffer, so it has to be ready. */
	if (view) {
		drm_syncobj_surface_settle(surface);
	}

	if (surface->view) {
		wl_list_remove(&surface->view_handler.link);
	}

	surface->view = view;
	struct subsurface *child;
	wl_list_for_each(child, &surface->subsurfaces, link)
		subsurface_set_parent_view(child, view);

	if (view) {
		wl_list_insert(&view->handlers, &surface->view_handler.link);
		view_attach(view, surface->state.buffer);
		view_update(view);
	}
}

bool
surface_set_role(struct surface *surface, struct wl_resource *role)
{
	if (surface->role) {
		return false;
	}

	surface->role = role;
	wl_resource_add_destroy_listener(role, &surface->role_destroy_listener);
	return true;
}

bool
surface_has_buffer(struct surface *surface)
{
	return surface->state.buffer_resource ||
	       ((surface->pending.commit & SURFACE_COMMIT_ATTACH) &&
	        surface->pending.state.buffer_resource);
}

void
surface_commit_pending(struct surface *surface)
{
	surface_apply_pending(surface, true);
}

void
surface_show_buffer(struct surface *surface, struct wld_buffer *buffer)
{
	if (!surface->view) {
		return;
	}

	/* Whatever the commit damaged was drawn from the old buffer. */
	if (buffer) {
		pixman_region32_union_rect(&surface->state.damage,
		                           &surface->state.damage, 0, 0,
		                           buffer->width, buffer->height);
	}
	view_attach(surface->view, buffer);
	view_update(surface->view);
}
