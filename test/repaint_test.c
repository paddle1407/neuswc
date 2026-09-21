/* Exercise the damage, clipping and repaint path with pixman, without a DRM
 * device: the live per-frame renderer, the capture renderer, and hit testing
 * all have to agree about where a view and its border are. */
#include "../libswc/compositor.c"
#include <wld/pixman.h>
struct swc swc;
static struct swc_backend backend;
static const uint32_t wallpaper = 0xff101010, bar = 0xff8899aa;
void wallpaper_repaint(struct screen *s, struct wld_renderer *r, pixman_region32_t *region)
{ wld_fill_region(r, wallpaper, region); }
/* The real titlebar paints a cached buffer; a flat fill has the same extent. */
void decor_repaint(struct wld_renderer *r, const struct swc_rectangle *target,
                   struct compositor_view *v, pixman_region32_t *damage)
{
	pixman_region32_t region;
	pixman_region32_init_rect(&region, v->base.geometry.x, v->base.geometry.y - v->decor.top,
	                          v->base.geometry.width, v->decor.top);
	pixman_region32_intersect(&region, &region, damage);
	pixman_region32_subtract(&region, &region, &v->clip);
	pixman_region32_translate(&region, -target->x, -target->y);
	wld_fill_region(r, bar, &region);
	pixman_region32_fini(&region);
}
int titlebar_hit(struct compositor_view *v, int32_t x, int32_t y)
{
	if (!v || !v->window || !v->visible || !v->decor.titlebar.enabled) return -2;
	struct swc_rectangle r = v->base.geometry;
	r.y -= v->decor.top; r.height = v->decor.top;
	return rectangle_contains_point(&r, x, y) ? -1 : -2;
}

static struct wld_buffer *buffer(unsigned w, unsigned h, uint32_t format, uint32_t color)
{
	struct wld_buffer *b = wld_create_buffer(backend.context, w, h, format, 0);
	assert(b && wld_set_target_buffer(backend.renderer, b));
	wld_fill_rectangle(backend.renderer, color, 0, 0, w, h);
	wld_flush(backend.renderer);
	return b;
}
static uint32_t pixel(struct wld_buffer *b, unsigned x, unsigned y)
{
	assert(wld_map(b));
	uint32_t p = ((uint32_t *)b->map)[y * b->pitch / 4 + x] | 0xff000000;
	wld_unmap(b);
	return p;
}
static struct surface surfaces[6];
static struct compositor_view views[6];
static struct window windows[6];
static struct compositor_view *add(unsigned i, bool window, struct swc_rectangle g,
                                   struct wld_buffer *b, int32_t ox, int32_t oy)
{
	struct surface *s = &surfaces[i];
	struct compositor_view *v = &views[i];
	pixman_region32_init(&s->state.damage);
	pixman_region32_init(&s->state.opaque);
	pixman_region32_init_rect(&s->state.input, 0, 0, b->width, b->height);
	wl_list_init(&s->state.frame_callbacks);
	v->surface = s;
	v->window = window ? &windows[i] : NULL;
	v->base.geometry = g;
	v->base.buffer = v->buffer = b;
	v->buffer_offset_x = ox; v->buffer_offset_y = oy;
	v->visible = true;
	v->decor.hover_button = v->decor.pressed_button = -1;
	pixman_region32_init(&v->clip);
	/* Inserted at the head: each view added is above the previous ones. */
	wl_list_insert(&compositor.views, &v->link);
	return v;
}

static void live(struct target *t, pixman_region32_t *damage)
{
	struct compositor_view *v;
	pixman_region32_t base;
	const struct swc_rectangle *g = &t->view->geometry;
	pixman_region32_init(&base);
	pixman_region32_subtract(&base, damage, &compositor.opaque);
	pixman_region32_translate(&base, -g->x, -g->y);
	wld_fill_region(backend.renderer, wallpaper, &base);
	pixman_region32_fini(&base);
	wl_list_for_each_reverse(v, &compositor.views, link)
		if (v->visible) repaint_view(t, v, damage);
	wld_flush(backend.renderer);
}

int main(void)
{
	backend.context = wld_pixman_context;
	backend.renderer = wld_create_renderer(backend.context);
	assert(backend.renderer);
	swc.backend = &backend;
	wl_list_init(&compositor.views);
	pixman_region32_init(&compositor.damage);
	pixman_region32_init(&compositor.opaque);

	struct screen screen = { .base.geometry = {10, 20, 120, 90} };
	struct view target_view = { .geometry = screen.base.geometry };
	struct target target = { .view = &target_view };

	/* Bottom: a bordered window with a titlebar. */
	struct compositor_view *a = add(0, true, (struct swc_rectangle){30, 50, 40, 30},
	    buffer(40, 30, WLD_FORMAT_XRGB8888, 0xffff0000), 0, 0);
	a->border.outwidth = 3; a->border.outcolor = 0xff00ff00;
	a->border.inwidth = 2; a->border.incolor = 0xff0000ff;
	a->decor.titlebar.enabled = true; a->decor.top = 6;
	/* A translucent popup with a shadow margin and a half-opaque buffer. */
	struct compositor_view *b = add(1, false, (struct swc_rectangle){60, 70, 26, 14},
	    buffer(30, 20, WLD_FORMAT_ARGB8888, 0x80000080), 2, 3);
	b->border.outwidth = 1; b->border.outcolor = 0xffffff00;
	pixman_region32_union_rect(&b->surface->state.opaque, &b->surface->state.opaque, 0, 0, 15, 20);
	/* Wholly covered by the next window: skipped, and must stay invisible. */
	add(4, true, (struct swc_rectangle){40, 64, 8, 8},
	    buffer(8, 8, WLD_FORMAT_XRGB8888, 0xff00ff00), 0, 0);
	/* An opaque window covering part of the first. */
	struct compositor_view *c = add(2, true, (struct swc_rectangle){36, 60, 20, 20},
	    buffer(20, 20, WLD_FORMAT_XRGB8888, 0xffffffff), 0, 0);
	pixman_region32_union_rect(&c->surface->state.opaque, &c->surface->state.opaque, 0, 0, 20, 20);
	/* Hidden: must not be drawn or hit. */
	struct compositor_view *d = add(3, true, (struct swc_rectangle){15, 25, 100, 80},
	    buffer(100, 80, WLD_FORMAT_XRGB8888, 0xff00ffff), 0, 0);
	d->visible = false;
	struct compositor_view *v;
	wl_list_for_each(v, &compositor.views, link) update_extents(v);

	pixman_region32_union_rect(&compositor.damage, &compositor.damage, 10, 20, 120, 90);
	calculate_damage();
	/* A view's clip only ever matters inside its own extents. */
	pixman_box32_t *clip = pixman_region32_extents(&a->clip);
	assert(clip->x1 >= a->extents.x1 && clip->x2 <= a->extents.x2 &&
	       clip->y1 >= a->extents.y1 && clip->y2 <= a->extents.y2);
	assert(pixman_region32_contains_rectangle(&a->clip,
	    &(pixman_box32_t){36, 60, 56, 80}) == PIXMAN_REGION_IN);

	struct wld_buffer *frame = buffer(120, 90, WLD_FORMAT_XRGB8888, 0xffff00ff);
	struct wld_buffer *capture = buffer(120, 90, WLD_FORMAT_XRGB8888, 0xffff00ff);
	pixman_region32_t full, local, part;
	pixman_region32_init_rect(&full, 10, 20, 120, 90);
	pixman_region32_init_rect(&local, 0, 0, 120, 90);
	assert(wld_set_target_buffer(backend.renderer, frame));
	live(&target, &full);
	/* Local = global - (10, 20). */
	assert(pixel(frame, 30, 30) == 0xffff0000);  /* content */
	assert(pixel(frame, 30, 26) == bar);         /* titlebar */
	assert(pixel(frame, 18, 26) == 0xff0000ff);  /* inner border, beside the bar */
	assert(pixel(frame, 16, 26) == 0xff00ff00);  /* outer border */
	assert(pixel(frame, 30, 23) == 0xff0000ff);  /* inner border, above the bar */
	assert(pixel(frame, 30, 20) == 0xff00ff00);  /* outer border, above the bar */
	assert(pixel(frame, 14, 26) == wallpaper);   /* outside the border */
	assert(pixel(frame, 30, 45) == 0xffffffff);  /* opaque window above */
	assert(pixel(frame, 49, 48) == 0xff000080);  /* opaque half of the popup is copied */
	assert(pixel(frame, 70, 50) == 0xff080888);  /* the rest is blended */
	assert(pixel(frame, 48, 47) == 0xff000080);  /* shadow margin belongs to the buffer */
	assert(pixel(frame, 5, 5) == wallpaper);     /* hidden view */

	/* A capture draws the same scene from scratch; it must match. */
	assert(wld_set_target_buffer(backend.renderer, capture));
	render_scene(&screen, backend.renderer, &local, &full);
	wld_flush(backend.renderer);
	for (unsigned y = 0; y < 90; ++y)
		for (unsigned x = 0; x < 120; ++x)
			if (pixel(frame, x, y) != pixel(capture, x, y)) {
				fprintf(stderr, "capture differs at %u,%u: %08x != %08x\n",
				        x, y, pixel(capture, x, y), pixel(frame, x, y));
				return 1;
			}

	/* Partial damage leaves everything outside it alone. */
	assert(wld_set_target_buffer(backend.renderer, frame));
	wld_fill_rectangle(backend.renderer, 0xffff00ff, 0, 0, 120, 90);
	pixman_region32_init_rect(&part, 24, 40, 20, 20);
	pixman_region32_union_rect(&part, &part, 90, 90, 10, 10);
	live(&target, &part);
	assert(pixel(frame, 16, 25) == 0xff00ff00);  /* outer border, damaged */
	assert(pixel(frame, 22, 21) == 0xff00ff00);
	assert(pixel(frame, 22, 26) == bar);
	assert(pixel(frame, 13, 19) == 0xffff00ff);  /* outside the damage */
	assert(pixel(frame, 85, 75) == wallpaper);   /* damaged, nothing there */

	/* Hit testing: topmost view whose input region, or titlebar, is hit. */
	assert(view_at(40, 65) == c);
	assert(view_at(32, 52) == a);
	assert(view_at(32, 46) == a);              /* titlebar */
	assert(view_at(59, 69) == b);              /* shadow margin is buffer input */
	assert(view_at(28, 50) == NULL);           /* border takes no input */
	assert(view_at(12, 22) == NULL);           /* hidden view */

	puts("repaint: clipping, borders, titlebars, blending, damage, capture and hit testing passed");
	return 0;
}
