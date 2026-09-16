/* swc: libswc/pointer_constraints.c
 *
 * zwp_pointer_constraints_v1. Constraints belong to a surface and a seat;
 * pointer locking does not change the client's cursor image or visibility.
 */

#include "pointer_constraints.h"
#include "compositor.h"
#include "internal.h"
#include "pointer.h"
#include "surface.h"
#include "util.h"

#include "pointer-constraints-unstable-v1-server-protocol.h"

#include <limits.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wld/wld.h>

struct constraint {
	struct wl_resource *resource;
	struct surface *surface;
	struct pointer *pointer;
	struct wl_listener surface_destroy, surface_commit, pointer_destroy;
	struct wl_list link;

	bool locked, active, defunct;
	uint32_t lifetime;
	pixman_region32_t region, effective;
	bool has_region;
	wl_fixed_t hint_x, hint_y;
	bool has_hint;

	/* set_region and set_cursor_position_hint take effect on surface commit. */
	struct {
		pixman_region32_t region;
		bool region_set, has_region, hint_set;
		wl_fixed_t hint_x, hint_y;
	} pending;
};

static struct wl_list constraints = { &constraints, &constraints };
static struct constraint *active_constraint;

static void
remove_listener(struct wl_listener *listener)
{
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

static void
make_defunct(struct constraint *constraint)
{
	constraint->defunct = true;
	remove_listener(&constraint->surface_commit);
	remove_listener(&constraint->surface_destroy);
	remove_listener(&constraint->pointer_destroy);
	constraint->surface = NULL;
	constraint->pointer = NULL;
}

static void
constraint_deactivate(struct constraint *constraint)
{
	if (!constraint->active)
		return;
	constraint->active = false;
	if (active_constraint == constraint)
		active_constraint = NULL;

	/* Focus loss must not warp the cursor back into the old surface. */
	if (constraint->locked)
		zwp_locked_pointer_v1_send_unlocked(constraint->resource);
	else
		zwp_confined_pointer_v1_send_unconfined(constraint->resource);

	/* Keep the protocol object alive for requests already in flight. */
	if (constraint->lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT)
		make_defunct(constraint);
}

static struct compositor_view *
constraint_view(struct constraint *constraint)
{
	struct compositor_view *view = constraint->pointer ? constraint->pointer->focus.view : NULL;

	return constraint->surface && view && view->visible &&
	       view->surface == constraint->surface ? view : NULL;
}

static void
update_region(struct constraint *constraint)
{
	struct surface *surface = constraint->surface;
	struct wld_buffer *buffer = surface->view ? surface->view->buffer : NULL;

	pixman_region32_intersect_rect(&constraint->effective, &surface->state.input,
	    0, 0, buffer ? buffer->width : 0, buffer ? buffer->height : 0);
	if (constraint->has_region)
		pixman_region32_intersect(&constraint->effective, &constraint->effective,
		                         &constraint->region);
}

static bool
contains_fixed(pixman_region32_t *region, int64_t x, int64_t y)
{
	/* Surface-local constraint regions have already been clipped to the buffer. */
	return x >= 0 && y >= 0 && x / 256 <= INT32_MAX && y / 256 <= INT32_MAX &&
	       pixman_region32_contains_point(region, x / 256, y / 256, NULL);
}

bool
pointer_constraints_pointer_locked(void)
{
	return active_constraint && active_constraint->locked;
}

void
pointer_constraints_update_focus(struct pointer *pointer)
{
	struct constraint *constraint, *want = NULL;
	struct compositor_view *view = pointer->focus.view;

	if (swc.active && view && view->visible) {
		wl_list_for_each(constraint, &constraints, link) {
			if (constraint->defunct || constraint->pointer != pointer ||
			    constraint->surface != view->surface)
				continue;
			int64_t x = (int64_t)pointer->x -
			    ((int64_t)view->base.geometry.x - view->buffer_offset_x) * 256;
			int64_t y = (int64_t)pointer->y -
			    ((int64_t)view->base.geometry.y - view->buffer_offset_y) * 256;
			if (contains_fixed(&constraint->effective, x, y))
				want = constraint;
			break;
		}
	}

	if (active_constraint && active_constraint != want)
		constraint_deactivate(active_constraint);
	if (!want || want->active)
		return;

	want->active = true;
	active_constraint = want;
	if (want->locked)
		zwp_locked_pointer_v1_send_locked(want->resource);
	else
		zwp_confined_pointer_v1_send_confined(want->resource);
}

/* Clip one axis to the connected interval containing the starting point.
 * pixman's rectangles are ordered by y, then x. Filtering by the other axis
 * preserves this order, so adjacent rectangles can be merged in one pass.
 * Fixed-point endpoints keep the last subpixel inside right/bottom edges. */
static int64_t
confine_axis(pixman_region32_t *region, int64_t start, int64_t other,
             int64_t target, bool horizontal)
{
	int count;
	pixman_box32_t *boxes = pixman_region32_rectangles(region, &count);
	int64_t low = 0, high = -1;

	for (int i = 0; i < count; ++i) {
		int64_t perpendicular_low = (int64_t)(horizontal ? boxes[i].y1 : boxes[i].x1) * 256;
		int64_t perpendicular_high = (int64_t)(horizontal ? boxes[i].y2 : boxes[i].x2) * 256;
		if (other < perpendicular_low || other >= perpendicular_high)
			continue;
		int64_t a = (int64_t)(horizontal ? boxes[i].x1 : boxes[i].y1) * 256;
		int64_t b = (int64_t)(horizontal ? boxes[i].x2 : boxes[i].y2) * 256 - 1;
		if (high >= low && a > high + 1) {
			if (start >= low && start <= high)
				return MAX(low, MIN(target, high));
			low = a;
		} else if (high < low) {
			low = a;
		}
		high = MAX(high, b);
	}
	return start >= low && start <= high ? MAX(low, MIN(target, high)) : start;
}

void
pointer_constraints_confine(struct pointer *pointer, wl_fixed_t *x, wl_fixed_t *y)
{
	struct constraint *constraint = active_constraint;
	struct compositor_view *view;

	if (!constraint || constraint->locked || constraint->pointer != pointer ||
	    !(view = constraint_view(constraint)))
		return;

	int64_t ox = ((int64_t)view->base.geometry.x - view->buffer_offset_x) * 256;
	int64_t oy = ((int64_t)view->base.geometry.y - view->buffer_offset_y) * 256;
	int64_t sx = (int64_t)pointer->x - ox, sy = (int64_t)pointer->y - oy;
	int64_t tx = (int64_t)*x - ox, ty = (int64_t)*y - oy;

	/* Slide along edges without jumping across holes or disconnected regions.
	 * Try both axis orders to avoid favoring horizontal or vertical movement. */
	int64_t ax = confine_axis(&constraint->effective, sx, sy, tx, true);
	int64_t ay = confine_axis(&constraint->effective, sy, ax, ty, false);
	int64_t by = confine_axis(&constraint->effective, sy, sx, ty, false);
	int64_t bx = confine_axis(&constraint->effective, sx, by, tx, true);
	double adx = tx - ax, ady = ty - ay, bdx = tx - bx, bdy = ty - by;
	if (bdx * bdx + bdy * bdy < adx * adx + ady * ady) {
		ax = bx;
		ay = by;
	}
	*x = ox + ax;
	*y = oy + ay;
}

static void
apply_position_hint(struct constraint *constraint)
{
	struct compositor_view *view;
	int64_t x, y;

	if (!constraint->has_hint || !(view = constraint_view(constraint)) ||
	    !contains_fixed(&constraint->effective, constraint->hint_x, constraint->hint_y))
		return;

	x = ((int64_t)view->base.geometry.x - view->buffer_offset_x) * 256 + constraint->hint_x;
	y = ((int64_t)view->base.geometry.y - view->buffer_offset_y) * 256 + constraint->hint_y;
	if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
		return;
	pointer_warp(constraint->pointer, x, y);
}

static void
destroy_constraint(struct wl_resource *resource)
{
	struct constraint *constraint = wl_resource_get_user_data(resource);
	bool was_active = constraint->active;

	/* Remove it BEFORE restoring the position. Never let a dying lock become
	 * eligible for activation again during focus or cursor updates. */
	wl_list_remove(&constraint->link);
	constraint->active = false;
	if (active_constraint == constraint)
		active_constraint = NULL;
	if (was_active && constraint->locked)
		apply_position_hint(constraint);
	make_defunct(constraint);
	pixman_region32_fini(&constraint->region);
	pixman_region32_fini(&constraint->effective);
	pixman_region32_fini(&constraint->pending.region);
	free(constraint);
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct constraint *constraint = wl_container_of(listener, constraint, surface_destroy);
	constraint_deactivate(constraint);
	make_defunct(constraint);
}

static void
handle_pointer_destroy(struct wl_listener *listener, void *data)
{
	struct constraint *constraint = wl_container_of(listener, constraint, pointer_destroy);
	constraint_deactivate(constraint);
	make_defunct(constraint);
}

static void
handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct constraint *constraint = wl_container_of(listener, constraint, surface_commit);
	struct pointer *pointer = constraint->pointer;

	if (constraint->pending.region_set) {
		pixman_region32_copy(&constraint->region, &constraint->pending.region);
		constraint->has_region = constraint->pending.has_region;
		constraint->pending.region_set = false;
	}
	if (constraint->pending.hint_set) {
		constraint->hint_x = constraint->pending.hint_x;
		constraint->hint_y = constraint->pending.hint_y;
		constraint->has_hint = true;
		constraint->pending.hint_set = false;
	}
	update_region(constraint);
	pointer_constraints_update_focus(pointer);
}

static void
set_region(struct wl_client *client, struct wl_resource *resource,
           struct wl_resource *region_resource)
{
	struct constraint *constraint = wl_resource_get_user_data(resource);

	if (constraint->defunct)
		return;
	constraint->pending.region_set = true;
	constraint->pending.has_region = region_resource != NULL;
	if (region_resource)
		pixman_region32_copy(&constraint->pending.region,
		                     wl_resource_get_user_data(region_resource));
	else
		pixman_region32_clear(&constraint->pending.region);
}

static void
set_cursor_position_hint(struct wl_client *client, struct wl_resource *resource,
                         wl_fixed_t x, wl_fixed_t y)
{
	struct constraint *constraint = wl_resource_get_user_data(resource);

	if (constraint->defunct)
		return;
	constraint->pending.hint_set = true;
	constraint->pending.hint_x = x;
	constraint->pending.hint_y = y;
}

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
	.destroy = destroy_resource,
	.set_cursor_position_hint = set_cursor_position_hint,
	.set_region = set_region,
};

static const struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
	.destroy = destroy_resource,
	.set_region = set_region,
};

static void
constraint_new(struct wl_client *client, struct wl_resource *resource,
               uint32_t id, struct wl_resource *surface_resource,
               struct wl_resource *pointer_resource, struct wl_resource *region_resource,
               uint32_t lifetime, bool locked)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct pointer *pointer = wl_resource_get_user_data(pointer_resource);
	struct constraint *constraint, *other;

	if (lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT &&
	    lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT) {
		wl_resource_post_error(wl_client_get_object(client, 1), WL_DISPLAY_ERROR_INVALID_METHOD,
		                       "invalid pointer constraint lifetime");
		return;
	}
	wl_list_for_each(other, &constraints, link) {
		if (!other->defunct && other->surface == surface && other->pointer == pointer) {
			wl_resource_post_error(resource, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
			                       "surface already has a constraint for this seat");
			return;
		}
	}
	constraint = calloc(1, sizeof(*constraint));
	if (!constraint) {
		wl_client_post_no_memory(client);
		return;
	}
	constraint->resource = wl_resource_create(client,
	    locked ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!constraint->resource) {
		free(constraint);
		wl_client_post_no_memory(client);
		return;
	}
	constraint->surface = surface;
	constraint->pointer = pointer;
	constraint->locked = locked;
	constraint->lifetime = lifetime;
	pixman_region32_init(&constraint->region);
	pixman_region32_init(&constraint->effective);
	pixman_region32_init(&constraint->pending.region);
	wl_list_init(&constraint->surface_destroy.link);
	wl_list_init(&constraint->surface_commit.link);
	wl_list_init(&constraint->pointer_destroy.link);
	if (region_resource) {
		pixman_region32_copy(&constraint->region, wl_resource_get_user_data(region_resource));
		constraint->has_region = true;
	}
	wl_resource_set_implementation(constraint->resource,
	    locked ? (const void *)&locked_pointer_impl : (const void *)&confined_pointer_impl,
	    constraint, destroy_constraint);
	wl_list_insert(&constraints, &constraint->link);
	if (!surface || !pointer) {
		make_defunct(constraint);
		return;
	}
	constraint->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->signal.destroy, &constraint->surface_destroy);
	constraint->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->signal.commit, &constraint->surface_commit);
	constraint->pointer_destroy.notify = handle_pointer_destroy;
	wl_signal_add(&pointer->destroy_signal, &constraint->pointer_destroy);
	update_region(constraint);
	pointer_constraints_update_focus(pointer);
}

static void
lock_pointer(struct wl_client *client, struct wl_resource *resource,
             uint32_t id, struct wl_resource *surface, struct wl_resource *pointer,
             struct wl_resource *region, uint32_t lifetime)
{
	constraint_new(client, resource, id, surface, pointer, region, lifetime, true);
}

static void
confine_pointer(struct wl_client *client, struct wl_resource *resource,
                uint32_t id, struct wl_resource *surface, struct wl_resource *pointer,
                struct wl_resource *region, uint32_t lifetime)
{
	constraint_new(client, resource, id, surface, pointer, region, lifetime, false);
}

static const struct zwp_pointer_constraints_v1_interface constraints_impl = {
	.destroy = destroy_resource,
	.lock_pointer = lock_pointer,
	.confine_pointer = confine_pointer,
};

static void
bind_pointer_constraints(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
	    &zwp_pointer_constraints_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &constraints_impl, NULL, NULL);
}

struct wl_global *
pointer_constraints_create(struct wl_display *display)
{
	return wl_global_create(display, &zwp_pointer_constraints_v1_interface, 1,
	                        NULL, bind_pointer_constraints);
}
