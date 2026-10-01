/* Observe protocol events from the actual focus-entry and late-bind paths. */
#include "../libswc/keyboard.c"
#include "../libswc/input.c"
#include <stdarg.h>

struct swc swc;
struct resource { struct wl_list link; struct wl_client *client; unsigned events; };
static struct surface surface;
static struct keyboard keyboard;
static uint32_t serial;

uint32_t wl_display_next_serial(struct wl_display *display) { return ++serial; }
struct wl_list *wl_resource_get_link(struct wl_resource *r)
{ return &((struct resource *)r)->link; }
struct wl_resource *wl_resource_from_link(struct wl_list *link)
{
	struct resource *r = wl_container_of(link, r, link);
	return (struct wl_resource *)r;
}
struct wl_client *wl_resource_get_client(struct wl_resource *r)
{ return ((struct resource *)r)->client; }
void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode, ...)
{
	struct resource *r = (struct resource *)resource;
	va_list args; va_start(args, opcode);
	assert(va_arg(args, uint32_t) == serial);
	if (r->events++ == 0) {
		assert(opcode == WL_KEYBOARD_ENTER);
		assert(va_arg(args, struct wl_resource *) == surface.resource);
		assert(va_arg(args, struct wl_array *) == &keyboard.client_keys);
	} else {
		assert(opcode == WL_KEYBOARD_MODIFIERS && r->events == 2);
		assert(va_arg(args, uint32_t) == keyboard.modifier_state.depressed);
		assert(va_arg(args, uint32_t) == keyboard.modifier_state.latched);
		assert(va_arg(args, uint32_t) == keyboard.modifier_state.locked);
		assert(va_arg(args, uint32_t) == keyboard.modifier_state.group);
	}
	va_end(args);
}

int main(void)
{
	int tag;
	struct wl_client *client = (struct wl_client *)&tag;
	struct compositor_view view = { .surface=&surface };
	surface.resource = (struct wl_resource *)&tag;
	keyboard.focus_handler.enter = enter;
	keyboard.focus.handler = &keyboard.focus_handler;
	keyboard.focus.client = client; keyboard.focus.view = &view;
	wl_list_init(&keyboard.focus.active); wl_list_init(&keyboard.focus.inactive);
	wl_array_init(&keyboard.client_keys);
	struct resource first = { .client=client }, second = { .client=client };
	wl_list_insert(&keyboard.focus.active, &first.link);
	/* Distinct masks catch swapped latched/locked arguments. */
	struct keyboard_modifier_state cases[] = {
		{ .depressed=1 }, { .latched=2 }, { .locked=4 },
		{ .depressed=1, .latched=2, .locked=4, .group=3 },
	};
	for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
		keyboard.modifier_state = cases[i]; first.events = 0;
		enter(&keyboard.focus_handler, &keyboard.focus.active, &view);
		assert(first.events == 2);
	}
	input_focus_add_resource(&keyboard.focus, (struct wl_resource *)&second);
	assert(second.events == 2);
	assert(first.events == 2); /* Late binding does not re-enter older resources. */
	puts("Keyboard focus: enter order, held/latched/locked modifiers and late binding passed");
	return 0;
}
