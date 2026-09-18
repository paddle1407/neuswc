/* swc: libswc/cursor_shape.c
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

#include "cursor_shape.h"
#include "internal.h"
#include "pointer.h"
#include "seat.h"
#include "util.h"

#include "cursor-shape-v1-server-protocol.h"

#include <inttypes.h>
#include <stdlib.h>

/*
 * cursor-shape-v1 lets a client name the cursor it wants -- a text bar, a
 * resize arrow -- instead of drawing one and attaching it as a surface. The
 * compositor then supplies the themed image, so every application's I-beam
 * looks the same and matches appearance.cursor.theme.
 */

/*
 * Shape numbers start at 1 and are dense, so a table indexed by shape is the
 * clearest mapping. Index 0 is unused; the protocol has no shape 0.
 */
static const enum swc_cursor_kind shape_to_kind[] = {
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT] = SWC_CURSOR_DEFAULT,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU] = SWC_CURSOR_CONTEXT_MENU,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP] = SWC_CURSOR_HELP,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER] = SWC_CURSOR_POINTER,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS] = SWC_CURSOR_PROGRESS,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT] = SWC_CURSOR_WAIT,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL] = SWC_CURSOR_CELL,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR] = SWC_CURSOR_CROSSHAIR,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT] = SWC_CURSOR_TEXT,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT] = SWC_CURSOR_VERTICAL_TEXT,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS] = SWC_CURSOR_ALIAS,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY] = SWC_CURSOR_COPY,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE] = SWC_CURSOR_MOVE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP] = SWC_CURSOR_NO_DROP,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED] = SWC_CURSOR_NOT_ALLOWED,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB] = SWC_CURSOR_GRAB,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING] = SWC_CURSOR_GRABBING,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE] = SWC_CURSOR_E_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE] = SWC_CURSOR_N_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE] = SWC_CURSOR_NE_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE] = SWC_CURSOR_NW_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE] = SWC_CURSOR_S_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE] = SWC_CURSOR_SE_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE] = SWC_CURSOR_SW_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE] = SWC_CURSOR_W_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE] = SWC_CURSOR_EW_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE] = SWC_CURSOR_NS_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE] = SWC_CURSOR_NESW_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE] = SWC_CURSOR_NWSE_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE] = SWC_CURSOR_COL_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE] = SWC_CURSOR_ROW_RESIZE,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL] = SWC_CURSOR_ALL_SCROLL,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN] = SWC_CURSOR_ZOOM_IN,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT] = SWC_CURSOR_ZOOM_OUT,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK] = SWC_CURSOR_DND_ASK,
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE] = SWC_CURSOR_ALL_RESIZE,
};

/* Whether this device drives the pointer. Tablet devices are accepted so that
 * a client binding the manager does not fail, but swc has no tablet input, so
 * nothing can ever reach their set_shape. */
struct cursor_shape_device {
	bool is_pointer;
};

static void
device_set_shape(struct wl_client *client, struct wl_resource *resource,
                 uint32_t serial, uint32_t shape)
{
	struct cursor_shape_device *device = wl_resource_get_user_data(resource);

	(void)client;
	(void)serial;

	if (shape == 0 || shape >= ARRAY_LENGTH(shape_to_kind)) {
		wl_resource_post_error(resource,
		                       WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
		                       "unknown cursor shape %" PRIu32, shape);
		return;
	}

	if (!device || !device->is_pointer || !swc.seat) {
		return;
	}

	pointer_set_shape(swc.seat->pointer, shape_to_kind[shape]);
}

static void
destroy_device(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

static const struct wp_cursor_shape_device_v1_interface device_impl = {
	.destroy = destroy_resource,
	.set_shape = device_set_shape,
};

static void
create_device(struct wl_client *client, struct wl_resource *manager,
              uint32_t id, bool is_pointer)
{
	struct cursor_shape_device *device;
	struct wl_resource *resource;

	device = malloc(sizeof(*device));
	if (!device) {
		wl_client_post_no_memory(client);
		return;
	}
	device->is_pointer = is_pointer;

	resource = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
	                              wl_resource_get_version(manager), id);
	if (!resource) {
		free(device);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &device_impl, device,
	                               destroy_device);
}

static void
get_pointer(struct wl_client *client, struct wl_resource *manager, uint32_t id,
            struct wl_resource *pointer)
{
	(void)pointer;
	create_device(client, manager, id, true);
}

static void
get_tablet_tool_v2(struct wl_client *client, struct wl_resource *manager,
                   uint32_t id, struct wl_resource *tablet_tool)
{
	(void)tablet_tool;
	create_device(client, manager, id, false);
}

static const struct wp_cursor_shape_manager_v1_interface manager_impl = {
	.destroy = destroy_resource,
	.get_pointer = get_pointer,
	.get_tablet_tool_v2 = get_tablet_tool_v2,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface,
	                              version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *
cursor_shape_manager_create(struct wl_display *display)
{
	return wl_global_create(display, &wp_cursor_shape_manager_v1_interface, 2,
	                        NULL, &bind_manager);
}
