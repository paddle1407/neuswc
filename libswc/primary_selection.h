/* swc: libswc/primary_selection.h
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

#ifndef SWC_PRIMARY_SELECTION_H
#define SWC_PRIMARY_SELECTION_H

#include <stdbool.h>
#include <wayland-server.h>

enum { PRIMARY_SELECTION_EVENT_SELECTION_CHANGED };

/*
 * The primary selection -- what a middle click pastes -- is a second clipboard
 * that follows the text a user highlights, rather than an explicit copy. It is
 * deliberately a near-copy of data_device rather than shared code with it: the
 * two carry different object types, and the protocol keeps their lifetimes and
 * their serial validation separate.
 */
struct primary_selection_device {
	/* The source resource behind the current selection, or NULL. */
	struct wl_resource *selection;
	struct wl_listener selection_destroy_listener;

	struct wl_signal event_signal;
	struct wl_list resources;
};

struct primary_selection_device *
primary_selection_device_create(void);
void
primary_selection_device_destroy(struct primary_selection_device *device);
/* Send the current selection, if any, to one client's device resource. */
void
primary_selection_device_offer(struct primary_selection_device *device,
                               struct wl_client *client);

struct wl_global *
primary_selection_device_manager_create(struct wl_display *display);

#endif
