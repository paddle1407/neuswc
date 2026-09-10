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
#include <wayland-server.h>
#include <wld/wld.h>

/* Frames queued by copy_with_damage, waiting for the screen to be damaged. */
static struct wl_list pending = {&pending, &pending};

struct screencopy_frame {
	struct wl_resource *resource;
	struct screen *screen;

	/* Capture region, in screen-local coordinates. */
	struct swc_rectangle rect;
	bool overlay_cursor;

	/* Set once copy or copy_with_damage has been requested. */
	bool used;

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
                   struct swc_shm_buffer_info *info, bool post_error)
{
	uint32_t row_bytes = frame->rect.width * 4;

	if (!shm_buffer_get_info(buffer_resource, info))
		goto invalid;
	if (info->format != WL_SHM_FORMAT_XRGB8888 && info->format != WL_SHM_FORMAT_ARGB8888)
		goto invalid;
	if ((uint32_t)info->width != frame->rect.width || (uint32_t)info->height != frame->rect.height)
		goto invalid;
	if ((uint32_t)info->stride < row_bytes || !info->writable)
		goto invalid;

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

static void
frame_send_damage(struct screencopy_frame *frame, pixman_region32_t *damage)
{
	pixman_region32_t clipped;
	pixman_box32_t *boxes;
	int i, count;

	if (wl_resource_get_version(frame->resource) < ZWLR_SCREENCOPY_FRAME_V1_DAMAGE_SINCE_VERSION)
		return;

	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage, frame->rect.x, frame->rect.y,
	                               frame->rect.width, frame->rect.height);
	pixman_region32_translate(&clipped, -frame->rect.x, -frame->rect.y);

	boxes = pixman_region32_rectangles(&clipped, &count);
	for (i = 0; i < count; i++) {
		zwlr_screencopy_frame_v1_send_damage(frame->resource, boxes[i].x1, boxes[i].y1,
		                                     boxes[i].x2 - boxes[i].x1,
		                                     boxes[i].y2 - boxes[i].y1);
	}

	pixman_region32_fini(&clipped);
}

/**
 * Render the screen and copy the frame's region into the client's buffer.
 *
 * 'damage' is in screen-local coordinates, or NULL for a plain copy.
 */
static void
frame_copy(struct screencopy_frame *frame, struct wl_resource *buffer_resource,
           pixman_region32_t *damage, bool post_error)
{
	struct swc_shm_buffer_info target;
	struct wld_buffer *source;
	uint8_t *src_pixels, *dst_pixels;
	uint32_t row_bytes = frame->rect.width * 4;
	struct timespec ts;
	uint64_t tv_sec;

	if (!frame_check_buffer(frame, buffer_resource, &target, post_error))
		return;

	source = compositor_render_to_shm(frame->screen);
	if (!source) {
		frame_fail(frame);
		return;
	}

	if (!wld_map(source) || !source->map) {
		wld_buffer_unreference(source);
		frame_fail(frame);
		return;
	}

	src_pixels = source->map;

	if (frame->overlay_cursor) {
		snap_overlay_cursor(src_pixels, source->width, source->height, source->pitch,
		                    frame->screen);
	}

	dst_pixels = target.data;
	for (uint32_t y = 0; y < frame->rect.height; y++) {
		memcpy(dst_pixels + (size_t)y * target.stride,
		       src_pixels + (size_t)(y + frame->rect.y) * source->pitch
		           + (size_t)frame->rect.x * 4,
		       row_bytes);
	}

	wld_unmap(source);
	wld_buffer_unreference(source);

	frame_dequeue(frame);

	/* compositor_render_to_shm() renders top-down, so no y_invert. */
	zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);

	if (damage)
		frame_send_damage(frame, damage);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	tv_sec = (uint64_t)ts.tv_sec;
	zwlr_screencopy_frame_v1_send_ready(frame->resource, (uint32_t)(tv_sec >> 32),
	                                    (uint32_t)tv_sec, (uint32_t)ts.tv_nsec);
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
	struct swc_shm_buffer_info info;

	if (!frame_claim(frame))
		return;

	/* Validate now so that a bad buffer is reported at request time. */
	if (!frame_check_buffer(frame, buffer_resource, &info, true))
		return;

	frame->buffer = buffer_resource;
	frame->buffer_destroy.notify = &handle_buffer_destroy;
	wl_resource_add_destroy_listener(buffer_resource, &frame->buffer_destroy);
	wl_list_insert(&pending, &frame->link);
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
	if (frame->screen)
		wl_list_remove(&frame->screen_destroy.link);
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
	int32_t x1, y1, x2, y2;
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
	frame->screen = output && output->screen ? output->screen : NULL;
	frame->overlay_cursor = overlay_cursor != 0;
	frame->used = false;
	frame->buffer = NULL;
	frame->rect = (struct swc_rectangle){0, 0, 0, 0};

	if (!frame->screen) {
		zwlr_screencopy_frame_v1_send_failed(resource);
		return;
	}

	frame->screen_destroy.notify = &handle_screen_destroy;
	wl_signal_add(&frame->screen->destroy_signal, &frame->screen_destroy);

	geom = &frame->screen->base.geometry;

	if (region) {
		x1 = MAX(region->x, 0);
		y1 = MAX(region->y, 0);
		x2 = MIN(region->x + (int32_t)region->width, (int32_t)geom->width);
		y2 = MIN(region->y + (int32_t)region->height, (int32_t)geom->height);
	} else {
		x1 = 0;
		y1 = 0;
		x2 = geom->width;
		y2 = geom->height;
	}

	if (x2 <= x1 || y2 <= y1) {
		zwlr_screencopy_frame_v1_send_failed(resource);
		return;
	}

	frame->rect = (struct swc_rectangle){x1, y1, (uint32_t)(x2 - x1), (uint32_t)(y2 - y1)};

	zwlr_screencopy_frame_v1_send_buffer(resource, WL_SHM_FORMAT_XRGB8888, frame->rect.width,
	                                     frame->rect.height, frame->rect.width * 4);

	/* No linux_dmabuf event: neuswc has no dmabuf capture path, so clients
	 * fall back to wl_shm. */
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
bind_screencopy(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
screencopy_handle_damage(pixman_region32_t *global_damage)
{
	struct screencopy_frame *frame, *tmp;
	pixman_region32_t damage;

	if (wl_list_empty(&pending))
		return;

	wl_list_for_each_safe (frame, tmp, &pending, link) {
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
	return wl_global_create(display, &zwlr_screencopy_manager_v1_interface, 3, NULL,
	                        &bind_screencopy);
}
