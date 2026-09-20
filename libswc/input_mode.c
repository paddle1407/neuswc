/* A WM input mode scoped to one monitor. Static handlers own releases
 * after the mode ends, so an exit click cannot leak into a client. */
#include "swc.h"
#include "internal.h"
#include "util.h"
#include "keyboard.h"
#include "pointer.h"
#include "seat.h"
#include "session_lock.h"
#include <xkbcommon/xkbcommon-keysyms.h>

static const struct swc_input_mode_handler *mode;
static void *mode_data;
static struct swc_screen *mode_screen;
static bool pointer_inside;

/* Focus guards and input dispatch share this test. The pointer position is
 * already updated when motion handlers run, including on monitor crossings. */
bool input_mode_active(void)
{
	if (!mode || !mode_screen || !swc.seat || !swc.seat->pointer) return false;
	const struct swc_rectangle *g = &mode_screen->geometry;
	int32_t x = wl_fixed_to_int(swc.seat->pointer->x);
	int32_t y = wl_fixed_to_int(swc.seat->pointer->y);
	return x >= g->x && y >= g->y && (int64_t)x < (int64_t)g->x + g->width &&
	       (int64_t)y < (int64_t)g->y + g->height;
}

static bool key(struct keyboard *keyboard, uint32_t time, struct key *key, uint32_t state)
{
	if (!state) return true;
	if (!input_mode_active()) return false;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->xkb.state, XKB_KEY(key->press.value));
	/* Preserve the compositor's VT escape route. */
	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) return false;
	if (mode && mode->key) mode->key(mode_data, sym, keyboard->modifiers);
	return true;
}
static bool motion(struct pointer_handler *h, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	bool inside = input_mode_active();
	if (inside && !pointer_inside) {
		swc_window_focus(NULL);
		pointer_set_focus(swc.seat->pointer, NULL);
	}
	pointer_inside = inside;
	/* The WM sees crossings too, so it can restore focus on the other output. */
	if (mode && mode->motion) mode->motion(mode_data, x, y);
	return inside;
}
static bool button(struct pointer_handler *h, uint32_t time, struct button *b, uint32_t state)
{
	if (!state) return true;
	if (!input_mode_active()) return false;
	if (mode && mode->button) mode->button(mode_data, b->press.value);
	return true;
}
static bool axis(struct pointer_handler *h, uint32_t time, enum wl_pointer_axis a,
                 enum wl_pointer_axis_source s, wl_fixed_t value, int value120)
{ return input_mode_active(); }
static struct keyboard_handler keys = { .key = key };
static struct pointer_handler pointer = { .motion = motion, .button = button, .axis = axis };

EXPORT bool swc_input_mode_begin(struct swc_screen *screen, const struct swc_input_mode_handler *handler, void *data)
{
	if (mode || !screen || !handler || !swc.active || session_lock_active() || !swc.seat ||
	    !swc.seat->keyboard || !swc.seat->pointer || swc.seat->pointer->buttons.size)
		return false;
	mode = handler; mode_data = data; mode_screen = screen;
	pointer_inside = input_mode_active();
	wl_list_insert(&swc.seat->keyboard->handlers, &keys.link);
	wl_list_insert(&swc.seat->pointer->handlers, &pointer.link);
	if (pointer_inside) {
		swc_window_focus(NULL);
		pointer_set_focus(swc.seat->pointer, NULL);
	}
	return true;
}
EXPORT void swc_input_mode_end(void)
{
	if (!mode) return;
	mode = NULL; mode_data = NULL; mode_screen = NULL;
	pointer_inside = false;
	wl_list_remove(&keys.link);
	wl_list_remove(&pointer.link);
}
void input_mode_cancel(void)
{
	const struct swc_input_mode_handler *handler = mode;
	void *data = mode_data;
	swc_input_mode_end();
	if (handler && handler->cancel) handler->cancel(data);
}
