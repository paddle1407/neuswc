/* swc: libswc/screencopy.h */

#ifndef SWC_SCREENCOPY_H
#define SWC_SCREENCOPY_H

#include <pixman.h>
#include <stdint.h>

struct wl_display;
struct wl_global;
struct screen;

struct wl_global *
screencopy_manager_create(struct wl_display *display);

/** Complete queued copies intersecting this successfully repainted output.
 * Damage uses global coordinates and includes accumulated page-flip damage. */
void
screencopy_handle_damage(struct screen *screen, pixman_region32_t *damage);

void
screencopy_cursor_changed(uint32_t screens);
void
screencopy_finalize(void);

#endif
