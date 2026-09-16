/* Solid compositor titlebars: shared geometry for painting and input. */
#include "titlebar.h"
#include "backend.h"
#include "compositor.h"
#include "internal.h"
#include "util.h"
#include <wld/wld.h>

static struct wld_renderer *bar_renderer;

static struct swc_rectangle
bar_geometry(struct compositor_view *view)
{
	struct swc_rectangle r = view->base.geometry;
	r.y -= view->decor.top;
	r.height = view->decor.top;
	return r;
}

struct bar_layout {
	int button_width, button_start;
	int text_start, text_end;
};

/* Both input and painting use these same button cells, including tiny bars. */
static struct bar_layout
bar_layout(struct compositor_view *view)
{
	int width = view->base.geometry.width;
	int count = view->decor.titlebar.count;
	struct bar_layout layout = { .text_end = width };
	if (!count) return layout;
	bool circles = view->decor.titlebar.buttons_style == SWC_TITLEBAR_BUTTONS_CIRCLES;
	int inset = circles ? MIN(6, width / (count + 2)) : 0;
	int cell = circles ? MIN(22, view->decor.top) : view->decor.top;
	layout.button_width = MIN(cell, (width - 2 * inset) / count);
	int group = layout.button_width * count;
	if (view->decor.titlebar.buttons_left) {
		layout.button_start = inset;
		layout.text_start = group + 2 * inset;
	} else {
		layout.button_start = width - inset - group;
		layout.text_end = width - group - 2 * inset;
	}
	return layout;
}

int
titlebar_hit(struct compositor_view *view, int32_t x, int32_t y)
{
	if (!view || !view->window || !view->visible || !view->decor.titlebar.enabled)
		return -2;
	struct swc_rectangle r = bar_geometry(view);
	if (!rectangle_contains_point(&r, x, y)) return -2;
	struct bar_layout layout = bar_layout(view);
	int offset = x - r.x - layout.button_start;
	return layout.button_width && offset >= 0 &&
	       offset < layout.button_width * (int)view->decor.titlebar.count ?
	       offset / layout.button_width : -1;
}

void
titlebar_highlight(struct compositor_view *view, int hover, int pressed)
{
	if (view->decor.hover_button == hover && view->decor.pressed_button == pressed)
		return;
	view->decor.hover_button = hover;
	view->decor.pressed_button = pressed;
	view->decor.bar_dirty = true;
	compositor_view_damage_decor(view);
}

void
titlebar_finish(struct compositor_view *view)
{
	if (view->decor.bar_buffer) wld_buffer_unreference(view->decor.bar_buffer);
	view->decor.bar_buffer = NULL;
	view->decor.bar_dirty = true;
}

void
titlebar_finalize(void)
{
	if (bar_renderer) wld_destroy_renderer(bar_renderer);
	bar_renderer = NULL;
}

static void
button_icon(struct wld_renderer *renderer, enum swc_titlebar_action action,
            uint32_t color, int x, int y, int size)
{
	if (size < 3) return;
	switch (action) {
	case SWC_TITLEBAR_MINIMIZE:
		wld_fill_rectangle(renderer, color, x, y + size - 2, size, 2);
		break;
	case SWC_TITLEBAR_FULLSCREEN:
		wld_fill_rectangle(renderer, color, x, y, size, 1);
		wld_fill_rectangle(renderer, color, x, y + size - 1, size, 1);
		wld_fill_rectangle(renderer, color, x, y, 1, size);
		wld_fill_rectangle(renderer, color, x + size - 1, y, 1, size);
		break;
	case SWC_TITLEBAR_CLOSE:
		for (int i = 0; i < size; ++i) {
			wld_fill_rectangle(renderer, color, x + i, y + i, 1, 1);
			wld_fill_rectangle(renderer, color, x + size - i - 1, y + i, 1, 1);
		}
		break;
	default: break;
	}
}

static uint32_t
mix_color(uint32_t a, uint32_t b, unsigned weight)
{
	uint32_t color = 0xff000000;
	for (unsigned shift = 0; shift < 24; shift += 8) {
		unsigned channel = (((a >> shift) & 255) * (256 - weight) +
		                    ((b >> shift) & 255) * weight + 128) / 256;
		color |= channel << shift;
	}
	return color;
}

/* Supersample just these small cached decorations. Opaque edge pixels are
 * blended against the bar color, so this works on every wld backend. Runs of
 * equal pixels share one fill instead of issuing a draw for every pixel. */
static void
circle_button(struct wld_renderer *renderer, int x, int y, int diameter,
              uint32_t color, uint32_t background)
{
	if (diameter < 3) return;
	int radius = diameter * 4, inner = radius - 5;
	uint32_t rim = mix_color(color, 0xff000000, 45);
	for (int py = 0; py < diameter; ++py) {
		int start = 0;
		uint32_t previous = background;
		for (int px = 0; px <= diameter; ++px) {
			uint32_t pixel = background;
			if (px < diameter) {
				unsigned coverage = 0, fill = 0;
				for (int sy = 1; sy < 8; sy += 2) {
					for (int sx = 1; sx < 8; sx += 2) {
						int dx = px * 8 + sx - radius, dy = py * 8 + sy - radius;
						int distance = dx * dx + dy * dy;
						coverage += distance <= radius * radius;
						fill += distance <= inner * inner;
					}
				}
				if (coverage)
					pixel = mix_color(background, mix_color(rim, color, fill * 256 / coverage),
					                  coverage * 16);
			}
			if (px && (pixel != previous || px == diameter)) {
				if (previous != background)
					wld_fill_rectangle(renderer, previous, x + start, y + py, px - start, 1);
				start = px;
			}
			previous = pixel;
		}
	}
}

static void
circle_icon(struct wld_renderer *renderer, enum swc_titlebar_action action,
            uint32_t color, int x, int y, int diameter)
{
	if (diameter < 10) return;
	int cx = x + diameter / 2, cy = y + diameter / 2;
	if (action == SWC_TITLEBAR_MINIMIZE)
		wld_fill_rectangle(renderer, color, cx - 3, cy, 6, 1);
	else if (action == SWC_TITLEBAR_FULLSCREEN) {
		/* A compact plus keeps the green control legible at twelve pixels. */
		wld_fill_rectangle(renderer, color, cx - 3, cy, 6, 1);
		wld_fill_rectangle(renderer, color, cx, cy - 3, 1, 6);
	} else if (action == SWC_TITLEBAR_CLOSE)
		button_icon(renderer, action, color, cx - 3, cy - 3, 6);
}

/*
 * Painting the bar can only fail through the renderer, and when it does the
 * caller falls back to a flat rectangle in the bar colour: the titlebar still
 * reserves its space, but the title and the buttons are simply absent. That
 * looks like a layout bug rather than an allocation failure, so say which step
 * failed instead of degrading in silence.
 */
static bool
paint_failed(const char *step)
{
	static const char *last;

	if (last != step) {
		last = step;
		WARNING("Titlebar contents could not be painted (%s); the bar will be "
		        "drawn as a plain rectangle\n", step);
		fd_report("titlebar painting failed");
	}
	return false;
}

static bool
paint_buffer(struct compositor_view *view, uint32_t width, uint32_t height)
{
	if (!bar_renderer && !(bar_renderer = wld_create_renderer(swc.backend->context)))
		return paint_failed("no renderer");
	if (view->decor.bar_buffer && (view->decor.bar_buffer->width != width ||
	    view->decor.bar_buffer->height != height)) titlebar_finish(view);
	if (!view->decor.bar_buffer)
		view->decor.bar_buffer = wld_create_buffer(swc.backend->context, width, height,
		                                          WLD_FORMAT_XRGB8888, 0);
	if (!view->decor.bar_buffer) return paint_failed("no buffer");
	if (!view->decor.bar_dirty) return true;
	if (!wld_set_target_buffer(bar_renderer, view->decor.bar_buffer))
		return paint_failed("buffer is not a render target");
	wld_fill_rectangle(bar_renderer, view->decor.color, 0, 0, width, height);

	const struct swc_decor_text *text = &view->decor.text;
	struct bar_layout layout = bar_layout(view);
	const struct swc_titlebar *bar = &view->decor.titlebar;
	int bw = layout.button_width;
	uint32_t available = layout.text_end - layout.text_start;
	struct wld_font *font = view->decor.font;
	const char *title = view->decor.string;
	if (text->enabled && font && title && available > 2 * text->padding && height >= font->height) {
		uint32_t len = 0;
		struct wld_extents ext;
		while (title[len]) {
			uint32_t next = len + 1;
			while ((title[next] & 0xc0) == 0x80) ++next;
			wld_font_text_extents_n(font, title, next, &ext);
			if (ext.advance > available - 2 * text->padding) break;
			len = next;
		}
		if (len) {
			wld_font_text_extents_n(font, title, len, &ext);
			int left = layout.text_start + text->padding;
			int right = layout.text_end - text->padding - ext.advance;
			int x = left;
			if (text->align == SWC_DECOR_ALIGN_CENTER)
				x = MAX(left, MIN(right, ((int)width - (int)ext.advance) / 2));
			if (text->align == SWC_DECOR_ALIGN_END) x = right;
			wld_draw_text(bar_renderer, font, text->color, x,
			              (height - font->height) / 2 + font->ascent, title, len, NULL);
		}
	}
	/* Fill every button after the text, so glyph overhang never covers it. */
	for (uint32_t i = 0; bw && i < bar->count; ++i) {
		int x = layout.button_start + i * bw;
		bool hover = (int)i == view->decor.hover_button;
		bool pressed = hover && (int)i == view->decor.pressed_button;
		if (bar->buttons_style == SWC_TITLEBAR_BUTTONS_CIRCLES) {
			uint32_t color = bar->buttons[i] == SWC_TITLEBAR_CLOSE ? bar->close_color :
			                 bar->buttons[i] == SWC_TITLEBAR_MINIMIZE ? bar->minimize_color :
			                 bar->fullscreen_color;
			if (pressed) color = mix_color(color, 0xff000000, 50);
			else if (hover) color = mix_color(color, 0xffffffff, 25);
			wld_fill_rectangle(bar_renderer, view->decor.color, x, 0, bw, height);
			int diameter = MIN(12, MIN(bw, (int)height) - 6);
			int cx = x + (bw - diameter) / 2, cy = ((int)height - diameter) / 2;
			circle_button(bar_renderer, cx, cy, diameter, color, view->decor.color);
			if (hover)
				circle_icon(bar_renderer, bar->buttons[i], mix_color(color, 0xff000000, 190),
				            cx, cy, diameter);
			continue;
		}
		uint32_t color = view->decor.color;
		if (hover) {
			color = pressed ? bar->pressed_color : bar->hover_color;
			if (!color) color = mix_color(view->decor.color, text->color, pressed ? 64 : 32);
		}
		wld_fill_rectangle(bar_renderer, color, x, 0, bw, height);
		int size = MIN(10, MIN(bw, height) / 2);
		button_icon(bar_renderer, view->decor.titlebar.buttons[i], text->color,
		            x + (bw - size) / 2, (height - size) / 2, size);
	}
	wld_flush(bar_renderer);
	wld_set_target_buffer(bar_renderer, NULL);
	view->decor.bar_dirty = false;
	return true;
}

void
titlebar_repaint(struct wld_renderer *renderer, const struct swc_rectangle *target,
                 struct compositor_view *view, pixman_region32_t *damage)
{
	struct swc_rectangle r = bar_geometry(view);
	if (!r.width || !r.height) return;
	pixman_region32_t region;
	pixman_region32_init_rect(&region, r.x, r.y, r.width, r.height);
	pixman_region32_intersect(&region, &region, damage);
	pixman_region32_subtract(&region, &region, &view->clip);
	if (pixman_region32_not_empty(&region)) {
		bool painted = paint_buffer(view, r.width, r.height);
		/* GBM renderers share GL state. Restore the scene FBO and viewport
		 * after painting the cache, including allocation/failure paths. */
		if (!wld_set_target_buffer(renderer, renderer->target)) {
			pixman_region32_fini(&region);
			return;
		}
		if (painted) {
			pixman_region32_translate(&region, -r.x, -r.y);
			wld_copy_region(renderer, view->decor.bar_buffer, r.x - target->x, r.y - target->y, &region);
		} else {
			pixman_region32_translate(&region, -target->x, -target->y);
			wld_fill_region(renderer, view->decor.color, &region);
		}
	}
	pixman_region32_fini(&region);
}

bool
titlebar_prepare(struct compositor_view *view)
{
	return !view->decor.titlebar.enabled || !view->base.geometry.width ||
	       !view->decor.top || paint_buffer(view, view->base.geometry.width, view->decor.top);
}
