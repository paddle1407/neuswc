/* swc: libswc/primary_plane.h
 *
 * Copyright (c) 2013, 2016 Michael Forney
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

#ifndef SWC_PRIMARY_PLANE_H
#define SWC_PRIMARY_PLANE_H

#include "mode.h"
#include "view.h"
#ifdef ENABLE_DRM
#include "drm.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

#ifdef ENABLE_DRM
struct primary_plane;

struct primary_plane_flip {
	struct drm_handler handler;
	/* NULL once the plane is finalized with a flip still pending. */
	struct primary_plane *plane;
	bool pending;
};
#endif

struct primary_plane {
#ifdef ENABLE_DRM
	uint32_t crtc;
	drmModeCrtcPtr original_crtc_state;
#endif
	struct mode mode;
	struct view view;
#ifdef ENABLE_DRM
	struct wl_array connectors;
	bool need_modeset;
	struct primary_plane_flip *flip;

	/*
	 * A framebuffer whose rendering the GPU has not finished, and the
	 * sync_file that signals when it has. It is presented from the event
	 * loop once the fence signals rather than by blocking for it.
	 */
	struct wl_event_source *fence_source;
	int fence_fd;
	uint32_t fence_fb;
#endif
	struct wl_listener swc_listener;
};

#ifdef ENABLE_DRM
bool
primary_plane_initialize(struct primary_plane *plane, uint32_t crtc,
                         struct mode *mode, uint32_t *connectors,
                         uint32_t num_connectors);
#else
bool
primary_plane_initialize(struct primary_plane *plane, struct mode *mode);
#endif
#ifdef ENABLE_DRM
/* Turn the CRTC off for good, for a screen whose monitor was unplugged;
 * primary_plane_finalize then leaves it off instead of restoring it. */
void
primary_plane_disable(struct primary_plane *plane);
#endif
void
primary_plane_finalize(struct primary_plane *plane);

#endif
