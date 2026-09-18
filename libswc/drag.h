/* swc: drag.h
 *
 * Copyright (c) 2013-2020 Michael Forney
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

#ifndef SWC_DRAG_H
#define SWC_DRAG_H

#include <stdint.h>

struct data_device;
struct wl_resource;

/* wl_data_device.start_drag. Invalid requests are ignored, as the protocol
 * asks: a client that lost the race for the pointer must not be killed for it. */
void
drag_start(struct data_device *data_device, struct wl_resource *device_resource,
           struct wl_resource *source_resource,
           struct wl_resource *origin_resource,
           struct wl_resource *icon_resource, uint32_t serial);

/*
 * Hooks from data.c.
 *
 * A wl_data_offer and a wl_data_source carry the drag-and-drop half of their
 * interfaces whether or not a drag is in progress, so data.c forwards those
 * requests here and each one does nothing unless the object it names belongs
 * to the drag currently under way.
 */
void
drag_offer_accept(struct wl_resource *offer, const char *mime_type);
void
drag_offer_set_actions(struct wl_resource *offer, uint32_t actions,
                       uint32_t preferred);
void
drag_offer_finish(struct wl_resource *offer);
void
drag_source_actions_changed(struct wl_resource *source);

#endif
