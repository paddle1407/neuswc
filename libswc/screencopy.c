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
#include "pointer.h"
#include "screen.h"
#include "seat.h"
#include "shm.h"
#include "snap.h"
#include "util.h"

#include "wlr-screencopy-unstable-v1-server-protocol.h"

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
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
	/* Where the overlaid cursor was, screen-local; empty if not drawn. */
	pixman_box32_t cursor_box;
};

/*
 * What each screen's damage was over its last few repaints, so that a copy
 * can be limited to what changed since a buffer was last filled, and the
 * damage event can say what changed since the client's last copy. 'evicted'
 * is the newest image revision that has fallen out of the ring: anything
 * older than that can no longer be answered, and gets the full rectangle.
 */
#define DAMAGE_HISTORY 32

static struct damage_history {
	/* Only compared: a new screen that took over the id starts afresh. */
	struct screen *screen;
	struct {
		uint64_t image;
		pixman_region32_t region;
	} entries[DAMAGE_HISTORY];
	unsigned next, count;
	uint64_t evicted;
} damage_history[SWC_MAX_SCREENS];

/*
 * What a client's buffer was last filled with. A client that captures into a
 * pool of buffers -- a screencast usually does -- gets each one back several
 * frames later, and only what changed on the screen since then has to be
 * copied into it again. A buffer that has never been filled, or that failed
 * part way through, is copied in full.
 */
struct buffer_state {
	struct wl_resource *buffer;
	struct wl_listener destroy_listener;
	struct wl_list link;
	uint32_t screen_id;
	struct swc_rectangle rect;
	bool overlay_cursor;
	uint64_t image;
	pixman_box32_t cursor_box;
};

static struct wl_list buffer_states = {&buffer_states, &buffer_states};

/* A handful of boxes is worth copying one by one; beyond that, the time goes
 * on per-copy overhead and the box around them all is copied instead. */
#define MAX_COPY_BOXES 16
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

static void
buffer_state_destroy(struct buffer_state *state)
{
	wl_list_remove(&state->destroy_listener.link);
	wl_list_remove(&state->link);
	free(state);
}

void
screencopy_finalize(void)
{
	struct buffer_state *state, *tmp;

	if (capture_timer) wl_event_source_remove(capture_timer);
	capture_timer = NULL;
	timer_armed = false;
	wl_list_for_each_safe(state, tmp, &buffer_states, link)
		buffer_state_destroy(state);
	for (unsigned i = 0; i < SWC_MAX_SCREENS; ++i) {
		struct damage_history *history = &damage_history[i];
		for (unsigned j = 0; j < history->count; ++j)
			pixman_region32_fini(&history->entries[j].region);
		*history = (struct damage_history){0};
	}
}

static void
damage_history_add(struct screen *screen, uint64_t image, pixman_region32_t *region)
{
	struct damage_history *history = &damage_history[screen->id];
	unsigned slot;

	if (history->screen != screen) {
		for (unsigned i = 0; i < history->count; ++i)
			pixman_region32_fini(&history->entries[i].region);
		/* Buffers filled from the old screen are copied in full. */
		*history = (struct damage_history){.screen = screen, .evicted = image};
	}
	slot = history->next;

	if (history->count < DAMAGE_HISTORY) {
		pixman_region32_init(&history->entries[slot].region);
		++history->count;
	} else {
		history->evicted = history->entries[slot].image;
	}
	history->entries[slot].image = image;
	pixman_region32_copy(&history->entries[slot].region, region);
	history->next = (slot + 1) % DAMAGE_HISTORY;
}

/* Add everything that changed on the screen after image revision 'since' to
 * 'region', in screen-local coordinates. False if the history no longer goes
 * back that far. */
static bool
damage_since(uint32_t id, uint64_t since, pixman_region32_t *region)
{
	struct damage_history *history = &damage_history[id];

	if (since < history->evicted) return false;
	for (unsigned i = 0; i < history->count; ++i) {
		if (history->entries[i].image > since)
			pixman_region32_union(region, region, &history->entries[i].region);
	}
	return true;
}

/* The box the overlaid cursor covers on 'screen', screen-local, as
 * snap_overlay_cursor_region and snap_render_cursor draw it. */
static pixman_box32_t
cursor_box(struct screen *screen)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;
	pixman_box32_t box = {0, 0, 0, 0};

	if (!pointer || !pointer->cursor.buffer || !pointer->cursor.view.buffer ||
	    !(pointer->cursor.view.screens & screen_mask(screen)))
		return box;
	box.x1 = pointer->cursor.view.geometry.x - screen->base.geometry.x;
	box.y1 = pointer->cursor.view.geometry.y - screen->base.geometry.y;
	box.x2 = box.x1 + (int32_t)pointer->cursor.buffer->width;
	box.y2 = box.y1 + (int32_t)pointer->cursor.buffer->height;
	return box;
}

static void
region_add_box(pixman_region32_t *region, const pixman_box32_t *box)
{
	if (box->x2 > box->x1 && box->y2 > box->y1)
		pixman_region32_union_rect(region, region, box->x1, box->y1,
		                           box->x2 - box->x1, box->y2 - box->y1);
}

/* Clip to the frame, and trade a scattering of boxes for the one box around
 * them. */
static void
region_finish(pixman_region32_t *region, const struct swc_rectangle *rect)
{
	int count;

	pixman_region32_intersect_rect(region, region, rect->x, rect->y,
	                               rect->width, rect->height);
	pixman_region32_rectangles(region, &count);
	if (count > MAX_COPY_BOXES) {
		pixman_box32_t extents = *pixman_region32_extents(region);
		pixman_region32_fini(region);
		pixman_region32_init_with_extents(region, &extents);
	}
}

static void
handle_buffer_state_destroy(struct wl_listener *listener, void *data)
{
	struct buffer_state *state = wl_container_of(listener, state, destroy_listener);

	(void)data;
	buffer_state_destroy(state);
}

static struct buffer_state *
buffer_state_find(struct wl_resource *buffer)
{
	struct buffer_state *state;

	wl_list_for_each(state, &buffer_states, link) {
		if (state->buffer == buffer)
			return state;
	}
	return NULL;
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

	/*
	 * A GPU copy has been issued and the frame is waiting for its fence to
	 * say it is done before telling the client so. What the completion has
	 * to know about the copy is kept here meanwhile.
	 */
	bool copying;
	int fence_fd;
	struct wl_event_source *fence_source;
	bool with_damage;
	pixman_box32_t cursor;
	enum capture_path path;
	struct timespec start;

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
frame_fence_forget(struct screencopy_frame *frame)
{
	if (frame->fence_source) wl_event_source_remove(frame->fence_source);
	frame->fence_source = NULL;
	if (frame->fence_fd >= 0) close(frame->fence_fd);
	frame->fence_fd = -1;
	frame->copying = false;
}

static void
frame_fail(struct screencopy_frame *frame)
{
	if (frame->completed) return;
	frame_fence_forget(frame);
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

/*
 * Read the already-composited image into the client's memory. A GPU readback
 * is still synchronous, but there is no second scene render or staging copy.
 *
 * Only 'region' (screen-local, inside the frame) is read. GLES2 cannot read
 * into a destination with a different row length, so a box narrower than the
 * frame would cost one read per row; reading whole-width bands of rows keeps
 * it to one read per band while still skipping the rows nothing touched.
 */
static bool
copy_shm(struct screencopy_frame *frame, struct swc_shm_buffer_info *target,
         pixman_region32_t *region, enum capture_path *path)
{
	struct wld_buffer *source = compositor_capture_buffer(frame->screen);
	struct wld_renderer *renderer = swc.backend->renderer;
	bool copied = false;
	const struct swc_rectangle *r = &frame->rect;
	int count;
	pixman_box32_t *boxes = pixman_region32_rectangles(region, &count);
	if (source && renderer) {
		if (wld_set_target_buffer(renderer, source)) {
			copied = true;
			/* Boxes come sorted by their top edge, so bands that overlap or
			 * touch are next to each other. */
			for (int i = 0; i < count && copied;) {
				int32_t y1 = boxes[i].y1, y2 = boxes[i].y2;
				for (++i; i < count && boxes[i].y1 <= y2; ++i)
					y2 = MAX(y2, boxes[i].y2);
				copied = wld_read_pixels(renderer, r->x, y1, r->width, y2 - y1,
				                         target->stride,
				                         (uint8_t *)target->data +
				                             (size_t)(y1 - r->y) * target->stride);
			}
		}
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
		for (int i = 0; i < count; ++i) {
			size_t bytes = (size_t)(boxes[i].x2 - boxes[i].x1) * 4;
			for (int32_t y = boxes[i].y1; y < boxes[i].y2; ++y)
				memcpy((uint8_t *)target->data + (size_t)(y - r->y) * target->stride +
				           (size_t)(boxes[i].x1 - r->x) * 4,
				       (uint8_t *)source->map + (size_t)y * source->pitch +
				           (size_t)boxes[i].x1 * 4,
				       bytes);
		}
		wld_unmap(source);
		*path = fallback ? CAPTURE_SHM_FALLBACK : CAPTURE_SHM_DIRECT;
		if (fallback) wld_buffer_unreference(fallback);
	}
	if (frame->overlay_cursor)
		snap_overlay_cursor_region(target->data, r->width, r->height, target->stride,
		                           frame->screen, r->x, r->y);
	return true;
}

/*
 * Copy on the GPU. The client's buffer is ready when the GPU says so, which
 * is what 'fence' comes back as: -1 once the copy has already completed, or
 * a sync_file to wait for from the event loop. Waiting here instead stopped
 * the whole compositor for a GPU frame on every capture.
 */
static bool
copy_dmabuf(struct screencopy_frame *frame, struct wld_buffer *destination,
            pixman_region32_t *region, int *fence)
{
	struct wld_buffer *source = compositor_capture_buffer(frame->screen);
	struct wld_renderer *renderer = swc.backend->renderer;
	*fence = -1;
	if (!source || !renderer ||
	    !(wld_capabilities(renderer, destination) & WLD_CAPABILITY_WRITE)) return false;
	bool copied = wld_set_target_buffer(renderer, destination);
	if (copied) {
		const struct swc_rectangle *r = &frame->rect;
		int count;
		pixman_box32_t *boxes = pixman_region32_rectangles(region, &count);
		for (int i = 0; i < count; ++i)
			wld_copy_rectangle(renderer, source, boxes[i].x1 - r->x,
			                   boxes[i].y1 - r->y, boxes[i].x1, boxes[i].y1,
			                   boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
		if (frame->overlay_cursor)
			copied = snap_render_cursor(renderer, frame->screen, r);
		*fence = wld_export_fence(renderer);
		wld_flush(renderer);
	}
	wld_set_target_buffer(renderer, NULL);
	return copied;
}

/*
 * The part of the screen to copy into the client's buffer: what changed since
 * the buffer was last filled for this same frame shape, plus the cursor where
 * it was drawn then and where it is now, so that the cursor is never blended
 * twice or left behind. Plain copy requests, and anything the history cannot
 * answer, copy the whole frame.
 */
static void
frame_copy_region(struct screencopy_frame *frame, struct buffer_state *state,
                  bool with_damage, pixman_box32_t current_cursor,
                  pixman_region32_t *region)
{
	const struct swc_rectangle *r = &frame->rect;

	if (!with_damage || !state || state->screen_id != frame->screen->id ||
	    memcmp(&state->rect, r, sizeof(*r)) != 0 ||
	    state->overlay_cursor != frame->overlay_cursor ||
	    !damage_since(frame->screen->id, state->image, region)) {
		pixman_region32_fini(region);
		pixman_region32_init_rect(region, r->x, r->y, r->width, r->height);
		return;
	}
	if (frame->overlay_cursor) {
		region_add_box(region, &state->cursor_box);
		region_add_box(region, &current_cursor);
	}
	region_finish(region, r);
}

/* What changed since this manager's last copy of the screen, as the damage
 * event reports it: in frame coordinates, and the whole frame when the last
 * copy was of something else or is too far back to tell. */
static void
frame_send_damage(struct screencopy_frame *frame, const struct capture_history *h,
                  pixman_box32_t current_cursor)
{
	const struct swc_rectangle *r = &frame->rect;
	pixman_region32_t damage;
	pixman_box32_t *boxes;
	int count;

	pixman_region32_init(&damage);
	if (!h->valid || memcmp(&h->rect, r, sizeof(*r)) != 0 ||
	    h->overlay_cursor != frame->overlay_cursor ||
	    !damage_since(frame->screen->id, h->image, &damage)) {
		pixman_region32_union_rect(&damage, &damage, r->x, r->y, r->width, r->height);
	} else if (frame->overlay_cursor) {
		region_add_box(&damage, &h->cursor_box);
		region_add_box(&damage, &current_cursor);
	}
	region_finish(&damage, r);
	boxes = pixman_region32_rectangles(&damage, &count);
	for (int i = 0; i < count; ++i)
		zwlr_screencopy_frame_v1_send_damage(frame->resource, boxes[i].x1 - r->x,
		                                      boxes[i].y1 - r->y,
		                                      boxes[i].x2 - boxes[i].x1,
		                                      boxes[i].y2 - boxes[i].y1);
	pixman_region32_fini(&damage);
}

/* The copy is in the client's buffer: say so, and what of it changed. */
static void
frame_complete(struct screencopy_frame *frame)
{
	uint32_t id = frame->screen->id;
	struct capture_history *history = &frame->manager->history[id];
	struct timespec ts;

	frame_fence_forget(frame);
	frame->completed = true;
	frame_dequeue(frame);
	zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);
	if (frame->with_damage && wl_resource_get_version(frame->resource) >= 2)
		frame_send_damage(frame, history, frame->cursor);
	*history = (struct capture_history){
		.image = image_revision[id], .cursor = cursor_revision[id],
		.rect = frame->rect, .valid = true, .overlay_cursor = frame->overlay_cursor,
		.cursor_box = frame->cursor,
	};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	profile_capture(frame->path, &frame->start, &ts);
	uint64_t seconds = ts.tv_sec;
	zwlr_screencopy_frame_v1_send_ready(frame->resource, seconds >> 32, seconds, ts.tv_nsec);
}

static int
handle_frame_fence(int fd, uint32_t mask, void *data)
{
	(void)fd; (void)mask;
	frame_complete(data);
	return 0;
}

static void
frame_copy(struct screencopy_frame *frame, struct wl_resource *buffer_resource,
           bool with_damage, bool post_error)
{
	struct capture_target target;
	struct timespec start = {0};
	int fence = -1;
	/* Still on its way from the last copy. */
	if (frame->copying) return;
	if (profile_enabled) clock_gettime(CLOCK_MONOTONIC, &start);
	if (!frame_check_buffer(frame, buffer_resource, &target, post_error)) return;
	if (!swc.active || !frame->rect.width || !frame->rect.height ||
	    (uint64_t)frame->rect.x + frame->rect.width > frame->screen->base.geometry.width ||
	    (uint64_t)frame->rect.y + frame->rect.height > frame->screen->base.geometry.height) {
		frame_fail(frame);
		return;
	}
	uint32_t id = frame->screen->id;
	struct buffer_state *state = buffer_state_find(buffer_resource);
	pixman_box32_t cursor = {0, 0, 0, 0};
	if (frame->overlay_cursor) cursor = cursor_box(frame->screen);
	pixman_region32_t region;
	pixman_region32_init(&region);
	frame_copy_region(frame, state, with_damage, cursor, &region);

	enum capture_path path = CAPTURE_DMABUF;
	bool copied = true;
	/* Nothing to copy is a copy that cannot fail. */
	if (pixman_region32_not_empty(&region))
		copied = target.dmabuf ? copy_dmabuf(frame, target.dmabuf, &region, &fence) :
		                         copy_shm(frame, &target.shm, &region, &path);
	pixman_region32_fini(&region);
	if (!copied) {
		/* Whatever the buffer holds now, it is not a frame. */
		if (fence >= 0) close(fence);
		if (state) buffer_state_destroy(state);
		if (target.dmabuf && !force_shm) {
			force_shm = true;
			WARNING("DMA-BUF capture target could not be rendered; offering SHM on subsequent frames\n");
		}
		frame_fail(frame);
		return;
	}
	if (!state && (state = calloc(1, sizeof(*state)))) {
		state->buffer = buffer_resource;
		state->destroy_listener.notify = &handle_buffer_state_destroy;
		wl_resource_add_destroy_listener(buffer_resource, &state->destroy_listener);
		wl_list_insert(&buffer_states, &state->link);
	}
	if (state) {
		state->screen_id = id;
		state->rect = frame->rect;
		state->overlay_cursor = frame->overlay_cursor;
		state->image = image_revision[id];
		state->cursor_box = cursor;
	}

	frame->with_damage = with_damage;
	frame->cursor = cursor;
	frame->path = path;
	frame->start = start;

	/* The GPU may still be copying. Nearly always it is already done, and
	 * the frame completes here; otherwise it completes from the loop. */
	if (fence >= 0) {
		struct pollfd pollfd = { .fd = fence, .events = POLLIN };
		if (poll(&pollfd, 1, 0) == 0) {
			frame->fence_source = wl_event_loop_add_fd(
			    swc.event_loop, fence, WL_EVENT_READABLE, &handle_frame_fence, frame);
			if (frame->fence_source) {
				frame->fence_fd = fence;
				frame->copying = true;
				return;
			}
		}
		close(fence);
	}
	frame_complete(frame);
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

	frame_copy(frame, buffer_resource, false, true);
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

	frame_fence_forget(frame);
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
	frame->copying = false;
	frame->fence_fd = -1;
	frame->fence_source = NULL;
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
		frame_copy(frame, frame->buffer, true, false);
	}
	return 0;
}

void
screencopy_handle_damage(struct screen *screen, pixman_region32_t *global_damage)
{
	struct screencopy_frame *frame, *tmp;
	const struct swc_rectangle *geom = &screen->base.geometry;
	pixman_region32_t local;

	if (!pixman_region32_not_empty(global_damage)) return;
	image_revision[screen->id] = ++revision;

	pixman_region32_init(&local);
	pixman_region32_intersect_rect(&local, global_damage, geom->x, geom->y, geom->width,
	                               geom->height);
	pixman_region32_translate(&local, -geom->x, -geom->y);
	damage_history_add(screen, image_revision[screen->id], &local);

	wl_list_for_each_safe (frame, tmp, &pending, link) {
		if (frame->screen != screen) continue;
		pixman_region32_t damage;

		pixman_region32_init(&damage);
		pixman_region32_intersect_rect(&damage, &local, frame->rect.x, frame->rect.y,
		                               frame->rect.width, frame->rect.height);
		if (pixman_region32_not_empty(&damage))
			frame_copy(frame, frame->buffer, true, false);
		pixman_region32_fini(&damage);
	}
	pixman_region32_fini(&local);
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
