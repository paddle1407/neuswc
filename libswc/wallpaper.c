#include "session_lock.h"
#include "wallpaper.h"
#include "backend.h"
#include "compositor.h"
#include "internal.h"
#include "screen.h"
#include "shm.h"
#include "swc.h"
#include "util.h"

#include <stdlib.h>
#include <wld/wld.h>

struct wallpaper_output {
	struct screen *screen;
	struct wld_buffer *cpu, *native;
	struct wl_list link;
};

struct swc_prepared_wallpaper {
	uint32_t background;
	struct wl_list outputs;
};

static struct swc_prepared_wallpaper *active;

static void
output_destroy(struct wallpaper_output *output)
{
	if (output->screen->wallpaper == output)
		output->screen->wallpaper = NULL;
	wl_list_remove(&output->link);
	if (output->native) wld_buffer_unreference(output->native);
	if (output->cpu) wld_buffer_unreference(output->cpu);
	free(output);
}

EXPORT void
swc_wallpaper_discard(struct swc_prepared_wallpaper *prepared)
{
	if (!prepared) return;
	struct wallpaper_output *output, *next;
	wl_list_for_each_safe(output, next, &prepared->outputs, link)
		output_destroy(output);
	free(prepared);
}

/* Scaling/compositing happens once per output at preparation time. The cached
 * result is opaque, so ordinary repaint only copies the exposed damage. */
static bool
scale_image(struct wld_buffer *buffer, pixman_image_t *source,
            uint32_t source_width, uint32_t source_height,
            enum swc_wallpaper_mode mode, uint32_t background)
{
	if (!wld_map(buffer)) return false;
	pixman_image_t *dest = pixman_image_create_bits(PIXMAN_x8r8g8b8,
	    buffer->width, buffer->height, buffer->map, buffer->pitch);
	if (!dest) { wld_unmap(buffer); return false; }
	pixman_color_t color = {
		.red = ((background >> 16) & 255) * 257,
		.green = ((background >> 8) & 255) * 257,
		.blue = (background & 255) * 257, .alpha = 65535,
	};
	pixman_box32_t full = { 0, 0, buffer->width, buffer->height };
	bool ok = pixman_image_fill_boxes(PIXMAN_OP_SRC, dest, &color, 1, &full);
	if (!ok) goto done;
	int32_t width = buffer->width, height = buffer->height;
	int32_t dx = 0, dy = 0;
	double sx = 1, sy = 1, tx = 0, ty = 0;
	if (mode == SWC_WALLPAPER_CENTER) {
		width = source_width; height = source_height;
		dx = ((int32_t)buffer->width - width) / 2;
		dy = ((int32_t)buffer->height - height) / 2;
	} else if (mode == SWC_WALLPAPER_FIT) {
		if ((uint64_t)source_width * height > (uint64_t)source_height * width)
			height = MAX(1, ((uint64_t)source_height * width + source_width / 2) / source_width);
		else
			width = MAX(1, ((uint64_t)source_width * height + source_height / 2) / source_height);
		dx = ((int32_t)buffer->width - width) / 2;
		dy = ((int32_t)buffer->height - height) / 2;
		sx = (double)source_width / width;
		sy = (double)source_height / height;
	} else {
		sx = sy = MIN((double)source_width / width, (double)source_height / height);
		tx = (source_width - width * sx) / 2;
		ty = (source_height - height * sy) / 2;
	}
	pixman_transform_t transform = {{{
		pixman_double_to_fixed(sx), 0, pixman_double_to_fixed(tx)
	}, {
		0, pixman_double_to_fixed(sy), pixman_double_to_fixed(ty)
	}, { 0, 0, pixman_fixed_1 }}};
	ok = pixman_image_set_transform(source, &transform) &&
	     pixman_image_set_filter(source, PIXMAN_FILTER_BILINEAR, NULL, 0);
	if (!ok) goto done;
	/* Clamp sampling at image edges; the destination rectangle still bounds fit
	 * and center, so padding remains the configured background color. */
	pixman_image_set_repeat(source, PIXMAN_REPEAT_PAD);
	pixman_image_composite32(PIXMAN_OP_OVER, source, NULL, dest,
	                         0, 0, 0, 0, dx, dy, width, height);
done:
	pixman_image_unref(dest);
	if (!wld_unmap(buffer)) ok = false;
	return ok;
}

EXPORT struct swc_prepared_wallpaper *
swc_wallpaper_prepare(const uint32_t *pixels, uint32_t width, uint32_t height,
                      enum swc_wallpaper_mode mode, uint32_t background)
{
	if (mode < SWC_WALLPAPER_FILL || mode > SWC_WALLPAPER_CENTER ||
	    (pixels && (!width || !height || width > 8192 || height > 8192 ||
	                (uint64_t)width * height * 4 > 64 * 1024 * 1024)))
		return NULL;
	struct swc_prepared_wallpaper *prepared = calloc(1, sizeof(*prepared));
	if (!prepared) return NULL;
	prepared->background = background | 0xff000000;
	wl_list_init(&prepared->outputs);
	if (!pixels) return prepared;

	pixman_image_t *source = pixman_image_create_bits(PIXMAN_a8r8g8b8,
	    width, height, (uint32_t *)pixels, width * 4);
	/* A separate renderer keeps staging from changing the live renderer target. */
	struct wld_renderer *renderer = wld_create_renderer(swc.backend->context);
	bool ok = false;
	if (!source || !renderer) goto done;
	uint64_t bytes = 0;
	struct screen *screen;
	wl_list_for_each(screen, &swc.screens, link) {
		uint32_t w = screen->base.geometry.width, h = screen->base.geometry.height;
		struct wallpaper_output *output = calloc(1, sizeof(*output));
		if (!output) goto done;
		output->screen = screen;
		struct wallpaper_output *other;
		wl_list_for_each(other, &prepared->outputs, link) {
			if (other->native->width != w || other->native->height != h) continue;
			output->cpu = other->cpu; output->native = other->native;
			wld_buffer_reference(output->cpu);
			wld_buffer_reference(output->native);
			break;
		}
		wl_list_insert(prepared->outputs.prev, &output->link);
		/* Equal-sized monitors can share these immutable buffers. */
		if (output->native) continue;
		/* Include the CPU cache, its upload texture, and native output buffer.
		 * This bounds nominal pixel storage; drivers can add padding/metadata. */
		bytes += (uint64_t)w * h * 12;
		if (!w || !h || w > 8192 || h > 8192 || bytes > 256 * 1024 * 1024) {
			ERROR("wallpaper: output dimensions/cache budget exceeded\n");
			goto done;
		}
		/* Use the backend's CPU storage so its renderer can upload it directly.
		 * The software renderer also reads this mapping for legacy capture/zoom. */
		output->cpu = wld_create_buffer(swc.backend->context, w, h,
		                               WLD_FORMAT_XRGB8888, WLD_FLAG_MAP);
		if (!output->cpu || !scale_image(output->cpu, source, width, height,
		                                mode, prepared->background)) goto done;
		output->native = wld_create_buffer(swc.backend->context, w, h,
		                                  WLD_FORMAT_XRGB8888, 0);
		if (!output->native ||
		    !(wld_capabilities(renderer, output->cpu) & WLD_CAPABILITY_READ) ||
		    /* Materialize and validate the upload texture before the copy. No
		     * GPU writes go to the CPU-backed buffer. */
		    !wld_set_target_buffer(renderer, output->cpu) ||
		    !wld_set_target_buffer(renderer, output->native)) goto done;
		wld_copy_rectangle(renderer, output->cpu, 0, 0, 0, 0, w, h);
		wld_flush(renderer);
	}
	ok = true;
done:
	if (renderer) {
		wld_set_target_buffer(renderer, NULL);
		wld_destroy_renderer(renderer);
	}
	if (source) pixman_image_unref(source);
	if (!ok) { swc_wallpaper_discard(prepared); return NULL; }
	return prepared;
}

EXPORT void
swc_wallpaper_commit(struct swc_prepared_wallpaper *prepared)
{
	if (!prepared) return;
	swc_wallpaper_discard(active);
	active = prepared;
	struct wallpaper_output *output;
	wl_list_for_each(output, &active->outputs, link)
		output->screen->wallpaper = output;
	compositor_damage_all();
}

void
wallpaper_repaint(struct screen *screen, struct wld_renderer *renderer,
                  pixman_region32_t *damage)
{
	struct wallpaper_output *output = screen->wallpaper;

	/* A locked session shows nothing of the desktop, the wallpaper least of
	 * all: it is the one thing guaranteed to be under every window. */
	if (session_lock_active()) {
		wld_fill_region(renderer, 0xff000000, damage);
		return;
	}

	if (output && output->native->width == screen->base.geometry.width &&
	    output->native->height == screen->base.geometry.height) {
		struct wld_buffer *buffer = renderer == swc.shm->renderer ?
		    output->cpu : output->native;
		wld_copy_region(renderer, buffer, 0, 0, damage);
	} else {
		wld_fill_region(renderer, active ? active->background : 0xff000000, damage);
	}
}

void
wallpaper_screen_finish(struct screen *screen)
{
	if (screen->wallpaper) output_destroy(screen->wallpaper);
}

void
wallpaper_finalize(void)
{
	swc_wallpaper_discard(active);
	active = NULL;
}
