/* swc: libswc/drm_syncobj.c
 *
 * Explicit client synchronization (wp_linux_drm_syncobj_v1).
 *
 * With implicit synchronization the kernel attaches a fence to a dmabuf's
 * reservation object, and the compositor can sample the buffer whenever it
 * likes: access is ordered behind the client's rendering for it. Some drivers,
 * the NVIDIA proprietary driver among them, do not do this. Composition then
 * races the client's rendering, and a buffer sampled mid-draw shows whatever
 * was in it, which for a freshly cleared render target is black.
 *
 * This protocol replaces that guesswork with DRM synchronization object
 * timeline points: the client names a point that must signal before the
 * compositor reads the buffer, and a point the compositor signals once it is
 * done reading.
 */

#include "drm_syncobj.h"

#ifdef ENABLE_DRM

#include "drm.h"
#include "internal.h"
#include "surface.h"
#include "util.h"

/*
 * Spelled out: libswc has its own drm.h, and an unqualified <drm.h> finds that
 * one first, leaving xf86drm.h without the kernel types it needs.
 */
#include <libdrm/drm.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wld/wld.h>
#include <xf86drm.h>

#include "linux-drm-syncobj-v1-server-protocol.h"

struct syncobj_timeline {
	uint32_t handle;
	/*
	 * The client may destroy a timeline while the compositor still owes it a
	 * release point, and destroying the object is not allowed to unset points
	 * already committed, so the handle outlives its resource.
	 */
	unsigned refs;
};

struct syncobj_point {
	struct syncobj_timeline *timeline;
	uint64_t value;
};

struct drm_syncobj_surface {
	struct wl_resource *resource;
	struct surface *surface;
	struct wl_listener surface_destroy_listener;

	/* Staged by set_acquire_point/set_release_point for the next commit. */
	struct {
		struct syncobj_point acquire, release;
		bool has_acquire, has_release;
	} pending;

	/* The release point owed for the buffer currently held by the surface. */
	struct syncobj_point release;
	bool has_release;
};

static struct syncobj_timeline *
timeline_ref(struct syncobj_timeline *timeline)
{
	++timeline->refs;
	return timeline;
}

static void
timeline_unref(struct syncobj_timeline *timeline)
{
	if (!timeline || --timeline->refs > 0) {
		return;
	}

	drmSyncobjDestroy(swc.drm->fd, timeline->handle);
	free(timeline);
}

static void
point_clear(struct syncobj_point *point)
{
	timeline_unref(point->timeline);
	point->timeline = NULL;
	point->value = 0;
}

static void
point_set(struct syncobj_point *point, struct syncobj_timeline *timeline,
          uint64_t value)
{
	struct syncobj_timeline *old = point->timeline;

	point->timeline = timeline ? timeline_ref(timeline) : NULL;
	point->value = value;
	timeline_unref(old);
}

/*
 * Turns a timeline point into a sync_file the renderer can wait on.
 *
 * A timeline point cannot be exported directly, so it is transferred into a
 * throwaway binary syncobj first.
 */
static int
point_export_sync_file(const struct syncobj_point *point)
{
	uint32_t binary;
	int fd = -1;

	if (drmSyncobjCreate(swc.drm->fd, 0, &binary) != 0) {
		return -1;
	}

	if (drmSyncobjTransfer(swc.drm->fd, binary, 0, point->timeline->handle,
	                       point->value, 0) == 0) {
		if (drmSyncobjExportSyncFile(swc.drm->fd, binary, &fd) != 0) {
			fd = -1;
		}
	}

	drmSyncobjDestroy(swc.drm->fd, binary);

	return fd;
}

/*
 * Block until the point signals.
 *
 * This is the fallback for when the point cannot be handed to the renderer as
 * a fence: exporting a sync_file needs a descriptor, and the renderer's GPU
 * wait needs one too, so both fail together once the process runs out. Waiting
 * on the CPU stalls the frame, but skipping the wait altogether hands the
 * renderer a buffer the client may still be drawing into, which is the
 * unsynchronized-sampling race this whole file exists to close: on a driver
 * without implicit fences the result is a black frame.
 *
 * The deadline bounds the damage from a client that never signals; letting one
 * wedge the compositor would be worse than one late frame.
 */
#define ACQUIRE_WAIT_MS 50

static void
point_wait_cpu(const struct syncobj_point *point)
{
	uint64_t value = point->value;
	uint32_t handle;
	struct timespec now;
	int64_t deadline;

	if (!point->timeline) {
		return;
	}
	handle = point->timeline->handle;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return;
	}
	deadline = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec +
	           ACQUIRE_WAIT_MS * 1000000;

	/*
	 * WAIT_FOR_SUBMIT: the client may have named a point it has not submitted
	 * work for yet, which without this flag is an immediate error rather than
	 * a wait.
	 */
	if (drmSyncobjTimelineWait(swc.drm->fd, &handle, &value, 1, deadline,
	                           DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
	                           NULL) != 0) {
		static bool warned;

		if (!warned) {
			warned = true;
			WARNING("Timed out waiting for a DRM syncobj acquire point: %s\n",
			        strerror(errno));
		}
	}
}

static void
point_signal(struct syncobj_point *point)
{
	uint64_t value = point->value;

	if (!point->timeline) {
		return;
	}

	if (drmSyncobjTimelineSignal(swc.drm->fd, &point->timeline->handle, &value,
	                             1) != 0) {
		WARNING("Could not signal DRM syncobj release point: %s\n",
		        strerror(errno));
	}

	point_clear(point);
}

/* Timeline object {{{ */

static void
destroy_timeline_resource(struct wl_resource *resource)
{
	timeline_unref(wl_resource_get_user_data(resource));
}

static void
timeline_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
    .destroy = timeline_destroy,
};

/* }}} */

/* Surface object {{{ */

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct drm_syncobj_surface *synced =
	    wl_container_of(listener, synced, surface_destroy_listener);

	/*
	 * The buffer will never be sampled again, so hand the client back the
	 * release point it is waiting on rather than leaving it stuck.
	 */
	point_signal(&synced->release);
	synced->has_release = false;
	synced->surface->synced = NULL;
	synced->surface = NULL;
	wl_list_remove(&synced->surface_destroy_listener.link);
	wl_list_init(&synced->surface_destroy_listener.link);
}

static void
destroy_syncobj_surface_resource(struct wl_resource *resource)
{
	struct drm_syncobj_surface *synced = wl_resource_get_user_data(resource);

	if (synced->surface) {
		/*
		 * The surface outlives this object, but the outstanding release point
		 * lives here and is about to be freed, so it has to be signalled now
		 * rather than on the commit that replaces the buffer. That is safe
		 * for the same reason the commit path is: the renderer finishes its
		 * GPU work before each frame reaches KMS, and requests are handled
		 * between frames, so nothing is still reading the buffer. It does
		 * mean a client destroying this object mid-frame gets its buffer
		 * back marginally earlier than the protocol promises.
		 */
		point_signal(&synced->release);
		synced->has_release = false;
		synced->surface->synced = NULL;
		wl_list_remove(&synced->surface_destroy_listener.link);
	}

	point_clear(&synced->pending.acquire);
	point_clear(&synced->pending.release);
	free(synced);
}

static void
syncobj_surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
set_point(struct wl_client *client, struct wl_resource *resource,
          struct wl_resource *timeline_resource, uint32_t point_hi,
          uint32_t point_lo, bool acquire)
{
	struct drm_syncobj_surface *synced = wl_resource_get_user_data(resource);
	struct syncobj_timeline *timeline =
	    wl_resource_get_user_data(timeline_resource);
	uint64_t value = (uint64_t)point_hi << 32 | point_lo;

	if (!synced->surface) {
		wl_resource_post_error(resource,
		                       WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
		                       "the wl_surface was destroyed");
		return;
	}

	if (acquire) {
		point_set(&synced->pending.acquire, timeline, value);
		synced->pending.has_acquire = true;
	} else {
		point_set(&synced->pending.release, timeline, value);
		synced->pending.has_release = true;
	}
}

static void
set_acquire_point(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *timeline, uint32_t point_hi,
                  uint32_t point_lo)
{
	set_point(client, resource, timeline, point_hi, point_lo, true);
}

static void
set_release_point(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *timeline, uint32_t point_hi,
                  uint32_t point_lo)
{
	set_point(client, resource, timeline, point_hi, point_lo, false);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface
    syncobj_surface_impl = {
        .destroy = syncobj_surface_destroy,
        .set_acquire_point = set_acquire_point,
        .set_release_point = set_release_point,
};

/* }}} */

/* Manager {{{ */

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
get_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
            struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct drm_syncobj_surface *synced;

	if (surface->synced) {
		wl_resource_post_error(
		    resource, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
		    "the surface already has a synchronization object");
		return;
	}

	if (!(synced = calloc(1, sizeof(*synced)))) {
		wl_resource_post_no_memory(resource);
		return;
	}

	synced->resource = wl_resource_create(
	    client, &wp_linux_drm_syncobj_surface_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!synced->resource) {
		free(synced);
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(synced->resource, &syncobj_surface_impl,
	                               synced,
	                               &destroy_syncobj_surface_resource);

	synced->surface = surface;
	synced->surface_destroy_listener.notify = &handle_surface_destroy;
	wl_signal_add(&surface->signal.destroy, &synced->surface_destroy_listener);
	surface->synced = synced;
}

static void
import_timeline(struct wl_client *client, struct wl_resource *resource,
                uint32_t id, int32_t fd)
{
	struct syncobj_timeline *timeline;
	struct wl_resource *timeline_resource;
	uint32_t handle;

	if (drmSyncobjFDToHandle(swc.drm->fd, fd, &handle) != 0) {
		close(fd);
		wl_resource_post_error(
		    resource, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
		    "could not import the DRM syncobj timeline");
		return;
	}

	close(fd);

	if (!(timeline = malloc(sizeof(*timeline)))) {
		drmSyncobjDestroy(swc.drm->fd, handle);
		wl_resource_post_no_memory(resource);
		return;
	}

	timeline->handle = handle;
	timeline->refs = 1;

	timeline_resource =
	    wl_resource_create(client, &wp_linux_drm_syncobj_timeline_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!timeline_resource) {
		timeline_unref(timeline);
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(timeline_resource, &timeline_impl, timeline,
	                               &destroy_timeline_resource);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .get_surface = get_surface,
    .import_timeline = import_timeline,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(
	    client, &wp_linux_drm_syncobj_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *
drm_syncobj_manager_create(struct wl_display *display)
{
	uint64_t cap = 0;

	if (drmGetCap(swc.drm->fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) {
		WARNING("DRM device has no timeline syncobj support; explicit client "
		        "synchronization is unavailable\n");
		return NULL;
	}

	/*
	 * Without a renderer that can wait on an acquire fence there is nothing to
	 * gain from advertising this: a client that sees the global is entitled to
	 * stop synchronizing implicitly, which would make the race it exists to
	 * fix strictly more likely.
	 */
	if (!wld_wait_fence(swc.backend->renderer, -1)) {
		WARNING("Renderer cannot wait on fences; explicit client "
		        "synchronization is unavailable\n");
		return NULL;
	}

	return wl_global_create(display, &wp_linux_drm_syncobj_manager_v1_interface,
	                        1, NULL, &bind_manager);
}

/* }}} */

/* Surface commit {{{ */

bool
drm_syncobj_surface_check_commit(struct surface *surface)
{
	struct drm_syncobj_surface *synced = surface->synced;
	bool attached, has_buffer;

	if (!synced) {
		return true;
	}

	attached = surface->pending.commit & SURFACE_COMMIT_ATTACH;
	has_buffer = attached && surface->pending.state.buffer_resource;

	if (!has_buffer) {
		if (synced->pending.has_acquire || synced->pending.has_release) {
			wl_resource_post_error(
			    synced->resource,
			    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
			    "a timeline point was set without attaching a buffer");
			return false;
		}
		return true;
	}

	if (!synced->pending.has_acquire) {
		wl_resource_post_error(
		    synced->resource,
		    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT,
		    "a buffer was attached without an acquire point");
		return false;
	}

	if (!synced->pending.has_release) {
		wl_resource_post_error(
		    synced->resource,
		    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
		    "a buffer was attached without a release point");
		return false;
	}

	if (synced->pending.acquire.timeline == synced->pending.release.timeline &&
	    synced->pending.acquire.value >= synced->pending.release.value) {
		wl_resource_post_error(
		    synced->resource,
		    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
		    "the acquire point is not before the release point on the same "
		    "timeline");
		return false;
	}

	return true;
}

void
drm_syncobj_surface_apply_commit(struct surface *surface, bool attached)
{
	struct drm_syncobj_surface *synced = surface->synced;
	int fence_fd;

	if (!synced) {
		return;
	}

	if (!attached) {
		/* The surface kept its buffer, so its release is still owed. */
		return;
	}

	/*
	 * Every read of the replaced buffer has completed: the renderer finishes
	 * its GPU work before each frame is handed to KMS, and commits are
	 * processed between frames, never during one.
	 */
	if (synced->has_release) {
		point_signal(&synced->release);
		synced->has_release = false;
	}

	if (!synced->pending.has_acquire) {
		/* A NULL buffer was attached; there is nothing left to wait for. */
		point_clear(&synced->pending.release);
		synced->pending.has_release = false;
		return;
	}

	/*
	 * A buffer the renderer samples on the GPU needs the wait; one that is
	 * copied on the CPU came from a client that already finished writing it.
	 */
	if (surface->state.buffer
	    && (wld_capabilities(swc.backend->renderer, surface->state.buffer)
	        & WLD_CAPABILITY_READ)) {
		bool waited = false;

		if ((fence_fd = point_export_sync_file(&synced->pending.acquire)) < 0) {
			static bool warned;

			if (!warned) {
				warned = true;
				WARNING("Could not export a DRM syncobj acquire fence: %s\n",
				        strerror(errno));
			}
			if (errno == EMFILE || errno == ENFILE) {
				fd_report("could not export a syncobj acquire fence");
			}
		} else {
			waited = wld_wait_fence(swc.backend->renderer, fence_fd);
			close(fence_fd);
		}

		if (!waited) {
			point_wait_cpu(&synced->pending.acquire);
		}
	}

	point_clear(&synced->pending.acquire);
	synced->pending.has_acquire = false;

	point_set(&synced->release, synced->pending.release.timeline,
	          synced->pending.release.value);
	synced->has_release = true;
	point_clear(&synced->pending.release);
	synced->pending.has_release = false;
}

void
drm_syncobj_surface_finish(struct surface *surface)
{
	struct drm_syncobj_surface *synced = surface->synced;

	if (!synced) {
		return;
	}

	point_signal(&synced->release);
	synced->has_release = false;
}

/* }}} */

#endif
