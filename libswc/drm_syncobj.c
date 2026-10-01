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
#include "view.h"
#include "wayland_buffer.h"

/*
 * Spelled out: libswc has its own drm.h, and an unqualified <drm.h> finds that
 * one first, leaving xf86drm.h without the kernel types it needs.
 */
#include <libdrm/drm.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
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

struct pending_buffer;

#define MAX_PENDING_BUFFERS 2

struct syncobj_commit {
	struct syncobj_point acquire, release;
	bool has_acquire, has_release;
};

struct drm_syncobj_surface {
	struct wl_resource *resource;
	struct surface *surface;
	struct wl_listener surface_destroy_listener;

	/* Staged by set_acquire_point/set_release_point for the next commit. */
	struct syncobj_commit pending, cached;

	/* The release point owed for the buffer on screen. */
	struct syncobj_point release;
	bool has_release;

	/*
	 * Committed buffers whose acquire points have not signalled yet, oldest
	 * first. Nothing may sample them until they do, so the view keeps
	 * showing what it shows and each one is put up from the event loop when
	 * it becomes ready. The commits themselves are not held up: everything
	 * else in them applies at once.
	 *
	 * The oldest is never displaced by a newer commit, only by a newer
	 * buffer becoming ready first, so a client that always has the next
	 * frame queued before the last one finishes still gets its frames
	 * shown. Commits in between replace one another in the last slot.
	 */
	struct pending_buffer *queue[MAX_PENDING_BUFFERS];
	unsigned queued;
	bool failed_acquire;

	/*
	 * The buffer on screen when it is no longer the surface's committed
	 * buffer, so that its wl_buffer.release is ours to send when it comes
	 * down rather than the commit path's.
	 */
	struct wl_resource *held;
	struct wl_listener held_destroy_listener;
};

/* A buffer waiting for its acquire point before it may be shown. */
struct pending_buffer {
	struct drm_syncobj_surface *synced;
	struct wl_resource *resource;
	struct wl_listener destroy_listener;
	struct wl_event_source *source;
	int fd;
	/* Signalled once the buffer is done with, whether shown or not. */
	struct syncobj_point release, acquire;
	struct wld_buffer *buffer;
	int32_t scale, transform;
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

static bool
point_wait_cpu(const struct syncobj_point *point)
{
	uint64_t value = point->value;
	uint32_t handle;
	struct timespec now;
	int64_t deadline;

	if (!point->timeline) {
		return false;
	}
	handle = point->timeline->handle;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return false;
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
		return false;
	}
	return true;
}

/* Whether a descriptor has become readable, without waiting for it to. */
static bool
fd_readable(int fd)
{
	struct pollfd pollfd = {.fd = fd, .events = POLLIN};
	int ret;

	do {
		ret = poll(&pollfd, 1, 0);
	} while (ret < 0 && errno == EINTR);

	return ret > 0 && (pollfd.revents & POLLIN) && !(pollfd.revents & POLLERR);
}

/*
 * Signals the point from the CPU, now. Only right for a buffer the renderer
 * never sampled, or after its GPU work is known to be complete.
 */
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

/*
 * Signals the point once the GPU has finished everything submitted so far,
 * which includes every frame that sampled the buffer it belongs to.
 *
 * Frames are only flushed to the GPU, not finished, so signalling from the
 * CPU here would let the client draw into a buffer a frame still in flight is
 * reading. Instead the renderer's fence is attached to the point, and the
 * kernel signals it when that fence does.
 */
static struct wl_list deferred_releases = { &deferred_releases, &deferred_releases };
struct deferred_point_release {
	struct wl_list link;
	struct syncobj_point point;
	struct wl_event_source *source;
	int fd;
};

static int
complete_point_release(int fd, uint32_t mask, void *data)
{
	struct deferred_point_release *release = data;
	(void)mask;
	if (fd_readable(fd)) point_signal(&release->point);
	else if (!(mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP))) return 0;
	point_clear(&release->point);
	wl_event_source_remove(release->source);
	close(release->fd);
	wl_list_remove(&release->link);
	free(release);
	return 0;
}

static void
point_release(struct syncobj_point *point)
{
	uint32_t binary;
	int fence;
	bool attached = false;

	if (!point->timeline) {
		return;
	}

	/* -1 means the renderer finished its work itself before returning. */
	fence = wld_export_fence(swc.backend->renderer);
	if (fence >= 0) {
		if (drmSyncobjCreate(swc.drm->fd, 0, &binary) == 0) {
			attached =
			    drmSyncobjImportSyncFile(swc.drm->fd, binary, fence) == 0 &&
			    drmSyncobjTransfer(swc.drm->fd, point->timeline->handle,
			                       point->value, binary, 0, 0) == 0;
			drmSyncobjDestroy(swc.drm->fd, binary);
		}
		if (!attached && !fd_readable(fence)) {
			struct deferred_point_release *release = calloc(1, sizeof(*release));
			if (release) {
				release->fd = fence;
				point_set(&release->point, point->timeline, point->value);
				release->source = wl_event_loop_add_fd(swc.event_loop, fence, WL_EVENT_READABLE,
		    complete_point_release, release);
				if (release->source) {
		    wl_list_insert(&deferred_releases, &release->link);
		    point_clear(point); return;
				}
				point_clear(&release->point);
				free(release);
			}
			/* Resource exhaustion must never claim that in-flight GPU reads ended. */
			WARNING("Could not defer DRM release; leaving its point unsignalled\n");
			close(fence);
			point_clear(point);
			return;
		}

		close(fence);
	}

	if (attached) {
		point_clear(point);
	} else {
		point_signal(point);
	}
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

/* Held-back buffer {{{ */

static void
handle_held_destroy(struct wl_listener *listener, void *data)
{
	struct drm_syncobj_surface *synced =
	    wl_container_of(listener, synced, held_destroy_listener);

	(void)data;
	/* The view keeps its own reference to the pixels; only the release has
	 * nowhere to go now. */
	wl_list_remove(&synced->held_destroy_listener.link);
	synced->held = NULL;
}

static void
held_set(struct drm_syncobj_surface *synced, struct wl_resource *resource)
{
	synced->held = resource;
	synced->held_destroy_listener.notify = &handle_held_destroy;
	wl_resource_add_destroy_listener(resource, &synced->held_destroy_listener);
}

/* Stop tracking the held buffer; 'release' says whether to send its release. */
static void
held_clear(struct drm_syncobj_surface *synced, bool release)
{
	if (!synced->held) {
		return;
	}

	wl_list_remove(&synced->held_destroy_listener.link);
	if (release) {
		wl_buffer_send_release(synced->held);
	}
	synced->held = NULL;
}

/* Everything owed for the buffer on screen, now that it is being replaced. */
static void
shown_release(struct drm_syncobj_surface *synced)
{
	if (synced->has_release) {
		point_release(&synced->release);
		synced->has_release = false;
	}
	held_clear(synced, true);
}

/* }}} */

/* Buffers waiting for their acquire points {{{ */

static void
pending_free(struct pending_buffer *pending)
{
	if (pending->source) {
		wl_event_source_remove(pending->source);
	}
	if (pending->fd >= 0) close(pending->fd);
	if (pending->resource) {
		wl_list_remove(&pending->destroy_listener.link);
	}
	point_clear(&pending->release);
	point_clear(&pending->acquire);
	if (pending->buffer) wld_buffer_unreference(pending->buffer);
	free(pending);
}

static void
queue_remove(struct drm_syncobj_surface *synced, unsigned index)
{
	--synced->queued;
	memmove(&synced->queue[index], &synced->queue[index + 1],
	        (synced->queued - index) * sizeof(synced->queue[0]));
}

/*
 * A buffer that will never be shown, because a newer one replaced it. Nothing
 * sampled it, so both of its releases are owed at once; 'release' is false
 * when its wl_buffer is back in use and must not be released.
 */
static void
queue_drop(struct drm_syncobj_surface *synced, unsigned index, bool release)
{
	struct pending_buffer *pending = synced->queue[index];

	queue_remove(synced, index);
	if (release && pending->resource) {
		wl_buffer_send_release(pending->resource);
	}
	point_signal(&pending->release);
	pending_free(pending);
}

static void
queue_clear(struct drm_syncobj_surface *synced)
{
	while (synced->queued > 0) {
		queue_drop(synced, synced->queued - 1, true);
	}
}

/*
 * Put the buffer at 'index' on screen. Everything older than it is dropped
 * unseen: showing it replaces them anyway.
 */
static void
queue_show(struct drm_syncobj_surface *synced, unsigned index)
{
	struct surface *surface = synced->surface;
	struct pending_buffer *pending = synced->queue[index];
	struct wl_resource *resource = pending->resource;

	while (index-- > 0) {
		queue_drop(synced, 0, true);
	}
	queue_remove(synced, 0);

	shown_release(synced);
	point_set(&synced->release, pending->release.timeline,
	          pending->release.value);
	synced->has_release = true;

	/* The committed buffer's release belongs to the commit that replaces it;
	 * any older one's is ours. */
	if (resource && resource != surface->state.buffer_resource) {
		held_set(synced, resource);
	}

	struct wld_buffer *buffer = pending->buffer;
	wld_buffer_reference(buffer);
	int32_t scale = pending->scale, transform = pending->transform;
	pending_free(pending);
	surface_show_buffer(surface, buffer, scale, transform);
	wld_buffer_unreference(buffer);
}

static unsigned
queue_index(struct drm_syncobj_surface *synced, struct pending_buffer *pending)
{
	unsigned index;

	for (index = 0; synced->queue[index] != pending; ++index) {
	}

	return index;
}

static int
handle_acquire(int fd, uint32_t mask, void *data)
{
	struct pending_buffer *pending = data;
	struct drm_syncobj_surface *synced = pending->synced;

	if (fd_readable(fd)) queue_show(synced, queue_index(synced, pending));
	else if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		if (pending->buffer == synced->surface->state.buffer) synced->failed_acquire = true;
		wl_client_post_implementation_error(wl_resource_get_client(synced->surface->resource), "DRM acquire fence failed");
		queue_drop(synced, queue_index(synced, pending), true);
	}
	return 0;
}

static void
handle_pending_destroy(struct wl_listener *listener, void *data)
{
	struct pending_buffer *pending =
	    wl_container_of(listener, pending, destroy_listener);
	(void)data;
	/* The resource is gone, but its pixels remain committed. The client keeps the old contents on screen. */
	wl_list_remove(&pending->destroy_listener.link);
	pending->resource = NULL;
	/* The committed pixels and timeline points outlive the wl_buffer object. */
}

/*
 * The client attached 'resource' again, so any claim on it from an earlier
 * commit is void: it is neither waiting nor ours to release any more.
 */
static void
forget_resource(struct drm_syncobj_surface *synced,
                struct wl_resource *resource)
{
	unsigned index;

	if (synced->held == resource) {
		held_clear(synced, false);
	}

	for (index = synced->queued; index-- > 0;) {
		if (synced->queue[index]->resource == resource) {
			queue_drop(synced, index, false);
		}
	}
}

static bool
owns_resource(struct drm_syncobj_surface *synced, struct wl_resource *resource)
{
	unsigned index;

	if (synced->held == resource) {
		return true;
	}

	for (index = 0; index < synced->queued; ++index) {
		if (synced->queue[index]->resource == resource) {
			return true;
		}
	}

	return false;
}

/*
 * Whether the renderer may sample the buffer behind this acquire point now.
 *
 * If not, *fd_out is a descriptor that becomes readable when it may, or -1 if
 * there is no way to find out without blocking.
 */
static bool
acquire_ready(const struct syncobj_point *point, int *fd_out)
{
	int fd;

	*fd_out = -1;

	if ((fd = point_export_sync_file(point)) >= 0) {
		/*
		 * Either the client is already done, or the GPU can be told to wait
		 * for it in its own command stream. Where the renderer cannot do that
		 * without blocking, it says no and the fence goes to the event loop.
		 */
		if (fd_readable(fd) || wld_wait_fence(swc.backend->renderer, fd)) {
			close(fd);
			return true;
		}
		*fd_out = fd;
		return false;
	}

	if (errno == EMFILE || errno == ENFILE) {
		static bool warned;

		if (!warned) {
			warned = true;
			WARNING("Could not export a DRM syncobj acquire fence: %s\n",
			        strerror(errno));
		}
		fd_report("could not export a syncobj acquire fence");
		return false;
	}

#ifdef DRM_IOCTL_SYNCOBJ_EVENTFD
	/*
	 * A point the client has not submitted work for yet has no fence to
	 * export. The kernel can still tell us through an eventfd once it both
	 * exists and signals.
	 */
	if ((fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) >= 0) {
		if (drmSyncobjEventfd(swc.drm->fd, point->timeline->handle,
		                      point->value, fd, 0) == 0) {
			*fd_out = fd;
			return false;
		}
		close(fd);
	}
#endif

	return false;
}

static int
poll_acquire(void *data)
{
	struct pending_buffer *pending = data;
	uint32_t handle = pending->acquire.timeline->handle;
	uint64_t value = pending->acquire.value;
	if (drmSyncobjTimelineWait(swc.drm->fd, &handle, &value, 1, 0,
		    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == 0)
		queue_show(pending->synced, queue_index(pending->synced, pending));
	else if (errno == ETIME || errno == EBUSY || errno == EINTR)
		wl_event_source_timer_update(pending->source, 16);
	else {
		struct drm_syncobj_surface *synced = pending->synced;
		if (pending->buffer == synced->surface->state.buffer) synced->failed_acquire = true;
		wl_client_post_implementation_error(wl_resource_get_client(synced->surface->resource), "DRM acquire point wait failed");
		queue_drop(synced, queue_index(synced, pending), true);
	}
	return 0;
}

/*
 * Queue the surface's committed buffer to go up once 'fd' is readable. False,
 * with nothing changed, if the event loop cannot watch it.
 */
static bool
queue_add(struct drm_syncobj_surface *synced, int fd)
{
	struct surface *surface = synced->surface;
	struct pending_buffer *pending;
	struct syncobj_commit *commit = surface->applying_cached ? &synced->cached : &synced->pending;

	if (!(pending = calloc(1, sizeof(*pending)))) {
		return false;
	}

	pending->source = fd >= 0 ?
	    wl_event_loop_add_fd(swc.event_loop, fd, WL_EVENT_READABLE, &handle_acquire, pending) :
	    wl_event_loop_add_timer(swc.event_loop, poll_acquire, pending);
	if (!pending->source) {
		free(pending);
		return false;
	}

	pending->synced = synced;
	pending->fd = fd;
	pending->resource = surface->state.buffer_resource;
	pending->destroy_listener.notify = &handle_pending_destroy;
	if (pending->resource) wl_resource_add_destroy_listener(pending->resource, &pending->destroy_listener);
	pending->buffer = surface->state.buffer;
	wld_buffer_reference(pending->buffer);
	struct surface_pending *state = surface->applying_cached ? &surface->cached : &surface->pending;
	pending->scale = (state->commit & SURFACE_COMMIT_SCALE) ? state->state.buffer_scale : surface->state.buffer_scale;
	pending->transform = (state->commit & SURFACE_COMMIT_TRANSFORM) ? state->state.buffer_transform : surface->state.buffer_transform;
	point_set(&pending->acquire, commit->acquire.timeline, commit->acquire.value);
	if (fd < 0) wl_event_source_timer_update(pending->source, 16);
	point_set(&pending->release, commit->release.timeline,
	          commit->release.value);

	/* The oldest stays put; the newest replaces whatever was queued after. */
	if (synced->queued == MAX_PENDING_BUFFERS) {
		queue_drop(synced, synced->queued - 1, true);
	}
	synced->queue[synced->queued++] = pending;

	return true;
}

/* Put up the newest waiting buffer only if its acquire fence already signalled. */
static void
queue_settle(struct drm_syncobj_surface *synced)
{
	struct pollfd pollfd;

	if (synced->queued == 0) {
		return;
	}

	pollfd.fd = synced->queue[synced->queued - 1]->fd;
	pollfd.events = POLLIN;
	if (pollfd.fd >= 0 && fd_readable(pollfd.fd))
	queue_show(synced, synced->queued - 1);
}

/* }}} */

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct drm_syncobj_surface *synced =
	    wl_container_of(listener, synced, surface_destroy_listener);

	/*
	 * The buffers will never be sampled again, so hand the client back the
	 * release points it is waiting on rather than leaving it stuck.
	 */
	queue_clear(synced);
	shown_release(synced);
	synced->surface->synced = NULL;
	synced->surface = NULL;
	wl_list_remove(&synced->surface_destroy_listener.link);
	wl_list_init(&synced->surface_destroy_listener.link);
	if (!synced->resource) {
		point_clear(&synced->pending.acquire);
		point_clear(&synced->pending.release);
		point_clear(&synced->cached.acquire);
		point_signal(&synced->cached.release);
		free(synced);
	}
}

static void
destroy_syncobj_surface_resource(struct wl_resource *resource)
{
	struct drm_syncobj_surface *synced = wl_resource_get_user_data(resource);
	synced->resource = NULL;
	point_clear(&synced->pending.acquire);
	point_clear(&synced->pending.release);
	memset(&synced->pending, 0, sizeof(synced->pending));
	/* Committed acquire/release obligations survive destruction of this object. */
	if (synced->surface) return;
	point_clear(&synced->cached.acquire);
	point_clear(&synced->cached.release);
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

	if (surface->synced && surface->synced->resource) {
		wl_resource_post_error(
		    resource, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
		    "the surface already has a synchronization object");
		return;
	}

	synced = surface->synced;
	if (!synced && !(synced = calloc(1, sizeof(*synced)))) {
		wl_resource_post_no_memory(resource);
		return;
	}

	synced->resource = wl_resource_create(
	    client, &wp_linux_drm_syncobj_surface_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!synced->resource) {
		if (!synced->surface) free(synced);
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(synced->resource, &syncobj_surface_impl,
	                               synced,
	                               &destroy_syncobj_surface_resource);

	bool newly_created = !synced->surface;
	synced->surface = surface;
	synced->surface_destroy_listener.notify = &handle_surface_destroy;
	if (newly_created) wl_signal_add(&surface->signal.destroy, &synced->surface_destroy_listener);
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
	struct surface_pending *state = surface->applying_cached ? &surface->cached : &surface->pending;
	struct syncobj_commit *commit = synced ? (surface->applying_cached ? &synced->cached : &synced->pending) : NULL;
	bool attached, has_buffer;

	if (!synced || (!synced->resource && !commit->has_acquire && !commit->has_release)) {
		return true;
	}

	attached = state->commit & SURFACE_COMMIT_ATTACH;
	has_buffer = attached && state->state.buffer;

	if (!has_buffer) {
		if (commit->has_acquire || commit->has_release) {
			wl_resource_post_error(
			    synced->resource,
			    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
			    "a timeline point was set without attaching a buffer");
			return false;
		}
		return true;
	}

	if (!commit->has_acquire) {
		wl_resource_post_error(
		    synced->resource,
		    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT,
		    "a buffer was attached without an acquire point");
		return false;
	}

	if (!commit->has_release) {
		wl_resource_post_error(
		    synced->resource,
		    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
		    "a buffer was attached without a release point");
		return false;
	}

	if (commit->acquire.timeline == commit->release.timeline &&
	    commit->acquire.value >= commit->release.value) {
		wl_resource_post_error(
		    synced->resource,
		    WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
		    "the acquire point is not before the release point on the same "
		    "timeline");
		return false;
	}

	return true;
}

bool
drm_syncobj_surface_apply_commit(struct surface *surface, bool attached,
                                 struct wl_resource **replaced)
{
	struct drm_syncobj_surface *synced = surface->synced;
	struct syncobj_commit *commit = synced ? (surface->applying_cached ? &synced->cached : &synced->pending) : NULL;
	int fd;

	if (!synced) {
		return true;
	}

	if (!attached) {
		/* The surface kept its buffer, so its release is still owed. */
		return true;
	}

	synced->failed_acquire = false;
	if (surface->state.buffer_resource) {
		forget_resource(synced, surface->state.buffer_resource);
	}
	/* A buffer still waiting to go up, or still up, is ours to release. */
	if (*replaced && owns_resource(synced, *replaced)) {
		*replaced = NULL;
	}

	if (!commit->has_acquire) {
		/* A NULL buffer was attached; there is nothing left to wait for. */
		queue_clear(synced);
		shown_release(synced);
		point_clear(&commit->release);
		commit->has_release = false;
		return true;
	}

	/*
	 * A buffer the renderer samples on the GPU has to wait for its acquire
	 * point; one that is copied on the CPU came from a client that already
	 * finished writing it.
	 */
	if (surface->state.buffer &&
	    (wld_capabilities(swc.backend->renderer, surface->state.buffer) &
	     WLD_CAPABILITY_READ) &&
	    !acquire_ready(&commit->acquire, &fd)) {
		/* The first frame may wait behind a blank view too. */
		if (queue_add(synced, fd)) {
			/* The replaced buffer stays on screen, so its release waits. */
			if (*replaced &&
			    surface->view && wayland_buffer_get(*replaced) == surface->view->buffer) {
				held_clear(synced, true);
				held_set(synced, *replaced);
				*replaced = NULL;
			}

			point_clear(&commit->acquire);
			commit->has_acquire = false;
			point_clear(&commit->release);
			commit->has_release = false;
			return false;
		}

		if (fd >= 0) {
			close(fd);
		}
		if (!point_wait_cpu(&commit->acquire)) {
			synced->failed_acquire = true;
			wl_resource_post_no_memory(surface->resource);
			point_clear(&commit->acquire); commit->has_acquire = false;
			point_signal(&commit->release); commit->has_release = false;
			return false;
		}
	}

	/* Ready now, so it goes up at once, over anything still waiting. */
	queue_clear(synced);
	shown_release(synced);

	point_clear(&commit->acquire);
	commit->has_acquire = false;

	point_set(&synced->release, commit->release.timeline,
	          commit->release.value);
	synced->has_release = true;
	point_clear(&commit->release);
	commit->has_release = false;

	return true;
}

void
drm_syncobj_surface_cache_commit(struct surface *surface)
{
	struct drm_syncobj_surface *synced = surface->synced;
	if (!synced || !(surface->pending.commit & SURFACE_COMMIT_ATTACH)) return;
	point_clear(&synced->cached.acquire);
	point_signal(&synced->cached.release);
	synced->cached = synced->pending;
	memset(&synced->pending, 0, sizeof(synced->pending));
}

bool
drm_syncobj_surface_buffer_ready(struct surface *surface)
{
	return !surface->synced || (!surface->synced->failed_acquire && surface->synced->queued == 0);
}

void
drm_syncobj_surface_settle(struct surface *surface)
{
	/* Preserve a blank/old image unless its newest committed buffer is ready. */
	if (surface->synced) {
		queue_settle(surface->synced);
	}
}

void
drm_syncobj_surface_finish(struct surface *surface)
{
	struct drm_syncobj_surface *synced = surface->synced;

	if (!synced) {
		return;
	}

	queue_clear(synced);
	shown_release(synced);
	point_clear(&synced->cached.acquire);
	point_signal(&synced->cached.release);
}

void
drm_syncobj_manager_finish(void)
{
	struct deferred_point_release *release, *tmp;
	wl_list_for_each_safe(release, tmp, &deferred_releases, link) {
		if (fd_readable(release->fd)) point_signal(&release->point);
		point_clear(&release->point);
		wl_event_source_remove(release->source);
		close(release->fd);
		wl_list_remove(&release->link);
		free(release);
	}
}

/* }}} */

#endif
