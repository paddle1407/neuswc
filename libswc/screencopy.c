/* swc: libswc/screencopy.c
 *
 * Copyright (c) 2026 the neuswc authors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "screencopy.h"
#include "compositor.h"
#include "backend.h"
#include "wayland_buffer.h"
#ifdef ENABLE_DRM
#include "drm.h"
#include <wld/drm.h>
#endif
#include "internal.h"
#include "output.h"
#include "screen.h"
#include "shm.h"
#include "snap.h"
#include "util.h"

#include "wlr-screencopy-unstable-v1-server-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <wayland-server.h>
#include <wld/wld.h>

/* Frames queued by copy_with_damage, waiting for the screen to be damaged. */
static struct wl_list pending = {&pending, &pending};
static bool force_shm, profile_enabled;
static uint64_t revision, image_revision[SWC_MAX_SCREENS],
    cursor_revision[SWC_MAX_SCREENS];
static struct wl_event_source *capture_timer;
static bool timer_armed;
static int handle_capture_timer(void *data);
struct screencopy_frame;
static bool frame_is_dirty(struct screencopy_frame *frame);

struct capture_history {
	uint64_t image, cursor;
	struct swc_rectangle rect;
	bool valid, overlay_cursor;
};
struct capture_manager {
	unsigned references;
	struct capture_history history[SWC_MAX_SCREENS];
};

static void
manager_unref(struct capture_manager *manager)
{
	if (!--manager->references) free(manager);
}

static void
schedule_capture(int delay_ms)
{
	if (timer_armed || wl_list_empty(&pending)) return;
	if (!capture_timer)
		capture_timer = wl_event_loop_add_timer(swc.event_loop, handle_capture_timer, NULL);
	if (capture_timer && wl_event_source_timer_update(capture_timer, delay_ms) == 0)
		timer_armed = true;
}

void
screencopy_cursor_changed(uint32_t screens)
{
	if (!screens) return;
	uint64_t value = ++revision;
	for (unsigned i = 0; i < SWC_MAX_SCREENS; ++i)
		if (screens & (1u << i)) cursor_revision[i] = value;
	/* Coalesce high-rate hardware cursor events. No composition is required
	 * when only the separate cursor plane has changed. */
	schedule_capture(16);
}

void
screencopy_finalize(void)
{
	if (capture_timer) wl_event_source_remove(capture_timer);
	capture_timer = NULL;
	timer_armed = false;
}

enum capture_path { CAPTURE_DMABUF, CAPTURE_SHM_DIRECT, CAPTURE_SHM_FALLBACK, CAPTURE_PATHS };
static const char *path_names[] = { "GPU dmabuf", "SHM direct readback", "SHM recomposition fallback" };
static struct {
	bool announced;
	unsigned count;
	uint64_t total_ns, max_ns;
} profile[CAPTURE_PATHS];

static void
profile_capture(enum capture_path path, const struct timespec *start,
                const struct timespec *end)
{
	if (!profile[path].announced) {
		fprintf(stderr, "screencopy: capture path: %s\n", path_names[path]);
		profile[path].announced = true;
	}
	if (!profile_enabled) return;
	uint64_t ns = (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000 +
	              end->tv_nsec - start->tv_nsec;
	profile[path].total_ns += ns;
	if (ns > profile[path].max_ns) profile[path].max_ns = ns;
	if (++profile[path].count == 120) {
		fprintf(stderr, "screencopy: %s: 120 copies, average %.3f ms, worst %.3f ms\n",
		        path_names[path], profile[path].total_ns / 120000000.0,
		        profile[path].max_ns / 1000000.0);
		profile[path].count = 0;
		profile[path].total_ns = profile[path].max_ns = 0;
	}
}

static bool
can_capture_dmabuf(struct screen *screen)
{
#ifdef ENABLE_DRM
	if (force_shm || !swc.drm->context || !swc.backend->renderer) return false;
	struct wld_buffer *source = compositor_capture_buffer(screen);
	if (!source || !(wld_capabilities(swc.backend->renderer, source) & WLD_CAPABILITY_READ))
		return false;
	/* Use the same format/modifier support advertised by linux-dmabuf. */
	static int supported = -1;
	if (supported < 0) {
		uint64_t modifier;
		supported = wld_drm_query_modifiers(swc.drm->context, WLD_FORMAT_XRGB8888, &modifier, 1) != 0;
	}
	return supported;
#else
	return false;
#endif
}

struct capture_target {
	struct swc_shm_buffer_info shm;
	struct wld_buffer *dmabuf;
};

struct screencopy_frame {
	struct wl_resource *resource;
	struct capture_manager *manager;
	struct screen *screen;

	/* Capture region, in screen-local coordinates. */
	struct swc_rectangle rect;
	bool overlay_cursor;
	bool dmabuf_allowed;
	bool dirty_at_request;

	/* Set once copy or copy_with_damage has been requested. */
	bool used;
	bool completed;

	/* Non-NULL only while queued on 'pending'. */
	struct wl_resource *buffer;
	struct wl_list link;

	struct wl_listener screen_destroy;
	struct wl_listener buffer_destroy;
};

static void
frame_dequeue(struct screencopy_frame *frame)
{
	if (!frame->buffer)
		return;

	wl_list_remove(&frame->link);
	wl_list_remove(&frame->buffer_destroy.link);
	frame->buffer = NULL;
}

static void
frame_fail(struct screencopy_frame *frame)
{
	if (frame->completed) return;
	frame->completed = true;
	frame_dequeue(frame);
	zwlr_screencopy_frame_v1_send_failed(frame->resource);
}

/**
 * Check that the client's buffer can receive this frame.
 *
 * Mismatched attributes are a protocol error rather than a capture failure, so
 * the caller passes 'post_error' when it is still handling the copy request and
 * may legally kill the client.
 */
static bool
frame_check_buffer(struct screencopy_frame *frame, struct wl_resource *buffer_resource,
                   struct capture_target *target, bool post_error)
{
	uint32_t row_bytes = frame->rect.width * 4;
	memset(target, 0, sizeof(*target));
	struct swc_shm_buffer_info *info = &target->shm;
	if (shm_buffer_get_info(buffer_resource, info)) {
		if (info->format != WL_SHM_FORMAT_XRGB8888 && info->format != WL_SHM_FORMAT_ARGB8888)
			goto invalid;
		if ((uint32_t)info->width != frame->rect.width || (uint32_t)info->height != frame->rect.height)
			goto invalid;
		if ((uint32_t)info->stride < row_bytes || !info->writable)
			goto invalid;
		return true;
	}
	struct wld_buffer *buffer = wayland_buffer_get(buffer_resource);
	if (!frame->dmabuf_allowed || !buffer || buffer->format != WLD_FORMAT_XRGB8888 ||
	    buffer->width != frame->rect.width || buffer->height != frame->rect.height)
		goto invalid;
	target->dmabuf = buffer;
	return true;

invalid:
	if (post_error) {
		wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
		                       "invalid buffer attributes");
	} else {
		frame_fail(frame);
	}
	return false;
}

/* Read the already-composited image into the client's memory. A GPU readback
 * is still synchronous, but there is no second scene render or staging copy. */
static bool
copy_shm(struct screencopy_frame *frame, struct swc_shm_buffer_info *target,
         enum capture_path *path)
{
	struct wld_buffer *source = compositor_capture_buffer(frame->screen);
	struct wld_renderer *renderer = swc.backend->renderer;
	bool copied = false;
	const struct swc_rectangle *r = &frame->rect;
	if (source && renderer) {
		if (wld_set_target_buffer(renderer, source))
			copied = wld_read_pixels(renderer, r->x, r->y, r->width, r->height,
			                         target->stride, target->data);
		wld_set_target_buffer(renderer, NULL);
	}
	if (copied) {
		*path = CAPTURE_SHM_DIRECT;
	} else {
		/* CPU backends can map the composed image; only an unavailable output
		 * or an unreadable backend needs the old scene-rendering fallback. */
		struct wld_buffer *fallback = NULL;
		if (!source || !wld_map(source)) {
			fallback = compositor_render_to_shm(frame->screen);
			source = fallback;
			if (!source || !wld_map(source)) {
				if (fallback) wld_buffer_unreference(fallback);
				return false;
			}
		}
		if (!source->map) {
			wld_unmap(source);
			if (fallback) wld_buffer_unreference(fallback);
			return false;
		}
		for (uint32_t y = 0; y < r->height; ++y)
			memcpy((uint8_t *)target->data + (size_t)y * target->stride,
			       (uint8_t *)source->map + (size_t)(y + r->y) * source->pitch + (size_t)r->x * 4,
			       r->width * 4);
		wld_unmap(source);
		*path = fallback ? CAPTURE_SHM_FALLBACK : CAPTURE_SHM_DIRECT;
		if (fallback) wld_buffer_unreference(fallback);
	}
	if (frame->overlay_cursor)
		snap_overlay_cursor_region(target->data, r->width, r->height, target->stride,
		                           frame->screen, r->x, r->y);
	return true;
}

static bool
copy_dmabuf(struct screencopy_frame *frame, struct wld_buffer *destination)
{
	struct wld_buffer *source = compositor_capture_buffer(frame->screen);
	struct wld_renderer *renderer = swc.backend->renderer;
	if (!source || !renderer ||
	    !(wld_capabilities(renderer, destination) & WLD_CAPABILITY_WRITE)) return false;
	bool copied = wld_set_target_buffer(renderer, destination);
	if (copied) {
		const struct swc_rectangle *r = &frame->rect;
		wld_copy_rectangle(renderer, source, 0, 0, r->x, r->y, r->width, r->height);
		if (frame->overlay_cursor)
			copied = snap_render_cursor(renderer, frame->screen, r);
		/* Complete GPU writes before ready: retain the synchronization barrier
		 * until an explicit fence path can replace it. No CPU pixel readback. */
		wld_flush(renderer);
	}
	wld_set_target_buffer(renderer, NULL);
	return copied;
}

static void
frame_copy(struct screencopy_frame *frame, struct wl_resource *buffer_resource,
           pixman_region32_t *damage, bool post_error)
{
	struct capture_target target;
	struct timespec start = {0}, ts;
	if (profile_enabled) clock_gettime(CLOCK_MONOTONIC, &start);
	if (!frame_check_buffer(frame, buffer_resource, &target, post_error)) return;
	if (!swc.active || !frame->rect.width || !frame->rect.height ||
	    (uint64_t)frame->rect.x + frame->rect.width > frame->screen->base.geometry.width ||
	    (uint64_t)frame->rect.y + frame->rect.height > frame->screen->base.geometry.height) {
		frame_fail(frame);
		return;
	}
	enum capture_path path = CAPTURE_DMABUF;
	bool copied = target.dmabuf ? copy_dmabuf(frame, target.dmabuf) :
	                             copy_shm(frame, &target.shm, &path);
	if (!copied) {
		if (target.dmabuf && !force_shm) {
			force_shm = true;
			WARNING("DMA-BUF capture target could not be rendered; offering SHM on subsequent frames\n");
		}
		frame_fail(frame);
		return;
	}
	struct capture_history *history = &frame->manager->history[frame->screen->id];
	*history = (struct capture_history){
		.image = image_revision[frame->screen->id], .cursor = cursor_revision[frame->screen->id],
		.rect = frame->rect, .valid = true, .overlay_cursor = frame->overlay_cursor,
	};
	frame->completed = true;
	frame_dequeue(frame);
	zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);
	/* Full damage is conservative and covers changes between requests too.
	 * Buffers are filled in full; reporting only this repaint's damage could
	 * omit earlier changes or a cursor moving between capture requests. */
	if (damage && wl_resource_get_version(frame->resource) >= 2)
		zwlr_screencopy_frame_v1_send_damage(frame->resource, 0, 0,
		                                      frame->rect.width, frame->rect.height);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	profile_capture(path, &start, &ts);
	uint64_t seconds = ts.tv_sec;
	zwlr_screencopy_frame_v1_send_ready(frame->resource, seconds >> 32, seconds, ts.tv_nsec);
}

static void
handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct screencopy_frame *frame = wl_container_of(listener, frame, buffer_destroy);

	frame_fail(frame);
}

static void
handle_screen_destroy(struct wl_listener *listener, void *data)
{
	struct screencopy_frame *frame = wl_container_of(listener, frame, screen_destroy);

	frame_fail(frame);
	frame->screen = NULL;

	/* The signal is going away with the screen; drop out of it now and leave
	 * the link safe for destroy_frame() to remove again. */
	wl_list_remove(&frame->screen_destroy.link);
	wl_list_init(&frame->screen_destroy.link);
}

static bool
frame_claim(struct screencopy_frame *frame)
{
	if (frame->used) {
		wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
		                       "frame has already been used");
		return false;
	}
	frame->used = true;
	if (frame->completed) return false;

	if (!frame->screen) {
		frame_fail(frame);
		return false;
	}

	return true;
}

static void
copy(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
	struct screencopy_frame *frame = wl_resource_get_user_data(resource);

	if (!frame_claim(frame))
		return;

	frame_copy(frame, buffer_resource, NULL, true);
}

static void
copy_with_damage(struct wl_client *client, struct wl_resource *resource,
                 struct wl_resource *buffer_resource)
{
	struct screencopy_frame *frame = wl_resource_get_user_data(resource);
	struct capture_target target;

	if (!frame_claim(frame))
		return;

	/* Validate now so that a bad buffer is reported at request time. */
	if (!frame_check_buffer(frame, buffer_resource, &target, true))
		return;

	frame->buffer = buffer_resource;
	frame->buffer_destroy.notify = &handle_buffer_destroy;
	wl_resource_add_destroy_listener(buffer_resource, &frame->buffer_destroy);
	wl_list_insert(&pending, &frame->link);
	/* Snapshot pending damage before another concurrent frame consumes the
	 * manager history. Cursor-only updates are paced even if requests arrive
	 * rapidly from a high-refresh preview. */
	frame->dirty_at_request = frame_is_dirty(frame);
	struct capture_history *h = &frame->manager->history[frame->screen->id];
	schedule_capture(!h->valid || h->image != image_revision[frame->screen->id] ? 1 : 16);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
    .copy = copy,
    .destroy = destroy_resource,
    .copy_with_damage = copy_with_damage,
};

static void
destroy_frame(struct wl_resource *resource)
{
	struct screencopy_frame *frame = wl_resource_get_user_data(resource);

	frame_dequeue(frame);
	wl_list_remove(&frame->screen_destroy.link);
	manager_unref(frame->manager);
	free(frame);
}

/**
 * 'region' is in screen-local coordinates, or NULL to capture the whole screen.
 */
static void
frame_create(struct wl_client *client, struct wl_resource *manager, uint32_t id,
             int32_t overlay_cursor, struct wl_resource *output_resource,
             const struct swc_rectangle *region)
{
	struct output *output = wl_resource_get_user_data(output_resource);
	struct screencopy_frame *frame;
	struct wl_resource *resource;
	const struct swc_rectangle *geom;
	int64_t x1, y1, x2, y2;
	uint32_t version = wl_resource_get_version(manager);

	frame = malloc(sizeof(*frame));
	if (!frame) {
		wl_client_post_no_memory(client);
		return;
	}

	resource = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface, version, id);
	if (!resource) {
		free(frame);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &frame_impl, frame, &destroy_frame);

	frame->resource = resource;
	frame->manager = wl_resource_get_user_data(manager);
	++frame->manager->references;
	frame->screen = output && output->screen ? output->screen : NULL;
	frame->overlay_cursor = overlay_cursor != 0;
	frame->dmabuf_allowed = false;
	frame->dirty_at_request = false;
	frame->used = false;
	frame->completed = false;
	frame->buffer = NULL;
	frame->rect = (struct swc_rectangle){0, 0, 0, 0};

	wl_list_init(&frame->screen_destroy.link);

	if (!frame->screen) {
		frame_fail(frame);
		return;
	}

	frame->screen_destroy.notify = &handle_screen_destroy;
	wl_list_remove(&frame->screen_destroy.link);
	wl_signal_add(&frame->screen->destroy_signal, &frame->screen_destroy);

	geom = &frame->screen->base.geometry;

	if (region) {
		x1 = MAX(region->x, 0);
		y1 = MAX(region->y, 0);
		x2 = MIN((int64_t)region->x + region->width, (int32_t)geom->width);
		y2 = MIN((int64_t)region->y + region->height, (int32_t)geom->height);
	} else {
		x1 = 0;
		y1 = 0;
		x2 = geom->width;
		y2 = geom->height;
	}

	if (x2 <= x1 || y2 <= y1) {
		frame_fail(frame);
		return;
	}

	frame->rect = (struct swc_rectangle){x1, y1, (uint32_t)(x2 - x1), (uint32_t)(y2 - y1)};

	zwlr_screencopy_frame_v1_send_buffer(resource, WL_SHM_FORMAT_XRGB8888, frame->rect.width,
	                                     frame->rect.height, frame->rect.width * 4);

	if (version >= ZWLR_SCREENCOPY_FRAME_V1_LINUX_DMABUF_SINCE_VERSION &&
	    can_capture_dmabuf(frame->screen)) {
		frame->dmabuf_allowed = true;
		zwlr_screencopy_frame_v1_send_linux_dmabuf(resource, WLD_FORMAT_XRGB8888,
		                                           frame->rect.width, frame->rect.height);
	}
	if (version >= ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION)
		zwlr_screencopy_frame_v1_send_buffer_done(resource);
}

static void
capture_output(struct wl_client *client, struct wl_resource *resource, uint32_t id,
               int32_t overlay_cursor, struct wl_resource *output_resource)
{
	frame_create(client, resource, id, overlay_cursor, output_resource, NULL);
}

static void
capture_output_region(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                      int32_t overlay_cursor, struct wl_resource *output_resource, int32_t x,
                      int32_t y, int32_t width, int32_t height)
{
	struct swc_rectangle region;

	if (width <= 0 || height <= 0) {
		frame_create(client, resource, id, overlay_cursor, output_resource,
		             &(struct swc_rectangle){0, 0, 0, 0});
		return;
	}

	region = (struct swc_rectangle){x, y, (uint32_t)width, (uint32_t)height};
	frame_create(client, resource, id, overlay_cursor, output_resource, &region);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
    .capture_output = capture_output,
    .capture_output_region = capture_output_region,
    .destroy = destroy_resource,
};

static void
destroy_manager(struct wl_resource *resource)
{
	manager_unref(wl_resource_get_user_data(resource));
}

static void
bind_screencopy(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct capture_manager *manager = calloc(1, sizeof(*manager));
	if (!manager) { wl_client_post_no_memory(client); return; }
	struct wl_resource *resource = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface, version, id);
	if (!resource) {
		free(manager);
		wl_client_post_no_memory(client);
		return;
	}
	manager->references = 1;
	wl_resource_set_implementation(resource, &manager_impl, manager, destroy_manager);
}

static bool
frame_is_dirty(struct screencopy_frame *frame)
{
	struct capture_history *h = &frame->manager->history[frame->screen->id];
	return !h->valid || h->image != image_revision[frame->screen->id] ||
	       h->overlay_cursor != frame->overlay_cursor ||
	       (frame->overlay_cursor && h->cursor != cursor_revision[frame->screen->id]) ||
	       h->rect.x != frame->rect.x || h->rect.y != frame->rect.y ||
	       h->rect.width != frame->rect.width || h->rect.height != frame->rect.height;
}

static int
handle_capture_timer(void *data)
{
	struct screencopy_frame *frame, *tmp;
	timer_armed = false;
	wl_list_for_each_safe(frame, tmp, &pending, link) {
		if (!swc.active) { frame_fail(frame); continue; }
		if (!frame->dirty_at_request && !frame_is_dirty(frame)) continue;
		pixman_region32_t damage;
		pixman_region32_init_rect(&damage, frame->rect.x, frame->rect.y,
		                          frame->rect.width, frame->rect.height);
		frame_copy(frame, frame->buffer, &damage, false);
		pixman_region32_fini(&damage);
	}
	return 0;
}

void
screencopy_handle_damage(struct screen *screen, pixman_region32_t *global_damage)
{
	struct screencopy_frame *frame, *tmp;
	pixman_region32_t damage;

	if (pixman_region32_not_empty(global_damage))
		image_revision[screen->id] = ++revision;
	if (wl_list_empty(&pending)) return;

	wl_list_for_each_safe (frame, tmp, &pending, link) {
		if (frame->screen != screen) continue;
		const struct swc_rectangle *geom = &frame->screen->base.geometry;
		struct wl_resource *buffer = frame->buffer;

		pixman_region32_init(&damage);
		pixman_region32_intersect_rect(&damage, global_damage, geom->x, geom->y, geom->width,
		                               geom->height);
		pixman_region32_translate(&damage, -geom->x, -geom->y);
		pixman_region32_intersect_rect(&damage, &damage, frame->rect.x, frame->rect.y,
		                               frame->rect.width, frame->rect.height);

		if (pixman_region32_not_empty(&damage))
			frame_copy(frame, buffer, &damage, false);

		pixman_region32_fini(&damage);
	}
}

struct wl_global *
screencopy_manager_create(struct wl_display *display)
{
	const char *shm = getenv("SWC_SCREENCOPY_SHM"), *timing = getenv("SWC_CAPTURE_PROFILE");
	force_shm = shm && !strcmp(shm, "1");
	profile_enabled = timing && !strcmp(timing, "1");
	return wl_global_create(display, &zwlr_screencopy_manager_v1_interface, 3, NULL,
	                        &bind_screencopy);
}
