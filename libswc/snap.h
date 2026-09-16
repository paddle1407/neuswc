#ifndef SWC_SNAP_H
#define SWC_SNAP_H

#include <stdint.h>
#include <stdbool.h>

struct screen;
struct swc_rectangle;
struct wld_renderer;
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

void
snap_overlay_cursor_region(uint8_t *, uint32_t width, uint32_t height,
                           uint32_t pitch, struct screen *, int32_t x, int32_t y);
bool
snap_render_cursor(struct wld_renderer *, struct screen *, const struct swc_rectangle *);

struct wl_global *
snap_manager_create(struct wl_display *display);

#endif
