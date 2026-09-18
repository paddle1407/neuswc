/* swc: libswc/xdg_shell.c
 *
 * Copyright (c) 2014, 2018 Michael Forney
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

#include "xdg_shell.h"
#include "compositor.h"
#include "internal.h"
#include "output.h"
#include "screen.h"
#include "seat.h"
#include "surface.h"
#include "util.h"
#include "window.h"

#include "xdg-shell-server-protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server.h>

struct xdg_surface {
	struct wl_resource *resource, *role;
	struct surface *surface;
	struct wl_listener surface_destroy_listener, role_destroy_listener;
	uint32_t configure_serial;
	enum {
		XDG_ROLE_NONE,
		XDG_ROLE_TOPLEVEL,
		XDG_ROLE_POPUP,
	} role_type;
};

struct xdg_positioner {
	int32_t width, height;
	int32_t anchor_x, anchor_y;
	int32_t anchor_width, anchor_height;
	enum xdg_positioner_anchor anchor;
	enum xdg_positioner_gravity gravity;
	enum xdg_positioner_constraint_adjustment constraint;
	int32_t offset_x, offset_y;

	/* Version 3. The parent size and the configure it belongs to let a
	 * client describe the geometry it positioned against, so that a
	 * compositor constraining the popup knows what it was aiming at. A
	 * reactive popup expects to be repositioned when that geometry moves. */
	bool reactive;
	bool has_parent_size;
	int32_t parent_width, parent_height;
	bool has_parent_configure;
	uint32_t parent_configure;
};

struct toplevel_configure {
	struct wl_list link;
	uint32_t serial;
	uint64_t generation;
};

struct xdg_toplevel {
	struct window window;
	struct wl_resource *resource;
	struct wl_array states;
	struct wl_list configures;
	struct wl_event_source *configure_idle;
	uint64_t generation;
	unsigned configure_count;
	bool initial_commit, destroying;
	struct xdg_surface *xdg_surface;
	struct wl_listener surface_commit_listener;
	struct {
		uint32_t min_width, min_height;
		uint32_t max_width, max_height;
		bool min_dirty, max_dirty;
	} pending;
};

struct xdg_popup {
	struct wl_resource *resource;
	struct xdg_surface *xdg_surface;
	struct xdg_positioner positioner;
	struct compositor_view *view;
	struct compositor_view *parent;
	struct wl_listener parent_destroy_listener;
	bool configured;
};

static void queue_configure(struct xdg_toplevel *toplevel);
static void send_wm_capabilities(struct xdg_toplevel *toplevel);

/* xdg_positioner */
static void
destroy_positioner(struct wl_resource *resource)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	free(positioner);
}

static void
set_size(struct wl_client *client, struct wl_resource *resource, int32_t width,
         int32_t height)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "invalid size");
		return;
	}
	positioner->width = width;
	positioner->height = height;
}

static void
set_anchor_rect(struct wl_client *client, struct wl_resource *resource,
                int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "invalid anchor size");
		return;
	}
	positioner->anchor_x = x;
	positioner->anchor_y = y;
	positioner->anchor_width = width;
	positioner->anchor_height = height;
}

static void
set_anchor(struct wl_client *client, struct wl_resource *resource,
           uint32_t anchor)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	positioner->anchor = anchor;
}

static void
set_gravity(struct wl_client *client, struct wl_resource *resource,
            uint32_t gravity)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	positioner->gravity = gravity;
}

static void
set_constraint_adjustment(struct wl_client *client,
                          struct wl_resource *resource, uint32_t constraint)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	positioner->constraint = constraint;
}

static void
set_offset(struct wl_client *client, struct wl_resource *resource, int32_t x,
           int32_t y)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	positioner->offset_x = x;
	positioner->offset_y = y;
}

static void
set_reactive(struct wl_client *client, struct wl_resource *resource)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	(void)client;
	positioner->reactive = true;
}

static void
set_parent_size(struct wl_client *client, struct wl_resource *resource,
                int32_t width, int32_t height)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	(void)client;
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "invalid parent size");
		return;
	}
	positioner->has_parent_size = true;
	positioner->parent_width = width;
	positioner->parent_height = height;
}

static void
set_parent_configure(struct wl_client *client, struct wl_resource *resource,
                     uint32_t serial)
{
	struct xdg_positioner *positioner = wl_resource_get_user_data(resource);

	(void)client;
	positioner->has_parent_configure = true;
	positioner->parent_configure = serial;
}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = destroy_resource,
    .set_size = set_size,
    .set_anchor_rect = set_anchor_rect,
    .set_anchor = set_anchor,
    .set_gravity = set_gravity,
    .set_constraint_adjustment = set_constraint_adjustment,
    .set_offset = set_offset,
    .set_reactive = set_reactive,
    .set_parent_size = set_parent_size,
    .set_parent_configure = set_parent_configure,
};

static struct swc_rectangle
unadjusted_position(const struct xdg_positioner *positioner)
{
	/*
	 * Every input here is a client-controlled int32. Accumulating in int64
	 * and clamping once at the end keeps the arithmetic defined; the worst a
	 * client gets for absurd values is a popup at the edge of the coordinate
	 * space instead of signed overflow.
	 */
	int64_t x = positioner->offset_x, y = positioner->offset_y;
	int64_t width = positioner->width, height = positioner->height;

	switch (positioner->anchor) {
	case XDG_POSITIONER_ANCHOR_TOP:
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
		y += positioner->anchor_y;
		break;
	case XDG_POSITIONER_ANCHOR_BOTTOM:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		y += (int64_t)positioner->anchor_y + positioner->anchor_height;
		break;
	default:
		y += (int64_t)positioner->anchor_y + positioner->anchor_height / 2;
	}
	switch (positioner->anchor) {
	case XDG_POSITIONER_ANCHOR_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
		x += positioner->anchor_x;
		break;
	case XDG_POSITIONER_ANCHOR_RIGHT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		x += (int64_t)positioner->anchor_x + positioner->anchor_width;
		break;
	default:
		x += (int64_t)positioner->anchor_x + positioner->anchor_width / 2;
	}

	switch (positioner->gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		y -= height;
		break;
	case XDG_POSITIONER_GRAVITY_BOTTOM:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		break;
	default:
		y -= height / 2;
	}
	switch (positioner->gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		x -= width;
		break;
	case XDG_POSITIONER_GRAVITY_RIGHT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		break;
	default:
		x -= width / 2;
	}

	return (struct swc_rectangle){
	    .x = (int32_t)MIN(MAX(x, (int64_t)INT32_MIN), (int64_t)INT32_MAX),
	    .y = (int32_t)MIN(MAX(y, (int64_t)INT32_MIN), (int64_t)INT32_MAX),
	    .width = positioner->width,
	    .height = positioner->height,
	};
}

/*
 * Flipping is per axis: the anchor and the gravity both move to the opposite
 * side and everything else in the positioner is left alone, so the popup ends
 * up mirrored about the anchor rectangle.
 */
static enum xdg_positioner_anchor
anchor_flip_x(enum xdg_positioner_anchor anchor)
{
	switch (anchor) {
	case XDG_POSITIONER_ANCHOR_LEFT:
		return XDG_POSITIONER_ANCHOR_RIGHT;
	case XDG_POSITIONER_ANCHOR_RIGHT:
		return XDG_POSITIONER_ANCHOR_LEFT;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
		return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
		return XDG_POSITIONER_ANCHOR_TOP_LEFT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
		return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
	default:
		return anchor;
	}
}

static enum xdg_positioner_anchor
anchor_flip_y(enum xdg_positioner_anchor anchor)
{
	switch (anchor) {
	case XDG_POSITIONER_ANCHOR_TOP:
		return XDG_POSITIONER_ANCHOR_BOTTOM;
	case XDG_POSITIONER_ANCHOR_BOTTOM:
		return XDG_POSITIONER_ANCHOR_TOP;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
		return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
		return XDG_POSITIONER_ANCHOR_TOP_LEFT;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
		return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
	default:
		return anchor;
	}
}

static enum xdg_positioner_gravity
gravity_flip_x(enum xdg_positioner_gravity gravity)
{
	switch (gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT:
		return XDG_POSITIONER_GRAVITY_RIGHT;
	case XDG_POSITIONER_GRAVITY_RIGHT:
		return XDG_POSITIONER_GRAVITY_LEFT;
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
		return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		return XDG_POSITIONER_GRAVITY_TOP_LEFT;
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
	default:
		return gravity;
	}
}

static enum xdg_positioner_gravity
gravity_flip_y(enum xdg_positioner_gravity gravity)
{
	switch (gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:
		return XDG_POSITIONER_GRAVITY_BOTTOM;
	case XDG_POSITIONER_GRAVITY_BOTTOM:
		return XDG_POSITIONER_GRAVITY_TOP;
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
		return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		return XDG_POSITIONER_GRAVITY_TOP_LEFT;
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
	default:
		return gravity;
	}
}

static int32_t
clamp_i32(int64_t value)
{
	return (int32_t)MIN(MAX(value, (int64_t)INT32_MIN), (int64_t)INT32_MAX);
}

/*
 * The area the popup has to fit inside: the usable part of the screen it is
 * opening on. That is the screen under the middle of the anchor rectangle --
 * the point the menu is being pulled out of -- falling back to a screen the
 * parent is on, and finally to any screen at all. A window straddling two
 * monitors would otherwise have its menus constrained to the wrong one.
 */
static bool
constraint_box(const struct xdg_positioner *positioner,
               struct compositor_view *parent, struct swc_rectangle *box)
{
	struct screen *screen, *found = NULL;
	int32_t x, y;

	x = clamp_i32((int64_t)parent->base.geometry.x + positioner->anchor_x
	              + positioner->anchor_width / 2);
	y = clamp_i32((int64_t)parent->base.geometry.y + positioner->anchor_y
	              + positioner->anchor_height / 2);

	wl_list_for_each(screen, &swc.screens, link) {
		if (rectangle_contains_point(&screen->base.geometry, x, y)) {
			found = screen;
			break;
		}
	}
	if (!found) {
		wl_list_for_each(screen, &swc.screens, link) {
			if (parent->base.screens & screen_mask(screen)) {
				found = screen;
				break;
			}
		}
	}
	if (!found && !wl_list_empty(&swc.screens))
		found = wl_container_of(swc.screens.next, found, link);
	if (!found)
		return false;

	*box = found->base.usable_geometry;
	/* Before a panel has reported its exclusive zone there is nothing here. */
	if (box->width == 0 || box->height == 0)
		*box = found->base.geometry;

	return box->width > 0 && box->height > 0;
}

static bool
fits(int64_t position, int64_t size, int64_t min, int64_t max)
{
	return position >= min && position + size <= max;
}

/*
 * Anchor and gravity say where the client would like the popup; they say
 * nothing about the screen it has to land on, so a menu opened near an edge is
 * placed partly outside it, where it can be neither seen nor clicked. What the
 * client will accept instead is in set_constraint_adjustment, and xdg-shell
 * applies those per axis in a fixed order: flip to the other side of the
 * anchor, slide along the edge, then shrink.
 *
 * The returned rectangle stays in the parent's window-geometry coordinates,
 * which is what xdg_popup.configure reports.
 */
static struct swc_rectangle
calculate_position(const struct xdg_positioner *positioner,
                   struct compositor_view *parent)
{
	struct swc_rectangle rect = unadjusted_position(positioner);
	struct xdg_positioner flipped;
	struct swc_rectangle box;
	int64_t px, py, x, y, w, h, min, max, candidate;

	if (!parent || rect.width == 0 || rect.height == 0
	    || !constraint_box(positioner, parent, &box)) {
		return rect;
	}

	px = parent->base.geometry.x;
	py = parent->base.geometry.y;
	x = px + rect.x;
	y = py + rect.y;
	w = rect.width;
	h = rect.height;

	min = box.x;
	max = (int64_t)box.x + box.width;
	if (!fits(x, w, min, max)
	    && (positioner->constraint
	        & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X)) {
		flipped = *positioner;
		flipped.anchor = anchor_flip_x(positioner->anchor);
		flipped.gravity = gravity_flip_x(positioner->gravity);
		candidate = px + unadjusted_position(&flipped).x;
		/* A flip that is constrained too is no improvement; the protocol
		 * says to keep the original in that case. */
		if (fits(candidate, w, min, max))
			x = candidate;
	}
	if (!fits(x, w, min, max)
	    && (positioner->constraint
	        & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X)) {
		if (x + w > max)
			x = max - w;
		if (x < min)
			x = min;
	}
	if (!fits(x, w, min, max)
	    && (positioner->constraint
	        & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X)) {
		if (x < min) {
			w -= min - x;
			x = min;
		}
		if (x + w > max)
			w = max - x;
		if (w < 1)
			w = 1;
	}
	/*
	 * Whatever the client allowed, a popup off the edge of every screen is
	 * one the user cannot reach, so the last step is to slide it back on
	 * regardless. A popup wider than the screen keeps its left edge visible.
	 */
	if (x + w > max)
		x = max - w;
	if (x < min)
		x = min;

	min = box.y;
	max = (int64_t)box.y + box.height;
	if (!fits(y, h, min, max)
	    && (positioner->constraint
	        & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y)) {
		flipped = *positioner;
		flipped.anchor = anchor_flip_y(positioner->anchor);
		flipped.gravity = gravity_flip_y(positioner->gravity);
		candidate = py + unadjusted_position(&flipped).y;
		if (fits(candidate, h, min, max))
			y = candidate;
	}
	if (!fits(y, h, min, max)
	    && (positioner->constraint
	        & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)) {
		if (y + h > max)
			y = max - h;
		if (y < min)
			y = min;
	}
	if (!fits(y, h, min, max)
	    && (positioner->constraint
	        & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)) {
		if (y < min) {
			h -= min - y;
			y = min;
		}
		if (y + h > max)
			h = max - y;
		if (h < 1)
			h = 1;
	}
	if (y + h > max)
		y = max - h;
	if (y < min)
		y = min;

	return (struct swc_rectangle){
	    .x = clamp_i32(x - px),
	    .y = clamp_i32(y - py),
	    .width = (uint32_t)w,
	    .height = (uint32_t)h,
	};
}

/* xdg_toplevel */
static void
handle_toplevel_surface_commit(struct wl_listener *listener, void *data)
{
	struct xdg_toplevel *toplevel =
	    wl_container_of(listener, toplevel, surface_commit_listener);
	uint32_t min_width, min_height, max_width, max_height;

	if (!toplevel->initial_commit) {
		toplevel->initial_commit = true;
		queue_configure(toplevel);
	}
	(void)data;
	/* A state-only commit can acknowledge a configure without a new buffer. */
	window_commit(&toplevel->window);
	min_width = toplevel->pending.min_dirty ? toplevel->pending.min_width
	                                         : toplevel->window.base.min_width;
	min_height = toplevel->pending.min_dirty ? toplevel->pending.min_height
	                                          : toplevel->window.base.min_height;
	max_width = toplevel->pending.max_dirty ? toplevel->pending.max_width
	                                         : toplevel->window.base.max_width;
	max_height = toplevel->pending.max_dirty ? toplevel->pending.max_height
	                                          : toplevel->window.base.max_height;
	if ((min_width && max_width && min_width > max_width) ||
	    (min_height && max_height && min_height > max_height)) {
		wl_resource_post_error(toplevel->resource,
		                       XDG_TOPLEVEL_ERROR_INVALID_SIZE,
		                       "minimum size exceeds maximum size");
		return;
	}
	if (toplevel->pending.min_dirty) {
		toplevel->window.base.min_width = toplevel->pending.min_width;
		toplevel->window.base.min_height = toplevel->pending.min_height;
		toplevel->pending.min_dirty = false;
	}
	if (toplevel->pending.max_dirty) {
		toplevel->window.base.max_width = toplevel->pending.max_width;
		toplevel->window.base.max_height = toplevel->pending.max_height;
		toplevel->pending.max_dirty = false;
	}
}

static void
destroy_toplevel(struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	toplevel->destroying = true;
	if (toplevel->configure_idle) wl_event_source_remove(toplevel->configure_idle);
	struct toplevel_configure *c, *tmp;
	wl_list_for_each_safe(c, tmp, &toplevel->configures, link) {
		wl_list_remove(&c->link);
		free(c);
	}
	wl_list_remove(&toplevel->surface_commit_listener.link);
	window_finalize(&toplevel->window);
	wl_array_release(&toplevel->states);
	free(toplevel);
}

static bool
add_state(struct xdg_toplevel *toplevel, uint32_t state)
{
	uint32_t *current_state;

	wl_array_for_each(current_state, &toplevel->states)
	{
		if (*current_state == state) {
			return false;
		}
	}

	if (!(current_state = wl_array_add(&toplevel->states, sizeof(state)))) {
		WARNING("xdg_toplevel: Failed to allocate new state\n");
		return false;
	}

	*current_state = state;
	return true;
}

static bool
remove_state(struct xdg_toplevel *toplevel, uint32_t state)
{
	uint32_t *current_state;

	wl_array_for_each(current_state, &toplevel->states)
	{
		if (*current_state == state) {
			array_remove(&toplevel->states, current_state, sizeof(state));
			return true;
		}
	}

	return false;
}

/*
 * Tell the client how large it can usefully be before it picks a size of its
 * own -- the usable area of the monitor it is on, so a window that wants to
 * open "as big as sensible" does not pick something taller than the screen
 * minus the bar. Sent before the configure it belongs to, as the protocol
 * requires.
 */
static void
send_configure_bounds(struct xdg_toplevel *toplevel)
{
	struct screen *screen;
	const struct swc_rectangle *geom;
	uint32_t screens;

	if (wl_resource_get_version(toplevel->resource) <
	    XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION) {
		return;
	}

	screens = toplevel->window.view ? toplevel->window.view->base.screens : 0;
	wl_list_for_each(screen, &swc.screens, link)
	{
		if (screens && !(screens & screen_mask(screen))) {
			continue;
		}
		geom = &screen->base.usable_geometry;
		xdg_toplevel_send_configure_bounds(toplevel->resource,
		                                   (int32_t)geom->width,
		                                   (int32_t)geom->height);
		return;
	}
}

static void
send_configure(void *data)
{
	struct xdg_toplevel *toplevel = data;
	struct window *window = &toplevel->window;
	toplevel->configure_idle = NULL;
	/* Bound memory if a client stops acknowledging configure events. */
	if (toplevel->configure_count >= 1024) {
		wl_resource_post_no_memory(toplevel->resource);
		return;
	}
	struct toplevel_configure *c = calloc(1, sizeof(*c));
	if (!c) { wl_resource_post_no_memory(toplevel->resource); return; }
	c->serial = wl_display_next_serial(swc.display);
	c->generation = toplevel->generation;
	wl_list_insert(toplevel->configures.prev, &c->link);
	++toplevel->configure_count;
	uint32_t width = window->configure.pending ? window->configure.width : window->view->base.geometry.width;
	uint32_t height = window->configure.pending ? window->configure.height : window->view->base.geometry.height;
	send_configure_bounds(toplevel);
	xdg_toplevel_send_configure(toplevel->resource, width, height, &toplevel->states);
	xdg_surface_send_configure(toplevel->xdg_surface->resource, c->serial);
}

static void
queue_configure(struct xdg_toplevel *toplevel)
{
	if (toplevel->destroying || !toplevel->initial_commit || toplevel->configure_idle) return;
	toplevel->configure_idle = wl_event_loop_add_idle(swc.event_loop, send_configure, toplevel);
	if (!toplevel->configure_idle) wl_resource_post_no_memory(toplevel->resource);
}

static void
configure(struct window *window, uint32_t width, uint32_t height)
{
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);
	window->configure.width = width;
	window->configure.height = height;
	window->configure.pending = true;
	window->configure.acknowledged = false;
	++toplevel->generation;
	queue_configure(toplevel);
}

static void
focus(struct window *window)
{
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);
	if (add_state(toplevel, XDG_TOPLEVEL_STATE_ACTIVATED)) queue_configure(toplevel);
}

static void
unfocus(struct window *window)
{
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);
	if (remove_state(toplevel, XDG_TOPLEVEL_STATE_ACTIVATED)) queue_configure(toplevel);
}

static void
close_(struct window *window)
{
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);

	xdg_toplevel_send_close(toplevel->resource);
}

/*
 * The tiled states, from the window manager's SWC_WINDOW_EDGE_* bits.
 *
 * A window whose size the window manager owns is told so with the four tiled
 * states rather than with "maximized": a client that believes it is maximized
 * offers to restore itself and, more visibly, a client drawing its own frame
 * keeps its rounded corners and its drop shadow, which inside a tile looks
 * like a mistake. The tiled states say which edges it is up against, and a
 * client squares off exactly those. They arrived in version 2, so a client
 * older than that falls back to being told it is maximized.
 *
 * With no edges named at all -- a lone window filling its workspace -- there
 * is nothing to square off against, and maximized is both true and the only
 * thing such a client would understand.
 */
static bool
set_tiled_states(struct xdg_toplevel *toplevel, uint32_t edges)
{
	static const struct {
		uint32_t edge, state;
	} map[] = {
		{ SWC_WINDOW_EDGE_LEFT, XDG_TOPLEVEL_STATE_TILED_LEFT },
		{ SWC_WINDOW_EDGE_RIGHT, XDG_TOPLEVEL_STATE_TILED_RIGHT },
		{ SWC_WINDOW_EDGE_TOP, XDG_TOPLEVEL_STATE_TILED_TOP },
		{ SWC_WINDOW_EDGE_BOTTOM, XDG_TOPLEVEL_STATE_TILED_BOTTOM },
	};
	bool tiled = edges && wl_resource_get_version(toplevel->resource) >=
	                          XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION;
	bool changed = false;

	for (unsigned i = 0; i < ARRAY_LENGTH(map); ++i) {
		if (tiled && (edges & map[i].edge)) {
			changed |= add_state(toplevel, map[i].state);
		} else {
			changed |= remove_state(toplevel, map[i].state);
		}
	}
	/* Maximized stays for the clients that cannot be told anything else. */
	if (tiled) {
		changed |= remove_state(toplevel, XDG_TOPLEVEL_STATE_MAXIMIZED);
	} else {
		changed |= add_state(toplevel, XDG_TOPLEVEL_STATE_MAXIMIZED);
	}
	return changed;
}

static void
clear_tiled_states(struct xdg_toplevel *toplevel)
{
	remove_state(toplevel, XDG_TOPLEVEL_STATE_MAXIMIZED);
	remove_state(toplevel, XDG_TOPLEVEL_STATE_TILED_LEFT);
	remove_state(toplevel, XDG_TOPLEVEL_STATE_TILED_RIGHT);
	remove_state(toplevel, XDG_TOPLEVEL_STATE_TILED_TOP);
	remove_state(toplevel, XDG_TOPLEVEL_STATE_TILED_BOTTOM);
}

static void
set_mode(struct window *window, unsigned mode)
{
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);

	switch (window->mode) {
	case WINDOW_MODE_TILED:
		clear_tiled_states(toplevel);
		break;
	case WINDOW_MODE_FULLSCREEN:
		remove_state(toplevel, XDG_TOPLEVEL_STATE_FULLSCREEN);
		break;
	}

	switch (mode) {
	case WINDOW_MODE_TILED:
		set_tiled_states(toplevel, window->tiled_edges);
		break;
	case WINDOW_MODE_FULLSCREEN:
		add_state(toplevel, XDG_TOPLEVEL_STATE_FULLSCREEN);
		break;
	}

	++toplevel->generation;
	window->configure.acknowledged = false;
	queue_configure(toplevel);
}

/*
 * A tiled window's edges changed without its mode changing.
 *
 * Unlike set_mode this leaves the pending acknowledgement alone: the window is
 * already tiled and only the states are being corrected, so there is no reason
 * to throw away a configure the window manager is waiting on.
 */
static void
set_tiled_edges(struct window *window, uint32_t edges)
{
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);

	if (window->mode != WINDOW_MODE_TILED) {
		return;
	}
	if (set_tiled_states(toplevel, edges)) {
		++toplevel->generation;
		queue_configure(toplevel);
	}
}

static const struct window_impl toplevel_window_impl = {
    .configure = configure,
    .focus = focus,
    .unfocus = unfocus,
    .close = close_,
    .set_mode = set_mode,
    .set_tiled_edges = set_tiled_edges,
};

static void
set_parent(struct wl_client *client, struct wl_resource *resource,
           struct wl_resource *parent_resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource),
	                    *parent = NULL;

	if (parent_resource) {
		parent = wl_resource_get_user_data(parent_resource);
	}
	window_set_parent(&toplevel->window, parent ? &parent->window : NULL);
}

static void
set_title(struct wl_client *client, struct wl_resource *resource,
          const char *title)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	window_set_title(&toplevel->window, title, -1);
}

static void
set_app_id(struct wl_client *client, struct wl_resource *resource,
           const char *app_id)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	window_set_app_id(&toplevel->window, app_id);
}

static void
show_window_menu(struct wl_client *client, struct wl_resource *resource,
                 struct wl_resource *seat, uint32_t serial, int32_t x,
                 int32_t y)
{
}

static void
move(struct wl_client *client, struct wl_resource *resource,
     struct wl_resource *seat, uint32_t serial)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct button *button;

	button = pointer_get_button(swc.seat->pointer, serial);
	if (button) {
		window_begin_move(&toplevel->window, button);
	}
}

static void
resize(struct wl_client *client, struct wl_resource *resource,
       struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct button *button;

	button = pointer_get_button(swc.seat->pointer, serial);
	if (button) {
		window_begin_resize(&toplevel->window, edges, button);
	}
}

static void
set_max_size(struct wl_client *client, struct wl_resource *resource,
             int32_t width, int32_t height)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	(void)client;
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
		                       "negative maximum size");
		return;
	}
	toplevel->pending.max_width = (uint32_t)width;
	toplevel->pending.max_height = (uint32_t)height;
	toplevel->pending.max_dirty = true;
}

static void
set_min_size(struct wl_client *client, struct wl_resource *resource,
             int32_t width, int32_t height)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	(void)client;
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
		                       "negative minimum size");
		return;
	}
	toplevel->pending.min_width = (uint32_t)width;
	toplevel->pending.min_height = (uint32_t)height;
	toplevel->pending.min_dirty = true;
}

static void
set_maximized(struct wl_client *client, struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	(void)client;
	if (toplevel->window.handler->request_maximized)
		toplevel->window.handler->request_maximized(
		    toplevel->window.handler_data, true);
}

static void
unset_maximized(struct wl_client *client, struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	(void)client;
	if (toplevel->window.handler->request_maximized)
		toplevel->window.handler->request_maximized(
		    toplevel->window.handler_data, false);
}

static void
set_fullscreen(struct wl_client *client, struct wl_resource *resource,
               struct wl_resource *output)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct swc_screen *screen = NULL;

	(void)client;
	if (output) {
		struct output *requested = wl_resource_get_user_data(output);
		if (requested && requested->screen)
			screen = &requested->screen->base;
	}
	if (toplevel->window.handler->request_fullscreen)
		toplevel->window.handler->request_fullscreen(
		    toplevel->window.handler_data, true, screen);
}

static void
unset_fullscreen(struct wl_client *client, struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	(void)client;
	if (toplevel->window.handler->request_fullscreen)
		toplevel->window.handler->request_fullscreen(
		    toplevel->window.handler_data, false, NULL);
}

static void
set_minimized(struct wl_client *client, struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct window *window = &toplevel->window;
	if (window->handler && window->handler->titlebar_action)
		window->handler->titlebar_action(window->handler_data, SWC_TITLEBAR_MINIMIZE);
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = destroy_resource,
    .set_parent = set_parent,
    .set_title = set_title,
    .set_app_id = set_app_id,
    .show_window_menu = show_window_menu,
    .move = move,
    .resize = resize,
    .set_max_size = set_max_size,
    .set_min_size = set_min_size,
    .set_maximized = set_maximized,
    .unset_maximized = unset_maximized,
    .set_fullscreen = set_fullscreen,
    .unset_fullscreen = unset_fullscreen,
    .set_minimized = set_minimized,
};

static struct xdg_toplevel *
xdg_toplevel_new(struct wl_client *client, uint32_t version, uint32_t id,
                 struct xdg_surface *xdg_surface)
{
	struct xdg_toplevel *toplevel;

	toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		goto error0;
	}
	toplevel->xdg_surface = xdg_surface;
	toplevel->resource =
	    wl_resource_create(client, &xdg_toplevel_interface, version, id);
	if (!toplevel->resource) {
		goto error1;
	}
	if (!surface_set_role(xdg_surface->surface, toplevel->resource)) {
		goto error2;
	}
	if (!window_initialize(&toplevel->window, &toplevel_window_impl,
	                       xdg_surface->surface)) {
		goto error2;
	}
	wl_array_init(&toplevel->states);
	wl_list_init(&toplevel->configures);
	toplevel->surface_commit_listener.notify = handle_toplevel_surface_commit;
	wl_signal_add(&xdg_surface->surface->signal.commit,
	              &toplevel->surface_commit_listener);
	wl_resource_set_implementation(toplevel->resource, &toplevel_impl, toplevel,
	                               &destroy_toplevel);
	send_wm_capabilities(toplevel);
	window_manage(&toplevel->window);

	return toplevel;

error2:
	wl_resource_destroy(toplevel->resource);
error1:
	free(toplevel);
error0:
	return NULL;
}

/*
 * Version 5 lets the compositor say which window-management actions exist, so
 * a client can leave out buttons that would do nothing. charaWC maximizes,
 * minimizes and fullscreens; it has no window menu, so that one is absent
 * rather than advertised and ignored.
 */
static void
send_wm_capabilities(struct xdg_toplevel *toplevel)
{
	static const uint32_t capabilities[] = {
		XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE,
		XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE,
		XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN,
	};
	struct wl_array array;

	if (wl_resource_get_version(toplevel->resource) <
	    XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
		return;
	}

	/* wl_array_init plus a borrowed buffer: the array is only read while
	 * the event is marshalled, so there is nothing to allocate or free. */
	wl_array_init(&array);
	array.data = (void *)capabilities;
	array.size = sizeof(capabilities);
	array.alloc = 0;
	xdg_toplevel_send_wm_capabilities(toplevel->resource, &array);
}

/* xdg_popup */
static void
handle_popup_parent_destroy(struct wl_listener *listener, void *data)
{
	struct xdg_popup *popup =
	    wl_container_of(listener, popup, parent_destroy_listener);

	(void)data;

	wl_list_remove(&popup->parent_destroy_listener.link);
	wl_list_init(&popup->parent_destroy_listener.link);
	popup->parent = NULL;
	compositor_view_set_parent(popup->view, NULL);
	compositor_view_hide(popup->view);
	xdg_popup_send_popup_done(popup->resource);
}

static void
destroy_popup(struct wl_resource *resource)
{
	struct xdg_popup *popup = wl_resource_get_user_data(resource);

	if (popup->parent) {
		wl_list_remove(&popup->parent_destroy_listener.link);
	}
	compositor_view_destroy(popup->view);
	free(popup);
}

static void
grab(struct wl_client *client, struct wl_resource *resource,
     struct wl_resource *seat, uint32_t serial)
{
}

/*
 * Version 3 lets a client move an already-mapped popup -- a submenu that has
 * to flip to the other side of its parent, say -- without tearing it down and
 * building a new one. The token comes back in 'repositioned' so the client can
 * tell which of several requests it is seeing the answer to.
 */
static void
reposition(struct wl_client *client, struct wl_resource *resource,
           struct wl_resource *positioner_resource, uint32_t token)
{
	struct xdg_popup *popup = wl_resource_get_user_data(resource);
	struct xdg_positioner *positioner;
	struct swc_rectangle rect;
	uint32_t serial;

	(void)client;
	if (!popup || !positioner_resource) {
		return;
	}
	positioner = wl_resource_get_user_data(positioner_resource);
	if (!positioner) {
		return;
	}

	popup->positioner = *positioner;

	if (!popup->parent) {
		/* Nothing to position against yet; the geometry is applied when the
		 * parent arrives. */
		return;
	}

	rect = calculate_position(&popup->positioner, popup->parent);
	view_move(&popup->view->base, popup->parent->base.geometry.x + rect.x,
	          popup->parent->base.geometry.y + rect.y);

	serial = wl_display_next_serial(swc.display);
	popup->xdg_surface->configure_serial = serial;
	xdg_popup_send_repositioned(popup->resource, token);
	xdg_popup_send_configure(popup->resource, rect.x, rect.y, rect.width,
	                         rect.height);
	xdg_surface_send_configure(popup->xdg_surface->resource, serial);
}

static const struct xdg_popup_interface popup_impl = {
    .destroy = destroy_resource,
    .grab = grab,
    .reposition = reposition,
};

bool
xdg_popup_set_parent(struct wl_resource *popup_resource,
                     struct compositor_view *parent)
{
	struct xdg_popup *popup;
	struct swc_rectangle rect;
	uint32_t serial;

	if (!popup_resource || !parent ||
	    !wl_resource_instance_of(popup_resource, &xdg_popup_interface,
	                             &popup_impl)) {
		return false;
	}

	popup = wl_resource_get_user_data(popup_resource);
	if (!popup || popup->parent || popup->configured) {
		return false;
	}

	popup->parent = parent;
	popup->parent_destroy_listener.notify = handle_popup_parent_destroy;
	wl_signal_add(&parent->destroy_signal, &popup->parent_destroy_listener);

	rect = calculate_position(&popup->positioner, parent);
	popup->view->always_top = parent->always_top;
	compositor_view_set_stack_layer(popup->view, parent->stack_layer, true);
	view_move(&popup->view->base, parent->base.geometry.x + rect.x,
	          parent->base.geometry.y + rect.y);
	compositor_view_set_parent(popup->view, parent);
	compositor_view_restack(popup->view, parent, true);

	serial = wl_display_next_serial(swc.display);
	popup->xdg_surface->configure_serial = serial;
	popup->configured = true;
	xdg_popup_send_configure(popup->resource, rect.x, rect.y, rect.width,
	                         rect.height);
	xdg_surface_send_configure(popup->xdg_surface->resource, serial);

	return true;
}

static struct xdg_popup *
xdg_popup_new(struct wl_client *client, uint32_t version, uint32_t id,
              struct xdg_surface *xdg_surface, struct xdg_surface *parent,
              struct xdg_positioner *positioner)
{
	struct xdg_popup *popup;
	struct compositor_view *parent_view = NULL;

	if (parent) {
		parent_view = compositor_view(parent->surface->view);
		if (!parent_view) {
			return NULL;
		}
	}

	popup = calloc(1, sizeof(*popup));
	if (!popup) {
		goto error0;
	}
	popup->xdg_surface = xdg_surface;
	popup->positioner = *positioner;
	wl_list_init(&popup->parent_destroy_listener.link);
	popup->resource =
	    wl_resource_create(client, &xdg_popup_interface, version, id);
	if (!popup->resource) {
		goto error1;
	}
	if (!surface_set_role(xdg_surface->surface, popup->resource)) {
		goto error2;
	}
	popup->view = compositor_create_view(xdg_surface->surface);
	if (!popup->view) {
		goto error2;
	}
	wl_resource_set_implementation(popup->resource, &popup_impl, popup,
	                               &destroy_popup);

	if (parent_view && !xdg_popup_set_parent(popup->resource, parent_view)) {
		wl_resource_destroy(popup->resource);
		return NULL;
	}

	return popup;

error2:
	wl_resource_destroy(popup->resource);
error1:
	free(popup);
error0:
	return NULL;
}

/* xdg_surface */
static void
get_toplevel(struct wl_client *client, struct wl_resource *resource,
             uint32_t id)
{
	struct xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
	struct xdg_toplevel *toplevel;

	if (xdg_surface->role) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
		                       "surface already has a role");
		return;
	}
	if (xdg_surface->surface->role) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
		                       "surface already has a role");
		return;
	}
	toplevel = xdg_toplevel_new(client, wl_resource_get_version(resource), id,
	                            xdg_surface);
	if (!toplevel) {
		wl_client_post_no_memory(client);
		return;
	}
	xdg_surface->role = toplevel->resource;
	xdg_surface->role_type = XDG_ROLE_TOPLEVEL;
	wl_resource_add_destroy_listener(xdg_surface->role,
	                                 &xdg_surface->role_destroy_listener);
}

static void
get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
          struct wl_resource *parent_resource,
          struct wl_resource *positioner_resource)
{
	struct xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
	struct xdg_surface *parent =
	    parent_resource ? wl_resource_get_user_data(parent_resource) : NULL;
	struct xdg_positioner *positioner =
	    wl_resource_get_user_data(positioner_resource);
	struct xdg_popup *popup;

	if (xdg_surface->role) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
		                       "surface already has a role");
		return;
	}
	if (xdg_surface->surface->role) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
		                       "surface already has a role");
		return;
	}
	if (positioner->width <= 0 || positioner->height <= 0 ||
	    positioner->anchor_width <= 0 || positioner->anchor_height <= 0) {
		wl_resource_post_error(resource,
		                       XDG_WM_BASE_ERROR_INVALID_POSITIONER,
		                       "xdg_positioner is incomplete");
		return;
	}
	if (parent && !compositor_view(parent->surface->view)) {
		wl_resource_post_error(resource,
		                       XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
		                       "popup parent has no view");
		return;
	}
	popup = xdg_popup_new(client, wl_resource_get_version(resource), id,
	                      xdg_surface, parent, positioner);
	if (!popup) {
		wl_client_post_no_memory(client);
		return;
	}
	xdg_surface->role = popup->resource;
	xdg_surface->role_type = XDG_ROLE_POPUP;
	wl_resource_add_destroy_listener(xdg_surface->role,
	                                 &xdg_surface->role_destroy_listener);
}

static void
ack_configure(struct wl_client *client, struct wl_resource *resource,
              uint32_t serial)
{
	struct xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
	struct xdg_toplevel *toplevel;

	if (!xdg_surface->role) {
		return;
	}
	if (xdg_surface->role_type == XDG_ROLE_TOPLEVEL) {
		toplevel = wl_resource_get_user_data(xdg_surface->role);
		struct toplevel_configure *c, *match = NULL, *tmp;
		wl_list_for_each(c, &toplevel->configures, link)
			if (c->serial == serial) { match = c; break; }
		if (!match) {
			wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL, "unknown configure serial");
			return;
		}
		toplevel->window.configure.acknowledged = match->generation == toplevel->generation;
		wl_list_for_each_safe(c, tmp, &toplevel->configures, link) {
			bool last = c == match;
			wl_list_remove(&c->link);
			free(c);
			--toplevel->configure_count;
			if (last) break;
		}
	} else if (serial != xdg_surface->configure_serial) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL, "unknown popup configure serial");
	}
}

static void
set_window_geometry(struct wl_client *client, struct wl_resource *resource,
                    int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	struct xdg_surface *xdg_surface = wl_resource_get_user_data(resource);
	struct surface *surface = xdg_surface->surface;

	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE, "invalid window geometry");
		return;
	}
	/* Window geometry is double-buffered with wl_surface.commit. The view
	 * position already denotes the window origin, not its shadow origin. */
	surface->pending.window_geometry = (struct swc_rectangle){x, y, width, height};
	surface->pending.commit |= SURFACE_COMMIT_GEOMETRY;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = destroy_resource,
    .get_toplevel = get_toplevel,
    .get_popup = get_popup,
    .ack_configure = ack_configure,
    .set_window_geometry = set_window_geometry,
};

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct xdg_surface *xdg_surface =
	    wl_container_of(listener, xdg_surface, surface_destroy_listener);

	wl_resource_destroy(xdg_surface->resource);
}

static void
handle_role_destroy(struct wl_listener *listener, void *data)
{
	struct xdg_surface *xdg_surface =
	    wl_container_of(listener, xdg_surface, role_destroy_listener);

	xdg_surface->role = NULL;
	xdg_surface->role_type = XDG_ROLE_NONE;
}

static void
destroy_xdg_surface(struct wl_resource *resource)
{
	struct xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

	wl_list_remove(&xdg_surface->surface_destroy_listener.link);
	if (xdg_surface->role) {
		wl_resource_destroy(xdg_surface->role);
	}
	free(xdg_surface);
}

static struct xdg_surface *
xdg_surface_new(struct wl_client *client, uint32_t version, uint32_t id,
                struct surface *surface)
{
	struct xdg_surface *xdg_surface;

	xdg_surface = malloc(sizeof(*xdg_surface));
	if (!xdg_surface) {
		goto error0;
	}
	xdg_surface->resource =
	    wl_resource_create(client, &xdg_surface_interface, version, id);
	if (!xdg_surface->resource) {
		goto error1;
	}
	xdg_surface->surface = surface;
	xdg_surface->configure_serial = 0;
	xdg_surface->surface_destroy_listener.notify = &handle_surface_destroy;
	xdg_surface->role = NULL;
	xdg_surface->role_type = XDG_ROLE_NONE;
	xdg_surface->role_destroy_listener.notify = &handle_role_destroy;
	wl_resource_add_destroy_listener(surface->resource,
	                                 &xdg_surface->surface_destroy_listener);
	wl_resource_set_implementation(xdg_surface->resource, &xdg_surface_impl,
	                               xdg_surface, destroy_xdg_surface);

	return xdg_surface;

error1:
	free(xdg_surface);
error0:
	return NULL;
}

/* xdg_shell */
static void
create_positioner(struct wl_client *client, struct wl_resource *resource,
                  uint32_t id)
{
	struct xdg_positioner *positioner;
	struct wl_resource *positioner_resource;
	uint32_t version;

	positioner = calloc(1, sizeof(*positioner));
	if (!positioner) {
		goto error0;
	}

	version = wl_resource_get_version(resource);
	positioner_resource =
	    wl_resource_create(client, &xdg_positioner_interface, version, id);
	if (!positioner_resource) {
		goto error1;
	}
	wl_resource_set_implementation(positioner_resource, &positioner_impl,
	                               positioner, &destroy_positioner);
	return;

error1:
	free(positioner);
error0:
	wl_resource_post_no_memory(resource);
}

static void
get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                uint32_t id, struct wl_resource *surface_resource)
{
	struct xdg_surface *xdg_surface;
	struct surface *surface = wl_resource_get_user_data(surface_resource);

	xdg_surface =
	    xdg_surface_new(client, wl_resource_get_version(resource), id, surface);
	if (!xdg_surface) {
		wl_client_post_no_memory(client);
	}
}

static void
pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = destroy_resource,
    .create_positioner = create_positioner,
    .get_xdg_surface = get_xdg_surface,
    .pong = pong,
};

static void
bind_wm_base(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wm_base_impl, NULL, NULL);
}

struct wl_global *
xdg_shell_create(struct wl_display *display)
{
	return wl_global_create(display, &xdg_wm_base_interface, 5, NULL,
	                        &bind_wm_base);
}
