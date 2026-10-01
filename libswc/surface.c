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
#include "shm.h"
#include "screen.h"
#include "subsurface.h"
#include "util.h"
#include "view.h"
#include "wayland_buffer.h"

#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
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
	if (!(mask & WL_EVENT_READABLE)) {
		if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) pending_release_free(release);
		return 0;
	}
	struct pollfd pollfd = { .fd = fd, .events = POLLIN };
	if (poll(&pollfd, 1, 0) <= 0 || !(pollfd.revents & POLLIN) || (pollfd.revents & POLLERR)) return 0;
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
	int ready;
	do { ready = poll(&pollfd, 1, 0); } while (ready < 0 && errno == EINTR);
	if (ready > 0 && (pollfd.revents & POLLIN) && !(pollfd.revents & POLLERR)) {
		close(fd); wl_buffer_send_release(resource); return;
	}
	release = malloc(sizeof(*release));
	if (release) {
		release->source = wl_event_loop_add_fd(swc.event_loop, fd, WL_EVENT_READABLE,
		    handle_release_fence, release);
		if (release->source) {
			release->resource = resource; release->fd = fd;
			release->destroy_listener.notify = handle_release_destroy;
			wl_resource_add_destroy_listener(resource, &release->destroy_listener);
			return;
		}
		free(release);
	}
	close(fd);
	wl_resource_post_no_memory(resource);
}

/**
 * Removes a buffer from a surface state.
 */
static void
handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct surface_state *state;

	state = wl_container_of(listener, state, buffer_destroy_listener);
	(void)data;
	wl_list_remove(&state->buffer_destroy_listener.link);
	wl_list_init(&state->buffer_destroy_listener.link);
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
	wl_list_init(&state->buffer_destroy_listener.link);
	state->buffer_scale = 1;
	state->buffer_transform = WL_OUTPUT_TRANSFORM_NORMAL;

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

	if (state->buffer_resource) wl_list_remove(&state->buffer_destroy_listener.link);
	if (state->buffer) wld_buffer_unreference(state->buffer);

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
state_assign_buffer(struct surface_state *state, struct wld_buffer *buffer,
                    struct wl_resource *resource)
{
	if (buffer) wld_buffer_reference(buffer);
	if (state->buffer_resource) wl_list_remove(&state->buffer_destroy_listener.link);
	if (state->buffer) wld_buffer_unreference(state->buffer);
	wl_list_init(&state->buffer_destroy_listener.link);
	state->buffer = buffer;
	state->buffer_resource = resource;
	if (resource) wl_resource_add_destroy_listener(resource, &state->buffer_destroy_listener);
}

static void
state_set_buffer(struct surface_state *state, struct wl_resource *resource)
{
	state_assign_buffer(state, resource ? wayland_buffer_get(resource) : NULL, resource);
	}

/* A commit, rather than a subsequent request, takes ownership of cached state. */
static void
cache_pending(struct surface *surface)
{
	struct surface_pending *p = &surface->pending, *c = &surface->cached;
	if (p->commit & SURFACE_COMMIT_ATTACH) {
		if (c->state.buffer_resource && c->state.buffer != p->state.buffer)
			release_buffer(c->state.buffer_resource);
		state_assign_buffer(&c->state, p->state.buffer, p->state.buffer_resource);
		state_set_buffer(&p->state, NULL);
		c->x = p->x; c->y = p->y;
	}
	if (p->commit & SURFACE_COMMIT_DAMAGE)
		pixman_region32_union(&c->state.damage, &c->state.damage, &p->state.damage);
	if (p->commit & SURFACE_COMMIT_OPAQUE) pixman_region32_copy(&c->state.opaque, &p->state.opaque);
	if (p->commit & SURFACE_COMMIT_INPUT) pixman_region32_copy(&c->state.input, &p->state.input);
	if (p->commit & SURFACE_COMMIT_GEOMETRY) c->window_geometry = p->window_geometry;
	if (p->commit & SURFACE_COMMIT_SCALE) c->state.buffer_scale = p->state.buffer_scale;
	if (p->commit & SURFACE_COMMIT_TRANSFORM) c->state.buffer_transform = p->state.buffer_transform;
	if (!wl_list_empty(&p->state.frame_callbacks)) {
		wl_list_insert_list(c->state.frame_callbacks.prev, &p->state.frame_callbacks);
		wl_list_init(&p->state.frame_callbacks);
	}
	c->commit |= p->commit;
	drm_syncobj_surface_cache_commit(surface);
	subsurface_cache_parent_commit(surface);
	p->commit = 0;
	pixman_region32_clear(&p->state.damage);
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
	if (width <= 0 || height <= 0) return;

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

/* Rasterize the inverse client transform into logical surface coordinates.
	* The usual scale=1/normal path keeps the original GPU buffer unchanged. */
static struct wld_buffer *
logical_buffer(struct surface *surface, struct wld_buffer *source, int32_t scale,
               int32_t transform)
{
	if (!source) return NULL;
	if (scale == 1 && transform == WL_OUTPUT_TRANSFORM_NORMAL) {
		wld_buffer_reference(source);
		return source;
	}
	uint32_t rotated_width = (transform & 1) ? source->height : source->width;
	uint32_t rotated_height = (transform & 1) ? source->width : source->height;
	if (rotated_width % scale || rotated_height % scale) {
		wl_resource_post_error(surface->resource, WL_SURFACE_ERROR_INVALID_SIZE,
		    "buffer size is not divisible by its scale");
		return NULL;
	}
	uint32_t width = rotated_width / scale, height = rotated_height / scale;
	if (!width || !height || source->width > UINT32_MAX / 4 ||
		    source->height > SIZE_MAX / ((size_t)source->width * 4)) return NULL;
	struct wld_buffer *out = wld_create_buffer(swc.shm->context, width, height,
		    source->format, WLD_FLAG_MAP);
	if (!out || !wld_map(out)) {
		if (out) wld_buffer_unreference(out);
		wl_resource_post_no_memory(surface->resource);
		return NULL;
	}
	bool mapped = wld_map(source), ok = mapped;
	void *pixels = mapped ? source->map : malloc((size_t)source->width * source->height * 4);
	uint32_t pitch = mapped ? source->pitch : source->width * 4;
	if (!mapped && pixels && swc.backend && swc.backend->context && swc.backend->renderer) {
		struct wld_renderer *renderer = swc.backend->renderer;
		struct wld_buffer *scratch = wld_create_buffer(swc.backend->context,
		    source->width, source->height, WLD_FORMAT_ARGB8888, 0);
		if (scratch && wld_set_target_buffer(renderer, scratch)) {
			wld_copy_rectangle(renderer, source, 0, 0, 0, 0, source->width, source->height);
			ok = wld_read_pixels(renderer, 0, 0, source->width, source->height, pitch, pixels);
			wld_flush(renderer);
		}
		if (scratch) wld_buffer_unreference(scratch);
	}
	if (ok) {
		for (uint32_t y = 0; y < height; ++y) {
			uint32_t *row = (uint32_t *)((char *)out->map + (size_t)y * out->pitch);
			for (uint32_t x = 0; x < width; ++x) {
				uint32_t tx = x * scale + scale / 2, ty = y * scale + scale / 2;
				uint32_t sx, sy;
				if (transform & 4) tx = rotated_width - 1 - tx;
				switch (transform & 3) {
				case 1: sx = ty; sy = source->height - 1 - tx; break;
				case 2: sx = source->width - 1 - tx; sy = source->height - 1 - ty; break;
				case 3: sx = source->width - 1 - ty; sy = tx; break;
				default: sx = tx; sy = ty; break;
				}
				row[x] = *(uint32_t *)((char *)pixels + (size_t)sy * pitch + (size_t)sx * 4);
			}
		}
	}
	if (mapped) wld_unmap(source); else free(pixels);
	wld_unmap(out);
	if (!ok) {
		wld_buffer_unreference(out);
		wl_resource_post_no_memory(surface->resource);
		return NULL;
	}
	return out;
}

static void
surface_apply_pending(struct surface *surface, bool flush_children, bool cached)
{
	struct wld_buffer *buffer;
	struct surface_pending *pending = cached ? &surface->cached : &surface->pending;
	surface->applying_cached = cached;
	struct wl_resource *replaced = NULL;
	bool attached, ready;

	/*
	 * An explicit-synchronization protocol error leaves the client dead, so
	 * nothing of this commit may be applied.
	 */
	if (!drm_syncobj_surface_check_commit(surface)) {
		return;
	}

	if (!cached && !shm_buffer_read((pending->commit & SURFACE_COMMIT_ATTACH) ?
	    pending->state.buffer_resource : surface->state.buffer_resource)) return;

	/* Attach */
	attached = pending->commit & SURFACE_COMMIT_ATTACH;
	if (attached) {
		if (surface->state.buffer &&
		    surface->state.buffer != pending->state.buffer) {
			replaced = surface->state.buffer_resource;
		}

		state_assign_buffer(&surface->state, pending->state.buffer, pending->state.buffer_resource);
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
	if ((!ready || !drm_syncobj_surface_buffer_ready(surface)) && surface->view)
		buffer = surface->view->buffer;
	if (pending->commit & SURFACE_COMMIT_GEOMETRY) {
		const struct swc_rectangle *g = &pending->window_geometry;
		surface->has_window_geometry = true;
		surface->window_x = g->x;
		surface->window_y = g->y;
		surface->window_width = g->width;
		surface->window_height = g->height;
	}

	if (pending->commit & SURFACE_COMMIT_SCALE) surface->state.buffer_scale = pending->state.buffer_scale;
	if (pending->commit & SURFACE_COMMIT_TRANSFORM) surface->state.buffer_transform = pending->state.buffer_transform;

	/* Damage */
	if (pending->commit & SURFACE_COMMIT_DAMAGE) {
		pixman_region32_union(&surface->state.damage, &surface->state.damage,
		                      &pending->state.damage);
		pixman_region32_clear(&pending->state.damage);
	}

	/* Opaque */
	if (pending->commit & SURFACE_COMMIT_OPAQUE) {
		pixman_region32_copy(&surface->state.opaque,
		                     &pending->state.opaque);
	}

	/* Input */
	if (pending->commit & SURFACE_COMMIT_INPUT) {
		pixman_region32_copy(&surface->state.input,
		                     &pending->state.input);
	}

	/* Frame */
	if (pending->commit & SURFACE_COMMIT_FRAME) {
		wl_list_insert_list(&surface->state.frame_callbacks,
		                    &pending->state.frame_callbacks);
		wl_list_init(&pending->state.frame_callbacks);
	}

	struct wld_buffer *logical = NULL;
	bool refresh = ready && drm_syncobj_surface_buffer_ready(surface) &&
		(pending->commit & (SURFACE_COMMIT_ATTACH | SURFACE_COMMIT_SCALE |
				SURFACE_COMMIT_TRANSFORM | SURFACE_COMMIT_DAMAGE | SURFACE_COMMIT_DAMAGE_BUFFER));
	if (refresh && buffer) {
		logical = logical_buffer(surface, buffer, surface->state.buffer_scale,
		    surface->state.buffer_transform);
		if (!logical) return;
		buffer = logical;
	}
	if (pending->commit & (SURFACE_COMMIT_ATTACH | SURFACE_COMMIT_SCALE |
		    SURFACE_COMMIT_TRANSFORM | SURFACE_COMMIT_DAMAGE_BUFFER)) {
		pixman_region32_union_rect(&surface->state.damage, &surface->state.damage, 0, 0,
		    buffer ? buffer->width : 0, buffer ? buffer->height : 0);
	}
	if (!refresh && surface->view) buffer = surface->view->buffer;
	trim_region(&surface->state.damage, buffer);
	trim_region(&surface->state.opaque, buffer);

	if (surface->view) {
		if (refresh || (pending->commit & SURFACE_COMMIT_GEOMETRY)) {
			view_attach(surface->view, buffer);
		}
		view_update(surface->view);
	}

	if (logical) wld_buffer_unreference(logical);
	pending->commit = 0;
	state_set_buffer(&pending->state, NULL);

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
				surface_apply_pending(child->surface, true, true);
			}
		}
	}
	surface->applying_cached = false;
}

static void
commit(struct wl_client *client, struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->applying_cached = false;

	if (surface->subsurface &&
	    subsurface_is_synchronized(surface->subsurface)) {
		if (!drm_syncobj_surface_check_commit(surface) ||
		    !shm_buffer_read((surface->pending.commit & SURFACE_COMMIT_ATTACH) ?
		       surface->pending.state.buffer_resource :
		       (surface->cached.state.buffer_resource ? surface->cached.state.buffer_resource : surface->state.buffer_resource))) return;
		cache_pending(surface);
		surface->subsurface->pending = true;
		return;
	}

	surface_apply_pending(surface, true, false);
}

static void
set_buffer_transform(struct wl_client *client, struct wl_resource *surface,
                     int32_t transform)
{
	if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
	    transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(surface, WL_SURFACE_ERROR_INVALID_TRANSFORM,
		                       "buffer transform %" PRId32 " is not a valid "
		                       "wl_output.transform value",
		                       transform);
		return;
	}

	struct surface *s = wl_resource_get_user_data(surface);
	s->pending.state.buffer_transform = transform;
	s->pending.commit |= SURFACE_COMMIT_TRANSFORM;
}

static void
set_buffer_scale(struct wl_client *client, struct wl_resource *surface,
                 int32_t scale)
{
	if (scale <= 0) {
		wl_resource_post_error(surface, WL_SURFACE_ERROR_INVALID_SCALE,
		                       "buffer scale %" PRId32 " is not positive",
		                       scale);
		return;
	}

	struct surface *s = wl_resource_get_user_data(surface);
	s->pending.state.buffer_scale = scale;
	s->pending.commit |= SURFACE_COMMIT_SCALE;
}

static void
damage_buffer(struct wl_client *client, struct wl_resource *surface, int32_t x,
              int32_t y, int32_t w, int32_t h)
{
	(void)client; (void)x; (void)y;
	if (w <= 0 || h <= 0) return;
	struct surface *s = wl_resource_get_user_data(surface);
	s->pending.commit |= SURFACE_COMMIT_DAMAGE_BUFFER;
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
	state_finalize(&surface->cached.state);

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
	surface->cached.commit = 0;
	surface->applying_cached = false;
	wl_signal_init(&surface->signal.commit);
	wl_signal_init(&surface->signal.destroy);
	surface->view = NULL;
	surface->view_handler.impl = &view_handler_impl;
	surface->role = NULL;
	surface->role_name = NULL;
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
	state_initialize(&surface->cached.state);

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
		if (drm_syncobj_surface_buffer_ready(surface)) {
			struct wld_buffer *logical = logical_buffer(surface, surface->state.buffer,
			    surface->state.buffer_scale, surface->state.buffer_transform);
			view_attach(view, logical);
			if (logical) wld_buffer_unreference(logical);
		}
		view_update(view);
	}
}

bool
surface_set_role(struct surface *surface, struct wl_resource *role)
{
	if (surface->role == role) return true;
	const char *name = wl_resource_get_class(role);
	if (surface->role || (surface->role_name && strcmp(surface->role_name, name) != 0)) {
		return false;
	}

	surface->role_name = name;
	surface->role = role;
	wl_resource_add_destroy_listener(role, &surface->role_destroy_listener);
	return true;
}

bool
surface_has_buffer(struct surface *surface)
{
	return surface->state.buffer || surface->cached.state.buffer ||
	       ((surface->pending.commit & SURFACE_COMMIT_ATTACH) &&
	        surface->pending.state.buffer_resource);
}

void
surface_commit_pending(struct surface *surface)
{
	surface_apply_pending(surface, true, true);
}

void
surface_show_buffer(struct surface *surface, struct wld_buffer *buffer, int32_t scale, int32_t transform)
{
	if (!surface->view) {
		return;
	}

	struct wld_buffer *logical = logical_buffer(surface, buffer, scale, transform);
	if (buffer && !logical) return;
	buffer = logical;

	/* Whatever the commit damaged was drawn from the old buffer. */
	if (buffer) {
		pixman_region32_union_rect(&surface->state.damage,
		                           &surface->state.damage, 0, 0,
		                           buffer->width, buffer->height);
	}
	view_attach(surface->view, buffer);
	view_update(surface->view);
	if (logical) wld_buffer_unreference(logical);
	if (surface->subsurface) subsurface_update_visibility(surface->subsurface);
	/* Roles must map a first frame deferred until its acquire fence signalled. */
	wl_signal_emit(&surface->signal.commit, surface);
}
