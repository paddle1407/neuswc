/* swc: libswc/drm_syncobj.h
 *
 * Explicit client synchronization (wp_linux_drm_syncobj_v1).
 */

#ifndef SWC_DRM_SYNCOBJ_H
#define SWC_DRM_SYNCOBJ_H

#include <stdbool.h>

struct surface;
struct wl_display;
struct wl_global;
struct wl_resource;

#ifdef ENABLE_DRM

/**
 * Creates the wp_linux_drm_syncobj_manager_v1 global.
 *
 * Returns NULL when the DRM device or the renderer cannot honor the acquire
 * fences the protocol is built around. Advertising it in that case would be
 * worse than not supporting it: clients are allowed to stop synchronizing
 * implicitly as soon as they see the global.
 */
struct wl_global *drm_syncobj_manager_create(struct wl_display *display);
void drm_syncobj_manager_finish(void);

/**
 * Validates the explicit-synchronization state staged for this commit.
 *
 * Returns false after posting a protocol error, in which case the commit must
 * not be applied.
 */
bool drm_syncobj_surface_check_commit(struct surface *surface);
void drm_syncobj_surface_cache_commit(struct surface *surface);

/**
 * Applies that state: signals the release point of the buffer this commit
 * replaced, and orders rendering after the new buffer's acquire point.
 *
 * 'attached' says whether this commit replaced the surface's buffer, and
 * '*replaced' is the wl_buffer it replaced, which the caller releases unless
 * this sets it to NULL, taking that over.
 *
 * Returns false when the new buffer cannot be sampled yet. The caller must
 * then leave the view showing what it shows; the buffer is put up with
 * surface_show_buffer() once its acquire point signals, without blocking the
 * commit.
 */
bool drm_syncobj_surface_apply_commit(struct surface *surface, bool attached,
                                      struct wl_resource **replaced);

/**
	* Puts up the newest queued buffer only when its acquire fence is ready.
	* Callers must also check surface_buffer_ready before attaching current state.
 */
void drm_syncobj_surface_settle(struct surface *surface);
bool drm_syncobj_surface_buffer_ready(struct surface *surface);

/** Signals any outstanding release point, when a surface is going away. */
void drm_syncobj_surface_finish(struct surface *surface);

#else

static inline struct wl_global *
drm_syncobj_manager_create(struct wl_display *display)
{
	(void)display;
	return NULL;
}

static inline void
drm_syncobj_manager_finish(void)
{
}

static inline bool
drm_syncobj_surface_check_commit(struct surface *surface)
{
	(void)surface;
	return true;
}

static inline void
drm_syncobj_surface_cache_commit(struct surface *surface)
{
	(void)surface;
}

static inline bool
drm_syncobj_surface_apply_commit(struct surface *surface, bool attached,
                                 struct wl_resource **replaced)
{
	(void)surface;
	(void)attached;
	(void)replaced;
	return true;
}

static inline bool
drm_syncobj_surface_buffer_ready(struct surface *surface)
{
	(void)surface;
	return true;
}

static inline void
drm_syncobj_surface_settle(struct surface *surface)
{
	(void)surface;
}

static inline void
drm_syncobj_surface_finish(struct surface *surface)
{
	(void)surface;
}

#endif

#endif
