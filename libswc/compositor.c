/* swc: libswc/compositor.c
 *
 * Copyright (c) 2013-2020 Michael Forney
 *
 * Based in part upon compositor.c from weston, which is:
 *
 *     Copyright © 2010-2011 Intel Corporation
 *     Copyright © 2008-2011 Kristian Høgsberg
 *     Copyright © 2012 Collabora, Ltd.
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

#include "compositor.h"
#include "backend.h"
#include "data_device_manager.h"
#include "decor.h"
#include "titlebar.h"
#include <unistd.h>
#include <linux/input-event-codes.h>
#ifdef ENABLE_DRM
#include "drm.h"
#endif
#include "event.h"
#include "foreign_toplevel.h"
#include "internal.h"
#include "layer_shell.h"
#include "launch.h"
#include "output.h"
#include "pointer.h"
#include "region.h"
#include "screen.h"
#include "screencopy.h"
#include "seat.h"
#include "session_lock.h"
#include "shm.h"
#include "subsurface.h"
#include "surface.h"
#include "swc.h"
#include "util.h"
#include "view.h"
#include "window.h"
#include "wallpaper.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Enabled by the existing input diagnostic flag as well, so one run covers
 * time spent between input events, not just inside the pointer handler. */
static bool profile_render;
static uint64_t compositor_started;
static uint64_t frame_draw, frame_finish;

static uint64_t monotonic_us(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void report_frame(struct screen *screen, uint64_t total,
                         uint64_t submit, uint64_t capture)
{
	static uint64_t last_report;
	static uint64_t totals[5], worst;
	static unsigned count;
	uint64_t now = monotonic_us();
	uint64_t sample[] = {total, frame_draw, frame_finish, submit, capture};
	for (unsigned i = 0; i < 5; ++i) totals[i] += sample[i];
	worst = MAX(worst, total);
	++count;
	if (total >= 25000 && now - last_report >= 1000000) {
		fprintf(stderr, "render-profile: slow output %u: total %.3f ms, draw %.3f ms, GPU finish %.3f ms, submit %.3f ms, capture %.3f ms\n",
		        screen->id, total / 1000.0, frame_draw / 1000.0,
		        frame_finish / 1000.0, submit / 1000.0, capture / 1000.0);
		last_report = now;
	}
	if (count == 60) {
		fprintf(stderr, "render-profile: 60 output frames, average total %.3f ms (draw %.3f, GPU finish %.3f, submit %.3f, capture %.3f), worst %.3f ms\n",
		        totals[0] / 60000.0, totals[1] / 60000.0, totals[2] / 60000.0,
		        totals[3] / 60000.0, totals[4] / 60000.0, worst / 1000.0);
		memset(totals, 0, sizeof(totals));
		count = 0; worst = 0;
	}
}
#ifdef ENABLE_DRM
#include <wld/drm.h>
#endif
#include <wld/wld.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static inline int32_t
clamp_i32(int64_t v)
{
	if (v > INT32_MAX) {
		return INT32_MAX;
	}
	if (v < INT32_MIN) {
		return INT32_MIN;
	}
	return (int32_t)v;
}

static inline uint32_t
span_u32(int32_t a, int32_t b)
{
	int64_t d = (int64_t)b - (int64_t)a;

	if (d <= 0) {
		return 0;
	}
	if (d > UINT32_MAX) {
		return UINT32_MAX;
	}
	return (uint32_t)d;
}

struct target {
	bool first_frame_presented;
	/* Set while the last attempted flip/modeset for this screen failed, so a
	 * persistently failing output retries once rather than spinning on idle. */
	bool swap_failed;
	struct wld_surface *surface;
	struct wld_buffer *next_buffer, *current_buffer;
	struct view *view;
	struct view_handler view_handler;
	uint32_t mask;

	struct wl_listener screen_destroy_listener;
};

static bool
handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t x,
              wl_fixed_t y);
static bool
handle_button(struct pointer_handler *handler, uint32_t time,
              struct button *button, uint32_t state);
static void
perform_update(void *data);

static struct pointer_handler pointer_handler = {
    .motion = handle_motion,
    .button = handle_button,
};

static struct {
	struct wl_list views;
	pixman_region32_t damage, opaque;
	struct wl_listener swc_listener;

	/* A mask of screens that have been repainted but are waiting on a page
	 * flip. */
	uint32_t pending_flips;

	/* A mask of screens that are scheduled to be repainted on the next idle. */
	uint32_t scheduled_updates;

	/* A mask of screens whose last frame was never presented because the
	 * mode set or page flip failed, and which need another update. */
	uint32_t recover_updates;

	bool updating;
	struct wl_global *global;
	bool initialized;

	/* zoom level (1.0 = normal, >1 = zoomed in, <1 = zoomed out) */
	float zoom;
	struct swc_screen *overview_screen;
	struct swc_overview_item *overview_items;
	unsigned overview_count;
} compositor;

/* The static handler retains ownership of a claimed button even if the window
 * disappears before release. Hide/destroy clears both view references. */
static struct {
	struct compositor_view *hover, *pressed;
	int button;
	int32_t offset_x, offset_y;
	bool left_down;
	/* The press landed on the draggable part of the bar and the window is
	 * being moved, rather than one of its buttons being held. */
	bool moving;
} bar_grab;

static void
bar_move_notify(struct compositor_view *view, bool active)
{
	struct window *window = view ? view->window : NULL;

	if (window && window->handler && window->handler->interactive_move)
		window->handler->interactive_move(window->handler_data, active);
}

/*
 * keep_drag: a window being dragged by its titlebar must survive being
 * redecorated. Anything that re-applies a decoration mid-drag -- a title
 * change, the window manager repainting the bar because focus moved to
 * another monitor -- used to drop the grab here, which stranded the window
 * halfway through the move with the button still held. A held *button* is
 * dropped either way: the decoration that replaces it may not have that
 * button at all.
 */
static void
bar_forget(struct compositor_view *view, bool keep_drag)
{
	bool dragging = keep_drag && bar_grab.moving && bar_grab.pressed == view;
	bool ended = false;

	if (bar_grab.hover == view) bar_grab.hover = NULL;
	if (bar_grab.pressed == view && !dragging) {
		ended = bar_grab.moving;
		bar_grab.moving = false;
		bar_grab.pressed = NULL;
	}
	titlebar_highlight(view, -1, -1);
	/* Last: the window manager may hide or move windows from here, and must
	 * not find a grab still pointing at a view it is finishing with. */
	if (ended)
		bar_move_notify(view, false);
}

static struct {
	bool active;
	int32_t x, y;
	uint32_t width, height;
	uint32_t color;
	uint32_t border_width;
} overlay;

struct swc_compositor swc_compositor = {
    .pointer_handler = &pointer_handler,
};

static void
handle_screen_destroy(struct wl_listener *listener, void *data)
{
	struct target *target =
	    wl_container_of(listener, target, screen_destroy_listener);

	if (compositor.overview_screen && screen_mask((struct screen *)compositor.overview_screen) == target->mask) {
		input_mode_cancel();
		swc_overview_end();
	}
	wl_list_remove(&target->view_handler.link);
	wl_list_remove(&target->screen_destroy_listener.link);
	wld_destroy_surface(target->surface);
	free(target);
}

static struct target *
target_get(struct screen *screen)
{
	struct wl_listener *listener =
	    wl_signal_get(&screen->destroy_signal, &handle_screen_destroy);
	struct target *target;

	return listener ? wl_container_of(listener, target, screen_destroy_listener)
	                : NULL;
}

static void
handle_screen_frame(struct view_handler *handler, uint32_t time)
{
	struct target *target = wl_container_of(handler, target, view_handler);
	struct compositor_view *view;
	if (!target->first_frame_presented) {
		fprintf(stderr, "startup: output mask 0x%x first frame presented %.3f ms after compositor initialization began\n",
		        target->mask, (monotonic_us() - compositor_started) / 1000.0);
		target->first_frame_presented = true;
	}

	compositor.pending_flips &= ~target->mask;

	/* Only the surface answers a frame event, and only with the callbacks
	 * it has queued, so skip the handler walk for views with none. */
	wl_list_for_each(view, &compositor.views, link)
	{
		if (view->visible && view->base.screens & target->mask &&
		    !wl_list_empty(&view->surface->state.frame_callbacks)) {
			view_frame(&view->base, time);
		}
	}

	if (target->current_buffer) {
		wld_surface_release(target->surface, target->current_buffer);
	}

	target->current_buffer = target->next_buffer;

	/* If we had scheduled updates that couldn't run because we were waiting on
	 * a page flip, run them now. If the compositor is currently updating, then
	 * the frame finished immediately, and we can be sure that there are no
	 * pending updates. */
	if (compositor.scheduled_updates && !compositor.updating) {
		perform_update(NULL);
	}
}

static const struct view_handler_impl screen_view_handler = {
    .frame = handle_screen_frame,
};

static int
target_swap_buffers(struct target *target)
{
	struct wld_buffer *buffer;
	int ret;

	if (!(buffer = wld_surface_take(target->surface))) {
		return -ENOMEM;
	}

	if ((ret = view_attach(target->view, buffer)) < 0) {
		/*
		 * view_attach() only takes a reference once the attach succeeded, and
		 * only a completed page flip releases a taken buffer. Hand the buffer
		 * back to the surface, or it stays busy forever and the pool grows by
		 * one scanout buffer for every failure.
		 */
		wld_surface_release(target->surface, buffer);
		return ret;
	}

	target->next_buffer = buffer;
	return 0;
}

/*
 * Restore full damage for a screen whose frame was never presented.
 *
 * calculate_damage() has already consumed the client surface damage and
 * perform_update() clears compositor.damage, while wld_surface_take() cleared
 * the damage recorded on the buffer that was rendered into. Without this the
 * content of the dropped frame is lost until something else happens to damage
 * the same region.
 */
static void
target_restore_damage(struct target *target, const struct swc_rectangle *geom)
{
	pixman_region32_t full;

	pixman_region32_init_rect(&full, 0, 0, geom->width, geom->height);
	wld_surface_damage(target->surface, &full);
	pixman_region32_fini(&full);
}

static struct target *
target_new(struct screen *screen)
{
	struct target *target;
	struct swc_rectangle *geom = &screen->base.geometry;

	if (!(target = malloc(sizeof(*target)))) {
		goto error0;
	}
	target->first_frame_presented = false;
	target->swap_failed = false;

	target->surface =
	    wld_create_surface(swc.backend->context, geom->width, geom->height,
	                       WLD_FORMAT_XRGB8888,
#ifdef ENABLE_DRM
	                       WLD_DRM_FLAG_SCANOUT
#else
	                       WLD_FLAG_MAP
#endif
	    );

	if (!target->surface) {
		goto error1;
	}

	target->view = &screen->planes.primary.view;
	target->view_handler.impl = &screen_view_handler;
	wl_list_insert(&target->view->handlers, &target->view_handler.link);
	target->current_buffer = NULL;
	target->next_buffer = NULL;
	target->mask = screen_mask(screen);

	target->screen_destroy_listener.notify = &handle_screen_destroy;
	wl_signal_add(&screen->destroy_signal, &target->screen_destroy_listener);

	return target;

error1:
	free(target);
error0:
	return NULL;
}

/* Rendering {{{ */

/* Borders enclose the content and built-in titlebar as one frame. Client
 * buffer coordinates remain relative to the original content geometry. */
static struct swc_rectangle
frame_geometry(const struct compositor_view *view)
{
	struct swc_rectangle frame = view->base.geometry;
	if (view->decor.titlebar.enabled) {
		frame.y -= view->decor.top;
		frame.height += view->decor.top;
	}
	return frame;
}

static void
init_grown_rect(pixman_region32_t *region, const struct swc_rectangle *r,
                int64_t by)
{
	int32_t x1 = clamp_i32((int64_t)r->x - by), y1 = clamp_i32((int64_t)r->y - by);
	int32_t x2 = clamp_i32((int64_t)r->x + r->width + by);
	int32_t y2 = clamp_i32((int64_t)r->y + r->height + by);

	pixman_region32_init_rect(region, x1, y1, span_u32(x1, x2), span_u32(y1, y2));
}

/*
 * A border is two rings around the frame: the inner one is the frame grown by
 * the inner width, and the outer one is what the outer width adds around that.
 * Fill the parts of them inside damage, which is in global coordinates.
 */
static void
fill_border(struct wld_renderer *renderer, const struct swc_rectangle *target_geom,
            const struct compositor_view *view, pixman_region32_t *damage)
{
	const struct swc_rectangle frame = frame_geometry(view);
	uint32_t in = view->border.inwidth, out = view->border.outwidth;
	pixman_region32_t ring, hole;

	if (!in && !out)
		return;

	pixman_region32_init(&ring);
	if (out) {
		init_grown_rect(&hole, &frame, (int64_t)out + in);
		pixman_region32_intersect(&ring, &hole, damage);
		pixman_region32_fini(&hole);
		init_grown_rect(&hole, &frame, in);
		pixman_region32_subtract(&ring, &ring, &hole);
		pixman_region32_fini(&hole);
		if (pixman_region32_not_empty(&ring)) {
			pixman_region32_translate(&ring, -target_geom->x, -target_geom->y);
			wld_fill_region(renderer, view->border.outcolor, &ring);
		}
	}
	if (in) {
		init_grown_rect(&hole, &frame, in);
		pixman_region32_intersect(&ring, &hole, damage);
		pixman_region32_fini(&hole);
		init_grown_rect(&hole, &frame, 0);
		pixman_region32_subtract(&ring, &ring, &hole);
		pixman_region32_fini(&hole);
		if (pixman_region32_not_empty(&ring)) {
			pixman_region32_translate(&ring, -target_geom->x, -target_geom->y);
			wld_fill_region(renderer, view->border.incolor, &ring);
		}
	}
	pixman_region32_fini(&ring);
}

/*
 * Draw the part of src, a view's buffer, inside damage (global coordinates).
 * A window shows only its geometry; the rest of its buffer is the client's
 * shadow margin. Anything else shows its whole buffer. Nothing is read from
 * outside the client's buffer, which a proxy may be larger than.
 */
static void
draw_view_buffer(struct wld_renderer *renderer,
                 const struct swc_rectangle *target_geom,
                 struct compositor_view *view, struct wld_buffer *src,
                 pixman_region32_t *damage)
{
	const struct swc_rectangle *geom = &view->base.geometry;
	int32_t buf_x = geom->x - view->buffer_offset_x;
	int32_t buf_y = geom->y - view->buffer_offset_y;
	int32_t dst_x = buf_x - target_geom->x, dst_y = buf_y - target_geom->y;
	int64_t x1 = buf_x, y1 = buf_y;
	int64_t x2 = x1 + view->base.buffer->width;
	int64_t y2 = y1 + view->base.buffer->height;
	pixman_region32_t region, opaque;

	if (view->window) {
		x1 = MAX(x1, geom->x);
		y1 = MAX(y1, geom->y);
		x2 = MIN(x2, (int64_t)geom->x + geom->width);
		y2 = MIN(y2, (int64_t)geom->y + geom->height);
	}
	pixman_region32_init(&region);
	pixman_region32_intersect_rect(&region, damage, clamp_i32(x1), clamp_i32(y1),
	                               span_u32(clamp_i32(x1), clamp_i32(x2)),
	                               span_u32(clamp_i32(y1), clamp_i32(y2)));
	if (!pixman_region32_not_empty(&region)) {
		pixman_region32_fini(&region);
		return;
	}
	pixman_region32_translate(&region, -buf_x, -buf_y);

	if (src->format != WLD_FORMAT_ARGB8888) {
		wld_copy_region(renderer, src, dst_x, dst_y, &region);
		pixman_region32_fini(&region);
		return;
	}

	/* Trust the committed opaque region. Scanning a whole client buffer to
	 * discover opacity can force costly GPU mappings; blending the remaining
	 * pixels is correct even when opaque. */
	pixman_region32_init(&opaque);
	pixman_region32_intersect(&opaque, &region, &view->surface->state.opaque);
	if (pixman_region32_not_empty(&opaque)) {
		wld_copy_region(renderer, src, dst_x, dst_y, &opaque);
		pixman_region32_subtract(&region, &region, &opaque);
	}
	if (pixman_region32_not_empty(&region))
		wld_blend_region(renderer, src, dst_x, dst_y, &region);
	pixman_region32_fini(&opaque);
	pixman_region32_fini(&region);
}

/*
 * Draw a view -- buffer, border and decorations -- where it meets damage (in
 * global coordinates) into whatever target the renderer already has. src is
 * the buffer to draw, or NULL if the renderer cannot read one. clip, if given,
 * is the part covered by opaque views above, which paint it themselves.
 *
 * The live frame and a capture both come through here, so a screenshot shows
 * what the screen does.
 */
static void
paint_view(struct wld_renderer *renderer, const struct swc_rectangle *target_geom,
           struct compositor_view *view, struct wld_buffer *src,
           pixman_region32_t *damage, pixman_region32_t *clip)
{
	pixman_region32_t view_damage;

	if (!view->base.buffer) {
		return;
	}

	/*
	 * Everything drawn below lies inside the extents. A view the damage
	 * misses, or one wholly covered by opaque views above, draws nothing, and
	 * neither do its decorations, which clip themselves the same way.
	 */
	if (pixman_region32_contains_rectangle(damage, &view->extents) ==
	        PIXMAN_REGION_OUT ||
	    (clip && pixman_region32_contains_rectangle(clip, &view->extents) ==
	        PIXMAN_REGION_IN)) {
		return;
	}

	pixman_region32_init(&view_damage);
	pixman_region32_intersect_rect(&view_damage, damage, view->extents.x1,
	                               view->extents.y1,
	                               span_u32(view->extents.x1, view->extents.x2),
	                               span_u32(view->extents.y1, view->extents.y2));
	if (clip) {
		pixman_region32_subtract(&view_damage, &view_damage, clip);
	}

	if (src) {
		draw_view_buffer(renderer, target_geom, view, src, &view_damage);
	}
	fill_border(renderer, target_geom, view, &view_damage);
	pixman_region32_fini(&view_damage);

	if (view->decor.top || view->decor.right || view->decor.bottom ||
	    view->decor.left) {
		decor_repaint(renderer, target_geom, view, damage);
	}
}

static void
repaint_view(struct target *target, struct compositor_view *view,
             pixman_region32_t *damage)
{
	paint_view(swc.backend->renderer, &target->view->geometry, view,
	           view->buffer, damage, &view->clip);
}

static void
draw_overlays(struct wld_renderer *renderer, const struct swc_rectangle *target_geom)
{
	int32_t tx = (int32_t)target_geom->width;
	int32_t ty = (int32_t)target_geom->height;

/* draw box as 4 rectangles with wld */
#define CLAMP_LOW(v, lo) ((v) < (lo) ? (lo) : (v))
#define CLAMP_HIGH(v, hi) ((v) > (hi) ? (hi) : (v))
#define DRAW_CLIPPED(rx, ry, rw, rh, clr)                                     \
	do {                                                                       \
		int32_t _x1 = CLAMP_LOW((rx), 0);                                      \
		int32_t _y1 = CLAMP_LOW((ry), 0);                                      \
		int32_t _x2 = CLAMP_HIGH((rx) + (int32_t)(rw), tx);                    \
		int32_t _y2 = CLAMP_HIGH((ry) + (int32_t)(rh), ty);                    \
		if (_x2 > _x1 && _y2 > _y1)                                            \
			wld_fill_rectangle(renderer, (clr), _x1, _y1,                      \
			                   (uint32_t)(_x2 - _x1), (uint32_t)(_y2 - _y1)); \
	} while (0)

	if (overlay.active && overlay.border_width > 0) {
		int32_t x = overlay.x - target_geom->x;
		int32_t y = overlay.y - target_geom->y;
		uint32_t w = overlay.width, h = overlay.height,
		         bw = overlay.border_width;

		if (w > 0 && h > 0) {
			if (bw > w) {
				bw = w;
			}
			if (bw > h) {
				bw = h;
			}

			DRAW_CLIPPED(x, y, (int32_t)w, (int32_t)bw, overlay.color);
			DRAW_CLIPPED(x, y + (int32_t)h - (int32_t)bw, (int32_t)w,
			             (int32_t)bw, overlay.color);
			DRAW_CLIPPED(x, y, (int32_t)bw, (int32_t)h, overlay.color);
			DRAW_CLIPPED(x + (int32_t)w - (int32_t)bw, y, (int32_t)bw,
			             (int32_t)h, overlay.color);
		}
	}

#undef DRAW_CLIPPED
#undef CLAMP_HIGH
#undef CLAMP_LOW
}

static void
renderer_repaint(struct target *target, pixman_region32_t *damage,
                 pixman_region32_t *base_damage, struct wl_list *views,
                 struct screen *screen)
{
	struct compositor_view *view;
	const struct swc_rectangle *target_geom = &target->view->geometry;
	uint64_t start = profile_render ? monotonic_us() : 0;

	DEBUG("Rendering to target { x: %d, y: %d, w: %u, h: %u }\n",
	      target->view->geometry.x, target->view->geometry.y,
	      target->view->geometry.width, target->view->geometry.height);

	wld_set_target_surface(swc.backend->renderer, target->surface);

	if (pixman_region32_not_empty(base_damage)) {
		pixman_region32_translate(base_damage, -target->view->geometry.x,
		                          -target->view->geometry.y);
		wallpaper_repaint(screen, swc.backend->renderer, base_damage);
	}

	wl_list_for_each_reverse(view, views, link)
	{
		if (view->visible && view->base.screens & target->mask) {
			repaint_view(target, view, damage);
		}
	}

	draw_overlays(swc.backend->renderer, target_geom);

	uint64_t drawn = profile_render ? monotonic_us() : 0;
	wld_flush(swc.backend->renderer);
	if (profile_render) {
		frame_draw = drawn - start;
		frame_finish = monotonic_us() - drawn;
	}
}

static int
renderer_attach(struct compositor_view *view, struct wld_buffer *client_buffer)
{
	struct wld_buffer *buffer;
	uint32_t proxy_width, proxy_height;
	bool was_proxy = view->buffer != view->base.buffer;
	bool needs_proxy =
	    client_buffer && !(wld_capabilities(swc.backend->renderer, client_buffer) &
	                       WLD_CAPABILITY_READ);
	bool proxy_incompatible =
	    view->buffer && client_buffer &&
	    (view->buffer->format != client_buffer->format ||
	     view->buffer->width < client_buffer->width ||
	     view->buffer->height < client_buffer->height);

	if (client_buffer) {
		/* Create a proxy buffer if necessary (for example a hardware buffer
		 * backing a SHM buffer). */
		if (needs_proxy) {
			if (!was_proxy || proxy_incompatible) {
				DEBUG("Creating a proxy buffer\n");
				proxy_width = client_buffer->width;
				proxy_height = client_buffer->height;
				if (proxy_width <= UINT32_MAX - 255) {
					proxy_width = (proxy_width + 255) & ~255U;
				}
				if (proxy_height <= UINT32_MAX - 255) {
					proxy_height = (proxy_height + 255) & ~255U;
				}
				buffer = wld_create_buffer(
				    swc.backend->context, proxy_width, proxy_height,
				    client_buffer->format, WLD_FLAG_MAP);

				if (!buffer) {
					return -ENOMEM;
				}
				view->proxy_dirty = true;
			} else {
				/* Otherwise we can keep the original proxy buffer. */
				buffer = view->buffer;
			}
		} else {
			buffer = client_buffer;
		}
	} else {
		buffer = NULL;
	}

	/* If we no longer need a proxy buffer, or the original buffer is of a
	 * different size, destroy the old proxy image. */
	if (view->buffer &&
	    ((!needs_proxy && was_proxy) ||
	     (needs_proxy && was_proxy && proxy_incompatible))) {
		wld_buffer_unreference(view->buffer);
	}

	view->buffer = buffer;
	if (!needs_proxy) view->proxy_dirty = false;

	return 0;
}

static void
renderer_flush_view(struct compositor_view *view)
{
	/*
	 * view->buffer differs from base.buffer only when it is a proxy, and
	 * renderer_attach only creates one from a non-NULL client buffer, which
	 * view_attach then stores as base.buffer. So base.buffer is non-NULL
	 * below; detaching clears both and takes the early return.
	 */
	if (view->buffer == view->base.buffer) {
		return;
	}

	wld_set_target_buffer(swc.shm->renderer, view->buffer);

	/*
	 * A new proxy starts empty, so copying only the damaged region leaves it
	 * blank when a client attaches its first buffer without posting damage --
	 * which is allowed, and which wfreeze does. Copy the whole buffer once.
	 */
	if (view->proxy_dirty) {
		pixman_region32_t full;

		pixman_region32_init_rect(&full, 0, 0, view->base.buffer->width,
		                          view->base.buffer->height);
		wld_copy_region(swc.shm->renderer, view->base.buffer, 0, 0, &full);
		pixman_region32_fini(&full);
		view->proxy_dirty = false;
	} else {
		wld_copy_region(swc.shm->renderer, view->base.buffer, 0, 0,
		                &view->surface->state.damage);
	}

	wld_flush(swc.shm->renderer);
}

/* }}} */

/* Surface Views {{{ */

/**
 * Adds the region below a view to the compositor's damaged region.
 */
static void
damage_below_view(struct compositor_view *view)
{
	pixman_region32_t damage_below;

	pixman_region32_init_with_extents(&damage_below, &view->extents);
	pixman_region32_union(&compositor.damage, &compositor.damage,
	                      &damage_below);
	pixman_region32_fini(&damage_below);
}

/**
 * Completely damages the surface and its border.
 */
static void
damage_view(struct compositor_view *view)
{
	damage_below_view(view);
	view->border.damaged_border1 = true;
	view->border.damaged_border2 = true;
}

static void
update_extents_for_buffer(struct compositor_view *view, struct wld_buffer *buffer)
{
	int64_t total_border =
	    (int64_t)view->border.outwidth + (int64_t)view->border.inwidth;
	int64_t geom_x = view->base.geometry.x;
	int64_t geom_y = view->base.geometry.y;
	int64_t geom_w = view->base.geometry.width;
	int64_t geom_h = view->base.geometry.height;

	const struct swc_rectangle frame = frame_geometry(view);
	int64_t border_x1 = (int64_t)frame.x - total_border;
	int64_t border_y1 = (int64_t)frame.y - total_border;
	int64_t border_x2 = (int64_t)frame.x + frame.width + total_border;
	int64_t border_y2 = (int64_t)frame.y + frame.height + total_border;
	int64_t decor_x1 = geom_x - (int64_t)view->decor.left;
	int64_t decor_y1 = geom_y - (int64_t)view->decor.top;
	int64_t decor_x2 = geom_x + geom_w + (int64_t)view->decor.right;
	int64_t decor_y2 = geom_y + geom_h + (int64_t)view->decor.bottom;

	int64_t buffer_x1 = geom_x - view->buffer_offset_x;
	int64_t buffer_y1 = geom_y - view->buffer_offset_y;
	int64_t buffer_x2 =
	    buffer_x1 +
	    (buffer ? buffer->width : (uint32_t)geom_w);
	int64_t buffer_y2 =
	    buffer_y1 +
	    (buffer ? buffer->height : (uint32_t)geom_h);

	view->extents.x1 = clamp_i32(MIN(MIN(border_x1, decor_x1), buffer_x1));
	view->extents.y1 = clamp_i32(MIN(MIN(border_y1, decor_y1), buffer_y1));
	view->extents.x2 = clamp_i32(MAX(MAX(border_x2, decor_x2), buffer_x2));
	view->extents.y2 = clamp_i32(MAX(MAX(border_y2, decor_y2), buffer_y2));

	if (view->extents.x2 < view->extents.x1) {
		view->extents.x2 = view->extents.x1;
	}
	if (view->extents.y2 < view->extents.y1) {
		view->extents.y2 = view->extents.y1;
	}

	/* Damage border. */
	view->border.damaged_border1 = true;
	view->border.damaged_border2 = true;
	view->decor.damaged = true;
}

static void
update_extents(struct compositor_view *view)
{
	update_extents_for_buffer(view, view->base.buffer);
}

/*
 * Borders and titlebars are painted outside the content rectangle, so a window
 * pushed off the bottom of a screen can have nothing left on it but its
 * titlebar. A screen mask taken from the content geometry alone is empty for
 * such a window, and an empty mask drops it from that screen's frame entirely,
 * decoration included: the titlebar simply stopped being drawn. The extents are
 * what actually gets painted, so they are what decides which screens to paint.
 */
static void
update_view_screens(struct compositor_view *view)
{
	const struct swc_rectangle extents = {
		.x = view->extents.x1,
		.y = view->extents.y1,
		.width = span_u32(view->extents.x1, view->extents.x2),
		.height = span_u32(view->extents.y1, view->extents.y2),
	};
	struct screen *screen;
	uint32_t screens = 0;

	wl_list_for_each(screen, &swc.screens, link) {
		if (rectangle_overlap(&screen->base.geometry, &extents))
			screens |= screen_mask(screen);
	}

	view_set_screens(&view->base, screens);
}

static void
schedule_updates(uint32_t screens)
{
	if (compositor.scheduled_updates == 0) {
		wl_event_loop_add_idle(swc.event_loop, &perform_update, NULL);
	}

	if (screens == -1) {
		struct screen *screen;

		screens = 0;
		wl_list_for_each(screen, &swc.screens, link) screens |=
		    screen_mask(screen);
	}

	/* when zoomed, force full screen damage since actual area differs from
	 * world coords */
	if (compositor.zoom != 1.0f) {
		struct screen *screen;
		wl_list_for_each(screen, &swc.screens, link)
		{
			pixman_region32_union_rect(
			    &compositor.damage, &compositor.damage, screen->base.geometry.x,
			    screen->base.geometry.y, screen->base.geometry.width,
			    screen->base.geometry.height);
			screens |= screen_mask(screen);
		}
	}

	if (compositor.overview_screen) {
		const struct swc_rectangle *g = &compositor.overview_screen->geometry;
		pixman_region32_union_rect(&compositor.damage, &compositor.damage,
		    g->x, g->y, g->width, g->height);
		screens |= screen_mask((struct screen *)compositor.overview_screen);
	}
	compositor.scheduled_updates |= screens;
}

void
compositor_damage_all(void)
{
	struct screen *screen;

	if (!compositor.initialized) {
		return;
	}

	wl_list_for_each(screen, &swc.screens, link)
	{
		pixman_region32_union_rect(
		    &compositor.damage, &compositor.damage, screen->base.geometry.x,
		    screen->base.geometry.y, screen->base.geometry.width,
		    screen->base.geometry.height);
	}

	schedule_updates(-1);
}

static void
overlay_damage_region(int32_t x, int32_t y, uint32_t width, uint32_t height,
                      uint32_t border_width)
{
	(void)border_width;
	pixman_region32_union_rect(&compositor.damage, &compositor.damage, x, y,
	                           width, height);
}

EXPORT void
swc_overlay_set_box(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                    uint32_t color, uint32_t border_width)
{
	int32_t x = x1 < x2 ? x1 : x2;
	int32_t y = y1 < y2 ? y1 : y2;
	uint32_t width = (uint32_t)abs(x2 - x1);
	uint32_t height = (uint32_t)abs(y2 - y1);

	if (border_width == 0) {
		border_width = 1;
	}

	if (overlay.active) {
		overlay_damage_region(overlay.x, overlay.y, overlay.width,
		                      overlay.height, overlay.border_width);
	}

	overlay.active = true;
	overlay.x = x;
	overlay.y = y;
	overlay.width = width;
	overlay.height = height;
	overlay.color = color;
	overlay.border_width = border_width;

	overlay_damage_region(overlay.x, overlay.y, overlay.width, overlay.height,
	                      overlay.border_width);
	schedule_updates(-1);
}

EXPORT void
swc_overlay_clear(void)
{
	if (!overlay.active) {
		return;
	}

	overlay_damage_region(overlay.x, overlay.y, overlay.width, overlay.height,
	                      overlay.border_width);
	overlay.active = false;
	schedule_updates(-1);
}

EXPORT void
swc_set_zoom(float level)
{
	if (level < 0.1f) {
		level = 0.1f;
	}
	if (level > 10.0f) {
		level = 10.0f;
	}

	if (compositor.zoom != level) {
		compositor.zoom = level;
		/* damage entire screen to force full repaint */
		schedule_updates(-1);
	}
}

EXPORT float
swc_get_zoom(void)
{
	return compositor.zoom;
}

/*
 * Draw the screen's scene, scaled about the screen's centre, into whatever
 * target `renderer` already has.
 *
 * This used to composite into an shm buffer with pixman and blit the result.
 * That could only draw a window whose buffer it could map, and a buffer
 * imported from a GPU client is not CPU accessible, so every
 * hardware-accelerated window was silently left out of the zoomed picture.
 * Drawing with the backend's own renderer through wld_blend_scaled() has no
 * such restriction, and costs one quad per window instead of a full-screen
 * readback and upload per frame.
 *
 * Views that are always on top -- a panel, a status bar -- are drawn at their
 * real size and position: zooming the desktop should not shrink the furniture
 * around it.
 */
static void
fill_ring(struct wld_renderer *renderer, uint32_t color,
          double x, double y, double w, double h, double thickness)
{
	int32_t bx = (int32_t)(x - thickness), by = (int32_t)(y - thickness);
	int32_t bw = (int32_t)(w + 2 * thickness), bh = (int32_t)(h + 2 * thickness);
	int32_t bt = (int32_t)thickness;

	if (bt <= 0 || bw <= 2 * bt || bh <= 2 * bt)
		return;

	wld_fill_rectangle(renderer, color, bx, by, bw, bt);
	wld_fill_rectangle(renderer, color, bx, by + bh - bt, bw, bt);
	wld_fill_rectangle(renderer, color, bx, by + bt, bt, bh - 2 * bt);
	wld_fill_rectangle(renderer, color, bx + bw - bt, by + bt, bt, bh - 2 * bt);
}

/* fill_border() for a view drawn scaled, its frame landing at x, y, w, h in
 * the target. The same two rings, drawn outer first. */
static void
fill_border_scaled(struct wld_renderer *renderer,
                   const struct compositor_view *view,
                   double x, double y, double w, double h, double scale)
{
	double in = view->border.inwidth * scale;
	double out = view->border.outwidth * scale;

	if (view->border.outwidth > 0)
		fill_ring(renderer, view->border.outcolor, x, y, w, h, in + out);
	if (view->border.inwidth > 0)
		fill_ring(renderer, view->border.incolor, x, y, w, h, in);
}

static void
render_zoomed(struct screen *screen, struct wld_renderer *renderer, float zoom)
{
	const struct swc_rectangle *geom = &screen->base.geometry;
	double cx = geom->x + geom->width / 2.0;
	double cy = geom->y + geom->height / 2.0;
	struct compositor_view *view;
	pixman_region32_t full;

	pixman_region32_init_rect(&full, 0, 0, geom->width, geom->height);
	wallpaper_repaint(screen, renderer, &full);
	pixman_region32_fini(&full);

	wl_list_for_each_reverse(view, &compositor.views, link) {
		struct wld_buffer *src = view->buffer, *bar;
		const struct swc_rectangle *vg = &view->base.geometry;
		const struct swc_rectangle frame = frame_geometry(view);
		struct swc_rectangle bar_rect;
		struct wld_rect dst;
		struct wld_frect area;
		double scale, x, y, fx, fy, fw, fh, border;
		double sx, sy, sw, sh;

		/*
		 * Hidden means not on screen, and that includes everything a session
		 * lock put away. The pixman implementation walked the list without
		 * asking, so a lock that had just hidden the desktop still had it
		 * drawn underneath while the screen was zoomed.
		 */
		if (!view->visible || !src)
			continue;
		if (!(wld_capabilities(renderer, src) & WLD_CAPABILITY_READ))
			continue;

		scale = view->always_top ? 1.0 : zoom;
		x = (vg->x - cx) * scale + geom->width / 2.0;
		y = (vg->y - cy) * scale + geom->height / 2.0;
		/* The border encloses the titlebar too, as it does unzoomed. */
		fx = (frame.x - cx) * scale + geom->width / 2.0;
		fy = (frame.y - cy) * scale + geom->height / 2.0;
		fw = frame.width * scale;
		fh = frame.height * scale;
		border = ((double)view->border.outwidth + view->border.inwidth) * scale;

		if (fx + fw + border < 0 || fx - border >= geom->width ||
		    fy + fh + border < 0 || fy - border >= geom->height)
			continue;

		fill_border_scaled(renderer, view, fx, fy, fw, fh, scale);

		/*
		 * A window's geometry can be a sub-rectangle of its buffer -- the
		 * client's own margin for the shadow it is not drawing -- so the
		 * source is the window geometry, not the whole buffer.
		 */
		sx = view->window ? view->buffer_offset_x : 0;
		sy = view->window ? view->buffer_offset_y : 0;
		sw = vg->width;
		sh = vg->height;

		/* Clamp to what the buffer actually holds, and move the destination
		 * with it so the clamp crops rather than stretches. */
		if (sx < 0) { sw += sx; x -= sx * scale; sx = 0; }
		if (sy < 0) { sh += sy; y -= sy * scale; sy = 0; }
		if (sx + sw > src->width)
			sw = src->width - sx;
		if (sy + sh > src->height)
			sh = src->height - sy;
		if (sw <= 0 || sh <= 0)
			continue;

		dst = (struct wld_rect){ (int32_t)(x + 0.5), (int32_t)(y + 0.5),
		                         (uint32_t)(sw * scale + 0.5),
		                         (uint32_t)(sh * scale + 0.5) };
		area = (struct wld_frect){ sx, sy, sw, sh };
		if (dst.width == 0 || dst.height == 0)
			continue;

		wld_blend_scaled(renderer, src, &dst, &area);

		/*
		 * The titlebar is a compositor-drawn buffer of its own, sitting above
		 * the window's geometry. The pixman implementation never drew it, so
		 * every window lost its bar the moment the screen was zoomed.
		 */
		if ((bar = titlebar_content(view, &bar_rect))) {
			struct wld_rect bdst = {
				(int32_t)((bar_rect.x - cx) * scale + geom->width / 2.0 + 0.5),
				(int32_t)((bar_rect.y - cy) * scale + geom->height / 2.0 + 0.5),
				(uint32_t)(bar_rect.width * scale + 0.5),
				(uint32_t)(bar_rect.height * scale + 0.5),
			};
			struct wld_frect barea = { 0, 0, bar->width, bar->height };

			if (bdst.width > 0 && bdst.height > 0 &&
			    (wld_capabilities(renderer, bar) & WLD_CAPABILITY_READ))
				wld_blend_scaled(renderer, bar, &bdst, &barea);
		}
	}
}

/* Return the nearest managed toplevel. Transient toplevels get their own card;
 * subsurfaces and popups belong to their first window ancestor. */
static struct compositor_view *overview_owner(struct compositor_view *view)
{
	while (view && !view->window) view = view->parent;
	return view;
}

EXPORT bool swc_window_overview_geometry(struct swc_window *base, struct swc_rectangle *rect)
{
	struct window *window = (struct window *)base;
	if (!window || !window->view || !rect) return false;
	struct compositor_view *view = window->view;
	*rect = frame_geometry(view);
	int border = view->border.inwidth + view->border.outwidth;
	rect->x -= border; rect->y -= border;
	rect->width += 2 * border; rect->height += 2 * border;
	/* Include child surfaces outside the frame, so popups remain visible
	 * without overlapping another card. Transient toplevels have own cards. */
	int64_t x1 = rect->x, y1 = rect->y;
	int64_t x2 = x1 + rect->width, y2 = y1 + rect->height;
	struct compositor_view *child;
	wl_list_for_each(child, &compositor.views, link) {
		/* Most views are toplevels with no parent to walk. */
		if (child == view || !child->parent || !child->buffer ||
		    overview_owner(child) != view) continue;
		const struct swc_rectangle *g = &child->base.geometry;
		x1 = MIN(x1, g->x); y1 = MIN(y1, g->y);
		x2 = MAX(x2, (int64_t)g->x + g->width);
		y2 = MAX(y2, (int64_t)g->y + g->height);
	}
	rect->x = clamp_i32(x1); rect->y = clamp_i32(y1);
	rect->width = span_u32(rect->x, clamp_i32(x2));
	rect->height = span_u32(rect->y, clamp_i32(y2));
	return rect->width && rect->height;
}

static void overview_child_changed(struct compositor_view *view)
{
	struct compositor_view *owner = overview_owner(view);
	if (compositor.overview_screen && owner && owner != view && owner->window->managed &&
	    owner->window->handler && owner->window->handler->geometry_changed)
		owner->window->handler->geometry_changed(owner->window->handler_data);
}

EXPORT bool swc_overview_begin(struct swc_screen *screen,
                              const struct swc_overview_item *items, unsigned n)
{
	if (!compositor.initialized || !swc.active || session_lock_active() || !screen || !n || !items)
		return false;
	struct swc_overview_item *copy = calloc(n, sizeof(*copy));
	if (!copy) return false;
	for (unsigned i = 0; i < n; ++i) {
		if (!items[i].source.width || !items[i].source.height || !items[i].rect.width ||
		    !items[i].rect.height) { free(copy); return false; }
		copy[i] = items[i];
	}
	free(compositor.overview_items);
	compositor.overview_items = copy;
	compositor.overview_count = n;
	compositor.overview_screen = screen;
	compositor_damage_all();
	return true;
}
EXPORT void swc_overview_end(void)
{
	if (!compositor.overview_screen) return;
	compositor.overview_screen = NULL;
	free(compositor.overview_items);
	compositor.overview_items = NULL;
	compositor.overview_count = 0;
	compositor_damage_all();
}

/* Clip in destination space, then adjust the source by the same fraction.
 * This keeps popups, negative buffer insets, and offscreen panels in bounds. */
static void overview_blit(struct wld_renderer *renderer, struct wld_buffer *buffer,
                          double sx, double sy, double width, double height,
                          double x, double y, double scale_x, double scale_y,
                          struct swc_rectangle clip)
{
	if (!buffer || !(wld_capabilities(renderer, buffer) & WLD_CAPABILITY_READ)) return;
	if (sx < 0) { width += sx; x -= sx * scale_x; sx = 0; }
	if (sy < 0) { height += sy; y -= sy * scale_y; sy = 0; }
	width = fmin(width, buffer->width - sx);
	height = fmin(height, buffer->height - sy);
	double right = fmin(x + width * scale_x, (double)clip.x + clip.width);
	double bottom = fmin(y + height * scale_y, (double)clip.y + clip.height);
	double left = fmax(x, clip.x), top = fmax(y, clip.y);
	if (left >= right || top >= bottom) return;
	struct wld_rect dst = { (int32_t)lround(left), (int32_t)lround(top), 0, 0 };
	int32_t x2 = lround(right), y2 = lround(bottom);
	if (x2 <= dst.x || y2 <= dst.y) return;
	dst.width = x2 - dst.x; dst.height = y2 - dst.y;
	struct wld_frect src = { sx + (left - x) / scale_x, sy + (top - y) / scale_y,
	                        (right - left) / scale_x, (bottom - top) / scale_y };
	wld_blend_scaled(renderer, buffer, &dst, &src);
}

static void overview_view(struct wld_renderer *renderer, struct compositor_view *view,
                          struct swc_rectangle source, struct swc_rectangle dest,
                          struct swc_rectangle clip)
{
	double sx = (double)dest.width / source.width, sy = (double)dest.height / source.height;
	struct swc_rectangle g = view->base.geometry, bar_rect;
	struct wld_buffer *bar;
	struct swc_rectangle frame = frame_geometry(view);
	double x = dest.x + (frame.x - (double)source.x) * sx;
	double y = dest.y + (frame.y - (double)source.y) * sy;
	fill_border_scaled(renderer, view, x, y, frame.width * sx,
	                   frame.height * sy, sx);
	overview_blit(renderer, view->buffer,
	    view->window ? view->buffer_offset_x : 0, view->window ? view->buffer_offset_y : 0,
	    g.width, g.height, dest.x + (g.x - (double)source.x) * sx,
	    dest.y + (g.y - (double)source.y) * sy, sx, sy, clip);
	/* titlebar_content may change the GL target while rebuilding its cache. */
	if ((bar = titlebar_content(view, &bar_rect))) {
		wld_set_target_buffer(renderer, renderer->target);
		overview_blit(renderer, bar, 0, 0, bar->width, bar->height,
		    dest.x + (bar_rect.x - (double)source.x) * sx,
		    dest.y + (bar_rect.y - (double)source.y) * sy,
		    sx, sy, clip);
	}
}

static void overview_label(struct wld_renderer *renderer, struct compositor_view *view,
                           const struct swc_overview_item *item, struct swc_rectangle rect)
{
	uint32_t height = item->label_height;
	if (!height) return;
	wld_fill_rectangle(renderer, 0xff202020, rect.x, rect.y + rect.height, rect.width, height);
	struct wld_font *font = view->decor.font;
	if (!font || font->height > height || rect.width < 12) return;
	char title[1024];
	snprintf(title, sizeof(title), "%s%s", item->minimized ? "[minimized] " : "",
	         view->window->base.title ? view->window->base.title : "Untitled");
	unsigned len = 0;
	struct wld_extents ext;
	while (title[len]) {
		unsigned next = len + 1;
		while ((title[next] & 0xc0) == 0x80) ++next;
		wld_font_text_extents_n(font, title, next, &ext);
		if (ext.advance > rect.width - 12) break;
		len = next;
	}
	if (len) wld_draw_text(renderer, font, 0xffeeeeee, rect.x + 6,
	    rect.y + rect.height + (height - font->height) / 2 + font->ascent, title, len, NULL);
}

static void render_overview(struct screen *screen, struct wld_renderer *renderer)
{
	const struct swc_rectangle *g = &screen->base.geometry;
	struct swc_rectangle full = {0, 0, g->width, g->height};
	pixman_region32_t region;
	pixman_region32_init_rect(&region, 0, 0, g->width, g->height);
	wallpaper_repaint(screen, renderer, &region);
	pixman_region32_fini(&region);
	struct compositor_view *view;
	/* Keep shell backgrounds below the cards, panels and overlays above. */
	for (unsigned pass = 0; pass < 2; ++pass) {
		if (pass) for (unsigned i = 0; i < compositor.overview_count; ++i) {
			const struct swc_overview_item *item = &compositor.overview_items[i];
			struct compositor_view *root = NULL;
			wl_list_for_each(view, &compositor.views, link)
				if (view->window && &view->window->base == item->window) { root = view; break; }
			if (!root) continue;
			struct swc_rectangle r = item->rect;
			r.x -= g->x; r.y -= g->y;
			/* A retained window without a buffer still has a selectable card. */
			wld_fill_rectangle(renderer, 0xff303030, r.x, r.y, r.width, r.height);
			wl_list_for_each_reverse(view, &compositor.views, link)
				if (overview_owner(view) == root)
					overview_view(renderer, view, item->source, r, r);
			overview_label(renderer, root, item, r);
			/* Draw the selection inside the card, independent of window borders. */
			uint32_t h = r.height + item->label_height;
			unsigned border = MIN(2u, MIN(r.width, h));
			uint32_t color = item->highlighted ? item->color : 0xff606060;
			wld_fill_rectangle(renderer, color, r.x, r.y, r.width, border);
			wld_fill_rectangle(renderer, color, r.x, r.y + h - border, r.width, border);
			wld_fill_rectangle(renderer, color, r.x, r.y, border, h);
			wld_fill_rectangle(renderer, color, r.x + r.width - border, r.y, border, h);
			if (item->minimized && !item->label_height)
				wld_fill_rectangle(renderer, item->color, r.x, r.y + r.height - border, r.width / 2, border);
		}
		wl_list_for_each_reverse(view, &compositor.views, link) {
			if (!view->visible || overview_owner(view) ||
			    (view->stack_layer >= STACK_LAYER_NORMAL) != (pass != 0)) continue;
			struct swc_rectangle dest = view->base.geometry;
			dest.x -= g->x; dest.y -= g->y;
			if (dest.width && dest.height)
				overview_view(renderer, view, view->base.geometry, dest, full);
		}
	}
}

static bool
update(struct view *base)
{
	struct compositor_view *view = (void *)base;

	if (!swc.active) return false;
	if (!view->visible) {
		/* Hidden views retain their zero output mask and receive no synthetic
		 * frame callbacks. A new buffer may still refresh its thumbnail. */
		if (compositor.overview_screen) schedule_updates(0);
		return compositor.overview_screen != NULL;
	}

	/*
	 * Damage is scheduled per screen, and the screen mask is what says which
	 * ones. It was only recomputed when a view moved or changed size, so a view
	 * whose mask was stale -- or never computed, because it has not moved since
	 * it was shown -- scheduled an update for the wrong screens, or for none at
	 * all. Its damage then sat in the accumulated region unpainted until
	 * something else on that screen happened to schedule a frame, which for a
	 * panel on an otherwise idle monitor means it went stale until the pointer
	 * moved across it. Recomputing here is two rectangle tests, and
	 * view_set_screens does nothing when the mask has not actually changed.
	 */
	update_view_screens(view);
	schedule_updates(view->base.screens);

	return true;
}

static int
attach(struct view *base, struct wld_buffer *buffer)
{
	struct compositor_view *view = (void *)base;
	struct surface *surface = view->surface;
	pixman_box32_t old_extents = view->extents;
	int32_t old_offset_x = view->buffer_offset_x, old_offset_y = view->buffer_offset_y;
	bool buffer_resized = !view->base.buffer || !buffer ||
	    view->base.buffer->width != buffer->width || view->base.buffer->height != buffer->height;
	uint32_t new_width = buffer ? buffer->width : 0;
	uint32_t new_height = buffer ? buffer->height : 0;
	int ret;

	if ((ret = renderer_attach(view, buffer)) < 0) {
		return ret;
	}

	/*
	 * No update() here: both callers, surface_apply_pending() and
	 * surface_set_view(), follow view_attach() with view_update(), once the
	 * new size and offset are in place. Updating before them as well worked
	 * out the screen mask from the old extents, and again after a resize.
	 */
	view->buffer_offset_x = 0;
	view->buffer_offset_y = 0;
	if (surface && surface->has_window_geometry && buffer) {
		if (surface->window_width > 0 && surface->window_height > 0) {
			new_width = (uint32_t)surface->window_width;
			new_height = (uint32_t)surface->window_height;
			view->buffer_offset_x = surface->window_x;
			view->buffer_offset_y = surface->window_y;
		}
	}

	bool resized = view_set_size(&view->base, new_width, new_height);
	if (resized || buffer_resized || old_offset_x != view->buffer_offset_x ||
	    old_offset_y != view->buffer_offset_y) {
		/* base.buffer is replaced by view_attach only after this callback.
		 * Use the incoming buffer to calculate shadows and other extents. */
		update_extents_for_buffer(view, buffer);
		if (view->visible) {
			pixman_region32_union_rect(&compositor.damage, &compositor.damage,
			    old_extents.x1, old_extents.y1,
			    span_u32(old_extents.x1, old_extents.x2), span_u32(old_extents.y1, old_extents.y2));
			pixman_region32_clear(&view->clip);
			/* The old extents may be on a screen the new ones are not. The
			 * caller's update schedules the screens the view is on now. */
			if (swc.active)
				schedule_updates(view->base.screens);
			damage_view(view);
		}
	}

	if (resized || buffer_resized || (!buffer != !view->base.buffer))
		overview_child_changed(view);
	return 0;
}

static bool
move(struct view *base, int32_t x, int32_t y)
{
	struct compositor_view *view = (void *)base;
	struct compositor_view *child;
	int32_t old_x = view->base.geometry.x;
	int32_t old_y = view->base.geometry.y;

	if (old_x == x && old_y == y)
		return true;

	if (view->visible) {
		damage_below_view(view);
		update(&view->base);
	}

	if (view_set_position(&view->base, x, y)) {
		int32_t dx = x - old_x;
		int32_t dy = y - old_y;

		update_extents(view);

		if (view->visible) {
			/* Assume worst-case no clipping until we draw the next frame (in
			 * case the surface gets moved again before that). */
			pixman_region32_clear(&view->clip);

			update_view_screens(view);
			damage_below_view(view);
			update(&view->base);
		}

		/* Popup coordinates are relative to their parent surface. */
		wl_list_for_each(child, &compositor.views, link)
		{
			/* Subsurfaces already follow the parent's view-handler callback.
			 * Applying this delta too would move them twice on every drag. */
			if (child->parent == view && !child->surface->subsurface) {
				view_move(&child->base, child->base.geometry.x + dx,
				          child->base.geometry.y + dy);
			}
		}
	}

	overview_child_changed(view);
	return true;
}

static const struct view_impl view_impl = {
    .update = update,
    .attach = attach,
    .move = move,
};

/* A window's child surfaces are part of its stack entry. Keep their existing
 * above/below order when moving the entry; below-parent subsurfaces must not
 * become popups merely because the window changed mode. */
static bool
view_descends_from(struct compositor_view *view, struct compositor_view *parent)
{
	for (view = view->parent; view; view = view->parent)
		if (view == parent) return true;
	return false;
}

struct view_children {
	struct wl_list above, below;
};

static void
take_view_children(struct compositor_view *view, struct view_children *children)
{
	struct compositor_view *child, *next;
	bool above = true;
	wl_list_init(&children->above);
	wl_list_init(&children->below);
	wl_list_for_each_safe(child, next, &compositor.views, link) {
		if (child == view) above = false;
		if (!view_descends_from(child, view)) continue;
		damage_view(child);
		pixman_region32_clear(&child->clip);
		schedule_updates(child->base.screens);
		wl_list_remove(&child->link);
		wl_list_insert(above ? children->above.prev : children->below.prev,
		               &child->link);
	}
}

static void
restore_view_children(struct compositor_view *view, struct view_children *children)
{
	struct compositor_view *child;
	wl_list_for_each(child, &children->above, link) {
		child->stack_layer = view->stack_layer;
		child->always_top = view->always_top;
	}
	wl_list_for_each(child, &children->below, link) {
		child->stack_layer = view->stack_layer;
		child->always_top = view->always_top;
	}
	if (!wl_list_empty(&children->above))
		wl_list_insert_list(view->link.prev, &children->above);
	if (!wl_list_empty(&children->below))
		wl_list_insert_list(&view->link, &children->below);
}

static void
restack_view_for_layer(struct compositor_view *view, bool raise)
{
	struct compositor_view *other;
	struct wl_list *insert_after = &compositor.views;
	wl_list_for_each(other, &compositor.views, link) {
		if (other == view) continue;
		if (other->stack_layer < view->stack_layer ||
		    (raise && other->stack_layer == view->stack_layer)) break;
		insert_after = &other->link;
	}
	wl_list_remove(&view->link);
	wl_list_insert(insert_after, &view->link);
}

static struct compositor_view *
view_at(int32_t x, int32_t y)
{
	struct compositor_view *view;
	struct swc_rectangle *geom;
	struct swc_rectangle buffer_geom;

	/*
	 * Runs on every pointer motion. The titlebar, the content and a surface's
	 * buffer all lie inside the extents, so four comparisons reject nearly
	 * every view before the titlebar layout or the input region is consulted.
	 */
	wl_list_for_each(view, &compositor.views, link)
	{
		if (!view->visible || x < view->extents.x1 || x >= view->extents.x2 ||
		    y < view->extents.y1 || y >= view->extents.y2) {
			continue;
		}

		geom = &view->base.geometry;
		if (titlebar_hit(view, x, y) != -2) return view;
		if (view->window) {
			if (!rectangle_contains_point(geom, x, y)) {
				continue;
			}
		} else if (view->base.buffer) {
			buffer_geom.x = geom->x - view->buffer_offset_x;
			buffer_geom.y = geom->y - view->buffer_offset_y;
			buffer_geom.width = view->base.buffer->width;
			buffer_geom.height = view->base.buffer->height;
			if (!rectangle_contains_point(&buffer_geom, x, y)) {
				continue;
			}
		} else if (!rectangle_contains_point(geom, x, y)) {
			continue;
		}

		if (pixman_region32_contains_point(&view->surface->state.input,
		                                   x - geom->x + view->buffer_offset_x,
		                                   y - geom->y + view->buffer_offset_y,
		                                   NULL)) {
			return view;
		}
	}

	return NULL;
}

static struct compositor_view *
window_view(struct compositor_view *view)
{
	while (view && !view->window && view->parent && view->parent != view) {
		view = view->parent;
	}
	return (view && view->window) ? view : NULL;
}

static void
raise_window_from_click(struct compositor_view *view)
{
	struct compositor_view *window = window_view(view);
	if (!window || window->window->raise_on_click)
		raise_window(view);
}

void
raise_window(struct compositor_view *view)
{
	struct compositor_view *other;
	struct view_children children;
	struct wl_list *insert_after;
	uint32_t screens;

	view = window_view(view);
	if (!view || !view->visible) {
		return;
	}

	take_view_children(view, &children);
	insert_after = &compositor.views;
	wl_list_for_each(other, &compositor.views, link)
	{
		if (other == view) {
			continue;
		}

		if (!other->visible) {
			continue;
		}

		if (other->stack_layer > view->stack_layer ||
		    (other->stack_layer == view->stack_layer && other->always_top)) {
			insert_after = &other->link;
			continue;
		}

		if (other->stack_layer < view->stack_layer) {
			break;
		}

		if (other->window) {
			break;
		}
		insert_after = &other->link;
	}

	screens = view->base.screens;

	wl_list_remove(&view->link);
	wl_list_insert(insert_after, &view->link);
	restore_view_children(view, &children);

	view->border.damaged_border1 = true;
	pixman_region32_union_rect(&compositor.damage, &compositor.damage,
	                           view->extents.x1, view->extents.y1,
	                           span_u32(view->extents.x1, view->extents.x2),
	                           span_u32(view->extents.y1, view->extents.y2));
	schedule_updates(screens);
}

void
raise_window_top(struct compositor_view *view)
{
	compositor_view_set_stack_layer(view, STACK_LAYER_OVERLAY, true);
}

void
compositor_hide_for_lock(void)
{
	swc_overview_end();
	struct compositor_view *view;

	wl_list_for_each(view, &compositor.views, link)
	{
		if (!view->visible || view->stack_layer == STACK_LAYER_LOCK) {
			continue;
		}
		/* After the hide: hiding clears the flag, because an explicit hide
		 * means something other than the lock wants this view gone. */
		compositor_view_hide(view);
		view->hidden_by_lock = true;
	}
}

void
compositor_restore_after_lock(void)
{
	struct compositor_view *view;

	wl_list_for_each(view, &compositor.views, link)
	{
		if (!view->hidden_by_lock) {
			continue;
		}
		view->hidden_by_lock = false;
		/* The window manager's stacking order survived the lock, so come
		 * back where we were rather than on top. */
		compositor_view_show_in_place(view);
	}
}

void
compositor_view_set_stack_layer(struct compositor_view *view, uint32_t layer,
	                            bool raise)
{
	struct view_children children;
	if (view->stack_layer == layer && !raise) return;
	take_view_children(view, &children);
	damage_view(view);
	view->stack_layer = layer;
	restack_view_for_layer(view, raise);
	restore_view_children(view, &children);
	damage_view(view);
	schedule_updates(view->base.screens);
}

EXPORT struct swc_window *
swc_window_at(int32_t x, int32_t y)
{
	struct compositor_view *view = window_view(view_at(x, y));

	return view ? &view->window->base : NULL;
}

static struct compositor_view *
view_for_window(struct swc_window *base)
{
	struct window *window;

	if (!base) {
		return NULL;
	}

	window = (struct window *)base;
	return window->view;
}

static struct compositor_view *
prev_window_view(struct compositor_view *view)
{
	struct wl_list *link;
	struct compositor_view *other;

	for (link = view->link.prev; link != &compositor.views; link = link->prev) {
		other = wl_container_of(link, other, link);

		if (other->visible && other->window &&
		    other->stack_layer == view->stack_layer) {
			return other;
		}
	}

	return NULL;
}

static struct compositor_view *
next_window_view(struct compositor_view *view)
{
	struct wl_list *link;
	struct compositor_view *other;

	for (link = view->link.next; link != &compositor.views; link = link->next) {
		other = wl_container_of(link, other, link);

		if (other->visible && other->window &&
		    other->stack_layer == view->stack_layer) {
			return other;
		}
	}

	return NULL;
}

static void
damage_views(struct compositor_view *a, struct compositor_view *b)
{
	uint32_t screens = a->base.screens | (b ? b->base.screens : 0);

	a->border.damaged_border1 = true;
	a->border.damaged_border2 = true;
	pixman_region32_union_rect(&compositor.damage, &compositor.damage,
	                           a->extents.x1, a->extents.y1,
	                           span_u32(a->extents.x1, a->extents.x2),
	                           span_u32(a->extents.y1, a->extents.y2));

	if (b) {
		b->border.damaged_border1 = true;
		b->border.damaged_border2 = true;
		pixman_region32_union_rect(&compositor.damage, &compositor.damage,
		                           b->extents.x1, b->extents.y1,
		                           span_u32(b->extents.x1, b->extents.x2),
		                           span_u32(b->extents.y1, b->extents.y2));
	}

	schedule_updates(screens);
}

EXPORT void
swc_window_stack(struct swc_window *window, int32_t direction)
{
	struct compositor_view *view = view_for_window(window);
	struct compositor_view *other = NULL;

	if (!view || !view->visible || direction == 0) {
		return;
	}

	if (direction < 0) {
		other = prev_window_view(view);
		if (!other) {
			return;
		}
		compositor_view_restack(view, other, true);
	} else {
		other = next_window_view(view);
		if (!other) {
			return;
		}
		compositor_view_restack(view, other, false);
	}
}

static void
log_view_memory(const char *event)
{
	const char *enabled = getenv("CHARAWC_DEBUG_MEMORY");
	if (enabled && strcmp(enabled, "1") == 0)
		fprintf(stderr, "memory-profile: pid=%ld view-%s live_views=%d\n",
		        (long)getpid(), event, wl_list_length(&compositor.views));
}

struct compositor_view *
compositor_create_view(struct surface *surface)
{
	struct compositor_view *view;

	view = malloc(sizeof(*view));

	if (!view) {
		return NULL;
	}

	view_initialize(&view->base, &view_impl);
	view->surface = surface;
	view->buffer = NULL;
	view->proxy_dirty = false;
	view->window = NULL;
	view->parent = NULL;
	view->buffer_offset_x = 0;
	view->buffer_offset_y = 0;
	view->visible = false;
	view->always_top = false;
	view->stack_layer = STACK_LAYER_NORMAL;
	view->hidden_by_lock = false;
	view->extents.x1 = 0;
	view->extents.y1 = 0;
	view->extents.x2 = 0;
	view->extents.y2 = 0;
	view->border.outwidth = 0;
	view->border.outcolor = 0x000000;
	view->border.damaged_border1 = false;
	view->border.inwidth = 0;
	view->border.incolor = 0x000000;
	view->border.damaged_border2 = false;
	view->decor.color = 0x000000;
	view->decor.top = 0;
	view->decor.right = 0;
	view->decor.bottom = 0;
	view->decor.left = 0;
	decor_view_initialize(view);
	view->decor.damaged = false;
	pixman_region32_init(&view->clip);
	wl_signal_init(&view->destroy_signal);
	surface_set_view(surface, &view->base);
	wl_list_insert(&compositor.views, &view->link);
	log_view_memory("create");

	return view;
}

void
compositor_view_destroy(struct compositor_view *view)
{
	struct compositor_view *other;

	wl_signal_emit(&view->destroy_signal, NULL);
	/* Nothing may keep pointing at a view that is going away: view_descends_from
	 * walks these chains on every restack. Popups and subsurfaces drop their own
	 * edge through a destroy listener, but xdg_toplevel.set_parent has none. */
	wl_list_for_each(other, &compositor.views, link) {
		if (other->parent != view)
			continue;
		other->parent = NULL;
		if (other->window && view->window) {
			other->window->base.parent = NULL;
			/* window_set_parent tells them when a parent changes the
			 * ordinary way; a parent that is destroyed has to as well. */
			foreign_toplevel_window_parent(other->window);
		}
	}
	compositor_view_hide(view);
	surface_set_view(view->surface, NULL);
	/* The renderer's upload proxy owns a separate reference from the
	 * client's buffer retained by view_finalize(). */
	if (view->buffer && view->buffer != view->base.buffer)
		wld_buffer_unreference(view->buffer);
	view_finalize(&view->base);
	decor_view_finalize(view);
	pixman_region32_fini(&view->clip);
	wl_list_remove(&view->link);
	overview_child_changed(view);
	free(view);
	log_view_memory("destroy");
}

struct compositor_view *
compositor_view(struct view *view)
{
	/*
	 * A surface that has no view yet is a normal thing to be asked about --
	 * callers check the result for NULL -- so do not dereference before the
	 * impl comparison.
	 */
	return view && view->impl == &view_impl ? (struct compositor_view *)view
	                                        : NULL;
}

void
compositor_view_set_parent(struct compositor_view *view,
                           struct compositor_view *parent)
{
	view->parent = parent;

	if (!parent) {
		return;
	}

	view->stack_layer = parent->stack_layer;
	view->always_top = parent->always_top;
	if (parent->visible) {
		compositor_view_show(view);
	} else {
		compositor_view_hide(view);
	}
}

void
compositor_view_restack(struct compositor_view *view,
                        struct compositor_view *sibling, bool above)
{
	struct compositor_view *other;
	struct view_children children;
	struct wl_list *edge;
	if (!view || !sibling || view == sibling || view_descends_from(sibling, view))
		return;

	edge = &sibling->link;
	if (!view_descends_from(view, sibling)) {
		/* Sibling subsurface trees are indivisible stack entries. */
		wl_list_for_each(other, &compositor.views, link) {
			if (other != sibling && !view_descends_from(other, sibling)) continue;
			edge = &other->link;
			if (above) break;
		}
	}
	if ((above && view->link.next == edge) ||
	    (!above && view->link.prev == edge)) return;

	take_view_children(view, &children);
	wl_list_remove(&view->link);
	wl_list_insert(above ? edge->prev : edge, &view->link);
	restore_view_children(view, &children);
	damage_views(view, sibling);
}

/*
 * raise: a window being mapped for the first time belongs on top, but one that
 * is merely coming back -- a workspace being switched to -- belongs exactly
 * where its user left it. Raising those on the way back made the stacking
 * order a function of the order they happen to be shown in, so whichever
 * window was created last always won, and a window the user had deliberately
 * raised sank again on every trip through another workspace.
 */
static void
view_show(struct compositor_view *view, bool raise)
{
	struct compositor_view *other;
	struct subsurface *subsurface;

	if (view->visible) {
		return;
	}

	subsurface = view->surface ? view->surface->subsurface : NULL;
	if (subsurface) {
		if (!subsurface->added || !view->base.buffer) {
			return;
		}
	}

	/* Mapping something while the screen is locked must not put it on the
	 * screen. compositor_hide_for_lock only covers what was already visible
	 * when the lock came up. */
	if (session_lock_active() && view->stack_layer < STACK_LAYER_LOCK) {
		view->hidden_by_lock = true;
		return;
	}

	view->visible = true;
	/* The mask now follows the extents, so make sure they are not still the
	 * zeroed ones a view that has never been painted starts with. */
	update_extents(view);
	update_view_screens(view);

	if (view->window && raise) {
		raise_window(view);
	}

	/* Assume worst-case no clipping until we draw the next frame (in case the
	 * surface gets moved before that. */
	pixman_region32_clear(&view->clip);
	damage_view(view);
	update(&view->base);

	wl_list_for_each(other, &compositor.views, link)
	{
		if (other->parent == view) {
			view_show(other, raise);
		}
	}
}

void
compositor_view_show(struct compositor_view *view)
{
	view_show(view, true);
}

void
compositor_view_show_in_place(struct compositor_view *view)
{
	view_show(view, false);
}

void
compositor_view_hide(struct compositor_view *view)
{
	struct compositor_view *other;
	bar_forget(view, false);

	/* Whoever asked for this wants the view hidden for their own reason, so
	 * the lock no longer owes it a restore. Cleared before the early return:
	 * a view deferred by the lock is already invisible, and without this it
	 * would come back on unlock after being minimized. */
	view->hidden_by_lock = false;

	if (!view->visible) {
		return;
	}

	/* Update all the screens the view was on. */
	update(&view->base);
	damage_below_view(view);

	view_set_screens(&view->base, 0);
	view->visible = false;
	if (swc.seat && swc.seat->pointer && swc.seat->pointer->focus.view == view)
		pointer_set_focus(swc.seat->pointer, NULL);

	wl_list_for_each(other, &compositor.views, link)
	{
		if (other->parent == view) {
			compositor_view_hide(other);
		}
	}
}

void
compositor_view_set_border_width(struct compositor_view *view,
                                 uint32_t outwidth, uint32_t inwidth)
{
	if (view->border.outwidth == outwidth && view->border.inwidth == inwidth) {
		return;
	}

	if (view->visible) damage_below_view(view);

	view->border.outwidth = outwidth;
	view->border.damaged_border1 = true;

	view->border.inwidth = inwidth;
	view->border.damaged_border2 = true;

	/* XXX: Damage above surface for transparent surfaces? */

	update_extents(view);
	update(&view->base);
}

void
compositor_view_set_border_color(struct compositor_view *view,
                                 uint32_t outcolor, uint32_t incolor)
{
	if (view->border.outcolor == outcolor && view->border.incolor == incolor) {
		return;
	}

	view->border.outcolor = outcolor;
	view->border.damaged_border1 = true;

	view->border.incolor = incolor;
	view->border.damaged_border2 = true;

	/* XXX: Damage above surface for transparent surfaces? */

	update(&view->base);
}

void
compositor_view_set_decor(struct compositor_view *view,
                            const struct swc_decor *decor)
{
	/* decor_view_set() could shrink or remove the decoration, so wedamage the old
	 * extents before they are replaced */
	if (view->visible) {
		damage_below_view(view);
	}

	if (!decor || !decor->titlebar.enabled) bar_forget(view, false);
	decor_view_set(view, decor);
	update_extents(view);
	update(&view->base);
}

void
compositor_view_damage_decor(struct compositor_view *view)
{
	if (!view->decor.top && !view->decor.right && !view->decor.bottom &&
	    !view->decor.left) {
		return;
	}

	decor_view_damage(view);
	update(&view->base);
}

void
compositor_view_apply_decor(struct compositor_view *view, struct swc_prepared_decor *prepared)
{
	if (view->visible) damage_below_view(view);
	/* A new button order/side must not inherit an old hover or click index. */
	bar_forget(view, true);
	struct compositor_view old = {0};
	old.decor = view->decor;
	view->decor = prepared->view->decor;
	prepared->view->decor = old.decor;
	swc_decor_discard(prepared);
	update_extents(view);
	update(&view->base);
}

/* }}} */

static void
calculate_damage(void)
{
	struct compositor_view *view;
	struct swc_rectangle *geom;
	pixman_region32_t surface_opaque, *surface_damage;

	pixman_region32_clear(&compositor.opaque);
	pixman_region32_init(&surface_opaque);

	/* Go through views top-down to calculate clipping regions. */
	wl_list_for_each(view, &compositor.views, link)
	{
		if (!view->visible) {
			/* The overview still shows hidden windows as thumbnails, so their
			 * upload proxies have to keep up with what the client draws. */
			if (compositor.overview_screen && view->buffer && view->base.buffer) {
				renderer_flush_view(view);
				pixman_region32_clear(&view->surface->state.damage);
			}
			continue;
		}

		geom = &view->base.geometry;

		/*
		 * Clip the surface by the opaque region covering it. Everything that
		 * consults the clip works inside the view's extents, so keep only that
		 * part: a copy of the whole accumulated region grows with every opaque
		 * window above, which made this quadratic in the window count.
		 */
		pixman_region32_intersect_rect(&view->clip, &compositor.opaque,
		                               view->extents.x1, view->extents.y1,
		                               span_u32(view->extents.x1, view->extents.x2),
		                               span_u32(view->extents.y1, view->extents.y2));

		/* Add the surface's opaque region, in global coordinates, to the
		 * accumulated opaque region. Many clients declare none at all. */
		bool bar_opaque = view->decor.titlebar.enabled && view->base.buffer;
		if (bar_opaque || pixman_region32_not_empty(&view->surface->state.opaque)) {
			pixman_region32_copy(&surface_opaque, &view->surface->state.opaque);
			pixman_region32_translate(&surface_opaque,
			                          geom->x - view->buffer_offset_x,
			                          geom->y - view->buffer_offset_y);
			pixman_region32_intersect_rect(&surface_opaque, &surface_opaque,
			                               geom->x, geom->y, geom->width,
			                               geom->height);

			/* The cached solid bar is opaque too; avoid drawing windows
			 * behind it. */
			if (bar_opaque)
				pixman_region32_union_rect(&surface_opaque, &surface_opaque,
				    geom->x, geom->y - view->decor.top, geom->width, view->decor.top);

			pixman_region32_union(&compositor.opaque, &compositor.opaque,
			                      &surface_opaque);
		}

		surface_damage = &view->surface->state.damage;

		/*
		 * A client may attach its first buffer without posting damage, which
		 * is allowed. Fill the proxy anyway, or the surface renders as the
		 * blank buffer it was allocated as.
		 */
		bool copied_full_proxy = view->proxy_dirty;
		if (copied_full_proxy) {
			renderer_flush_view(view);
			pixman_region32_union_rect(&compositor.damage, &compositor.damage,
			                           geom->x, geom->y, geom->width,
			                           geom->height);
		}

		if (pixman_region32_not_empty(surface_damage)) {
			if (!copied_full_proxy) renderer_flush_view(view);

			/* Translate surface damage to global coordinates. */
			pixman_region32_translate(surface_damage,
			                          geom->x - view->buffer_offset_x,
			                          geom->y - view->buffer_offset_y);

			/* Add the surface damage to the compositor damage. */
			pixman_region32_union(&compositor.damage, &compositor.damage,
			                      surface_damage);
			pixman_region32_clear(surface_damage);
		}

		/* redraw entire thingy if border or decor changed */
		if (view->border.damaged_border1 || view->border.damaged_border2 ||
		    view->decor.damaged) {
			pixman_region32_t border_region, view_region;

			pixman_region32_init_with_extents(&border_region, &view->extents);
			pixman_region32_init_rect(&view_region, geom->x, geom->y,
			                          geom->width, geom->height);
			pixman_region32_subtract(&border_region, &border_region,
			                         &view_region);
			pixman_region32_fini(&view_region);

			pixman_region32_union(&compositor.damage, &compositor.damage,
			                      &border_region);

			pixman_region32_fini(&border_region);

			view->border.damaged_border1 = false;
			view->border.damaged_border2 = false;
			view->decor.damaged = false;
		}
	}

	pixman_region32_fini(&surface_opaque);
}

static void
update_screen(struct screen *screen)
{
	struct target *target;
	const struct swc_rectangle *geom = &screen->base.geometry;
	pixman_region32_t damage, *total_damage;
	uint64_t start = profile_render ? monotonic_us() : 0;

	if (!(compositor.scheduled_updates & screen_mask(screen))) {
		return;
	}

	if (!(target = target_get(screen))) {
		return;
	}

	pixman_region32_init(&damage);
	pixman_region32_intersect_rect(&damage, &compositor.damage, geom->x,
	                               geom->y, geom->width, geom->height);
	pixman_region32_translate(&damage, -geom->x, -geom->y);
	total_damage = wld_surface_damage(target->surface, &damage);

	/* Don't repaint the screen if it is waiting for a page flip. */
	if (compositor.pending_flips & screen_mask(screen)) {
		pixman_region32_fini(&damage);
		return;
	}

	/* check if zoom */
	if (profile_render) frame_draw = frame_finish = 0;
	if (compositor.overview_screen == &screen->base && !session_lock_active()) {
		if (!wld_set_target_surface(swc.backend->renderer, target->surface)) {
			pixman_region32_fini(&damage); return;
		}
		render_overview(screen, swc.backend->renderer);
		wld_flush(swc.backend->renderer);
		/* Screencopy consumes global damage, including on a second monitor. */
		pixman_region32_clear(&damage);
		pixman_region32_union_rect(&damage, &damage, geom->x, geom->y, geom->width, geom->height);
	} else if (compositor.zoom != 1.0f) {
		pixman_region32_clear(&damage);
		pixman_region32_union_rect(&damage, &damage, geom->x, geom->y, geom->width, geom->height);

		if (!wld_set_target_surface(swc.backend->renderer, target->surface)) {
			pixman_region32_fini(&damage);
			return;
		}
		render_zoomed(screen, swc.backend->renderer, compositor.zoom);
		wld_flush(swc.backend->renderer);
	} else {
		pixman_region32_t base_damage;
		pixman_region32_copy(&damage, total_damage);
		pixman_region32_translate(&damage, geom->x, geom->y);
		pixman_region32_init(&base_damage);
		pixman_region32_subtract(&base_damage, &damage, &compositor.opaque);
		renderer_repaint(target, &damage, &base_damage, &compositor.views,
		                 screen);
		pixman_region32_fini(&base_damage);
	}

	uint64_t rendered = profile_render ? monotonic_us() : 0;
	int swap_result = target_swap_buffers(target);
	uint64_t submitted = profile_render ? monotonic_us() : 0;
	if (swap_result == 0) {
		target->swap_failed = false;
		compositor.pending_flips |= screen_mask(screen);
		screencopy_handle_damage(screen, &damage);
	} else {
		/* This frame will never be presented, so its damage must come back. */
		target_restore_damage(target, geom);

		if (swap_result == -EACCES) {
			/* If we get an EACCES, it is because this session is being
			 * deactivated, but we haven't yet received the deactivate signal
			 * from swc-launch. */
			swc_deactivate();
		} else if (!target->swap_failed) {
			/* Retry once. A screen that keeps failing then waits for new
			 * damage instead of looping on the idle handler. */
			target->swap_failed = true;
			compositor.recover_updates |= screen_mask(screen);
		}
	}
	if (profile_render) {
		uint64_t end = monotonic_us();
		report_frame(screen, end - start, submitted - rendered, end - submitted);
	}
	pixman_region32_fini(&damage);
}

static void
perform_update(void *data)
{
	struct screen *screen;
	uint32_t updates = compositor.scheduled_updates & ~compositor.pending_flips;

	if (!swc.active || !updates) {
		return;
	}

	DEBUG("Performing update\n");

	compositor.updating = true;
	/*
	 * Descriptor exhaustion shows up as unrelated-looking failures all over the
	 * compositor -- client buffers that never arrive, dmabuf feedback that
	 * cannot be sent, synchronization fences that cannot be exported -- so
	 * sample the pressure here, where every frame passes, rather than waiting
	 * for one of those to be noticed. The sampling itself is rate limited.
	 */
	fd_pressure_check();
	uint64_t damage_start = profile_render ? monotonic_us() : 0;
	calculate_damage();
	if (profile_render) {
		static uint64_t last_report;
		uint64_t end = monotonic_us();
		if (end - damage_start >= 10000 && end - last_report >= 1000000) {
			fprintf(stderr, "render-profile: damage/proxy copies %.3f ms\n",
			        (end - damage_start) / 1000.0);
			last_report = end;
		}
	}

	wl_list_for_each(screen, &swc.screens, link) update_screen(screen);

	/* XXX: Should assert that all damage was covered by some output */
	pixman_region32_clear(&compositor.damage);
	compositor.scheduled_updates &= ~updates;
	compositor.updating = false;

	if (compositor.recover_updates) {
		uint32_t recover = compositor.recover_updates;

		compositor.recover_updates = 0;
		schedule_updates(recover);
	}
}

static void
bar_action(struct compositor_view *view, enum swc_titlebar_action action)
{
	struct window *w = view ? view->window : NULL;
	if (w && w->handler && w->handler->titlebar_action)
		w->handler->titlebar_action(w->handler_data, action);
}

static void
bar_pointer_at(int32_t x, int32_t y)
{
	struct compositor_view *view = view_at(x, y);
	int hit = titlebar_hit(view, x, y);
	struct compositor_view *hover = hit != -2 ? view : NULL;
	bool entered = hover && bar_grab.hover != hover;
	if (bar_grab.hover && bar_grab.hover != hover)
		titlebar_highlight(bar_grab.hover, -1, -1);
	bar_grab.hover = hover;
	if (hover) titlebar_highlight(hover, hit, -1);
	/* Decorations are compositor input: never send out-of-content coordinates
	 * or clicks to the application's wl_pointer. */
	pointer_set_focus(swc.seat->pointer, hover ? NULL : view);
	if (entered) bar_action(hover, SWC_TITLEBAR_FOCUS);
}

struct compositor_view *
compositor_view_at(int32_t x, int32_t y)
{
	return view_at(x, y);
}

void
compositor_refocus_pointer(void)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;

	if (!pointer) {
		return;
	}
	bar_pointer_at(wl_fixed_to_int(pointer->x), wl_fixed_to_int(pointer->y));
}

bool
handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx,
              wl_fixed_t fy)
{
	int32_t x = wl_fixed_to_int(fx), y = wl_fixed_to_int(fy);
	if (bar_grab.left_down) {
		struct compositor_view *view = bar_grab.pressed;
		if (view) {
			/* Re-checked per motion: a client can go maximized or
			 * fullscreen with the button still down, and a tiled window is
			 * not ours to place. */
			if (bar_grab.moving && view->window &&
			    view->window->movable &&
			    view->window->mode == WINDOW_MODE_STACKED)
				view_move(&view->base, x - bar_grab.offset_x, y - bar_grab.offset_y);
			else
				titlebar_highlight(view, view_at(x, y) == view ? titlebar_hit(view, x, y) : -1,
				                   bar_grab.button);
		}
		return true;
	}
	/* Preserve an application's implicit grab while any button is held. */
	if (swc.seat->pointer->buttons.size > 0) return false;
	bar_pointer_at(x, y);
	return bar_grab.hover != NULL;
}

static bool
handle_button(struct pointer_handler *handler, uint32_t time,
              struct button *button, uint32_t state)
{
	struct pointer *pointer = swc.seat->pointer;
	int32_t x = wl_fixed_to_int(pointer->x), y = wl_fixed_to_int(pointer->y);
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		if (button->press.value == BTN_LEFT && bar_grab.left_down) {
			struct compositor_view *view = bar_grab.pressed;
			int hit = titlebar_hit(view, x, y);
			int pressed = bar_grab.button;
			bool activate = view && pressed >= 0 && hit == pressed && view_at(x, y) == view;
			enum swc_titlebar_action action = activate ? view->decor.titlebar.buttons[pressed] : SWC_TITLEBAR_FOCUS;
			struct compositor_view *moved = bar_grab.moving ? view : NULL;
			bar_grab.left_down = false;
			bar_grab.moving = false;
			bar_grab.pressed = NULL;
			if (view) titlebar_highlight(view, hit, -1);
			/* Clear the grab before the action, which can hide/destroy its view. */
			if (activate) bar_action(view, action);
			/* A button press is never a drag, so the view a finished move
			 * reports back is one no action has just closed. */
			if (moved) bar_move_notify(moved, false);
		}
		/* The releasing button is still in the array until we return. */
		if (pointer->buttons.size == sizeof(struct button)) bar_pointer_at(x, y);
		return true;
	}
	if (bar_grab.left_down) return true;
	/* A second press during a client grab must remain with that client. */
	if (pointer->buttons.size > sizeof(struct button)) return false;
	struct compositor_view *view = view_at(x, y);
	int hit = titlebar_hit(view, x, y);
	if (hit != -2) {
		bar_pointer_at(x, y);
		bar_action(view, SWC_TITLEBAR_FOCUS);
		raise_window_from_click(view);
		if (button->press.value == BTN_LEFT) {
			bar_grab.left_down = true;
			bar_grab.pressed = view;
			bar_grab.button = hit;
			bar_grab.offset_x = x - view->base.geometry.x;
			bar_grab.offset_y = y - view->base.geometry.y;
			bar_grab.moving = hit == -1 && view->window &&
			                  view->window->movable &&
			                  view->window->mode == WINDOW_MODE_STACKED;
			titlebar_highlight(view, hit, hit);
			if (bar_grab.moving) bar_move_notify(view, true);
		}
		return true;
	}
	bar_pointer_at(x, y);
	layer_shell_handle_pointer_press(view);
	raise_window_from_click(view);
	return false;
}

static void
handle_terminate(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		wl_display_terminate(swc.display);
	}
}

static void
handle_switch_vt(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	uint8_t vt = value - XKB_KEY_XF86Switch_VT_1 + 1;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		launch_activate_vt(vt);
	}
}

static void
handle_swc_event(struct wl_listener *listener, void *data)
{
	struct event *event = data;

	switch (event->type) {
	case SWC_EVENT_ACTIVATED:
		schedule_updates(-1);
		break;
	case SWC_EVENT_DEACTIVATED: {
		struct compositor_view *moved = bar_grab.moving ? bar_grab.pressed : NULL;

		if (bar_grab.pressed) titlebar_highlight(bar_grab.pressed, -1, -1);
		if (bar_grab.hover) titlebar_highlight(bar_grab.hover, -1, -1);
		bar_grab.hover = bar_grab.pressed = NULL;
		bar_grab.left_down = false;
		bar_grab.moving = false;
		compositor.scheduled_updates = 0;
		compositor.recover_updates = 0;
		/* Switching away mid-drag drops the button, so the move is over. */
		if (moved) bar_move_notify(moved, false);
		break;
	}
	}
}

static void
create_surface(struct wl_client *client, struct wl_resource *resource,
               uint32_t id)
{
	struct surface *surface;

	/* Initialize surface. */
	surface = surface_new(client, wl_resource_get_version(resource), id);

	if (!surface) {
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_signal_emit(&swc_compositor.signal.new_surface, surface);
}

static void
create_region(struct wl_client *client, struct wl_resource *resource,
              uint32_t id)
{
	if (!region_new(client, wl_resource_get_version(resource), id)) {
		wl_resource_post_no_memory(resource);
	}
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = create_surface,
    .create_region = create_region,
};

static void
bind_compositor(struct wl_client *client, void *data, uint32_t version,
                uint32_t id)
{
	struct wl_resource *resource;

	resource =
	    wl_resource_create(client, &wl_compositor_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, NULL, NULL);
}

bool
compositor_initialize(void)
{
	compositor_started = monotonic_us();
	const char *render_profile = getenv("SWC_RENDER_PROFILE");
	const char *input_profile = getenv("SWC_INPUT_PROFILE");
	profile_render = (render_profile && !strcmp(render_profile, "1")) ||
	                 (input_profile && !strcmp(input_profile, "1"));
	struct screen *screen;
	uint32_t keysym;

	compositor.global = wl_global_create(swc.display, &wl_compositor_interface,
	                                     4, NULL, &bind_compositor);

	if (!compositor.global) {
		return false;
	}

	compositor.scheduled_updates = 0;
	compositor.recover_updates = 0;
	compositor.pending_flips = 0;
	compositor.updating = false;
	compositor.zoom = 1.0f;
	if (!decor_initialize()) {
		return false;
	}
	pixman_region32_init(&compositor.damage);
	pixman_region32_init(&compositor.opaque);
	wl_list_init(&compositor.views);
	wl_signal_init(&swc_compositor.signal.new_surface);
	compositor.swc_listener.notify = &handle_swc_event;
	wl_signal_add(&swc.event_signal, &compositor.swc_listener);

	wl_list_for_each(screen, &swc.screens, link) target_new(screen);
	if (swc.active) {
		schedule_updates(-1);
	}

	swc_add_binding(SWC_BINDING_KEY, SWC_MOD_CTRL | SWC_MOD_ALT,
	                XKB_KEY_BackSpace, &handle_terminate, NULL);

	for (keysym = XKB_KEY_XF86Switch_VT_1; keysym <= XKB_KEY_XF86Switch_VT_12;
	     ++keysym) {
		swc_add_binding(SWC_BINDING_KEY, SWC_MOD_ANY, keysym, &handle_switch_vt,
		                NULL);
	}

	compositor.initialized = true;

	return true;
}

void
compositor_finalize(void)
{
	wallpaper_finalize();
	screencopy_finalize();
	compositor_release_capture_cache();
	compositor.initialized = false;

	decor_finalize();
	pixman_region32_fini(&compositor.damage);
	pixman_region32_fini(&compositor.opaque);
	wl_global_destroy(compositor.global);
}

struct wld_buffer *
compositor_get_buffer(struct screen *screen)
{
	struct target *target = target_get(screen);
	if (!target) {
		return NULL;
	}
	return target->current_buffer;
}

/* Borrow the newest fully rendered output, including a submitted page flip.
 * The main renderer has finished its writes before target_swap_buffers().
 * Callers use it synchronously; the scanout buffer must never be modified. */
struct wld_buffer *
compositor_capture_buffer(struct screen *screen)
{
	struct target *target = target_get(screen);
	if (!swc.active || !target) return NULL;
	struct wld_buffer *buffer = (compositor.pending_flips & target->mask) ?
	    target->next_buffer : target->current_buffer;
	if (!buffer || buffer->width != screen->base.geometry.width ||
	    buffer->height != screen->base.geometry.height) return NULL;
	return buffer;
}

/*
 * Buffers reused across captures.
 *
 * A screencast asks for a frame many times a second, and allocating a scanout
 * buffer (with its dmabuf export, EGLImage and texture) plus an shm buffer
 * every time costs far more than the capture itself.
 */
static struct {
	struct wld_buffer *scratch, *shm;
	uint32_t width, height;
} capture_cache;

static void
capture_cache_reset(uint32_t width, uint32_t height)
{
	if (capture_cache.width == width && capture_cache.height == height)
		return;

	if (capture_cache.scratch) {
		wld_buffer_unreference(capture_cache.scratch);
		capture_cache.scratch = NULL;
	}
	if (capture_cache.shm) {
		wld_buffer_unreference(capture_cache.shm);
		capture_cache.shm = NULL;
	}
	capture_cache.width = width;
	capture_cache.height = height;
}

void
compositor_release_capture_cache(void)
{
	capture_cache_reset(0, 0);
}

/*
 * Draw the screen's views at their own geometry, for a capture. The live path
 * is renderer_repaint(), which works from accumulated damage; this one is
 * handed the whole screen and has no target surface to swap.
 */
static void
render_scene(struct screen *screen, struct wld_renderer *renderer,
             pixman_region32_t *region, pixman_region32_t *damage)
{
	struct compositor_view *view;

	/* background */
	wallpaper_repaint(screen, renderer, region);

	wl_list_for_each_reverse(view, &compositor.views, link)
	{
		struct wld_buffer *src = view->buffer;

		if (!view->visible) {
			continue;
		}

		if (src &&
		    !(wld_capabilities(renderer, src) & WLD_CAPABILITY_READ)) {
			src = view->base.buffer;
		}
		if (src &&
		    !(wld_capabilities(renderer, src) & WLD_CAPABILITY_READ)) {
			src = NULL;
		}

		/* No clip: it was computed for the last frame, and a capture can
		 * come after views moved. Drawing bottom-up covers the same ground. */
		paint_view(renderer, &screen->base.geometry, view, src, damage, NULL);
	}
}

struct wld_buffer *
compositor_render_to_shm(struct screen *screen)
{
	uint32_t width = screen->base.geometry.width;
	uint32_t height = screen->base.geometry.height;
	struct wld_buffer *buffer, *scratch = NULL;
	struct wld_renderer *renderer = swc.shm->renderer;
	pixman_region32_t region;
	pixman_region32_t damage;
	uint32_t caps;

	capture_cache_reset(width, height);

	if (!capture_cache.shm) {
		capture_cache.shm = wld_create_buffer(swc.shm->context, width, height,
		                                      WLD_FORMAT_ARGB8888,
		                                      WLD_FLAG_MAP);
	}
	buffer = capture_cache.shm;
	if (!buffer) {
		return NULL;
	}
	/* The caller drops a reference; the cache keeps its own. */
	wld_buffer_reference(buffer);

	/*
	 * Composite with the backend renderer where we can, and read the result
	 * back afterwards.
	 *
	 * The shm renderer can only draw a window whose buffer it can map, and a
	 * buffer imported from a GPU client is not CPU accessible. Those windows
	 * are simply skipped, so a screenshot shows the desktop with every
	 * hardware-accelerated window missing, even though they are on screen.
	 */
	if (swc.backend->renderer && swc.backend->context) {
		if (!capture_cache.scratch) {
			capture_cache.scratch = wld_create_buffer(
			    swc.backend->context, width, height, WLD_FORMAT_XRGB8888,
			    WLD_DRM_FLAG_SCANOUT);
		}
		scratch = capture_cache.scratch;
		if (scratch && wld_set_target_buffer(swc.backend->renderer, scratch))
			renderer = swc.backend->renderer;
		else
			scratch = NULL;
	}

	if (!scratch) {
		caps = wld_capabilities(renderer, buffer);
		if (!(caps & WLD_CAPABILITY_WRITE) ||
		    !wld_set_target_buffer(renderer, buffer)) {
			wld_buffer_unreference(buffer);
			return NULL;
		}
	}

	/* set region */
	pixman_region32_init_rect(&region, 0, 0, width, height);
	pixman_region32_init_rect(&damage, screen->base.geometry.x,
	                          screen->base.geometry.y, width, height);

	if (compositor.overview_screen == &screen->base && !session_lock_active()) {
		render_overview(screen, renderer);
	} else if (compositor.zoom != 1.0f) {
		/*
		 * The zoomed scene is drawn whole and its views land somewhere other
		 * than their own geometry, so it has its own pass rather than sharing
		 * the one above.
		 */
		render_zoomed(screen, renderer, compositor.zoom);
	} else {
		render_scene(screen, renderer, &region, &damage);
	}

	draw_overlays(renderer, &screen->base.geometry);

	if (scratch) {
		bool ok = false;

		if (wld_map(buffer)) {
			ok = wld_read_pixels(renderer, 0, 0, width, height, buffer->pitch,
			                     buffer->map);
			wld_unmap(buffer);
		}
		/* wld_flush() drops the target, so this has to come after the read. */
		wld_flush(renderer);

		if (!ok) {
			ERROR("Could not read back the composited screen\n");
			wld_buffer_unreference(buffer);
			buffer = NULL;
		}
	} else {
		wld_flush(renderer);
	}

	pixman_region32_fini(&region);
	pixman_region32_fini(&damage);

	return buffer;
}
