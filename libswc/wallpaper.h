#ifndef SWC_WALLPAPER_H
#define SWC_WALLPAPER_H

#include <pixman.h>

struct screen;
struct wld_renderer;
struct wallpaper_output;

/* Damage is in output-local coordinates. The caller has bound its target. */
void wallpaper_repaint(struct screen *, struct wld_renderer *, pixman_region32_t *);
void wallpaper_screen_finish(struct screen *);
void wallpaper_finalize(void);

#endif
