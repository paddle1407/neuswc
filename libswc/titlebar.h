#ifndef SWC_TITLEBAR_H
#define SWC_TITLEBAR_H
#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>
struct compositor_view;
struct swc_rectangle;
struct wld_renderer;
struct wld_buffer;
/* -2: outside, -1: draggable title, otherwise button index. */
int titlebar_hit(struct compositor_view *, int32_t x, int32_t y);
void titlebar_highlight(struct compositor_view *, int hover, int pressed);
void titlebar_finish(struct compositor_view *);
void titlebar_finalize(void);
bool titlebar_prepare(struct compositor_view *);
/* The bar's cached pixels and the rectangle they belong at, in global
 * coordinates, for a caller that draws the bar somewhere other than where it
 * lives. NULL when there is nothing cached to draw. */
struct wld_buffer *titlebar_content(struct compositor_view *,
                                    struct swc_rectangle *);
void titlebar_repaint(struct wld_renderer *, const struct swc_rectangle *,
                     struct compositor_view *, pixman_region32_t *);
#endif
