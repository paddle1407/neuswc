#include "../libswc/input_mode.c"
#include <assert.h>
#include <stdio.h>
#include <linux/input-event-codes.h>
struct swc swc;
static bool locked;
static unsigned key_calls, button_calls, motion_calls, cancels, focus_clears;
bool session_lock_active(void) { return locked; }
void swc_window_focus(struct swc_window *w) { assert(!w); ++focus_clears; }
void pointer_set_focus(struct pointer *p, struct compositor_view *v) { assert(!v); }
static void on_key(void *d, uint32_t sym, uint32_t mods) { ++key_calls; swc_input_mode_end(); }
static void on_button(void *d, uint32_t b) { ++button_calls; swc_input_mode_end(); }
static void on_motion(void *d, int32_t x, int32_t y) { ++motion_calls; }
static void on_cancel(void *d) { ++cancels; }
int main(void)
{
	struct swc_screen screen = {.geometry={0,0,1920,1080}};
	struct keyboard kb = {0};
	struct pointer p = {0};
	struct swc_seat seat = { .keyboard=&kb, .pointer=&p };
	swc.seat=&seat; swc.active=true;
	wl_list_init(&kb.handlers); wl_list_init(&p.handlers);
	kb.xkb.context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	kb.xkb.keymap.map=xkb_keymap_new_from_names(kb.xkb.context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	kb.xkb.state=xkb_state_new(kb.xkb.keymap.map);
	struct swc_input_mode_handler h = {on_key,on_motion,on_button,on_cancel};
	assert(swc_input_mode_begin(&screen,&h,NULL));
	assert(!swc_input_mode_begin(&screen,&h,NULL));
	assert(pointer.motion(&pointer,0,256,512) && motion_calls==1);
	assert(pointer.axis(&pointer,0,0,0,256,120));
	struct button b = { .press.value=BTN_LEFT };
	struct key outside_key = { .press.value=KEY_A };
	p.x = wl_fixed_from_int(2000); p.y = wl_fixed_from_int(100);
	assert(!pointer.motion(&pointer,0,p.x,p.y));
	assert(!input_mode_active() && mode);
	assert(!pointer.button(&pointer,0,&b,1) && button_calls == 0);
	assert(!pointer.axis(&pointer,0,0,0,256,120));
	assert(!keys.key(&kb,0,&outside_key,1) && key_calls == 0);
	assert(focus_clears == 1); /* leaving does not clear the other monitor */
	p.x = wl_fixed_from_int(100);
	assert(pointer.motion(&pointer,0,p.x,p.y));
	assert(input_mode_active() && focus_clears == 2);
	assert(pointer.axis(&pointer,0,0,0,256,120));
	assert(pointer.button(&pointer,0,&b,1));
	assert(!input_mode_active() && button_calls==1);
	assert(pointer.button(&pointer,0,&b,0) && button_calls==1); /* release still consumed */
	assert(wl_list_empty(&kb.handlers) && wl_list_empty(&p.handlers));
	assert(swc_input_mode_begin(&screen,&h,NULL));
	struct key k = { .press.value=KEY_ESC };
	assert(keys.key(&kb,0,&k,1));
	assert(!input_mode_active() && key_calls==1);
	assert(keys.key(&kb,0,&k,0) && key_calls==1);
	assert(swc_input_mode_begin(&screen,&h,NULL)); input_mode_cancel();
	assert(cancels==1 && !input_mode_active());
	input_mode_cancel(); assert(cancels==1);
	locked=true; assert(!swc_input_mode_begin(&screen,&h,NULL)); locked=false;
	swc.active=false; assert(!swc_input_mode_begin(&screen,&h,NULL)); swc.active=true;
	p.buttons.size=sizeof(b); assert(!swc_input_mode_begin(&screen,&h,NULL)); p.buttons.size=0;
	xkb_state_unref(kb.xkb.state); xkb_keymap_unref(kb.xkb.keymap.map); xkb_context_unref(kb.xkb.context);
	puts("input mode: monitor crossings, normal outside input, swallowed releases, cancellation, lock and drag guards passed");
	return 0;
}
