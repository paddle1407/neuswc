/* swc: libswc/session_lock.c
 *
 * Copyright (c) 2025 charaWC contributors
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

#include "session_lock.h"
#include "compositor.h"
#include "internal.h"
#include "keyboard.h"
#include "output.h"
#include "pointer.h"
#include "screen.h"
#include "seat.h"
#include "surface.h"
#include "util.h"
#include "view.h"

#include "ext-session-lock-v1-server-protocol.h"

#include <stdlib.h>

/*
 * ext-session-lock-v1 is how a screen locker works on Wayland. The client asks
 * to lock, paints one surface per output, and the compositor guarantees that
 * nothing else is shown or reachable until that same client says to unlock.
 *
 * The guarantee is the whole point, so the failure modes matter more than the
 * happy path:
 *
 *  - A locker that crashes must NOT unlock. The session stays locked with a
 *    blank screen, and the way out is a VT switch, which is why VT bindings
 *    keep working while locked (see bindings.c).
 *  - Content must not be visible even if the locker never covers an output.
 *    Rather than trusting the lock surfaces to be opaque and complete, every
 *    other view is hidden outright and the wallpaper paints black.
 *  - 'locked' is only sent once every output is actually covered. A locker
 *    that hides the password box until it sees that event depends on it.
 */

struct lock_surface {
	struct wl_resource *resource;
	struct surface *surface;
	struct compositor_view *view;
	struct screen *screen;

	struct wl_listener surface_destroy_listener;
	struct wl_listener surface_commit_listener;
	struct wl_listener screen_destroy_listener;

	uint32_t configure_serial;
	bool acked;
	bool mapped;

	struct wl_list link;
};

static struct {
	/* The live ext_session_lock_v1, or NULL when the locker has gone but
	 * the session is still locked. */
	struct wl_resource *resource;
	bool locked;
	bool sent_locked;
	bool views_hidden;

	struct wl_list surfaces;

	struct compositor_view *saved_focus;
	struct wl_listener saved_focus_destroy;
	bool saved_focus_active;
} lock;

static bool initialized;

bool
session_lock_active(void)
{
	return initialized && lock.locked;
}

/* --------------------------------------------------------- saved focus */

static void
handle_saved_focus_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;

	wl_list_remove(&lock.saved_focus_destroy.link);
	wl_list_init(&lock.saved_focus_destroy.link);
	lock.saved_focus = NULL;
	lock.saved_focus_active = false;
}

static void
clear_saved_focus(void)
{
	if (lock.saved_focus_active) {
		wl_list_remove(&lock.saved_focus_destroy.link);
		wl_list_init(&lock.saved_focus_destroy.link);
	}
	lock.saved_focus = NULL;
	lock.saved_focus_active = false;
}

static void
save_focus(void)
{
	struct compositor_view *view =
	    swc.seat && swc.seat->keyboard ? swc.seat->keyboard->focus.view : NULL;

	clear_saved_focus();
	if (!view) {
		return;
	}
	lock.saved_focus = view;
	lock.saved_focus_destroy.notify = handle_saved_focus_destroy;
	wl_signal_add(&view->destroy_signal, &lock.saved_focus_destroy);
	lock.saved_focus_active = true;
}

/* ------------------------------------------------------------- helpers */

static struct lock_surface *
surface_for_screen(struct screen *screen)
{
	struct lock_surface *surface;

	wl_list_for_each(surface, &lock.surfaces, link)
	{
		if (surface->screen == screen && surface->mapped) {
			return surface;
		}
	}
	return NULL;
}

/* Give the keyboard to any mapped lock surface, preferring the one the
 * pointer is over so that typing goes where the user is looking. */
static void
focus_a_lock_surface(void)
{
	struct lock_surface *surface, *chosen = NULL;
	int32_t x, y;
	bool have_pointer;

	have_pointer = swc.seat && swc.seat->pointer;
	if (have_pointer) {
		x = wl_fixed_to_int(swc.seat->pointer->x);
		y = wl_fixed_to_int(swc.seat->pointer->y);
	}

	wl_list_for_each(surface, &lock.surfaces, link)
	{
		if (!surface->mapped) {
			continue;
		}
		if (!chosen) {
			chosen = surface;
		}
		/* Type where the user is looking: prefer the surface on the monitor
		 * the pointer is on, and fall back to whichever mapped first. */
		if (have_pointer && surface->screen &&
		    rectangle_contains_point(&surface->screen->base.geometry, x, y)) {
			chosen = surface;
			break;
		}
	}

	if (chosen && swc.seat && swc.seat->keyboard) {
		keyboard_set_focus(swc.seat->keyboard, chosen->view);
	}
}

/* Every connected output has a mapped lock surface. */
static bool
all_screens_covered(void)
{
	struct screen *screen;

	if (wl_list_empty(&swc.screens)) {
		return false;
	}
	wl_list_for_each(screen, &swc.screens, link)
	{
		if (!surface_for_screen(screen)) {
			return false;
		}
	}
	return true;
}

static void
maybe_send_locked(void)
{
	if (lock.sent_locked || !lock.resource || !all_screens_covered()) {
		return;
	}
	lock.sent_locked = true;
	ext_session_lock_v1_send_locked(lock.resource);
}

static void
begin_lock(void)
{
	lock.locked = true;
	lock.sent_locked = false;

	save_focus();
	if (swc.seat && swc.seat->keyboard) {
		keyboard_set_focus(swc.seat->keyboard, NULL);
	}

	if (!lock.views_hidden) {
		compositor_hide_for_lock();
		lock.views_hidden = true;
	}
	compositor_damage_all();
}

static void
end_lock(void)
{
	struct compositor_view *restore;

	lock.locked = false;
	lock.sent_locked = false;
	lock.resource = NULL;

	if (lock.views_hidden) {
		compositor_restore_after_lock();
		lock.views_hidden = false;
	}

	restore = lock.saved_focus;
	clear_saved_focus();
	if (swc.seat && swc.seat->keyboard) {
		keyboard_set_focus(swc.seat->keyboard, restore);
	}
	compositor_damage_all();
}

/* ------------------------------------------------------- lock surface */

static void
send_configure(struct lock_surface *surface)
{
	const struct swc_rectangle *geom;

	if (!surface->screen) {
		return;
	}
	geom = &surface->screen->base.geometry;
	surface->configure_serial = wl_display_next_serial(swc.display);
	ext_session_lock_surface_v1_send_configure(
	    surface->resource, surface->configure_serial, geom->width,
	    geom->height);
}

static void
lock_surface_ack_configure(struct wl_client *client,
                           struct wl_resource *resource, uint32_t serial)
{
	struct lock_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (!surface) {
		return;
	}
	if (serial != surface->configure_serial) {
		wl_resource_post_error(
		    resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
		    "serial %u does not match the configure that was sent", serial);
		return;
	}
	surface->acked = true;
}

static const struct ext_session_lock_surface_v1_interface lock_surface_impl = {
	.destroy = destroy_resource,
	.ack_configure = lock_surface_ack_configure,
};

static void
handle_lock_surface_commit(struct wl_listener *listener, void *data)
{
	struct lock_surface *surface =
	    wl_container_of(listener, surface, surface_commit_listener);
	bool has_buffer = surface->view->base.buffer != NULL;
	const struct swc_rectangle *geom;

	(void)data;

	if (has_buffer && !surface->acked) {
		wl_resource_post_error(
		    surface->resource,
		    EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
		    "buffer committed before a configure was acknowledged");
		return;
	}

	if (!has_buffer) {
		if (surface->mapped) {
			wl_resource_post_error(
			    surface->resource,
			    EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
			    "a mapped lock surface may not commit a null buffer");
		}
		return;
	}

	if (!surface->screen) {
		return;
	}

	geom = &surface->screen->base.geometry;
	/* A lock surface must be exactly its output's size. Anything else would
	 * leave a strip of desktop showing at the edge. */
	if (surface->view->base.geometry.width != geom->width ||
	    surface->view->base.geometry.height != geom->height) {
		wl_resource_post_error(
		    surface->resource,
		    EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH,
		    "lock surface is %ux%u but its output is %ux%u",
		    surface->view->base.geometry.width,
		    surface->view->base.geometry.height, geom->width, geom->height);
		return;
	}

	view_move(&surface->view->base, geom->x, geom->y);

	if (!surface->mapped) {
		surface->mapped = true;
		surface->view->always_top = true;
		compositor_view_set_stack_layer(surface->view, STACK_LAYER_LOCK,
		                                true);
		compositor_view_show(surface->view);
		focus_a_lock_surface();
	}

	maybe_send_locked();
}

static void
destroy_lock_surface(struct wl_resource *resource)
{
	struct lock_surface *surface = wl_resource_get_user_data(resource);

	if (!surface) {
		return;
	}
	wl_list_remove(&surface->surface_destroy_listener.link);
	wl_list_remove(&surface->surface_commit_listener.link);
	if (surface->screen) {
		wl_list_remove(&surface->screen_destroy_listener.link);
	}
	wl_list_remove(&surface->link);
	compositor_view_destroy(surface->view);
	free(surface);

	/* Losing a surface while locked leaves that output blank, which is the
	 * safe direction: the wallpaper paints black and every other view is
	 * still hidden. */
	if (lock.locked) {
		compositor_damage_all();
	}
}

static void
handle_lock_surface_destroy(struct wl_listener *listener, void *data)
{
	struct lock_surface *surface =
	    wl_container_of(listener, surface, surface_destroy_listener);
	(void)data;
	wl_resource_destroy(surface->resource);
}

static void
handle_lock_screen_destroy(struct wl_listener *listener, void *data)
{
	struct lock_surface *surface =
	    wl_container_of(listener, surface, screen_destroy_listener);

	(void)data;
	wl_list_remove(&surface->screen_destroy_listener.link);
	wl_list_init(&surface->screen_destroy_listener.link);
	surface->screen = NULL;
	if (surface->mapped) {
		surface->mapped = false;
		compositor_view_hide(surface->view);
	}
}

/* --------------------------------------------------------------- lock */

static void
get_lock_surface(struct wl_client *client, struct wl_resource *resource,
                 uint32_t id, struct wl_resource *surface_resource,
                 struct wl_resource *output_resource)
{
	struct lock_surface *lock_surface = NULL;
	struct surface *surface;
	struct output *output;
	struct screen *screen;

	if (resource != lock.resource) {
		/* A lock object that was told 'finished' never owned the session,
		 * so it has no business putting surfaces on the screen. */
		return;
	}

	surface = surface_from_resource(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_ROLE,
		                       "object is not a wl_surface");
		return;
	}
	if (surface_has_buffer(surface)) {
		wl_resource_post_error(
		    resource, EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
		    "the surface already has a buffer attached");
		return;
	}

	/* A wl_output resource carries the struct output, not the screen behind
	 * it; every other user of one goes through ->screen the same way. */
	output = wl_resource_get_user_data(output_resource);
	screen = output ? output->screen : NULL;
	/* One lock surface per output. A second would leave the compositor
	 * picking between them, which the protocol forbids outright. */
	wl_list_for_each(lock_surface, &lock.surfaces, link)
	{
		if (screen && lock_surface->screen == screen) {
			wl_resource_post_error(
			    resource, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
			    "this output already has a lock surface");
			return;
		}
	}

	lock_surface = calloc(1, sizeof(*lock_surface));
	if (!lock_surface) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&lock_surface->link);
	wl_list_init(&lock_surface->screen_destroy_listener.link);

	lock_surface->resource = wl_resource_create(
	    client, &ext_session_lock_surface_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!lock_surface->resource) {
		free(lock_surface);
		wl_client_post_no_memory(client);
		return;
	}
	lock_surface->view = compositor_create_view(surface);
	if (!lock_surface->view) {
		wl_resource_destroy(lock_surface->resource);
		free(lock_surface);
		wl_client_post_no_memory(client);
		return;
	}
	if (!surface_set_role(surface, lock_surface->resource)) {
		compositor_view_destroy(lock_surface->view);
		wl_resource_destroy(lock_surface->resource);
		free(lock_surface);
		wl_resource_post_error(resource,
		                       EXT_SESSION_LOCK_V1_ERROR_ROLE,
		                       "the surface already has a role");
		return;
	}

	lock_surface->surface = surface;
	lock_surface->screen = screen;
	lock_surface->surface_destroy_listener.notify = handle_lock_surface_destroy;
	lock_surface->surface_commit_listener.notify = handle_lock_surface_commit;
	wl_resource_add_destroy_listener(surface->resource,
	                                 &lock_surface->surface_destroy_listener);
	wl_signal_add(&surface->signal.commit,
	              &lock_surface->surface_commit_listener);
	if (screen) {
		lock_surface->screen_destroy_listener.notify =
		    handle_lock_screen_destroy;
		wl_signal_add(&screen->destroy_signal,
		              &lock_surface->screen_destroy_listener);
	}
	wl_list_insert(lock.surfaces.prev, &lock_surface->link);
	wl_resource_set_implementation(lock_surface->resource, &lock_surface_impl,
	                               lock_surface, destroy_lock_surface);

	send_configure(lock_surface);
}

static void
unlock_and_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;

	if (resource != lock.resource) {
		wl_resource_post_error(
		    resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
		    "this lock object does not hold the session");
		return;
	}

	if (!lock.sent_locked) {
		wl_resource_post_error(
		    resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
		    "the session was never locked, so it cannot be unlocked");
		return;
	}

	end_lock();
	wl_resource_destroy(resource);
}

static const struct ext_session_lock_v1_interface lock_impl = {
	.destroy = destroy_resource,
	.get_lock_surface = get_lock_surface,
	.unlock_and_destroy = unlock_and_destroy,
};

static void
destroy_lock(struct wl_resource *resource)
{
	if (lock.resource != resource) {
		/* A refused second lock object; it never owned the session. */
		return;
	}

	/* Deliberately does NOT unlock. unlock_and_destroy is the only way out;
	 * a locker that dies leaves a locked, blank session behind. */
	lock.resource = NULL;
	if (lock.locked) {
		WARNING("session lock client disconnected while locked; "
		        "the session stays locked -- switch VT to recover\n");
		compositor_damage_all();
	}
}

static void
manager_lock(struct wl_client *client, struct wl_resource *manager,
             uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &ext_session_lock_v1_interface,
	                              wl_resource_get_version(manager), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	if (lock.locked) {
		/* Someone already holds the session. Tell this one so it can exit
		 * instead of drawing a second password box nobody can see. */
		wl_resource_set_implementation(resource, &lock_impl, NULL, NULL);
		ext_session_lock_v1_send_finished(resource);
		return;
	}

	wl_resource_set_implementation(resource, &lock_impl, NULL, destroy_lock);
	lock.resource = resource;
	begin_lock();
}

static const struct ext_session_lock_manager_v1_interface manager_impl = {
	.destroy = destroy_resource,
	.lock = manager_lock,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(
	    client, &ext_session_lock_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *
session_lock_manager_create(struct wl_display *display)
{
	struct wl_global *global;

	wl_list_init(&lock.surfaces);
	wl_list_init(&lock.saved_focus_destroy.link);
	global = wl_global_create(display, &ext_session_lock_manager_v1_interface,
	                          1, NULL, &bind_manager);
	if (global) {
		initialized = true;
	}
	return global;
}

void
session_lock_finish(void)
{
	struct lock_surface *surface, *tmp;

	if (!initialized) {
		return;
	}
	wl_list_for_each_safe(surface, tmp, &lock.surfaces, link)
	    wl_resource_destroy(surface->resource);
	clear_saved_focus();
	lock.locked = false;
	lock.resource = NULL;
	initialized = false;
}
