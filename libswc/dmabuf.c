/* swc: dmabuf.c
 *
 * Copyright (c) 2019 Michael Forney
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

#include "dmabuf.h"
#include "drm.h"
#include "internal.h"
#include "util.h"
#include "wayland_buffer.h"

#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wld/drm.h>
#include <wld/wld.h>

struct params {
	struct wl_resource *resource;
	int fd[4];
	uint32_t offset[4];
	uint32_t stride[4];
	uint64_t modifier[4];
	bool created;
};

static void
add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
    uint32_t i, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
    uint32_t modifier_lo)
{
	struct params *params = wl_resource_get_user_data(resource);

	if (params->created) {
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
		                       "buffer already created");
		close(fd);
		return;
	}
	if (i >= ARRAY_LENGTH(params->fd)) {
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
		                       "plane index too large");
		close(fd);
		return;
	}
	if (params->fd[i] != -1) {
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
		                       "buffer plane already set");
		close(fd);
		return;
	}
	params->fd[i] = fd;
	params->offset[i] = offset;
	params->stride[i] = stride;
	params->modifier[i] = (uint64_t)modifier_hi << 32 | modifier_lo;
}

static void
create_immed(struct wl_client *client, struct wl_resource *resource,
             uint32_t id, int32_t width, int32_t height, uint32_t format,
             uint32_t flags)
{
	struct params *params = wl_resource_get_user_data(resource);
	struct wld_buffer *buffer;
	struct wl_resource *buffer_resource;
	union wld_object object;
	int num_planes, i;

	if (params->created) {
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
		                       "buffer already created");
		return;
	}
	params->created = true;
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource,
		    ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
		    "dmabuf dimensions must be positive");
		return;
	}
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		num_planes = 1;
		break;
	default:
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
		                       "unsupported format %#" PRIx32, format);
		return;
	}
	for (i = 0; i < num_planes; ++i) {
		if (params->fd[i] == -1) {
			wl_resource_post_error(resource,
			                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			                       "missing plane %d", i);
			return;
		}
	}
	for (; i < ARRAY_LENGTH(params->fd); ++i) {
		if (params->fd[i] != -1) {
			wl_resource_post_error(resource,
			                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			                       "too many planes");
			return;
		}
	}
	if (params->stride[0] < (uint64_t)width * 4 ||
	    (uint64_t)params->offset[0] + (uint64_t)params->stride[0] * (height - 1)
	        + (uint64_t)width * 4 > UINT32_MAX) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
		                       "dmabuf stride or plane extent is invalid");
		return;
	}
	/* Y inversion and interlacing are not implemented by this renderer. */
	if (flags) {
		buffer = NULL;
		goto import_done;
	}
	/*
	 * Pass the layout through. A tiled buffer imported as a bare PRIME fd is
	 * assumed linear, which on most hardware means it either fails to import
	 * or samples as garbage.
	 */
	{
		struct wld_dmabuf_attributes attributes = {
			.fd = params->fd[0],
			.offset = params->offset[0],
			.pitch = params->stride[0],
			.modifier = params->modifier[0],
		};

		object.ptr = &attributes;
		buffer = wld_import_buffer(swc.drm->context, WLD_DRM_OBJECT_DMABUF,
		                           object, width, height, format,
		                           params->stride[0]);
		/* Legacy backends can only represent an implicit layout at offset 0.
		 * Retrying an explicit layout would silently discard its attributes. */
		if (!buffer && params->offset[0] == 0 &&
		    params->modifier[0] == DRM_FORMAT_MOD_INVALID) {
			object.i = params->fd[0];
			buffer = wld_import_buffer(swc.drm->context,
			                           WLD_DRM_OBJECT_PRIME_FD, object, width,
			                           height, format, params->stride[0]);
		}
	}
import_done:
	for (i = 0; i < num_planes; ++i) {
		close(params->fd[i]);
		params->fd[i] = -1;
	}
	if (!buffer) {
		/*
		 * create() may report failure and let the client try again;
		 * create_immed() has already been given an id, so the protocol
		 * requires a fatal error instead.
		 */
		if (id == 0) {
			zwp_linux_buffer_params_v1_send_failed(resource);
		} else {
			wl_resource_post_error(
			    resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
			    "failed to import dmabuf");
		}
		return;
	}

	buffer_resource = wayland_buffer_create_resource(client, 1, id, buffer);
	if (!buffer_resource) {
		wld_buffer_unreference(buffer);
		wl_resource_post_no_memory(resource);
		return;
	}
	if (id == 0) {
		zwp_linux_buffer_params_v1_send_created(resource, buffer_resource);
	}
}

static void
create(struct wl_client *client, struct wl_resource *resource, int32_t width,
       int32_t height, uint32_t format, uint32_t flags)
{
	create_immed(client, resource, 0, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = destroy_resource,
    .add = add,
    .create = create,
    .create_immed = create_immed,
};

static void
params_destroy(struct wl_resource *resource)
{
	struct params *params = wl_resource_get_user_data(resource);
	int i;

	for (i = 0; i < ARRAY_LENGTH(params->fd); ++i) {
		if (params->fd[i] != -1) {
			close(params->fd[i]);
		}
	}

	free(params);
}

static void
create_params(struct wl_client *client, struct wl_resource *resource,
              uint32_t id)
{
	struct params *params;
	int i;

	params = malloc(sizeof(*params));
	if (!params) {
		goto error0;
	}
	params->created = false;
	params->resource =
	    wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!params->resource) {
		goto error1;
	}
	for (i = 0; i < ARRAY_LENGTH(params->fd); ++i) {
		params->fd[i] = -1;
	}
	wl_resource_set_implementation(params->resource, &params_impl, params,
	                               params_destroy);
	return;

error1:
	free(params);
error0:
	wl_resource_post_no_memory(resource);
}

/*
 * Version 4 replaces the format/modifier events with feedback objects. A
 * client that only sees version 3 -- as GTK does, which requires 4 -- falls
 * back to CPU rendering, so its GL and Vulkan backends never produce a buffer
 * to attach and its windows stay unmapped.
 *
 * The feedback we send is the simplest form the protocol allows: one main
 * device, and a single tranche offering every format we support on it.
 */

struct format_entry {
	uint32_t format;
	uint32_t padding; /* the protocol requires this to be zero */
	uint64_t modifier;
};

static struct format_entry *format_table;
static size_t format_table_len;

/*
 * Advertise the modifiers the GPU actually accepts.
 *
 * DRM_FORMAT_MOD_INVALID is not a useful thing to offer here: a client
 * matches our list against what its own renderer supports, and nothing
 * matches INVALID, so it finds no usable format and never produces a buffer.
 */
static void
build_format_table(void)
{
	static const uint32_t formats[] = {
	    DRM_FORMAT_XRGB8888,
	    DRM_FORMAT_ARGB8888,
	};
	uint64_t modifiers[64];
	struct format_entry *entry;
	size_t i;
	int count, j;

	if (format_table)
		return;

	for (i = 0; i < ARRAY_LENGTH(formats); ++i) {
		count = wld_drm_query_modifiers(swc.drm->context, formats[i], modifiers,
		                                ARRAY_LENGTH(modifiers));

		/* Fall back to implicit if the backend cannot enumerate. */
		if (count == 0)
			continue;
		if (count < 0) {
			modifiers[0] = DRM_FORMAT_MOD_INVALID;
			count = 1;
		}

		entry = realloc(format_table,
		                (format_table_len + count) * sizeof(*format_table));
		if (!entry)
			return;
		format_table = entry;

		for (j = 0; j < count; ++j) {
			format_table[format_table_len++] = (struct format_entry){
				.format = formats[i],
				.padding = 0,
				.modifier = modifiers[j],
			};
		}
	}
}

static int
format_table_fd(size_t *size)
{
	int fd;

	build_format_table();
	if (!format_table_len)
		return -1;
	*size = format_table_len * sizeof(*format_table);

#ifdef __linux__
	fd = memfd_create("swc-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		return -1;
	if (write(fd, format_table, *size) != (ssize_t)*size)
		goto error;
	fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
#else
	{
		char path[] = "/tmp/swc-dmabuf-XXXXXX";

		fd = mkostemp(path, O_CLOEXEC);
		if (fd < 0)
			return -1;
		unlink(path);
		if (write(fd, format_table, *size) != (ssize_t)*size)
			goto error;
	}
#endif

	return fd;

error:
	close(fd);
	return -1;
}

static bool
main_device(struct wl_array *array)
{
	struct stat st;
	dev_t *device;
	char *node;

	/*
	 * Clients open this device to allocate on. Hand them the render node
	 * where there is one: the primary node needs DRM authentication, which a
	 * plain client has no way to obtain.
	 */
	node = drmGetRenderDeviceNameFromFd(swc.drm->fd);
	if (node) {
		int ret = stat(node, &st);

		drmFree(node);
		if (ret != 0 && fstat(swc.drm->fd, &st) != 0)
			return false;
	} else if (fstat(swc.drm->fd, &st) != 0) {
		return false;
	}

	wl_array_init(array);
	if (!(device = wl_array_add(array, sizeof(*device)))) {
		wl_array_release(array);
		return false;
	}
	*device = st.st_rdev;

	return true;
}

static void
send_feedback(struct wl_resource *resource)
{
	struct wl_array device, indices;
	uint16_t *index;
	size_t size, i;
	int fd;

	if ((fd = format_table_fd(&size)) < 0) {
		if (errno == EMFILE || errno == ENFILE) {
			fd_report("could not build the dmabuf format table");
		}
		return;
	}
	zwp_linux_dmabuf_feedback_v1_send_format_table(resource, fd, size);
	close(fd);

	if (!main_device(&device))
		return;
	zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &device);

	/* A single tranche on the main device, offering every format. */
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &device);

	wl_array_init(&indices);
	for (i = 0; i < format_table_len; ++i) {
		if ((index = wl_array_add(&indices, sizeof(*index))))
			*index = i;
	}
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &indices);
	wl_array_release(&indices);

	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, 0);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
	zwp_linux_dmabuf_feedback_v1_send_done(resource);

	wl_array_release(&device);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
	.destroy = destroy_resource,
};

static void
create_feedback(struct wl_client *client, struct wl_resource *dmabuf_resource,
                uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
	                              wl_resource_get_version(dmabuf_resource), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &feedback_impl, NULL, NULL);
	send_feedback(resource);
}

static void
get_default_feedback(struct wl_client *client, struct wl_resource *resource,
                     uint32_t id)
{
	create_feedback(client, resource, id);
}

static void
get_surface_feedback(struct wl_client *client, struct wl_resource *resource,
                     uint32_t id, struct wl_resource *surface)
{
	/* We have nothing surface-specific to say, so this matches the default. */
	create_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = destroy_resource,
    .create_params = create_params,
    .get_default_feedback = get_default_feedback,
    .get_surface_feedback = get_surface_feedback,
};

static void
bind_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	static const uint32_t formats[] = {
	    DRM_FORMAT_XRGB8888,
	    DRM_FORMAT_ARGB8888,
	};
	struct wl_resource *resource;
	size_t i;

	resource =
	    wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &dmabuf_impl, NULL, NULL);

	/* Superseded by feedback from version 4 on. */
	if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION)
		return;

	if (version >= 3) {
		build_format_table();
		for (i = 0; i < format_table_len; ++i) {
			uint64_t modifier = format_table[i].modifier;
			zwp_linux_dmabuf_v1_send_modifier(resource, format_table[i].format,
			    modifier >> 32, modifier & 0xffffffff);
		}
	} else {
		for (i = 0; i < ARRAY_LENGTH(formats); ++i)
			zwp_linux_dmabuf_v1_send_format(resource, formats[i]);
	}
}

struct wl_global *
swc_dmabuf_create(struct wl_display *display)
{
	return wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 4, NULL,
	                        &bind_dmabuf);
}
