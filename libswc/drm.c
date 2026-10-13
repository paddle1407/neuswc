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
#include "output_management.h"
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
	struct wl_global *output_manager;
	struct wl_event_source *event_source;

	/* Connector hotplug, reported by the kernel as a uevent on the card. */
#ifdef ENABLE_LIBUDEV
	struct udev *udev;
	struct udev_monitor *monitor;
	struct wl_event_source *monitor_source;
	dev_t devnum;
#endif
	/* "card1": connector uevents come from "card1-HDMI-A-1" and the like. */
	char card[32];
	struct wl_event_source *rescan_source;
	/* A connector can report connected before it can be lit: no modes until
	 * its EDID has been read, or no CRTC free yet. Such a rescan is tried
	 * again a few times rather than waiting for a uevent that need not come. */
	struct wl_event_source *retry_timer;
	unsigned retries;
	struct wl_listener swc_listener;
	/* What output management asked of each connector; see struct request. */
	struct wl_list requests;
} drm;

/*
 * Output management's settings for one connector, kept until the monitor is
 * unplugged so that they survive the screen being recreated. A connector
 * without one is configured automatically: enabled, preferred mode, placed
 * by the window manager.
 */
struct request {
	uint32_t connector;
	bool disabled;
	bool has_mode;
	drmModeModeInfo mode;
	bool has_position;
	int32_t x, y;
	struct wl_list link;
};

static struct request *
find_request(uint32_t connector)
{
	struct request *request;

	wl_list_for_each(request, &drm.requests, link)
	{
		if (request->connector == connector) {
			return request;
		}
	}
	return NULL;
}

static bool
connector_disabled(uint32_t connector)
{
	struct request *request = find_request(connector);

	return request && request->disabled;
}

static void
drop_request(uint32_t connector)
{
	struct request *request = find_request(connector);

	if (request) {
		wl_list_remove(&request->link);
		free(request);
	}
}

static void
drop_requests(void)
{
	struct request *request, *tmp;

	wl_list_for_each_safe(request, tmp, &drm.requests, link)
	{
		wl_list_remove(&request->link);
		free(request);
	}
}

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

static void
rescan_connectors(void *data);

/* Coalesced: a burst of uevents arrives for one plug; probe once, after it. */
static void
queue_rescan(void)
{
	if (!drm.rescan_source) {
		drm.rescan_source =
		    wl_event_loop_add_idle(swc.event_loop, &rescan_connectors, NULL);
	}
}

static int
handle_retry(void *data)
{
	(void)data;
	queue_rescan();
	return 0;
}

static void
handle_swc_event(struct wl_listener *listener, void *data)
{
	struct event *event = data;

	(void)listener;
	/* Monitors plugged or unplugged while switched away were never seen:
	 * the rescans of that time could not touch the display. */
	if (event->type == SWC_EVENT_ACTIVATED) {
		drm.retries = 0;
		queue_rescan();
	}
}

#ifdef ENABLE_LIBUDEV
/*
 * The card itself announces connector changes (HOTPLUG=1), but kernels also
 * send change events from the connector devices under it, which have no
 * device number of their own. Either one is a reason to look again.
 */
static bool
uevent_is_ours(struct udev_device *device)
{
	const char *sysname = udev_device_get_sysname(device);
	size_t len = strlen(drm.card);

	if (drm.devnum && udev_device_get_devnum(device) == drm.devnum) {
		return true;
	}
	return len && sysname && strncmp(sysname, drm.card, len) == 0 &&
	       (sysname[len] == '\0' || sysname[len] == '-');
}

static int
handle_uevent(int fd, uint32_t mask, void *data)
{
	struct udev_device *device;
	const char *action, *hotplug, *connector;
	bool rescan;

	while ((device = udev_monitor_receive_device(drm.monitor))) {
		action = udev_device_get_action(device);
		hotplug = udev_device_get_property_value(device, "HOTPLUG");
		connector = udev_device_get_property_value(device, "CONNECTOR");
		rescan = uevent_is_ours(device) &&
		         ((hotplug && strcmp(hotplug, "1") == 0) || connector ||
		          (action && strcmp(action, "change") == 0));
		udev_device_unref(device);

		if (rescan) {
			drm.retries = 0;
			queue_rescan();
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

	NOTICE("Watching %s for monitors being plugged in or out\n",
	       drm.card[0] ? drm.card : "the DRM device");
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
#else
static void
hotplug_initialize(void)
{
	WARNING("Built without libudev: monitors plugged in later are only "
	        "found by output management or a VT switch\n");
}

static void
hotplug_finalize(void)
{
}
#endif

bool
drm_initialize(void)
{
	memset(&drm, 0, sizeof(drm));
	wl_list_init(&drm.requests);
	uint64_t val;
	char primary[PATH_MAX];
	const char *card;

	if (!find_primary_drm_device(primary, sizeof(primary))) {
		ERROR("Could not find DRM device\n");
		goto error0;
	}
	card = strrchr(primary, '/');
	snprintf(drm.card, sizeof(drm.card), "%.31s", card ? card + 1 : primary);

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

	drm.swc_listener.notify = &handle_swc_event;
	wl_signal_add(&swc.event_signal, &drm.swc_listener);
	drm.retry_timer =
	    wl_event_loop_add_timer(swc.event_loop, &handle_retry, NULL);
	hotplug_initialize();

	/* Lets wlr-randr and the like list, enable, disable and place monitors;
	 * an extra, like hotplug. */
	drm.output_manager = output_management_create(swc.display);
	if (!drm.output_manager) {
		WARNING("Could not create wlr_output_manager global\n");
	}

	return true;

error4:
	wl_event_source_remove(drm.event_source);
error3:
	wld_destroy_renderer(swc.drm->renderer);
error2:
	wld_destroy_context(swc.drm->context);
error1:
	free(drm.path);
	drm.path = NULL;
	close(swc.drm->fd);
	swc.drm->fd = -1;
error0:
	return false;
}

void
drm_finalize(void)
{
	drm_syncobj_manager_finish();
	if (drm.output_manager) {
		output_management_finish();
		wl_global_destroy(drm.output_manager);
	}
	hotplug_finalize();
	wl_list_remove(&drm.swc_listener.link);
	if (drm.retry_timer) {
		wl_event_source_remove(drm.retry_timer);
	}
	if (drm.rescan_source) {
		wl_event_source_remove(drm.rescan_source);
	}
	drop_requests();
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
	swc.drm->fd = -1;
	swc.drm->context = NULL;
	swc.drm->renderer = NULL;
	swc.drm->backend.context = NULL;
	swc.drm->backend.renderer = NULL;
	memset(&drm, 0, sizeof(drm));
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

void
drm_connector_name(const drmModeConnector *connector, char *name, size_t size)
{
	const char *type = drmModeGetConnectorTypeName(connector->connector_type);

	snprintf(name, size, "%s-%" PRIu32, type ? type : "UNKNOWN",
	         connector->connector_type_id);
}

bool
drm_mode_info_equal(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
	return a->clock == b->clock && a->hdisplay == b->hdisplay &&
	       a->hsync_start == b->hsync_start && a->hsync_end == b->hsync_end &&
	       a->htotal == b->htotal && a->hskew == b->hskew &&
	       a->vdisplay == b->vdisplay && a->vsync_start == b->vsync_start &&
	       a->vsync_end == b->vsync_end && a->vtotal == b->vtotal &&
	       a->vscan == b->vscan && a->vrefresh == b->vrefresh &&
	       a->flags == b->flags;
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
	struct request *request;
	struct mode *mode;
	int32_t position[2];
	uint32_t taken_crtcs = 0;
	int crtc_index;
	char name[32];

	drm_connector_name(connector, name, sizeof(name));

	wl_list_for_each(screen, screens, link)
		taken_crtcs |= UINT32_C(1) << screen->id;

	if (!find_available_crtc(resources, connector, taken_crtcs, &crtc_index)) {
		WARNING("Could not find a free CRTC for %s\n", name);
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
		/* Usually a monitor whose EDID has not been read yet: the rescan
		 * is retried. */
		WARNING("Could not create output for %s (%d modes)\n", name,
		        connector->count_modes);
		/* The cursor plane was taken out of the list for this
		 * connector, so nothing else will free it. */
		if (cursor_plane) {
			plane_destroy(cursor_plane);
		}
		return NULL;
	}

	/* Output management's choices, where they still apply. */
	request = find_request(connector->connector_id);
	if (request && request->has_mode) {
		wl_array_for_each(mode, &output->modes)
		{
			if (drm_mode_info_equal(&mode->info, &request->mode)) {
				output->preferred_mode = mode;
				break;
			}
		}
	}
	if (request && request->has_position) {
		position[0] = request->x;
		position[1] = request->y;
	}

	output->screen = screen =
	    screen_new(resources->crtcs[crtc_index], output, cursor_plane,
	               request && request->has_position ? position : NULL);
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

/* Hotplug and output management {{{ */

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

struct screen *
drm_screen_for_connector(uint32_t connector)
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

/* Connected, and not turned off through output management. */
static bool
connector_wanted(const drmModeConnector *connector)
{
	return connector && connector->connection == DRM_MODE_CONNECTED &&
	       !connector_disabled(connector->connector_id);
}

/* A screen for every wanted connector that lacks one. */
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
		if (!connector_wanted(connectors[i])) {
			continue;
		}
		if ((screen = drm_screen_for_connector(connectors[i]->connector_id))) {
			output = wl_container_of(screen->outputs.next, output, link);
			/* The last screen, kept through an unplug, is back. The CRTC
			 * is still programmed for it, but set the mode again rather
			 * than trust a flip to a monitor that was just replugged. */
			if (output->disconnected) {
				NOTICE("Monitor %s reconnected\n", output->name);
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
			NOTICE("Monitor %s connected: %ux%u at %d,%d\n",
			       swc_screen_get_name(&screen->base),
			       screen->base.geometry.width, screen->base.geometry.height,
			       screen->base.geometry.x, screen->base.geometry.y);
			screen_added(screen);
			changed = true;
		}
	}
	destroy_planes(&planes);

	return changed;
}

static void
remove_screen(struct screen *screen)
{
	NOTICE("Monitor %s removed\n", swc_screen_get_name(&screen->base));
	/* Off before its buffers are freed with it. */
	primary_plane_disable(&screen->planes.primary);
	screen_destroy(screen);
}

/*
 * Tear down the screens whose connector is gone or turned off. The last
 * screen is kept even then: everything from the window manager's layout to
 * absolute pointer motion assumes there is one, and it has somewhere to put
 * the windows when a monitor comes back.
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
		if (connector_wanted(connector)) {
			continue;
		}
		if (swc.screens.next->next == &swc.screens) {
			output = wl_container_of(screen->outputs.next, output, link);
			if (!output->disconnected) {
				WARNING("Last screen was unplugged; keeping it until "
				        "another is connected\n");
				output->disconnected = true;
				changed = true;
			}
			continue;
		}
		remove_screen(screen);
		changed = true;
	}

	return changed;
}

/* Each connector, as of now. probe asks the monitors again, which takes a
 * while (EDID is read over DDC); without it the kernel's last answer is
 * given. */
static drmModeConnector **
get_connectors(drmModeRes *resources, bool probe)
{
	drmModeConnector **connectors;
	int i;

	connectors = calloc(resources->count_connectors, sizeof(*connectors));
	if (!connectors) {
		return NULL;
	}
	for (i = 0; i < resources->count_connectors; ++i)
		connectors[i] =
		    probe ? drmModeGetConnector(swc.drm->fd, resources->connectors[i])
		          : drmModeGetConnectorCurrent(swc.drm->fd,
		                                       resources->connectors[i]);
	return connectors;
}

static void
free_connectors(drmModeConnector **connectors, int count)
{
	int i;

	for (i = 0; i < count; ++i)
		drmModeFreeConnector(connectors[i]);
	free(connectors);
}

static void
screens_changed(void)
{
	screens_update_pointer_region();
	compositor_damage_all();
}

/* Try again later for a connector that is wanted but has no screen. */
static void
schedule_retry(drmModeRes *resources, drmModeConnector **connectors)
{
	enum { MAX_RETRIES = 5 };
	char name[32];
	int i;

	for (i = 0; i < resources->count_connectors; ++i) {
		if (!connector_wanted(connectors[i]) ||
		    drm_screen_for_connector(connectors[i]->connector_id)) {
			continue;
		}
		drm_connector_name(connectors[i], name, sizeof(name));
		if (drm.retries >= MAX_RETRIES || !drm.retry_timer) {
			WARNING("Monitor %s is connected but could not be lit; giving "
			        "up until it is plugged in again\n", name);
			return;
		}
		++drm.retries;
		NOTICE("Monitor %s is connected but not ready; trying again\n",
		       name);
		wl_event_source_timer_update(drm.retry_timer, 500 * drm.retries);
		return;
	}
	drm.retries = 0;
}

static void
rescan(bool probe)
{
	drmModeRes *resources;
	drmModeConnector **connectors;
	struct request *request, *tmp;
	bool changed;

	if (!(resources = drmModeGetResources(swc.drm->fd))) {
		ERROR("Could not get DRM resources: %s\n", strerror(errno));
		return;
	}
	/* Probed once, here: every question below is about this snapshot. */
	if (!(connectors = get_connectors(resources, probe))) {
		drmModeFreeResources(resources);
		return;
	}

	/* A monitor plugged in again starts afresh. */
	wl_list_for_each_safe(request, tmp, &drm.requests, link)
	{
		drmModeConnector *connector = find_connector(
		    connectors, resources->count_connectors, request->connector);
		if (!connector || connector->connection != DRM_MODE_CONNECTED) {
			drop_request(request->connector);
		}
	}

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
		screens_changed();
	}
	schedule_retry(resources, connectors);
	output_management_update();

	free_connectors(connectors, resources->count_connectors);
	drmModeFreeResources(resources);
}

static void
rescan_connectors(void *data)
{
	(void)data;
	drm.rescan_source = NULL;

	/* While switched away the display is not ours to set up; activation
	 * queues another rescan. */
	if (!swc.active) {
		return;
	}
	rescan(true);
}

void
drm_rescan(void)
{
	if (swc.active) {
		rescan(true);
	}
}

static bool
connector_has_mode(const drmModeConnector *connector,
                   const drmModeModeInfo *mode)
{
	int i;

	for (i = 0; i < connector->count_modes; ++i) {
		if (drm_mode_info_equal(&connector->modes[i], mode)) {
			return true;
		}
	}
	return false;
}

static const struct drm_head_config *
find_head_config(const struct drm_head_config *heads, size_t count,
                 uint32_t connector)
{
	size_t i;

	for (i = 0; i < count; ++i) {
		if (heads[i].connector == connector) {
			return &heads[i];
		}
	}
	return NULL;
}

/* Whether a screen would have to be built again for this configuration. */
static bool
screen_differs(struct screen *screen, const struct drm_head_config *head)
{
	if (!head->enabled) {
		return true;
	}
	if (head->mode &&
	    !drm_mode_info_equal(&screen->planes.primary.mode.info, head->mode)) {
		return true;
	}
	return head->has_position && (screen->base.geometry.x != head->x ||
	                              screen->base.geometry.y != head->y);
}

static bool
check_heads(drmModeRes *resources, drmModeConnector **connectors,
            const struct drm_head_config *heads, size_t count)
{
	const struct drm_head_config *head;
	drmModeConnector *connector;
	unsigned enabled = 0;
	size_t i;
	int j;

	for (i = 0; i < count; ++i) {
		head = &heads[i];
		connector = find_connector(connectors, resources->count_connectors,
		                           head->connector);
		if (!connector || connector->connection != DRM_MODE_CONNECTED) {
			WARNING("Output configuration names a monitor that is not "
			        "connected\n");
			return false;
		}
		if (!head->enabled) {
			continue;
		}
		if (head->mode && !connector_has_mode(connector, head->mode)) {
			WARNING("Output configuration asks for a mode the monitor "
			        "does not have\n");
			return false;
		}
		/* Roughly the bounds screen_new keeps to; it checks the rest. */
		if (head->has_position &&
		    (head->x < -32768 || head->y < -32768 || head->x > 32767 ||
		     head->y > 32767)) {
			WARNING("Output configuration places a monitor out of range\n");
			return false;
		}
	}

	/* Every connected monitor the configuration leaves alone stays as it
	 * is, lit or not. */
	for (j = 0; j < resources->count_connectors; ++j) {
		if (!connectors[j] || connectors[j]->connection != DRM_MODE_CONNECTED) {
			continue;
		}
		head = find_head_config(heads, count, connectors[j]->connector_id);
		if (head ? head->enabled
		         : drm_screen_for_connector(connectors[j]->connector_id) !=
		               NULL) {
			++enabled;
		}
	}
	if (enabled == 0) {
		WARNING("Output configuration would turn every monitor off\n");
		return false;
	}
	if (enabled > (unsigned)resources->count_crtcs) {
		WARNING("Output configuration lights more monitors than the GPU "
		        "has CRTCs\n");
		return false;
	}
	return true;
}

static void
record_requests(const struct drm_head_config *heads, size_t count)
{
	const struct drm_head_config *head;
	struct request *request;
	size_t i;

	for (i = 0; i < count; ++i) {
		head = &heads[i];
		if (!(request = find_request(head->connector))) {
			if (!(request = calloc(1, sizeof(*request)))) {
				continue;
			}
			request->connector = head->connector;
			wl_list_insert(&drm.requests, &request->link);
		}
		request->disabled = !head->enabled;
		if (!head->enabled) {
			continue;
		}
		if (head->mode) {
			request->has_mode = true;
			request->mode = *head->mode;
		}
		if (head->has_position) {
			request->has_position = true;
			request->x = head->x;
			request->y = head->y;
		}
	}
}

bool
drm_configure_heads(const struct drm_head_config *heads, size_t count,
                    bool test)
{
	drmModeRes *resources;
	drmModeConnector **connectors;
	struct screen *screen, *tmp;
	struct screen *rebuild[SWC_MAX_SCREENS];
	const struct drm_head_config *head;
	size_t num_rebuild = 0, i;
	bool ok;

	if (!swc.active) {
		return false;
	}
	if (!(resources = drmModeGetResources(swc.drm->fd))) {
		ERROR("Could not get DRM resources: %s\n", strerror(errno));
		return false;
	}
	if (!(connectors = get_connectors(resources, false))) {
		drmModeFreeResources(resources);
		return false;
	}

	ok = check_heads(resources, connectors, heads, count);
	if (!ok || test) {
		goto done;
	}

	/* Decided before anything changes: which screens have to go. A new mode
	 * or position means building the screen again, as for a replug. */
	wl_list_for_each(screen, &swc.screens, link)
	{
		head = find_head_config(heads, count, screen_connector(screen));
		if (head && screen_differs(screen, head) &&
		    num_rebuild < ARRAY_LENGTH(rebuild)) {
			rebuild[num_rebuild++] = screen;
		}
	}
	record_requests(heads, count);

	/* Monitors being turned on first, so the windows of those going off
	 * have somewhere to go; then the rest built again as asked. */
	add_screens(resources, connectors);
	wl_list_for_each_safe(screen, tmp, &swc.screens, link)
	{
		for (i = 0; i < num_rebuild; ++i) {
			if (rebuild[i] == screen) {
				remove_screen(screen);
				break;
			}
		}
	}
	add_screens(resources, connectors);

	/* Nothing could be lit as asked. Anything is better than no screen. */
	if (wl_list_empty(&swc.screens)) {
		ERROR("Output configuration left no screen; going back to the "
		      "automatic one\n");
		drop_requests();
		add_screens(resources, connectors);
		ok = false;
	}

	screens_changed();
	output_management_update();

done:
	free_connectors(connectors, resources->count_connectors);
	drmModeFreeResources(resources);
	return ok;
}

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

	struct wld_drm_layout fallback = { .num_planes = 1 };
	const struct wld_drm_layout *layout;
	if (wld_export(buffer, WLD_DRM_OBJECT_LAYOUT, &object)) {
		layout = object.ptr;
	} else {
		if (!wld_export(buffer, WLD_DRM_OBJECT_HANDLE, &object)) return 0;
		fallback.handles[0] = object.u32;
		fallback.pitches[0] = buffer->pitch;
		fallback.modifiers[0] = DRM_FORMAT_MOD_INVALID;
		if (wld_export(buffer, WLD_DRM_OBJECT_MODIFIER, &object))
			fallback.modifiers[0] = object.u64;
		layout = &fallback;
	}
	if (!layout || !layout->num_planes || layout->num_planes > 4) return 0;
	if (!(framebuffer = malloc(sizeof(*framebuffer)))) return 0;
	bool explicit_modifier = false;
	for (unsigned i = 0; i < layout->num_planes; ++i)
		explicit_modifier |= layout->modifiers[i] != DRM_FORMAT_MOD_INVALID &&
		    layout->modifiers[i] != DRM_FORMAT_MOD_LINEAR;
	if (explicit_modifier) {
		ret = drmModeAddFB2WithModifiers(swc.drm->fd, buffer->width, buffer->height,
			buffer->format, layout->handles, layout->pitches, layout->offsets,
			layout->modifiers, &framebuffer->id, DRM_MODE_FB_MODIFIERS);
		} else {
		ret = drmModeAddFB2(swc.drm->fd, buffer->width, buffer->height, buffer->format,
			layout->handles, layout->pitches, layout->offsets, &framebuffer->id, 0);
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
