#ifndef SWC_SNAP_H
#define SWC_SNAP_H

#include <stdint.h>

struct screen;
struct wl_display;
struct wl_global;

/**
 * Composite the current cursor into an ARGB8888 capture of 'screen'.
 *
 * Shared with screencopy.c so both capture paths blend the cursor identically.
 */
void
snap_overlay_cursor(uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
                    uint32_t dst_pitch, struct screen *screen);

struct wl_global *
snap_manager_create(struct wl_display *display);

#endif
