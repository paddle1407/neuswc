/* Exercise the actual overview renderer with pixman, without a DRM device.
 * Unused compositor sections are discarded by the linker. */
#include "../libswc/compositor.c"
#include <wld/pixman.h>
#include <fcntl.h>
static struct wld_context *test_context;
static struct wld_renderer *test_renderer;
static bool gpu;
struct swc swc;
void wallpaper_repaint(struct screen *s, struct wld_renderer *r, pixman_region32_t *region)
{ wld_fill_rectangle(r, 0xff101010, 0, 0, s->base.geometry.width, s->base.geometry.height); }
struct wld_buffer *titlebar_content(struct compositor_view *v, struct swc_rectangle *r)
{
	*r = v->base.geometry;
	r->y -= v->decor.top; r->height = v->decor.top;
	return v->decor.bar_buffer;
}
static struct wld_buffer *buffer(unsigned w, unsigned h, uint32_t color)
{
	struct wld_buffer *b = wld_create_buffer(test_context, w, h, WLD_FORMAT_ARGB8888, 0);
	assert(b && wld_set_target_buffer(test_renderer, b));
	wld_fill_rectangle(test_renderer, color, 0, 0, w, h);
	wld_flush(test_renderer);
	return b;
}
static uint32_t pixel(struct wld_buffer *b, unsigned x, unsigned y)
{
	uint32_t p;
	if (!gpu) {
		assert(wld_map(b));
		p = ((uint32_t *)b->map)[y*b->pitch/4+x];
		wld_unmap(b);
		return p;
	}
	assert(wld_set_target_buffer(test_renderer,b));
	assert(wld_read_pixels(test_renderer,x,y,1,1,4,&p));
	return p;
}
static struct wld_buffer *imported(struct wld_buffer *b)
{
	if (!gpu) return b;
	union wld_object fd, modifier;
	assert(wld_export(b,WLD_DRM_OBJECT_PRIME_FD,&fd));
	assert(wld_export(b,WLD_DRM_OBJECT_MODIFIER,&modifier));
	struct wld_dmabuf_attributes attributes = { .fd=fd.i, .pitch=b->pitch, .modifier=modifier.u64 };
	struct wld_buffer *copy = wld_import_buffer(test_context,WLD_DRM_OBJECT_DMABUF,
	    (union wld_object){.ptr=&attributes}, b->width,b->height,b->format,b->pitch);
	close(fd.i);
	assert(copy && !wld_map(copy)); /* A real imported, non-CPU-readable client buffer. */
	wld_buffer_unreference(b);
	return copy;
}
static void check_gpu_sampling(void)
{
	struct wld_buffer *src = buffer(16,16,0), *dst = buffer(48,48,0);
	assert(wld_set_target_buffer(test_renderer,src));
	for (unsigned y=0; y<16; ++y) for (unsigned x=0; x<16; ++x)
		wld_fill_rectangle(test_renderer,0xff000000 | (x*8 << 16) | (y*8 << 8),x,y,1,1);
	wld_flush(test_renderer);
	src = imported(src);
	struct wld_rect rect = {10,10,16,16};
	struct wld_frect source = {0,0,16,16};
	assert(wld_set_target_buffer(test_renderer,dst));
	wld_blend_scaled(test_renderer,src,&rect,&source);
	wld_flush(test_renderer);
	for (unsigned y=0; y<16; ++y) for (unsigned x=0; x<16; ++x)
		assert(pixel(dst,x+10,y+10) == (0xff000000 | (x*8 << 16) | (y*8 << 8)));
	rect.width=rect.height=8;
	assert(wld_set_target_buffer(test_renderer,dst));
	wld_blend_scaled(test_renderer,src,&rect,&source); wld_flush(test_renderer);
	assert(pixel(dst,13,13)==0xff343400); /* bilinear average at 6.5,6.5 */
	rect.width=rect.height=24;
	assert(wld_set_target_buffer(test_renderer,dst));
	wld_blend_scaled(test_renderer,src,&rect,&source); wld_flush(test_renderer);
	uint32_t p = pixel(dst,13,13);
	assert(abs((int)((p>>16)&255)-15)<=1 && abs((int)((p>>8)&255)-15)<=1);
	wld_set_target_buffer(test_renderer,NULL);
	wld_buffer_unreference(src); wld_buffer_unreference(dst);
	puts("GBM sampling: exact scale 1, minification, and magnification passed");
}

int main(int argc, char **argv)
{
	int fd = -1;
	gpu = argc == 3 && !strcmp(argv[1], "--gpu");
	test_context = wld_pixman_context;
	if (gpu) {
		fd = open(argv[2],O_RDWR | O_CLOEXEC);
		assert(fd >= 0);
		setenv("WLD_DRM_DRIVER","gbm",1);
		test_context = wld_drm_create_context(fd);
		assert(test_context && !wld_drm_is_dumb(test_context));
	}
	struct wld_renderer *r = test_renderer = wld_create_renderer(test_context);
	assert(r);
	struct wld_buffer *target = buffer(100,80,0);
	struct screen screen = { .base.geometry = {100,-50,100,80} };
	struct window window = {0}, other_window = {0};
	struct compositor_view root = { .window=&window, .base.geometry={200,200,40,40} };
	struct compositor_view child = { .parent=&root, .base.geometry={210,210,10,10} };
	struct compositor_view popup = { .parent=&child, .base.geometry={236,220,20,10} };
	struct compositor_view unrelated = { .window=&other_window, .parent=&root,
	    .base.geometry={200,200,40,40} };
	struct compositor_view panel = { .visible=true, .stack_layer=STACK_LAYER_TOP,
	    .base.geometry={100,-50,100,5} };
	root.buffer = imported(buffer(40,40,0xffff0000));
	child.buffer = buffer(10,10,0xff00ff00);
	popup.buffer = buffer(20,10,0xff0000ff);
	unrelated.buffer = buffer(40,40,0xffffffff);
	panel.buffer = buffer(100,5,0xff00ffff);
	wl_list_init(&compositor.views);
	wl_list_insert(&compositor.views, &root.link);
	wl_list_insert(&compositor.views, &child.link);
	wl_list_insert(&compositor.views, &popup.link);
	wl_list_insert(&compositor.views, &unrelated.link);
	wl_list_insert(&compositor.views, &panel.link);
	window.view = &root;
	struct swc_rectangle footprint;
	assert(swc_window_overview_geometry(&window.base,&footprint));
	assert(footprint.x==200 && footprint.y==200 && footprint.width==56 && footprint.height==40);
	struct swc_overview_item item = { .window=&window.base, .source={200,200,40,40},
	    .rect={110,-40,20,20}, .highlighted=true, .color=0xffeedd00 };
	compositor.overview_items=&item; compositor.overview_count=1;
	assert(wld_set_target_buffer(r,target));
	assert(wld_set_target_buffer(r,target));
	render_overview(&screen,r); wld_flush(r);
	assert(pixel(target,13,13)==0xffff0000); /* hidden root retained */
	assert(pixel(target,17,17)==0xff00ff00); /* child scales with parent */
	assert(pixel(target,32,22)==0xff101010); /* popup cannot spill into next card */
	assert(pixel(target,10,10)==0xffeedd00); /* selection and monitor offset */
	assert(pixel(target,50,2)==0xff00ffff); /* panel remains */
	assert(pixel(target,50,50)==0xff101010); /* unrelated hidden window omitted */
	wld_buffer_unreference(root.buffer);
	root.buffer=imported(buffer(40,40,0xffaa00aa));
	assert(wld_set_target_buffer(r,target));
	render_overview(&screen,r); wld_flush(r);
	assert(pixel(target,13,13)==0xffaa00aa); /* latest frame */
	root.decor.bar_buffer=buffer(40,8,0xff8899aa); root.decor.top=8;
	item.source=(struct swc_rectangle){200,192,40,48}; item.rect.height=24;
	assert(wld_set_target_buffer(r,target));
	render_overview(&screen,r); wld_flush(r);
	assert(pixel(target,20,12)==0xff8899aa); /* titlebar scales with image */
	wld_buffer_unreference(root.decor.bar_buffer);
	wld_buffer_unreference(root.buffer); wld_buffer_unreference(child.buffer);
	wld_buffer_unreference(popup.buffer); wld_buffer_unreference(unrelated.buffer);
	wld_buffer_unreference(panel.buffer);
	if (gpu) check_gpu_sampling();
	wld_destroy_renderer(r); wld_buffer_unreference(target);
	if (gpu) { wld_destroy_context(test_context); close(fd); puts("GBM imported-dmabuf path passed"); }
	puts("overview renderer: hidden windows, children, clipping, panel, titlebar, offsets, and updates passed");
	return 0;
}
