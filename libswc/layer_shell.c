#include "layer_shell.h"

#include "compositor.h"
#include "internal.h"
#include "keyboard.h"
#include "output.h"
#include "pointer.h"
#include "screen.h"
#include "seat.h"
#include "session_lock.h"
#include "surface.h"
#include "util.h"
#include "view.h"
#include "xdg_shell.h"

#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>

struct layer_surface_state {
	uint32_t layer;
	uint32_t anchor;
	int32_t exclusive_zone;
	uint32_t exclusive_edge;
	uint32_t keyboard_interactivity;
	struct {
		int32_t top, right, bottom, left;
	} margin;
	uint32_t desired_width, desired_height;
};

struct layer_configure {
	struct wl_list link;
	uint32_t serial;
};

struct layer_screen {
	struct screen *screen;
	struct screen_modifier modifier;
	struct wl_listener screen_destroy_listener;
	struct wl_list surfaces;
	struct wl_list link;
};

struct layer_surface {
	struct wl_resource *resource;
	struct wl_listener surface_destroy_listener;
	struct wl_listener surface_commit_listener;
	struct compositor_view *view;
	struct view_handler view_handler;
	struct layer_screen *layer_screen;
	struct wl_list screen_link;
	struct wl_list configures;
	uint32_t configure_count;
	struct layer_surface_state current, pending;
	uint32_t initial_layer;
	bool configured;
	bool mapped;
	bool closed;
};

static struct wl_list layer_screens;
static struct compositor_view *saved_keyboard_focus;
static struct wl_listener saved_keyboard_focus_destroy;
static bool saved_keyboard_focus_listener_active;

/* Matching xdg_toplevel's cap on unacknowledged configure events. */
#define LAYER_CONFIGURE_MAX 1024

static const uint32_t layer_order[] = {
	ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
	ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
};

static bool
state_equal(const struct layer_surface_state *a,
            const struct layer_surface_state *b)
{
	return a->layer == b->layer && a->anchor == b->anchor &&
	       a->exclusive_zone == b->exclusive_zone &&
	       a->exclusive_edge == b->exclusive_edge &&
	       a->keyboard_interactivity == b->keyboard_interactivity &&
	       a->margin.top == b->margin.top &&
	       a->margin.right == b->margin.right &&
	       a->margin.bottom == b->margin.bottom &&
	       a->margin.left == b->margin.left &&
	       a->desired_width == b->desired_width &&
	       a->desired_height == b->desired_height;
}

static void
clear_configures(struct layer_surface *surface)
{
	struct layer_configure *configure, *next;

	wl_list_for_each_safe(configure, next, &surface->configures, link)
	{
		wl_list_remove(&configure->link);
		free(configure);
	}
	surface->configure_count = 0;
}

static struct layer_surface *
layer_surface_from_view(struct compositor_view *view)
{
	struct layer_screen *layer_screen;
	struct layer_surface *surface;
	struct compositor_view *candidate;

	for (candidate = view; candidate; candidate = candidate->parent) {
		wl_list_for_each(layer_screen, &layer_screens, link)
		{
			wl_list_for_each(surface, &layer_screen->surfaces, screen_link)
			{
				if (surface->view == candidate) {
					return surface;
				}
			}
		}
	}

	return NULL;
}

static uint32_t
exclusive_edge_for_state(const struct layer_surface_state *state)
{
	if (state->exclusive_edge != 0) {
		return state->exclusive_edge;
	}

	switch (state->anchor) {
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	default:
		return 0;
	}
}

static bool
has_effective_exclusive_zone(const struct layer_surface *surface)
{
	return surface->current.exclusive_zone > 0 &&
	       exclusive_edge_for_state(&surface->current) != 0;
}

static void
shrink_box(pixman_box32_t *box, uint32_t edge, int32_t amount)
{
	if (amount <= 0) {
		return;
	}

	switch (edge) {
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
		box->y1 = MIN(box->y1 + amount, box->y2);
		break;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
		box->y2 = MAX(box->y2 - amount, box->y1);
		break;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
		box->x1 = MIN(box->x1 + amount, box->x2);
		break;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
		box->x2 = MAX(box->x2 - amount, box->x1);
		break;
	default:
		break;
	}
}

static struct swc_rectangle
rectangle_from_box(const pixman_box32_t *box)
{
	return (struct swc_rectangle){
	    .x = box->x1,
	    .y = box->y1,
	    .width = (uint32_t)MAX(box->x2 - box->x1, 0),
	    .height = (uint32_t)MAX(box->y2 - box->y1, 0),
	};
}

static void
update_position(struct layer_surface *surface,
                const struct swc_rectangle *bounds)
{
	const struct swc_rectangle *view = &surface->view->base.geometry;
	const struct layer_surface_state *state = &surface->current;
	int32_t x, y;

	if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
	    !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
		x = bounds->x + state->margin.left;
	} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) &&
	           !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
		x = bounds->x + (int32_t)bounds->width - (int32_t)view->width -
		    state->margin.right;
	} else {
		x = bounds->x +
		    ((int32_t)bounds->width - (int32_t)view->width +
		     state->margin.left - state->margin.right) /
		        2;
	}

	if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
	    !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
		y = bounds->y + state->margin.top;
	} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) &&
	           !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
		y = bounds->y + (int32_t)bounds->height - (int32_t)view->height -
		    state->margin.bottom;
	} else {
		y = bounds->y +
		    ((int32_t)bounds->height - (int32_t)view->height +
		     state->margin.top - state->margin.bottom) /
		        2;
	}

	view_move(&surface->view->base, x, y);
}

static void
calculate_available_box(struct layer_screen *layer_screen,
                        const struct swc_rectangle *geom,
                        const struct layer_surface *exclude,
                        pixman_box32_t *available)
{
	struct layer_surface *surface;
	size_t i;

	*available = (pixman_box32_t){
	    .x1 = geom->x,
	    .y1 = geom->y,
	    .x2 = geom->x + (int32_t)geom->width,
	    .y2 = geom->y + (int32_t)geom->height,
	};

	for (i = 0; i < ARRAY_LENGTH(layer_order); ++i) {
		wl_list_for_each(surface, &layer_screen->surfaces, screen_link)
		{
			if (surface == exclude || !surface->mapped ||
			    surface->current.layer != layer_order[i] ||
			    !has_effective_exclusive_zone(surface)) {
				continue;
			}

			shrink_box(available,
			           exclusive_edge_for_state(&surface->current),
			           surface->current.exclusive_zone);
		}
	}
}

static void
arrange_layer_screen(struct layer_screen *layer_screen,
                     const struct swc_rectangle *geom,
                     pixman_box32_t *available)
{
	pixman_box32_t full = {
	    .x1 = geom->x,
	    .y1 = geom->y,
	    .x2 = geom->x + (int32_t)geom->width,
	    .y2 = geom->y + (int32_t)geom->height,
	};
	struct layer_surface *surface;
	struct swc_rectangle bounds;
	size_t i;

	*available = full;

	/* Reserve positive zones first so panels on one edge stack. */
	for (i = 0; i < ARRAY_LENGTH(layer_order); ++i) {
		wl_list_for_each(surface, &layer_screen->surfaces, screen_link)
		{
			if (!surface->mapped ||
			    surface->current.layer != layer_order[i] ||
			    !has_effective_exclusive_zone(surface)) {
				continue;
			}

			bounds = rectangle_from_box(available);
			update_position(surface, &bounds);
			shrink_box(available,
			           exclusive_edge_for_state(&surface->current),
			           surface->current.exclusive_zone);
		}
	}

	/* Zone 0 avoids positive zones. Zone -1 uses the complete output. */
	for (i = 0; i < ARRAY_LENGTH(layer_order); ++i) {
		wl_list_for_each(surface, &layer_screen->surfaces, screen_link)
		{
			if (!surface->mapped ||
			    surface->current.layer != layer_order[i] ||
			    has_effective_exclusive_zone(surface)) {
				continue;
			}

			bounds = rectangle_from_box(
			    surface->current.exclusive_zone == -1 ? &full : available);
			update_position(surface, &bounds);
		}
	}
}

static void
modify(struct screen_modifier *modifier, const struct swc_rectangle *geom,
       pixman_region32_t *usable)
{
	struct layer_screen *layer_screen =
	    wl_container_of(modifier, layer_screen, modifier);
	pixman_box32_t available;

	arrange_layer_screen(layer_screen, geom, &available);
	pixman_region32_reset(usable, &available);
}

static void
restack_layer(struct layer_surface *surface)
{
	uint32_t stack_layer = STACK_LAYER_NORMAL;
	bool always_top = false;

	switch (surface->current.layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		stack_layer = STACK_LAYER_BACKGROUND;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		stack_layer = STACK_LAYER_BOTTOM;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		stack_layer = STACK_LAYER_TOP;
		always_top = true;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		stack_layer = STACK_LAYER_OVERLAY;
		always_top = true;
		break;
	default:
		break;
	}

	surface->view->always_top = always_top;
	compositor_view_set_stack_layer(surface->view, stack_layer, true);
}

static void
handle_saved_keyboard_focus_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	wl_list_remove(&saved_keyboard_focus_destroy.link);
	wl_list_init(&saved_keyboard_focus_destroy.link);
	saved_keyboard_focus = NULL;
	saved_keyboard_focus_listener_active = false;
}

static void
clear_saved_keyboard_focus(void)
{
	if (saved_keyboard_focus_listener_active) {
		wl_list_remove(&saved_keyboard_focus_destroy.link);
		wl_list_init(&saved_keyboard_focus_destroy.link);
	}
	saved_keyboard_focus = NULL;
	saved_keyboard_focus_listener_active = false;
}

static void
save_keyboard_focus(struct compositor_view *view)
{
	clear_saved_keyboard_focus();
	/* Taking the keyboard ends a menu's grab; remember what it was over. */
	view = xdg_popup_grab_focus_owner(view);
	if (!view) {
		return;
	}

	saved_keyboard_focus = view;
	saved_keyboard_focus_destroy.notify =
	    handle_saved_keyboard_focus_destroy;
	wl_signal_add(&view->destroy_signal, &saved_keyboard_focus_destroy);
	saved_keyboard_focus_listener_active = true;
}

static struct layer_surface *
exclusive_keyboard_surface(void)
{
	struct layer_screen *layer_screen;
	struct layer_surface *surface;
	const uint32_t layers[] = {
	    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	    ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	size_t i;

	for (i = 0; i < ARRAY_LENGTH(layers); ++i) {
		wl_list_for_each_reverse(layer_screen, &layer_screens, link)
		{
			wl_list_for_each_reverse(surface, &layer_screen->surfaces,
			                         screen_link)
			{
				if (surface->mapped &&
				    surface->current.layer == layers[i] &&
				    surface->current.keyboard_interactivity ==
				        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
					return surface;
				}
			}
		}
	}

	return NULL;
}

static void
update_keyboard_focus(void)
{
	struct layer_surface *exclusive;

	/* A lock surface owns the keyboard outright. A panel that maps or
	 * changes while locked must not pull focus out of the lock screen. */
	if (session_lock_active()) {
		return;
	}

	exclusive = exclusive_keyboard_surface();
	struct compositor_view *current = swc.seat->keyboard->focus.view;
	struct layer_surface *current_layer = layer_surface_from_view(current);

	if (exclusive) {
		if (current != exclusive->view) {
			if (current && !current_layer) {
				save_keyboard_focus(current);
			}
			keyboard_set_focus(swc.seat->keyboard, exclusive->view);
		}
		return;
	}

	if (current_layer &&
	    (!current_layer->mapped ||
	     current_layer->current.keyboard_interactivity ==
	         ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
		struct compositor_view *restore = saved_keyboard_focus;

		clear_saved_keyboard_focus();
		keyboard_set_focus(swc.seat->keyboard, restore);
	} else if (!current_layer) {
		clear_saved_keyboard_focus();
	}
}

void
layer_shell_handle_pointer_press(struct compositor_view *view)
{
	struct layer_surface *surface = layer_surface_from_view(view);
	struct layer_surface *exclusive;
	struct compositor_view *current;

	if (session_lock_active()) {
		return;
	}

	if (!surface || !surface->mapped ||
	    surface->current.keyboard_interactivity ==
	        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		return;
	}

	exclusive = exclusive_keyboard_surface();
	if (exclusive && exclusive != surface) {
		update_keyboard_focus();
		return;
	}

	current = swc.seat->keyboard->focus.view;
	if (current != surface->view) {
		if (current && !layer_surface_from_view(current)) {
			save_keyboard_focus(current);
		}
		keyboard_set_focus(swc.seat->keyboard, surface->view);
	}
}

static bool
validate_state(struct layer_surface *surface,
               const struct layer_surface_state *state)
{
	const uint32_t all_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	                             ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
	                             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	uint32_t version = wl_resource_get_version(surface->resource);

	if (state->anchor & ~all_anchors) {
		wl_resource_post_error(surface->resource,
		                       ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
		                       "invalid anchor bitfield %#" PRIx32,
		                       state->anchor);
		return false;
	}
	if ((state->desired_width == 0 &&
	     (state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) !=
	         (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	          ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) ||
	    (state->desired_height == 0 &&
	     (state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) !=
	         (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	          ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))) {
		wl_resource_post_error(surface->resource,
		                       ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
		                       "zero size requires opposite anchors");
		return false;
	}
	if (state->exclusive_zone < -1) {
		wl_resource_post_error(
		    surface->resource,
		    ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
		    "exclusive zone must be at least -1");
		return false;
	}
	if (state->keyboard_interactivity >
	        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND ||
	    (version < 4 &&
	     state->keyboard_interactivity ==
	         ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)) {
		wl_resource_post_error(
		    surface->resource,
		    ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
		    "invalid keyboard interactivity mode %" PRIu32,
		    state->keyboard_interactivity);
		return false;
	}
	if (state->exclusive_edge != 0 &&
	    ((state->exclusive_edge & (state->exclusive_edge - 1)) != 0 ||
	     !(state->exclusive_edge & all_anchors) ||
	     !(state->anchor & state->exclusive_edge))) {
		wl_resource_post_error(
		    surface->resource,
		    ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
		    "exclusive edge must be one anchored edge");
		return false;
	}

	return true;
}

static int32_t
available_size(uint32_t size, int32_t start_margin, int32_t end_margin)
{
	int64_t available =
	    (int64_t)size - (int64_t)start_margin - (int64_t)end_margin;

	if (available < 0) {
		return 0;
	}
	return available > INT32_MAX ? INT32_MAX : (int32_t)available;
}

static uint32_t
configure_size(uint32_t desired, uint32_t anchor, uint32_t first_anchor,
               uint32_t second_anchor, uint32_t total_size, int32_t start_margin,
               int32_t end_margin)
{
	if (desired != 0) {
		return desired;
	}
	if ((anchor & first_anchor) && (anchor & second_anchor)) {
		return (uint32_t)available_size(total_size, start_margin, end_margin);
	}
	return 0;
}

static bool
send_configure(struct layer_surface *surface)
{
	struct layer_configure *configure;
	struct swc_rectangle bounds;
	pixman_box32_t box;
	uint32_t width, height;

	if (!surface->layer_screen || surface->closed) {
		return false;
	}

	calculate_available_box(surface->layer_screen,
	                        &surface->layer_screen->screen->base.geometry,
	                        surface, &box);
	if (surface->current.exclusive_zone == -1) {
		const struct swc_rectangle *geom =
		    &surface->layer_screen->screen->base.geometry;
		box = (pixman_box32_t){
		    .x1 = geom->x,
		    .y1 = geom->y,
		    .x2 = geom->x + (int32_t)geom->width,
		    .y2 = geom->y + (int32_t)geom->height,
		};
	}
	bounds = rectangle_from_box(&box);

	width = configure_size(surface->current.desired_width,
	                       surface->current.anchor,
	                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
	                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, bounds.width,
	                       surface->current.margin.left,
	                       surface->current.margin.right);
	height = configure_size(surface->current.desired_height,
	                        surface->current.anchor,
	                        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
	                        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, bounds.height,
	                        surface->current.margin.top,
	                        surface->current.margin.bottom);

	/* Bound memory if a client stops acknowledging configure events, as
	 * xdg_toplevel does. */
	if (surface->configure_count >= LAYER_CONFIGURE_MAX) {
		wl_resource_post_error(
		    surface->resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
		    "too many unacknowledged configure events");
		return false;
	}

	configure = malloc(sizeof(*configure));
	if (!configure) {
		wl_resource_post_no_memory(surface->resource);
		return false;
	}
	configure->serial = wl_display_next_serial(swc.display);
	wl_list_insert(surface->configures.prev, &configure->link);
	++surface->configure_count;
	zwlr_layer_surface_v1_send_configure(surface->resource, configure->serial,
	                                     width, height);
	return true;
}

static void
set_size(struct wl_client *client, struct wl_resource *resource, uint32_t width,
         uint32_t height)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface->closed) {
		surface->pending.desired_width = width;
		surface->pending.desired_height = height;
	}
}

static void
set_anchor(struct wl_client *client, struct wl_resource *resource,
           uint32_t anchor)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface->closed) {
		surface->pending.anchor = anchor;
	}
}

static void
set_exclusive_zone(struct wl_client *client, struct wl_resource *resource,
                   int32_t zone)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface->closed) {
		surface->pending.exclusive_zone = zone;
	}
}

static void
set_margin(struct wl_client *client, struct wl_resource *resource, int32_t top,
           int32_t right, int32_t bottom, int32_t left)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface->closed) {
		surface->pending.margin.top = top;
		surface->pending.margin.right = right;
		surface->pending.margin.bottom = bottom;
		surface->pending.margin.left = left;
	}
}

static void
set_keyboard_interactivity(struct wl_client *client, struct wl_resource *resource,
                           uint32_t keyboard_interactivity)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface->closed) {
		surface->pending.keyboard_interactivity = keyboard_interactivity;
	}
}

static void
get_popup(struct wl_client *client, struct wl_resource *resource,
          struct wl_resource *popup)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;

	if (surface->closed) {
		return;
	}
	if (!xdg_popup_set_parent(popup, surface->view)) {
		wl_resource_post_error(
		    resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
		    "popup already has a parent or is not an xdg_popup");
	}
}

static void
ack_configure(struct wl_client *client, struct wl_resource *resource,
              uint32_t serial)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	struct layer_configure *configure, *next, *matched = NULL;

	(void)client;
	if (surface->closed) {
		return;
	}

	wl_list_for_each(configure, &surface->configures, link)
	{
		if (configure->serial == serial) {
			matched = configure;
			break;
		}
	}
	if (!matched) {
		wl_resource_post_error(
		    resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
		    "unknown configure serial %" PRIu32, serial);
		return;
	}

	wl_list_for_each_safe(configure, next, &surface->configures, link)
	{
		bool done = configure == matched;
		wl_list_remove(&configure->link);
		free(configure);
		--surface->configure_count;
		if (done) {
			break;
		}
	}
	surface->configured = true;
}

static void
set_layer(struct wl_client *client, struct wl_resource *resource, uint32_t layer)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;

	if (surface->closed) {
		return;
	}
	if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
		wl_resource_post_error(resource,
		                       ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
		                       "invalid layer %" PRIu32, layer);
		return;
	}
	surface->pending.layer = layer;
}

static void
set_exclusive_edge(struct wl_client *client, struct wl_resource *resource,
                   uint32_t edge)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface->closed) {
		surface->pending.exclusive_edge = edge;
	}
}

static const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
	.set_size = set_size,
	.set_anchor = set_anchor,
	.set_exclusive_zone = set_exclusive_zone,
	.set_margin = set_margin,
	.set_keyboard_interactivity = set_keyboard_interactivity,
	.get_popup = get_popup,
	.ack_configure = ack_configure,
	.destroy = destroy_resource,
	.set_layer = set_layer,
	.set_exclusive_edge = set_exclusive_edge,
};

static void
handle_resize(struct view_handler *handler, uint32_t old_width,
              uint32_t old_height)
{
	struct layer_surface *surface = wl_container_of(handler, surface, view_handler);
	(void)old_width;
	(void)old_height;

	if (surface->mapped && surface->layer_screen) {
		screen_update_usable_geometry(surface->layer_screen->screen);
	}
}

static const struct view_handler_impl view_handler_impl = {
	.resize = handle_resize,
};

static void
reset_after_unmap(struct layer_surface *surface)
{
	struct layer_surface_state initial = {
	    .layer = surface->initial_layer,
	    .exclusive_zone = 0,
	    .keyboard_interactivity =
	        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE,
	};

	surface->mapped = false;
	compositor_view_hide(surface->view);
	clear_configures(surface);
	surface->configured = false;
	surface->current = initial;
	surface->pending = initial;
	restack_layer(surface);
	update_keyboard_focus();
	if (surface->layer_screen) {
		screen_update_usable_geometry(surface->layer_screen->screen);
	}
}

static void
handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct layer_surface *surface =
	    wl_container_of(listener, surface, surface_commit_listener);
	bool has_buffer = surface->view->base.buffer != NULL;
	bool state_changed;
	bool newly_mapped = !surface->mapped;

	(void)data;
	if (surface->closed) {
		return;
	}
	if (!validate_state(surface, &surface->pending)) {
		return;
	}

	if (!has_buffer && surface->mapped) {
		reset_after_unmap(surface);
		return;
	}
	if (has_buffer && !surface->configured) {
		wl_resource_post_error(
		    surface->resource,
		    ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
		    "buffer committed before a configure was acknowledged");
		return;
	}

	state_changed = !state_equal(&surface->current, &surface->pending);
	if (state_changed) {
		surface->current = surface->pending;
		restack_layer(surface);
	}

	if (!has_buffer) {
		if (state_changed || wl_list_empty(&surface->configures)) {
			send_configure(surface);
		}
		return;
	}

	if (!surface->mapped) {
		surface->mapped = true;
		compositor_view_show(surface->view);
	}

	/* Pixel/frame-only commits do not change panel layout or keyboard focus.
	 * Size changes are handled by handle_resize. */
	if ((state_changed || newly_mapped) && surface->layer_screen) {
		screen_update_usable_geometry(surface->layer_screen->screen);
	}
	if (state_changed || newly_mapped)
		update_keyboard_focus();

	if (state_changed) {
		send_configure(surface);
	}
}

/* Detach an unmapped surface from its screen and tell the client. */
static void
close_layer_surface(struct layer_surface *surface)
{
	clear_configures(surface);
	surface->configured = false;
	surface->closed = true;
	surface->layer_screen = NULL;
	wl_list_remove(&surface->screen_link);
	wl_list_init(&surface->screen_link);
	zwlr_layer_surface_v1_send_closed(surface->resource);
}

static void
handle_screen_destroy(struct wl_listener *listener, void *data)
{
	struct layer_screen *layer_screen =
	    wl_container_of(listener, layer_screen, screen_destroy_listener);
	struct layer_surface *surface, *next;

	(void)data;
	wl_list_for_each(surface, &layer_screen->surfaces, screen_link)
	{
		surface->mapped = false;
		compositor_view_hide(surface->view);
	}
	update_keyboard_focus();

	wl_list_for_each_safe(surface, next, &layer_screen->surfaces, screen_link)
	    close_layer_surface(surface);

	wl_list_remove(&layer_screen->modifier.link);
	wl_list_remove(&layer_screen->screen_destroy_listener.link);
	wl_list_remove(&layer_screen->link);
	free(layer_screen);
}

static struct layer_screen *
get_layer_screen(struct screen *screen)
{
	struct layer_screen *layer_screen;

	wl_list_for_each(layer_screen, &layer_screens, link)
	{
		if (layer_screen->screen == screen) {
			return layer_screen;
		}
	}

	layer_screen = calloc(1, sizeof(*layer_screen));
	if (!layer_screen) {
		return NULL;
	}
	layer_screen->screen = screen;
	layer_screen->modifier.modify = modify;
	layer_screen->screen_destroy_listener.notify = handle_screen_destroy;
	wl_list_init(&layer_screen->surfaces);
	wl_list_insert(&screen->modifiers, &layer_screen->modifier.link);
	wl_signal_add(&screen->destroy_signal,
	              &layer_screen->screen_destroy_listener);
	wl_list_insert(layer_screens.prev, &layer_screen->link);
	return layer_screen;
}

static void
release_layer_screen_if_empty(struct layer_screen *layer_screen)
{
	struct screen *screen;

	if (!layer_screen || !wl_list_empty(&layer_screen->surfaces)) {
		return;
	}

	screen = layer_screen->screen;
	wl_list_remove(&layer_screen->modifier.link);
	wl_list_remove(&layer_screen->screen_destroy_listener.link);
	wl_list_remove(&layer_screen->link);
	free(layer_screen);
	screen_update_usable_geometry(screen);
}

static void
destroy_layer_surface(struct wl_resource *resource)
{
	struct layer_surface *surface = wl_resource_get_user_data(resource);
	struct layer_screen *layer_screen = surface->layer_screen;

	if (surface->mapped) {
		surface->mapped = false;
		compositor_view_hide(surface->view);
		update_keyboard_focus();
	}
	clear_configures(surface);
	wl_list_remove(&surface->surface_destroy_listener.link);
	wl_list_remove(&surface->surface_commit_listener.link);
	if (layer_screen) {
		wl_list_remove(&surface->screen_link);
		wl_list_init(&surface->screen_link);
	}
	compositor_view_destroy(surface->view);
	free(surface);

	if (layer_screen) {
		if (wl_list_empty(&layer_screen->surfaces)) {
			release_layer_screen_if_empty(layer_screen);
		} else {
			screen_update_usable_geometry(layer_screen->screen);
		}
	}
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct layer_surface *surface =
	    wl_container_of(listener, surface, surface_destroy_listener);
	(void)data;
	wl_resource_destroy(surface->resource);
}

static struct layer_surface *
layer_surface_new(struct wl_client *client, uint32_t version, uint32_t id,
                  struct surface *surface, struct screen *screen, uint32_t layer)
{
	struct layer_surface *layer_surface;
	struct layer_screen *layer_screen;

	layer_surface = calloc(1, sizeof(*layer_surface));
	if (!layer_surface) {
		return NULL;
	}
	wl_list_init(&layer_surface->screen_link);
	wl_list_init(&layer_surface->configures);
	layer_surface->configure_count = 0;

	layer_surface->resource =
	    wl_resource_create(client, &zwlr_layer_surface_v1_interface, version, id);
	if (!layer_surface->resource) {
		goto error1;
	}
	layer_surface->view = compositor_create_view(surface);
	if (!layer_surface->view) {
		goto error2;
	}
	if (!surface_set_role(surface, layer_surface->resource)) {
		goto error3;
	}
	layer_screen = get_layer_screen(screen);
	if (!layer_screen) {
		goto error3;
	}

	layer_surface->layer_screen = layer_screen;
	layer_surface->initial_layer = layer;
	layer_surface->current.layer = layer;
	layer_surface->current.exclusive_zone = 0;
	layer_surface->current.keyboard_interactivity =
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	layer_surface->pending = layer_surface->current;
	layer_surface->surface_destroy_listener.notify = handle_surface_destroy;
	layer_surface->surface_commit_listener.notify = handle_surface_commit;
	layer_surface->view_handler.impl = &view_handler_impl;
	wl_resource_set_implementation(layer_surface->resource, &layer_surface_impl,
	                               layer_surface, destroy_layer_surface);
	wl_resource_add_destroy_listener(surface->resource,
	                                 &layer_surface->surface_destroy_listener);
	wl_signal_add(&surface->signal.commit,
	              &layer_surface->surface_commit_listener);
	wl_list_insert(&layer_surface->view->base.handlers,
	               &layer_surface->view_handler.link);
	wl_list_insert(layer_screen->surfaces.prev, &layer_surface->screen_link);
	restack_layer(layer_surface);
	return layer_surface;

error3:
	compositor_view_destroy(layer_surface->view);
error2:
	wl_resource_destroy(layer_surface->resource);
error1:
	free(layer_surface);
	return NULL;
}

static struct screen *
default_screen(void)
{
	struct screen *screen;

	if (swc.seat && swc.seat->pointer) {
		int32_t x = wl_fixed_to_int(swc.seat->pointer->x);
		int32_t y = wl_fixed_to_int(swc.seat->pointer->y);

		wl_list_for_each(screen, &swc.screens, link)
		{
			if (rectangle_contains_point(&screen->base.geometry, x, y)) {
				return screen;
			}
		}
	}

	if (!wl_list_empty(&swc.screens)) {
		return wl_container_of(swc.screens.next, screen, link);
	}
	return NULL;
}

static void
get_layer_surface(struct wl_client *client, struct wl_resource *resource,
                  uint32_t id, struct wl_resource *surface_resource,
                  struct wl_resource *output_resource, uint32_t layer,
                  const char *namespace_)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct layer_surface *layer_surface;
	struct output *output;
	struct screen *screen;
	bool gone = false;

	(void)namespace_;
	if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
		wl_resource_post_error(resource,
		                       ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
		                       "invalid layer %" PRIu32, layer);
		return;
	}
	if (surface->role) {
		wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
		                       "surface already has a role");
		return;
	}
	if (surface_has_buffer(surface)) {
		wl_resource_post_error(resource,
		                       ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
		                       "surface already has a buffer");
		return;
	}

	if (output_resource) {
		output = wl_resource_get_user_data(output_resource);
		screen = output ? output->screen : NULL;
		/* Asked for on an output that has since been unplugged. That is a
		 * race the client cannot avoid, not an error: it gets a surface
		 * that is closed straight away, as if the output had gone after. */
		if (!screen) {
			gone = true;
			screen = default_screen();
		}
	} else {
		screen = default_screen();
	}

	if (!screen ||
	    !(layer_surface = layer_surface_new(client,
	                                        wl_resource_get_version(resource),
	                                        id, surface, screen, layer))) {
		wl_client_post_no_memory(client);
		return;
	}
	if (gone) {
		struct layer_screen *layer_screen = layer_surface->layer_screen;

		close_layer_surface(layer_surface);
		release_layer_screen_if_empty(layer_screen);
	}
}

static const struct zwlr_layer_shell_v1_interface layer_shell_impl = {
	.get_layer_surface = get_layer_surface,
	.destroy = destroy_resource,
};

static void
bind_layer_shell(struct wl_client *client, void *data, uint32_t version,
                 uint32_t id)
{
	struct wl_resource *resource;
	(void)data;

	resource =
	    wl_resource_create(client, &zwlr_layer_shell_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &layer_shell_impl, NULL, NULL);
}

struct wl_global *
layer_shell_create(struct wl_display *display)
{
	wl_list_init(&layer_screens);
	wl_list_init(&saved_keyboard_focus_destroy.link);
	return wl_global_create(display, &zwlr_layer_shell_v1_interface, 5, NULL,
	                        bind_layer_shell);
}
