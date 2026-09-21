/* swc: surface.h
 *
 * Copyright (c) 2013 Michael Forney
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

#ifndef SWC_SURFACE_H
#define SWC_SURFACE_H

#include "view.h"

#include <pixman.h>
#include <wayland-server.h>

struct subsurface;
struct drm_syncobj_surface;

enum {
	SURFACE_COMMIT_ATTACH = (1 << 0),
	SURFACE_COMMIT_DAMAGE = (1 << 1),
	SURFACE_COMMIT_OPAQUE = (1 << 2),
	SURFACE_COMMIT_INPUT = (1 << 3),
	SURFACE_COMMIT_FRAME = (1 << 4),
	SURFACE_COMMIT_GEOMETRY = (1 << 5)
};

struct surface_state {
	struct wld_buffer *buffer;
	struct wl_resource *buffer_resource;
	struct wl_listener buffer_destroy_listener;

	/* The region that needs to be repainted. */
	pixman_region32_t damage;

	/* The region that is opaque. */
	pixman_region32_t opaque;

	/* The region that accepts input. */
	pixman_region32_t input;

	struct wl_list frame_callbacks;

	/* subsurface order; double-buffered with surface state. */
	struct wl_list subsurfaces_below;
	struct wl_list subsurfaces_above;
};

struct surface {
	struct wl_resource *resource;
	struct {
		struct wl_signal commit;
		/* Emitted while the surface and its state are still alive. */
		struct wl_signal destroy;
	} signal;

	struct surface_state state;

	struct {
		struct surface_state state;
		uint32_t commit;
		int32_t x, y;
		struct swc_rectangle window_geometry;
	} pending;

	struct view *view;
	struct view_handler view_handler;
	struct wl_resource *role;
	struct wl_listener role_destroy_listener;

	struct subsurface *subsurface;
	struct wl_list subsurfaces;

	/* Explicit synchronization state, when the client asked for it. */
	struct drm_syncobj_surface *synced;

	bool has_window_geometry;
	int32_t window_x, window_y;
	int32_t window_width, window_height;
};

struct surface *
surface_new(struct wl_client *client, uint32_t version, uint32_t id);
/**
 * Returns the surface behind a resource, or NULL if it is not one of ours.
 *
 * Needed wherever an object id arrives from outside the Wayland connection it
 * names -- Xwayland's WL_SURFACE_ID, for one -- because wl_client_get_object()
 * will happily return a wl_buffer or a wl_output for the wrong id, and its
 * user data is not a struct surface.
 */
struct surface *
surface_from_resource(struct wl_resource *resource);
void
surface_set_view(struct surface *surface, struct view *view);
bool
surface_set_role(struct surface *surface, struct wl_resource *role);
bool
surface_has_buffer(struct surface *surface);
void
surface_commit_pending(struct surface *surface);
/**
 * Puts a buffer the surface committed on screen after explicit
 * synchronization held it back, with all of it as damage.
 */
void
surface_show_buffer(struct surface *surface, struct wld_buffer *buffer);

#endif
