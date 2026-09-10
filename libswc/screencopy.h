/* swc: libswc/screencopy.h */

#ifndef SWC_SCREENCOPY_H
#define SWC_SCREENCOPY_H

#include <pixman.h>

struct wl_display;
struct wl_global;

struct wl_global *
screencopy_manager_create(struct wl_display *display);

/**
 * Complete any frames queued by copy_with_damage that intersect 'damage'.
 *
 * 'damage' is in global compositor coordinates and must still be valid, i.e.
 * this is called from perform_update() after the screens have been painted but
 * before the accumulated damage is cleared.
 */
void
screencopy_handle_damage(pixman_region32_t *damage);

#endif
