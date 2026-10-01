/* swc: libswc/shm.c
 *
 * Copyright (c) 2013-2020 Michael Forney
 *
 * Based in part upon wayland-shm.c from wayland, which is:
 *
 *     Copyright © 2008 Kristian Høgsberg
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

#include "shm.h"
#include "internal.h"
#include "util.h"
#include "wayland_buffer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wld/pixman.h>
#include <wld/wld.h>

/* Bound compositor-owned snapshots independently of advertised pool sizes. */
#define CLIENT_SHM_BYTES (UINT64_C(512) * 1024 * 1024)
#define TOTAL_SHM_BYTES (UINT64_C(2048) * 1024 * 1024)
#define CLIENT_SHM_BUFFERS 1024
struct shm_storage {
	struct wl_list link;
	struct wl_listener destroy;
	struct wl_client *client;
	uint64_t bytes;
	unsigned buffers, refs;
};
static struct wl_list shm_storage_list = { &shm_storage_list, &shm_storage_list };
static uint64_t owned_pixel_bytes;

static void storage_unref(struct shm_storage *storage)
{
	if (!--storage->refs) { wl_list_remove(&storage->link); free(storage); }
}
static void storage_client_destroyed(struct wl_listener *listener, void *data)
{
	struct shm_storage *storage = wl_container_of(listener, storage, destroy);
	(void)data;
	wl_list_remove(&storage->destroy.link);
	storage->client = NULL;
	storage_unref(storage);
}
static struct shm_storage *storage_get(struct wl_client *client)
{
	struct shm_storage *storage;
	wl_list_for_each(storage, &shm_storage_list, link)
		if (storage->client == client) { ++storage->refs; return storage; }
	storage = calloc(1, sizeof(*storage));
	if (!storage) return NULL;
	storage->client = client; storage->refs = 2; /* client + new pool */
	storage->destroy.notify = storage_client_destroyed;
	wl_client_add_destroy_listener(client, &storage->destroy);
	wl_list_insert(&shm_storage_list, &storage->link);
	return storage;
}

struct pool_mapping {
	uint32_t size;
	unsigned references;
};

struct pool {
	struct wl_resource *resource;
	struct swc_shm *shm;
	struct shm_storage *storage;
	struct pool_mapping *mapping;
	int fd;
	bool writable;
	unsigned references;
};

struct pool_reference {
	struct wld_destructor destructor;
	struct pool *pool;
	struct pool_mapping *mapping;
	void *pixels;
	uint64_t bytes;
};

struct shm_buffer_record {
	struct wl_list link;
	struct wl_listener destroy_listener;
	struct wl_resource *resource;
	struct pool *pool;
	void *pixels;
	uint32_t offset;
	int32_t width;
	int32_t height;
	int32_t stride;
	uint32_t format;
};

static struct wl_list shm_buffer_records;
static bool shm_buffer_records_initialized;
static uint64_t live_mapping_bytes;
static unsigned live_mappings;

static void
log_mapping_memory(void)
{
	const char *enabled = getenv("CHARAWC_DEBUG_MEMORY");
	if (enabled && strcmp(enabled, "1") == 0)
		fprintf(stderr, "memory-profile: pid=%ld shm_mappings=%u shm_bytes=%" PRIu64 "\n",
		        (long)getpid(), live_mappings, live_mapping_bytes);
}

static void
ensure_shm_buffer_records(void)
{
	if (!shm_buffer_records_initialized) {
		wl_list_init(&shm_buffer_records);
		shm_buffer_records_initialized = true;
	}
}

static void
handle_shm_buffer_resource_destroy(struct wl_listener *listener, void *data)
{
	struct shm_buffer_record *record =
	    wl_container_of(listener, record, destroy_listener);
	(void)data;

	wl_list_remove(&record->destroy_listener.link);
	wl_list_remove(&record->link);
	free(record);
}

/* Existing pixman images retain their original data pointer. Keep each
 * mapping alive until the buffers imported from it have been released. */
static struct pool_mapping *
map_pool(int fd, uint32_t size, bool writable)
{
	struct pool_mapping *mapping = malloc(sizeof(*mapping));

	if (!mapping)
		return NULL;
	/* Keep bounds/lifetime metadata only. Never map client-controlled files:
	 * pread/pwrite report truncation as an error rather than raising SIGBUS. */
	(void)fd;
	(void)writable;
	mapping->size = size;
	mapping->references = 1;
	++live_mappings;
	live_mapping_bytes += size;
	log_mapping_memory();
	return mapping;
}

static void
unref_mapping(struct pool_mapping *mapping)
{
	if (--mapping->references)
		return;
	--live_mappings;
	live_mapping_bytes -= mapping->size;
	free(mapping);
	log_mapping_memory();
}

static void
unref_pool(struct pool *pool)
{
	if (--pool->references > 0) {
		return;
	}

	unref_mapping(pool->mapping);
	storage_unref(pool->storage);
	close(pool->fd);
	free(pool);
}

static void
destroy_pool_resource(struct wl_resource *resource)
{
	struct pool *pool = wl_resource_get_user_data(resource);
	unref_pool(pool);
}

static void
handle_buffer_destroy(struct wld_destructor *destructor)
{
	struct pool_reference *reference =
	    wl_container_of(destructor, reference, destructor);
	owned_pixel_bytes -= reference->bytes;
	reference->pool->storage->bytes -= reference->bytes;
	--reference->pool->storage->buffers;
	free(reference->pixels);
	unref_mapping(reference->mapping);
	unref_pool(reference->pool);
	free(reference);
}

static inline uint32_t
format_shm_to_wld(uint32_t format)
{
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
		return WLD_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888:
		return WLD_FORMAT_XRGB8888;
	default:
		return format;
	}
}

static void
create_buffer(struct wl_client *client, struct wl_resource *resource,
              uint32_t id, int32_t offset, int32_t width, int32_t height,
              int32_t stride, uint32_t format)
{
	struct pool *pool = wl_resource_get_user_data(resource);
	struct pool_reference *reference;
	struct shm_buffer_record *record = NULL;
	struct wld_buffer *buffer;
	struct wl_resource *buffer_resource;
	union wld_object object;
	void *pixels;

	if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT,
		                       "unsupported shm format");
		return;
	}
	if (offset < 0 || offset % 4 || width <= 0 || height <= 0 || stride <= 0 ||
	    stride % 4 || (uint64_t)width * 4 > (uint32_t)stride ||
	    (uint64_t)offset + (uint64_t)stride * height > pool->mapping->size) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
		                       "buffer dimensions exceed the pool or stride");
		return;
	}

	uint64_t bytes = (uint64_t)height * stride;
	if (bytes > CLIENT_SHM_BYTES - pool->storage->bytes ||
	    bytes > TOTAL_SHM_BYTES - owned_pixel_bytes ||
	    pool->storage->buffers >= CLIENT_SHM_BUFFERS) {
		wl_resource_post_no_memory(resource);
		return;
	}
	pixels = calloc((size_t)height, (size_t)stride);
	if (!pixels) {
		wl_resource_post_no_memory(resource);
		return;
	}
	object.ptr = pixels;
	buffer =
	    wld_import_buffer(pool->shm->context, WLD_OBJECT_DATA, object, width,
	                      height, format_shm_to_wld(format), stride);

	if (!buffer) {
		goto error0;
	}

	buffer_resource = wayland_buffer_create_resource(
	    client, wl_resource_get_version(resource), id, buffer);

	if (!buffer_resource) {
		goto error1;
	}

	ensure_shm_buffer_records();
	record = malloc(sizeof(*record));
	if (!record) {
		goto error2;
	}
	record->resource = buffer_resource;
	record->pool = pool;
	record->pixels = pixels;
	record->offset = (uint32_t)offset;
	record->width = width;
	record->height = height;
	record->stride = stride;
	record->format = format;
	record->destroy_listener.notify = &handle_shm_buffer_resource_destroy;
	wl_resource_add_destroy_listener(buffer_resource, &record->destroy_listener);
	wl_list_insert(&shm_buffer_records, &record->link);

	if (!(reference = malloc(sizeof(*reference)))) {
		goto error3;
	}

	reference->bytes = bytes;
	owned_pixel_bytes += bytes;
	pool->storage->bytes += bytes;
	++pool->storage->buffers;
	reference->pool = pool;
	reference->pixels = pixels;
	reference->mapping = pool->mapping;
	++reference->mapping->references;
	reference->destructor.destroy = &handle_buffer_destroy;
	wld_buffer_add_destructor(buffer, &reference->destructor);
	++pool->references;

	return;

error3:
	if (record) {
		wl_list_remove(&record->destroy_listener.link);
		wl_list_remove(&record->link);
		free(record);
	}
error2:
	wl_resource_destroy(buffer_resource);
	free(pixels);
	wl_resource_post_no_memory(resource);
	return;
error1:
	wld_buffer_unreference(buffer);
error0:
	free(pixels);
	wl_resource_post_no_memory(resource);
}

static void
resize(struct wl_client *client, struct wl_resource *resource, int32_t size)
{
	struct pool *pool = wl_resource_get_user_data(resource);
	struct pool_mapping *mapping;
	struct stat st;

	if (size <= 0 || (uint32_t)size < pool->mapping->size) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
		                       "shm pools cannot shrink");
		return;
	}
	if ((uint32_t)size == pool->mapping->size)
		return;
	if (fstat(pool->fd, &st) != 0 || st.st_size < size) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
		                       "backing file is smaller than the requested pool");
		return;
	}

	mapping = map_pool(pool->fd, size, pool->writable);
	if (!mapping) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
		                       "could not allocate pool metadata: %s", strerror(errno));
		return;
	}
	unref_mapping(pool->mapping);
	pool->mapping = mapping;
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = create_buffer,
    .destroy = destroy_resource,
    .resize = resize,
};

static void
create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id,
            int32_t fd, int32_t size)
{
	struct swc_shm *shm = wl_resource_get_user_data(resource);
	struct pool *pool;
	struct stat st;

	if (size <= 0 || fstat(fd, &st) != 0 || st.st_size < size) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
		                       "invalid shm pool size or backing file");
		goto error0;
	}
	pool = malloc(sizeof(*pool));
	if (!pool) {
		wl_resource_post_no_memory(resource);
		goto error0;
	}
	pool->storage = storage_get(client);
	if (!pool->storage) { wl_resource_post_no_memory(resource); goto error1; }
	pool->shm = shm;
	int access_mode = fcntl(fd, F_GETFL);
	pool->writable = access_mode >= 0 && (access_mode & O_ACCMODE) != O_RDONLY;
	pool->mapping = map_pool(fd, size, pool->writable);
	if (!pool->mapping) {
		pool->writable = false;
		pool->mapping = map_pool(fd, size, false);
		if (!pool->mapping) {
			wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
			                       "could not allocate pool metadata: %s", strerror(errno));
			goto error1;
		}
	}
	pool->references = 1;
	pool->fd = fd;
	pool->resource = wl_resource_create(client, &wl_shm_pool_interface,
	                                    wl_resource_get_version(resource), id);
	if (!pool->resource) {
		unref_mapping(pool->mapping);
		wl_resource_post_no_memory(resource);
		goto error1;
	}
	/* Install the destructor only after all the pool's fields are valid. */
	wl_resource_set_implementation(pool->resource, &shm_pool_impl, pool,
	                               &destroy_pool_resource);
	return;

error1:
	if (pool->storage) storage_unref(pool->storage);
	free(pool);
error0:
	close(fd);
}

static const struct wl_shm_interface shm_impl = {.create_pool = &create_pool};

static void
bind_shm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct swc_shm *shm = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_shm_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &shm_impl, shm, NULL);

	wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
	wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
}

struct swc_shm *
shm_create(struct wl_display *display)
{
	struct swc_shm *shm;

	shm = malloc(sizeof(*shm));
	if (!shm) {
		goto error0;
	}
	shm->context = wld_pixman_create_context();
	if (!shm->context) {
		goto error1;
	}
	shm->renderer = wld_create_renderer(shm->context);
	if (!shm->renderer) {
		goto error2;
	}
	shm->global =
	    wl_global_create(display, &wl_shm_interface, 1, shm, &bind_shm);
	if (!shm->global) {
		goto error3;
	}

	return shm;

error3:
	wld_destroy_renderer(shm->renderer);
error2:
	wld_destroy_context(shm->context);
error1:
	free(shm);
error0:
	return NULL;
}

void
shm_destroy(struct swc_shm *shm)
{
	wl_global_destroy(shm->global);
	wld_destroy_renderer(shm->renderer);
	wld_destroy_context(shm->context);
	free(shm);
}

bool
shm_buffer_get_info(struct wl_resource *resource, struct swc_shm_buffer_info *info)
{
	struct shm_buffer_record *record;
	uint64_t size;

	if (!shm_buffer_records_initialized) {
		return false;
	}

	wl_list_for_each(record, &shm_buffer_records, link)
	{
		if (record->resource != resource) {
			continue;
		}

		if (record->width < 0 || record->height < 0 || record->stride < 0) {
			return false;
		}

		size = (uint64_t)record->stride * (uint64_t)record->height;
		if ((uint64_t)record->offset + size > record->pool->mapping->size) {
			return false;
		}

		info->data = record->pixels;
		info->width = record->width;
		info->height = record->height;
		info->stride = record->stride;
		info->format = record->format;
		info->writable = record->pool->writable;
		return true;
	}

	return false;
}

/* Copy the committed pixels into compositor-owned storage. Even a file that
	* is truncated concurrently cannot fault this process: the system call fails. */
bool
shm_buffer_read(struct wl_resource *resource)
{
	struct shm_buffer_record *record;
	if (!resource || !shm_buffer_records_initialized) return true;
	wl_list_for_each(record, &shm_buffer_records, link) {
		if (record->resource != resource) continue;
		size_t done = 0, bytes = (size_t)record->stride * record->height;
		while (done < bytes) {
			ssize_t n = pread(record->pool->fd, (char *)record->pixels + done,
		    bytes - done, (off_t)record->offset + done);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) {
				wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD,
		    "shared memory backing file became unreadable or too small");
				return false;
			}
			done += (size_t)n;
		}
		return true;
	}
	return true;
}

bool
shm_buffer_write(struct wl_resource *resource)
{
	struct shm_buffer_record *record;
	if (!resource || !shm_buffer_records_initialized) return false;
	wl_list_for_each(record, &shm_buffer_records, link) {
		if (record->resource != resource) continue;
		if (!record->pool->writable) return false;
		struct stat st;
		if (fstat(record->pool->fd, &st) != 0 ||
		    (uint64_t)record->offset + (uint64_t)record->stride * record->height > (uint64_t)st.st_size)
			return false;
		for (int32_t y = 0; y < record->height; ++y) {
			size_t done = 0, bytes = (size_t)record->width * 4;
			while (done < bytes) {
				ssize_t n = pwrite(record->pool->fd,
		    (char *)record->pixels + (size_t)y * record->stride + done,
		    bytes - done, (off_t)record->offset + (off_t)y * record->stride + done);
				if (n < 0 && errno == EINTR) continue;
				if (n <= 0) return false;
				done += (size_t)n;
			}
		}
		return true;
	}
	return false;
}
