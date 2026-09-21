/* swc: drm.c
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

#include "drm.h"
#include "compositor.h"
#include "dmabuf.h"
#include "drm_syncobj.h"
#include "event.h"
#include "internal.h"
#include "launch.h"
#include "output.h"
#include "plane.h"
#include "screen.h"
#include "util.h"
#include "wayland_buffer.h"

#include "wayland-drm-server-protocol.h"
#include <dirent.h>
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wld/drm.h>
#include <wld/wld.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#ifdef ENABLE_LIBUDEV
#include <libudev.h>
#endif

struct swc_drm swc_drm;

static struct {
	char *path;

	struct wl_global *global;
	struct wl_global *dmabuf;
	struct wl_global *syncobj;
	struct wl_event_source *event_source;

	/* Connector hotplug, reported by the kernel as a uevent on the card. */
#ifdef ENABLE_LIBUDEV
	struct udev *udev;
	struct udev_monitor *monitor;
	struct wl_event_source *monitor_source;
	dev_t devnum;
#endif
	struct wl_event_source *rescan_source;
} drm;

static void
authenticate(struct wl_client *client, struct wl_resource *resource,
             uint32_t magic)
{
	wl_drm_send_authenticated(resource);
}

static void
create_buffer(struct wl_client *client, struct wl_resource *resource,
              uint32_t id, uint32_t name, int32_t width, int32_t height,
              uint32_t stride, uint32_t format)
{
	wl_resource_post_error(
	    resource, WL_DRM_ERROR_INVALID_NAME,
	    "GEM names are not supported, use a PRIME fd instead");
}

static void
create_planar_buffer(struct wl_client *client, struct wl_resource *resource,
                     uint32_t id, uint32_t name, int32_t width, int32_t height,
                     uint32_t format, int32_t offset0, int32_t stride0,
                     int32_t offset1, int32_t stride1, int32_t offset2,
                     int32_t stride2)
{
	wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_FORMAT,
	                       "planar buffers are not supported\n");
}

static void
create_prime_buffer(struct wl_client *client, struct wl_resource *resource,
                    uint32_t id, int32_t fd, int32_t width, int32_t height,
                    uint32_t format, int32_t offset0, int32_t stride0,
                    int32_t offset1, int32_t stride1, int32_t offset2,
                    int32_t stride2)
{
	struct wld_buffer *buffer;
	struct wl_resource *buffer_resource;
	union wld_object object = {.i = fd};

	/*
	 * These come straight off the wire as signed values. Unvalidated they
	 * reach the backends as a declared layout: the dumb backend maps
	 * pitch * height, and the GPU backends build commands from them.
	 * linux-dmabuf validates the same fields; this legacy path did not.
	 */
	if (width <= 0 || height <= 0) {
		close(fd);
		wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_FORMAT,
		                       "buffer dimensions must be positive");
		return;
	}

	switch (format) {
	case WL_DRM_FORMAT_XRGB8888:
	case WL_DRM_FORMAT_ARGB8888:
		break;
	default:
		close(fd);
		wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_FORMAT,
		                       "unsupported format %#" PRIx32, format);
		return;
	}

	/* Only stride0 reaches the importer, so a non-zero offset would be
	 * silently ignored and the wrong pixels imported. */
	if (offset0 != 0 || offset1 != 0 || stride1 != 0 || offset2 != 0 ||
	    stride2 != 0) {
		close(fd);
		wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_FORMAT,
		                       "only single-plane buffers at offset 0 are "
		                       "supported");
		return;
	}

	if (stride0 <= 0 || (uint64_t)stride0 < (uint64_t)width * 4 ||
	    (uint64_t)stride0 * (uint64_t)height > UINT32_MAX) {
		close(fd);
		wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_FORMAT,
		                       "buffer stride or extent is invalid");
		return;
	}

	buffer = wld_import_buffer(swc.drm->context, WLD_DRM_OBJECT_PRIME_FD,
	                           object, width, height, format, stride0);
	close(fd);

	if (!buffer) {
		goto error0;
	}

	buffer_resource = wayland_buffer_create_resource(
	    client, wl_resource_get_version(resource), id, buffer);

	if (!buffer_resource) {
		goto error1;
	}

	return;

error1:
	wld_buffer_unreference(buffer);
error0:
	wl_resource_post_no_memory(resource);
}

static const struct wl_drm_interface drm_impl = {
    .authenticate = authenticate,
    .create_buffer = create_buffer,
    .create_planar_buffer = create_planar_buffer,
    .create_prime_buffer = create_prime_buffer,
};

static int
select_card(const struct dirent *entry)
{
	unsigned num;
	return sscanf(entry->d_name, "card%u", &num) == 1;
}

static bool
find_primary_drm_device(char *path, size_t size)
{
	struct dirent **cards, *card = NULL;
	int num_cards, ret;
	unsigned index;
	FILE *file;
	unsigned char boot_vga;

	num_cards = scandir("/dev/dri", &cards, &select_card, &alphasort);

	if (num_cards == -1) {
		return false;
	}

	for (index = 0; index < num_cards; ++index) {
		snprintf(path, size, "/sys/class/drm/%s/device/boot_vga",
		         cards[index]->d_name);

		if ((file = fopen(path, "r"))) {
			ret = fscanf(file, "%hhu", &boot_vga);
			fclose(file);

			if (ret == 1 && boot_vga) {
				free(card);
				card = cards[index];
				DEBUG("/dev/dri/%s is the primary GPU\n", card->d_name);
				/* Leaving the loop early still leaves the rest of the
				 * directory entries to release. */
				while (++index < num_cards) {
					free(cards[index]);
				}
				break;
			}
		}

		if (!card) {
			card = cards[index];
		} else {
			free(cards[index]);
		}
	}

	free(cards);

	if (!card) {
		return false;
	}

	if (snprintf(path, size, "/dev/dri/%s", card->d_name) >= size) {
		return false;
	}

	free(card);
	return true;
}

static bool
find_available_crtc(drmModeRes *resources, drmModeConnector *connector,
                    uint32_t taken_crtcs, int *crtc_index)
{
	int i, j;
	uint32_t possible_crtcs;
	drmModeEncoder *encoder;

	for (i = 0; i < connector->count_encoders; ++i) {
		encoder = drmModeGetEncoder(swc.drm->fd, connector->encoders[i]);
		if (!encoder) {
			continue;
		}
		possible_crtcs = encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);

		/*
		 * screen->id is taken from this index and addresses a bit in a
		 * uint32_t screen mask, so CRTCs past that width cannot be used --
		 * and shifting by them would be undefined anyway.
		 */
		for (j = 0; j < resources->count_crtcs && j < SWC_MAX_SCREENS; ++j) {
			if ((possible_crtcs & (UINT32_C(1) << j)) &&
			    !(taken_crtcs & (UINT32_C(1) << j))) {
				*crtc_index = j;
				return true;
			}
		}
	}

	return false;
}

static void
handle_vblank(int fd, unsigned int sequence, unsigned int sec,
              unsigned int usec, void *data)
{
}

static void
handle_page_flip(int fd, unsigned int sequence, unsigned int sec,
                 unsigned int usec, unsigned int crtc_id, void *data)
{
	struct drm_handler *handler = data;

	handler->page_flip(handler, sec * 1000 + usec / 1000);
}

static drmEventContext event_context = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .vblank_handler = handle_vblank,
    .page_flip_handler2 = handle_page_flip,
};

static int
handle_data(int fd, uint32_t mask, void *data)
{
	drmHandleEvent(fd, &event_context);
	return 1;
}

static void
bind_drm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_drm_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &drm_impl, NULL, NULL);

	if (version >= 2) {
		wl_drm_send_capabilities(resource, WL_DRM_CAPABILITY_PRIME);
	}

	wl_drm_send_device(resource, drm.path);
	wl_drm_send_format(resource, WL_DRM_FORMAT_XRGB8888);
	wl_drm_send_format(resource, WL_DRM_FORMAT_ARGB8888);
}

#ifdef ENABLE_LIBUDEV
static void
rescan_connectors(void *data);

static int
handle_uevent(int fd, uint32_t mask, void *data)
{
	struct udev_device *device;
	const char *hotplug;
	bool ours;

	while ((device = udev_monitor_receive_device(drm.monitor))) {
		hotplug = udev_device_get_property_value(device, "HOTPLUG");
		ours = udev_device_get_devnum(device) == drm.devnum;
		udev_device_unref(device);

		/* A burst of these arrives for one plug; probe once, after it. */
		if (ours && hotplug && strcmp(hotplug, "1") == 0 &&
		    !drm.rescan_source) {
			drm.rescan_source = wl_event_loop_add_idle(
			    swc.event_loop, &rescan_connectors, NULL);
		}
	}

	return 0;
}

/* Hotplug is an extra: without it the screens found at startup stay. */
static void
hotplug_initialize(void)
{
	struct stat st;

	if (fstat(swc.drm->fd, &st) < 0) {
		goto error0;
	}
	drm.devnum = st.st_rdev;

	if (!(drm.udev = udev_new())) {
		goto error0;
	}
	if (!(drm.monitor = udev_monitor_new_from_netlink(drm.udev, "udev"))) {
		goto error1;
	}
	if (udev_monitor_filter_add_match_subsystem_devtype(drm.monitor, "drm",
	                                                    NULL) < 0 ||
	    udev_monitor_enable_receiving(drm.monitor) < 0) {
		goto error2;
	}
	drm.monitor_source = wl_event_loop_add_fd(
	    swc.event_loop, udev_monitor_get_fd(drm.monitor), WL_EVENT_READABLE,
	    &handle_uevent, NULL);
	if (!drm.monitor_source) {
		goto error2;
	}

	return;

error2:
	udev_monitor_unref(drm.monitor);
	drm.monitor = NULL;
error1:
	udev_unref(drm.udev);
	drm.udev = NULL;
error0:
	WARNING("Could not watch for monitor hotplug; screens are fixed at "
	        "startup\n");
}

static void
hotplug_finalize(void)
{
	if (drm.monitor_source) {
		wl_event_source_remove(drm.monitor_source);
	}
	if (drm.monitor) {
		udev_monitor_unref(drm.monitor);
	}
	if (drm.udev) {
		udev_unref(drm.udev);
	}
}
#endif

bool
drm_initialize(void)
{
	uint64_t val;
	char primary[PATH_MAX];

	if (!find_primary_drm_device(primary, sizeof(primary))) {
		ERROR("Could not find DRM device\n");
		goto error0;
	}

	swc.drm->fd = launch_open_device(primary, O_RDWR | O_CLOEXEC);
	if (swc.drm->fd == -1) {
		ERROR("Could not open DRM device at %s\n", primary);
		goto error0;
	}
	if (drmSetClientCap(swc.drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
		ERROR("Could not enable DRM universal planes\n");
		goto error1;
	}
	if (drmGetCap(swc.drm->fd, DRM_CAP_CURSOR_WIDTH, &val) < 0) {
		val = 64;
	}
	swc.drm->cursor_w = val;
	if (drmGetCap(swc.drm->fd, DRM_CAP_CURSOR_HEIGHT, &val) < 0) {
		val = 64;
	}
	swc.drm->cursor_h = val;
	swc.backend = &swc.drm->backend;
	swc.backend->cursor_width = swc.drm->cursor_w;
	swc.backend->cursor_height = swc.drm->cursor_h;

	drm.path = drmGetRenderDeviceNameFromFd(swc.drm->fd);
	if (!drm.path) {
		ERROR("Could not determine render node path\n");
		goto error1;
	}

	if (!(swc.drm->context = wld_drm_create_context(swc.drm->fd))) {
		ERROR("Could not create WLD DRM context\n");
		goto error1;
	}

	if (!(swc.drm->renderer = wld_create_renderer(swc.drm->context))) {
		ERROR("Could not create WLD DRM renderer\n");
		goto error2;
	}
	swc.backend->context = swc.drm->context;
	swc.backend->renderer = swc.drm->renderer;

	drm.event_source = wl_event_loop_add_fd(
	    swc.event_loop, swc.drm->fd, WL_EVENT_READABLE, &handle_data, NULL);

	if (!drm.event_source) {
		ERROR("Could not create DRM event source\n");
		goto error3;
	}

	if (!wld_drm_is_dumb(swc.drm->context)) {
		drm.global = wl_global_create(swc.display, &wl_drm_interface, 2, NULL,
		                              &bind_drm);
		if (!drm.global) {
			ERROR("Could not create wl_drm global\n");
			goto error4;
		}

		drm.dmabuf = swc_dmabuf_create(swc.display);
		if (!drm.dmabuf) {
			WARNING("Could not create wp_linux_dmabuf global\n");
		}

		/*
		 * Explicit synchronization. Drivers that do not attach implicit fences
		 * to a dmabuf give the compositor no other way to know when a client
		 * has finished drawing into the buffer it just committed.
		 */
		drm.syncobj = drm_syncobj_manager_create(swc.display);
	}

#ifdef ENABLE_LIBUDEV
	hotplug_initialize();
#endif

	return true;

error4:
	wl_event_source_remove(drm.event_source);
error3:
	wld_destroy_renderer(swc.drm->renderer);
error2:
	wld_destroy_context(swc.drm->context);
error1:
	close(swc.drm->fd);
error0:
	return false;
}

void
drm_finalize(void)
{
#ifdef ENABLE_LIBUDEV
	hotplug_finalize();
#endif
	if (drm.rescan_source) {
		wl_event_source_remove(drm.rescan_source);
	}
	if (drm.syncobj) {
		wl_global_destroy(drm.syncobj);
	}
	if (drm.global) {
		wl_global_destroy(drm.global);
	}
	if (drm.dmabuf) {
		wl_global_destroy(drm.dmabuf);
	}
	wl_event_source_remove(drm.event_source);
	wld_destroy_renderer(swc.drm->renderer);
	wld_destroy_context(swc.drm->context);
	free(drm.path);
	close(swc.drm->fd);
}

/*
 * The planes no screen holds yet, for cursor planes to be handed out from.
 * Each holds a listener on swc.event_signal that only plane_destroy
 * unregisters, so whatever is left over has to go through destroy_planes.
 */
static bool
get_free_planes(struct wl_list *screens, struct wl_list *planes)
{
	drmModePlaneRes *plane_ids;
	struct plane *plane;
	struct screen *screen;
	uint32_t i;
	bool used;

	wl_list_init(planes);
	plane_ids = drmModeGetPlaneResources(swc.drm->fd);
	if (!plane_ids) {
		ERROR("Could not get DRM plane resources\n");
		return false;
	}
	for (i = 0; i < plane_ids->count_planes; ++i) {
		used = false;
		wl_list_for_each(screen, screens, link)
		{
			if (screen->planes.cursor &&
			    screen->planes.cursor->id == plane_ids->planes[i]) {
				used = true;
				break;
			}
		}
		if (!used && (plane = plane_new(plane_ids->planes[i]))) {
			wl_list_insert(planes, &plane->link);
		}
	}
	drmModeFreePlaneResources(plane_ids);

	return true;
}

static void
destroy_planes(struct wl_list *planes)
{
	struct plane *plane, *tmp;

	wl_list_for_each_safe(plane, tmp, planes, link)
		plane_destroy(plane);
}

/* Leaves it to the caller to put the screen in the list: startup and hotplug
 * put them at different ends. */
static struct screen *
create_screen(drmModeRes *resources, drmModeConnector *connector,
              struct wl_list *screens, struct wl_list *planes)
{
	struct plane *plane, *cursor_plane;
	struct output *output;
	struct screen *screen;
	uint32_t taken_crtcs = 0;
	int crtc_index;

	wl_list_for_each(screen, screens, link)
		taken_crtcs |= UINT32_C(1) << screen->id;

	if (!find_available_crtc(resources, connector, taken_crtcs, &crtc_index)) {
		WARNING("Could not find CRTC for connector %" PRIu32 "\n",
		        connector->connector_id);
		return NULL;
	}

	cursor_plane = NULL;
	wl_list_for_each(plane, planes, link)
	{
		if (plane->type == DRM_PLANE_TYPE_CURSOR &&
		    plane->possible_crtcs & UINT32_C(1) << crtc_index) {
			wl_list_remove(&plane->link);
			cursor_plane = plane;
			break;
		}
	}
	if (!cursor_plane) {
		WARNING("Could not find cursor plane for CRTC %d\n", crtc_index);
	}

	if (!(output = output_new(connector))) {
		/* The cursor plane was taken out of the list for this
		 * connector, so nothing else will free it. */
		if (cursor_plane) {
			plane_destroy(cursor_plane);
		}
		return NULL;
	}

	output->screen = screen =
	    screen_new(resources->crtcs[crtc_index], output, cursor_plane);
	if (!screen) {
		ERROR("Could not create screen for CRTC %d\n", crtc_index);
		output_destroy(output);
		if (cursor_plane) {
			plane_destroy(cursor_plane);
		}
		return NULL;
	}
	screen->id = crtc_index;

	return screen;
}

bool
drm_create_screens(struct wl_list *screens)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	struct screen *screen;
	uint32_t i;
	struct wl_list planes;

	if (!get_free_planes(screens, &planes)) {
		return false;
	}

	resources = drmModeGetResources(swc.drm->fd);
	if (!resources) {
		destroy_planes(&planes);
		ERROR("Could not get DRM resources\n");
		return false;
	}
	for (i = 0; i < resources->count_connectors;
	     ++i, drmModeFreeConnector(connector)) {
		connector = drmModeGetConnector(swc.drm->fd, resources->connectors[i]);

		/* A connector can disappear between the enumeration and this
		 * query -- hotplug, or a GPU reset mid-startup. */
		if (!connector) {
			continue;
		}
		if (connector->connection == DRM_MODE_CONNECTED &&
		    (screen = create_screen(resources, connector, screens,
		                            &planes))) {
			wl_list_insert(screens, &screen->link);
		}
	}
	drmModeFreeResources(resources);
	destroy_planes(&planes);

	return true;
}

/* Hotplug {{{ */

#ifdef ENABLE_LIBUDEV

static uint32_t
screen_connector(struct screen *screen)
{
	struct output *output;

	if (wl_list_empty(&screen->outputs)) {
		return 0;
	}
	output = wl_container_of(screen->outputs.next, output, link);
	return output->connector;
}

static drmModeConnector *
find_connector(drmModeConnector **connectors, int count, uint32_t id)
{
	int i;

	for (i = 0; i < count; ++i) {
		if (connectors[i] && connectors[i]->connector_id == id) {
			return connectors[i];
		}
	}
	return NULL;
}

static struct screen *
find_screen(uint32_t connector)
{
	struct screen *screen;

	wl_list_for_each(screen, &swc.screens, link)
	{
		if (screen_connector(screen) == connector) {
			return screen;
		}
	}
	return NULL;
}

/* A screen for every connected connector that lacks one. */
static bool
add_screens(drmModeRes *resources, drmModeConnector **connectors)
{
	struct wl_list planes;
	struct screen *screen;
	struct output *output;
	bool changed = false;
	int i;

	if (!get_free_planes(&swc.screens, &planes)) {
		return false;
	}
	for (i = 0; i < resources->count_connectors; ++i) {
		if (!connectors[i] ||
		    connectors[i]->connection != DRM_MODE_CONNECTED) {
			continue;
		}
		if ((screen = find_screen(connectors[i]->connector_id))) {
			output = wl_container_of(screen->outputs.next, output, link);
			/* The last screen, kept through an unplug, is back. The CRTC
			 * is still programmed for it, but set the mode again rather
			 * than trust a flip to a monitor that was just replugged. */
			if (output->disconnected) {
				output->disconnected = false;
				screen->planes.primary.need_modeset = true;
				changed = true;
			}
			continue;
		}
		if ((screen = create_screen(resources, connectors[i], &swc.screens,
		                            &planes))) {
			/* At the end: the first screen is the fallback for a lot of
			 * things, and a monitor being plugged in should not move it. */
			wl_list_insert(swc.screens.prev, &screen->link);
			DEBUG("Screen %s connected\n", swc_screen_get_name(&screen->base));
			screen_added(screen);
			changed = true;
		}
	}
	destroy_planes(&planes);

	return changed;
}

/*
 * Tear down the screens whose connector is gone. The last screen is kept even
 * then: everything from the window manager's layout to absolute pointer motion
 * assumes there is one, and it has somewhere to put the windows when a
 * monitor comes back.
 */
static bool
remove_screens(drmModeConnector **connectors, int count)
{
	struct screen *screen, *tmp;
	struct output *output;
	drmModeConnector *connector;
	bool changed = false;

	wl_list_for_each_safe(screen, tmp, &swc.screens, link)
	{
		connector = find_connector(connectors, count, screen_connector(screen));
		if (connector && connector->connection == DRM_MODE_CONNECTED) {
			continue;
		}
		if (swc.screens.next->next == &swc.screens) {
			output = wl_container_of(screen->outputs.next, output, link);
			if (!output->disconnected) {
				WARNING("Last screen was unplugged; keeping it until "
				        "another is connected\n");
				output->disconnected = true;
			}
			continue;
		}
		DEBUG("Screen %s disconnected\n", swc_screen_get_name(&screen->base));
		/* Off before its buffers are freed with it. */
		primary_plane_disable(&screen->planes.primary);
		screen_destroy(screen);
		changed = true;
	}

	return changed;
}

static void
rescan_connectors(void *data)
{
	drmModeRes *resources;
	drmModeConnector **connectors;
	bool changed;
	int i;

	(void)data;
	drm.rescan_source = NULL;

	if (!(resources = drmModeGetResources(swc.drm->fd))) {
		ERROR("Could not get DRM resources: %s\n", strerror(errno));
		return;
	}
	connectors = calloc(resources->count_connectors, sizeof(*connectors));
	if (!connectors) {
		drmModeFreeResources(resources);
		return;
	}
	/* Probed once, here: every question below is about this snapshot. */
	for (i = 0; i < resources->count_connectors; ++i)
		connectors[i] = drmModeGetConnector(swc.drm->fd,
		                                    resources->connectors[i]);

	/*
	 * New screens first, so the window manager always has a screen to move
	 * the windows of a removed one to. Then again after the removals, for a
	 * connector that could only get a CRTC once a removed screen let go of
	 * its own.
	 */
	changed = add_screens(resources, connectors);
	if (remove_screens(connectors, resources->count_connectors)) {
		add_screens(resources, connectors);
		changed = true;
	}

	if (changed) {
		screens_update_pointer_region();
		compositor_damage_all();
	}

	for (i = 0; i < resources->count_connectors; ++i)
		drmModeFreeConnector(connectors[i]);
	free(connectors);
	drmModeFreeResources(resources);
}
#endif

/* }}} */

enum { WLD_USER_OBJECT_FRAMEBUFFER = WLD_USER_ID };

struct framebuffer {
	struct wld_exporter exporter;
	struct wld_destructor destructor;
	uint32_t id;
};

static bool
framebuffer_export(struct wld_exporter *exporter, struct wld_buffer *buffer,
                   uint32_t type, union wld_object *object)
{
	struct framebuffer *framebuffer =
	    wl_container_of(exporter, framebuffer, exporter);

	switch (type) {
	case WLD_USER_OBJECT_FRAMEBUFFER:
		object->u32 = framebuffer->id;
		break;
	default:
		return false;
	}

	return true;
}

static void
framebuffer_destroy(struct wld_destructor *destructor)
{
	struct framebuffer *framebuffer =
	    wl_container_of(destructor, framebuffer, destructor);

	drmModeRmFB(swc.drm->fd, framebuffer->id);
	free(framebuffer);
}

uint32_t
drm_get_framebuffer(struct wld_buffer *buffer)
{
	struct framebuffer *framebuffer;
	union wld_object object;
	int ret;

	if (!buffer) {
		return 0;
	}

	if (wld_export(buffer, WLD_USER_OBJECT_FRAMEBUFFER, &object)) {
		return object.u32;
	}

	if (!wld_export(buffer, WLD_DRM_OBJECT_HANDLE, &object)) {
		ERROR("Could not get buffer handle\n");
		return 0;
	}

	if (!(framebuffer = malloc(sizeof(*framebuffer)))) {
		return 0;
	}

	{
		union wld_object mod_object;
		uint32_t handle = object.u32;
		uint64_t modifier = DRM_FORMAT_MOD_INVALID;

		if (wld_export(buffer, WLD_DRM_OBJECT_MODIFIER, &mod_object)) {
			modifier = mod_object.u64;
		}

		/*
		 * A tiled buffer must be declared with its modifier. Without
		 * DRM_MODE_FB_MODIFIERS the kernel treats it as linear and rejects
		 * it, which is what happens to every buffer the GBM backend
		 * allocates on hardware that tiles them.
		 */
		if (modifier != DRM_FORMAT_MOD_INVALID &&
		    modifier != DRM_FORMAT_MOD_LINEAR) {
			ret = drmModeAddFB2WithModifiers(
			    swc.drm->fd, buffer->width, buffer->height, buffer->format,
			    (uint32_t[4]){handle}, (uint32_t[4]){buffer->pitch},
			    (uint32_t[4]){0}, (uint64_t[4]){modifier}, &framebuffer->id,
			    DRM_MODE_FB_MODIFIERS);
		} else {
			ret = drmModeAddFB2(swc.drm->fd, buffer->width, buffer->height,
			                    buffer->format, (uint32_t[4]){handle},
			                    (uint32_t[4]){buffer->pitch},
			                    (uint32_t[4]){0}, &framebuffer->id, 0);
		}
	}
	if (ret < 0) {
		ERROR("Could not add framebuffer: %s\n", strerror(errno));
		free(framebuffer);
		return 0;
	}

	framebuffer->exporter.export = &framebuffer_export;
	wld_buffer_add_exporter(buffer, &framebuffer->exporter);
	framebuffer->destructor.destroy = &framebuffer_destroy;
	wld_buffer_add_destructor(buffer, &framebuffer->destructor);

	return framebuffer->id;
}
