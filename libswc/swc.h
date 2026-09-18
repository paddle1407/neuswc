/* swc: libswc/swc.h
 *
 * Copyright (c) 2013 Michael Forney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SWC_H
#define SWC_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct libinput_device;
struct wl_display;
struct wl_event_loop;
struct wld_buffer;

enum swc_wallpaper_mode {
	SWC_WALLPAPER_FILL,
	SWC_WALLPAPER_FIT,
	SWC_WALLPAPER_CENTER,
};

struct swc_prepared_wallpaper;
/* Prepare output-sized caches after swc_initialize, on the compositor thread.
 * Pixels are tightly packed premultiplied ARGB8888, borrowed only for this call.
 * NULL pixels selects a solid background. A NULL result indicates failure.
 * Commit consumes the candidate; discard accepts NULL. No event dispatch may
 * occur between prepare and commit (the connected outputs must stay the same). */
struct swc_prepared_wallpaper *
swc_wallpaper_prepare(const uint32_t *pixels, uint32_t width, uint32_t height,
                      enum swc_wallpaper_mode mode, uint32_t background);
void swc_wallpaper_commit(struct swc_prepared_wallpaper *);
void swc_wallpaper_discard(struct swc_prepared_wallpaper *);

/**
 * Get the current cursor position.
 *
 * The returned coordinates are in compositor-global space, in wl_fixed_t
 * (24.8) fixed-point units, but exposed as raw int32_t to avoid needing
 * wayland headers.
 *
 */
bool
swc_cursor_position(int32_t *x, int32_t *y);

/**
 * Set the cursor position.
 *
 * The coordinates are in compositor-global space, exposed as raw int32_t
 * to avoid needing wayland headers. This has the same effect as the
 * cursor being moved there by an input device.
 */
bool
swc_cursor_set_position(int32_t x, int32_t y);

/**
 * Send a pointer button event to the currently focused client.
 *
 * This is intended for window managers which intercept button events (for
 * example for mouse chords) but want normal clicks to still reach clients.
 */
void
swc_pointer_send_button(uint32_t time, uint32_t button, uint32_t state);

/**
 * Send a pointer axis event to the currently focused client.
 *
 * This is intended for window managers which intercept axis events (for
 * example for mouse chords) but want normal scrolling to still reach clients.
 *
 * value120 uses the wl_pointer "120 units" convention.
 */
void
swc_pointer_send_axis(uint32_t time, uint32_t axis, int32_t value120);

/* Cursor control (compositor-internal cursor) */
/*
 * The first six are swc's own window-manager cursors. The rest mirror the
 * shapes in cursor-shape-v1 one for one, so that a client asking for, say, a
 * text cursor gets the themed image the window manager loaded rather than
 * having to ship its own. Existing values keep their numbers.
 */
enum swc_cursor_kind {
	SWC_CURSOR_DEFAULT = 0,
	SWC_CURSOR_BOX = 1,
	SWC_CURSOR_CROSS = 2,
	SWC_CURSOR_SIGHT = 3,
	SWC_CURSOR_UP = 4,
	SWC_CURSOR_DOWN = 5,

	SWC_CURSOR_CONTEXT_MENU = 6,
	SWC_CURSOR_HELP = 7,
	SWC_CURSOR_POINTER = 8,
	SWC_CURSOR_PROGRESS = 9,
	SWC_CURSOR_WAIT = 10,
	SWC_CURSOR_CELL = 11,
	SWC_CURSOR_CROSSHAIR = 12,
	SWC_CURSOR_TEXT = 13,
	SWC_CURSOR_VERTICAL_TEXT = 14,
	SWC_CURSOR_ALIAS = 15,
	SWC_CURSOR_COPY = 16,
	SWC_CURSOR_MOVE = 17,
	SWC_CURSOR_NO_DROP = 18,
	SWC_CURSOR_NOT_ALLOWED = 19,
	SWC_CURSOR_GRAB = 20,
	SWC_CURSOR_GRABBING = 21,
	SWC_CURSOR_E_RESIZE = 22,
	SWC_CURSOR_N_RESIZE = 23,
	SWC_CURSOR_NE_RESIZE = 24,
	SWC_CURSOR_NW_RESIZE = 25,
	SWC_CURSOR_S_RESIZE = 26,
	SWC_CURSOR_SE_RESIZE = 27,
	SWC_CURSOR_SW_RESIZE = 28,
	SWC_CURSOR_W_RESIZE = 29,
	SWC_CURSOR_EW_RESIZE = 30,
	SWC_CURSOR_NS_RESIZE = 31,
	SWC_CURSOR_NESW_RESIZE = 32,
	SWC_CURSOR_NWSE_RESIZE = 33,
	SWC_CURSOR_COL_RESIZE = 34,
	SWC_CURSOR_ROW_RESIZE = 35,
	SWC_CURSOR_ALL_SCROLL = 36,
	SWC_CURSOR_ZOOM_IN = 37,
	SWC_CURSOR_ZOOM_OUT = 38,
	SWC_CURSOR_DND_ASK = 39,
	SWC_CURSOR_ALL_RESIZE = 40,

	SWC_CURSOR_KIND_COUNT
};

enum swc_cursor_mode {
	/* Allow clients to set their own cursors (I-beam, resize, etc). */
	SWC_CURSOR_MODE_CLIENT = 0,
	/* Force compositor cursor; ignore client wl_pointer.set_cursor. */
	SWC_CURSOR_MODE_COMPOSITOR = 1,
};

/**
 * Override the compositor's internal cursor.
 *
 * this is intended for window managers to show mode cursors
 * (move/resize/select) like the ones in hevel If a client has set its own
 * cursor surface, swc may ignore the override.
 */
void
swc_set_cursor(enum swc_cursor_kind kind);

/**
 * Control whether client cursor surfaces are honored.
 */
void
swc_set_cursor_mode(enum swc_cursor_mode mode);

/**
 * set a custom argb8888 cursor image for a given kind
 *
 * `argb8888` is a pointer to `width*height` pixels in ARGB8888 order.
 * swc copies the pixels; failure leaves the previous image intact
 */
bool
swc_set_cursor_image(enum swc_cursor_kind kind, const uint32_t *argb8888,
                     uint32_t width, uint32_t height, int32_t hotspot_x,
                     int32_t hotspot_y);

void
swc_clear_cursor_image(enum swc_cursor_kind kind);

bool swc_pointer_has_buttons(void);

/**
 * draw [or update] a simple box overlay
 *
 * box is defined by two diagonally opposite corners in compositor-global
 * coordinates. this draws only the border. Call swc_overlay_clear() to remove
 * it
 */
void
swc_overlay_set_box(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                    uint32_t color, uint32_t border_width);

/**
 * Clear the current overlay, if any.
 */
void
swc_overlay_clear(void);

/**
 * Set the compositor zoom level.
 *
 * 1.0 = normal, >1.0 = zoomed in, <1.0 = zoomed out
 * Uses software (pixman) scaling.
 */
void
swc_set_zoom(float level);

/**
 * Get the current zoom level.
 */
float
swc_get_zoom(void);

/* Rectangles {{{ */

struct swc_rectangle {
	int32_t x, y;
	uint32_t width, height;
};

/* }}} */

/* Screens {{{ */

struct swc_screen_handler {
	/**
	 * Called when the screen is about to be destroyed.
	 *
	 * After this is called, the screen is no longer valid.
	 */
	void (*destroy)(void *data);

	/**
	 * Called when the total area of the screen has changed.
	 */
	void (*geometry_changed)(void *data);

	/**
	 * Called when the geometry of the screen available for laying out windows
	 * has changed.
	 *
	 * A window manager should respond by making sure all visible windows are
	 * within this area.
	 */
	void (*usable_geometry_changed)(void *data);

	/**
	 * Called when the pointer enters the screen.
	 */
	void (*entered)(void *data);
};

struct swc_screen {
	/**
	 * The total area of the screen.
	 */
	struct swc_rectangle geometry;

	/**
	 * The area of the screen available for placing windows.
	 */
	struct swc_rectangle usable_geometry;
};

/**
 * Set the handler associated with this screen.
 */
void
swc_screen_set_handler(struct swc_screen *screen,
                       const struct swc_screen_handler *handler, void *data);

/** Connector name, such as DVI-D-1 or HDMI-A-1; owned by the screen. */
const char *
swc_screen_get_name(const struct swc_screen *screen);

/**
 * Set desktop coordinates during the manager's new_screen callback only.
 * Must precede input/render initialization; no runtime output rearrangement.
 * Returns false outside that callback or for out-of-range coordinates.
 */
bool
swc_screen_set_initial_position(struct swc_screen *screen, int32_t x, int32_t y);

/* }}} */

/* Windows {{{ */

enum swc_titlebar_action {
	SWC_TITLEBAR_FOCUS,
	SWC_TITLEBAR_MINIMIZE,
	SWC_TITLEBAR_FULLSCREEN,
	SWC_TITLEBAR_CLOSE,
	/* Toggles whether the window is kept above the others. */
	SWC_TITLEBAR_PIN,
};

struct swc_window_handler {
	/**
	 * Called when the window is about to be destroyed.
	 *
	 * After this is called, the window is no longer valid.
	 */
	void (*destroy)(void *data);

	/**
	 * Called when the window's title changes.
	 */
	void (*title_changed)(void *data);

	/**
	 * Called when the window's application identifier changes.
	 */
	void (*app_id_changed)(void *data);

	/**
	 * Called when the window's parent changes.
	 *
	 * This can occur when the window becomes a transient for another window, or
	 * becomes a toplevel window.
	 */
	void (*parent_changed)(void *data);

	/**
	 * Called when the pointer enters the window.
	 */
	void (*entered)(void *data);

	/**
	 * Called when the window wants to initiate an interactive move, but the
	 * window is not in stacked mode.
	 *
	 * The window manager may respond by changing the window's mode, after which
	 * the interactive move will be honored.
	 */
	void (*move)(void *data);

	/**
	 * Called when the window wants to initiate an interactive resize, but the
	 * window is not in stacked mode.
	 *
	 * The window manager may respond by changing the window's mode, after which
	 * the interactive resize will be honored.
	 */
	void (*resize)(void *data);

	/* A compositor-owned titlebar was clicked. */
	void (*titlebar_action)(void *data, enum swc_titlebar_action action);

	/*
	 * An interactive move the compositor drives itself -- dragging a
	 * compositor-owned titlebar -- began or ended. The window's position is
	 * only final once it has ended, and the window manager is free to hold off
	 * anything that would fight the drag, such as following the pointer onto
	 * another monitor, until then.
	 */
	void (*interactive_move)(void *data, bool active);

	/* Requests from desktop taskbars and docks. */
	void (*request_activate)(void *data);
	void (*request_minimized)(void *data, bool minimized);
	void (*request_maximized)(void *data, bool maximized);
	void (*request_fullscreen)(void *data, bool fullscreen,
	                           struct swc_screen *screen);
};

struct swc_window {
	char *title;
	char *app_id;

	struct swc_window *parent;
	uint32_t motion_throttle_ms;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_width;
	uint32_t max_height;
};

/**
 * Set the handler associated with this window.
 */
void
swc_window_set_handler(struct swc_window *window,
                       const struct swc_window_handler *handler, void *data);

/** Control whether a direct pointer press raises this window. */
void
swc_window_set_raise_on_click(struct swc_window *window, bool enabled);

/** Control whether pointer-driven move requests may start for this window. */
void
swc_window_set_movable(struct swc_window *window, bool enabled);

/** Control whether pointer-driven resize requests may start for this window. */
void
swc_window_set_resizable(struct swc_window *window, bool enabled);

/** Raise the window above other windows in the same compositor layer. */
void
swc_window_raise(struct swc_window *window);

/**
 * Keep the window above the others, fullscreen ones included.
 *
 * A pinned window sits in its own layer: raising anything else cannot cover
 * it, and it stays pinned across mode changes. It remains below the overlay
 * layer, so a lock screen still covers it.
 */
void
swc_window_set_pinned(struct swc_window *window, bool pinned);

/**
 * Request that the specified window close.
 */
void
swc_window_close(struct swc_window *window);

/**
 * Make the specified window visible.
 */
void
swc_window_show(struct swc_window *window);

/**
 * Make the specified window visible without changing the stacking order.
 *
 * swc_window_show() raises the window, which is what a window being mapped
 * wants. A window that is merely coming back into view -- the workspace it is
 * on was switched to -- keeps the position in the stack it had when it left.
 */
void
swc_window_show_in_place(struct swc_window *window);

/**
 * Make the specified window hidden.
 */
void
swc_window_hide(struct swc_window *window);

/** Publish the minimized state to desktop taskbars. */
void
swc_window_set_minimized(struct swc_window *window, bool minimized);

/**
 * Publish the numbered workspace this window belongs to.
 *
 * Workspaces are numbered per screen, so the number only identifies a
 * workspace together with the screen the window is on. Pass zero when the
 * window manager does not use numbered workspaces. swc does not act on the
 * value; it forwards it to desktop taskbars, which need it to tell a window
 * hidden on another workspace from one on the workspace being shown.
 */
void
swc_window_set_workspace(struct swc_window *window, uint32_t workspace);

/**
 * Set the keyboard focus to the specified window.
 *
 * If window is NULL, the keyboard will have no focus.
 */
void
swc_window_focus(struct swc_window *window);

/**
 * Sets the window to stacked mode.
 *
 * A window in this mode has its size specified by the client. The window's
 * viewport will be adjusted to the size of the buffer attached by the
 * client.
 *
 * Use of this mode is required to allow interactive moving and resizing.
 */
void
swc_window_set_stacked(struct swc_window *window);

/**
 * Sets the window to tiled mode.
 *
 * A window in this mode has its size specified by the window manager.
 * Additionally, swc will configure the window to operate in a tiled or
 * maximized state in order to prevent the window from drawing shadows.
 *
 * It is invalid to interactively move or resize a window in tiled mode.
 */
void
swc_window_set_tiled(struct swc_window *window);

/**
 * Sets the window to fullscreen mode.
 */
void
swc_window_set_fullscreen(struct swc_window *window, struct swc_screen *screen);

/**
 * Set the window's position.
 *
 * The x and y coordinates refer to the top-left corner of the actual contents
 * of the window and should be adjusted for the border size.
 */
void
swc_window_set_position(struct swc_window *window, int32_t x, int32_t y);

/**
 * Set the window's size.
 *
 * The width and height refer to the dimension of the actual contents of the
 * window and should be adjusted for the border size.
 */
void
swc_window_set_size(struct swc_window *window, uint32_t width, uint32_t height);

/**
 * Set the window's size and position.
 *
 * This is a convenience function that is equivalent to calling
 * swc_window_set_size and then swc_window_set_position.
 */
void
swc_window_set_geometry(struct swc_window *window,
                        const struct swc_rectangle *geometry);

/**
 * Get the window's current geometry in compositor-global coordinates.
 */
bool
swc_window_get_geometry(const struct swc_window *window,
                        struct swc_rectangle *geometry);

/**
 * Get the pid of the client that owns this window
 *
 * returns pid, or 0 if unavailable
 */
pid_t
swc_window_get_pid(struct swc_window *window);

/**
 * Set the window's border color and width.
 *
 * NOTE: The window's geometry remains unchanged, and should be updated if a
 *       fixed top-left corner of the border is desired.
 *
 * info from dalem: unsure how much double borders break!
 */
void
swc_window_set_border(struct swc_window *window, uint32_t inner_border_color,
                      uint32_t inner_border_width, uint32_t outer_border_color,
                      uint32_t outer_border_width);

/*window decor things*/

/**
 * Select which decor edge a text slot is rendered on.
 */
enum swc_decor_edge {
	SWC_DECOR_EDGE_TOP,
	SWC_DECOR_EDGE_RIGHT,
	SWC_DECOR_EDGE_BOTTOM,
	SWC_DECOR_EDGE_LEFT,
};

/**
 * Choose text alignment within the selected decor edge.
 *
 * for top and bottom edges, alignment is horizontal
 * for left and right edges, alignment is vertical
 */
enum swc_decor_align {
	SWC_DECOR_ALIGN_START,
	SWC_DECOR_ALIGN_CENTER,
	SWC_DECOR_ALIGN_END,
};

/**
 * Describe a window decor text slot.
 *
 * if enabled is false, no text is drawn.
 *
 * the wm supplies some string to be rendered in this decor slot. swc
 * copies the string when swc_window_set_decor() is called, so caller doesn't
 * need to keep it after the call returns.
 *
 * for top and bottom edges, text is drawn horizontally
 * for left and right edges, text is drawn as stacked glyphs, one glyph
 * per row.
 *
 * the font field accepts some fontconfig pattern such as "sans-serif:size=10".
 * if font is NULL, a default font is used.
 */
struct swc_decor_text {
	bool enabled;
	enum swc_decor_edge edge;
	enum swc_decor_align align;
	const char *string;
	uint32_t color;
	uint32_t padding;
	int32_t offset_x, offset_y;
	const char *font;
};

/**
 * Describes one wm-supplied decor pixel block.
 *
 * swc copies the pixel data when swc_window_set_decor() is called, so caller
 * doesn't need to keep it after the call returns.
 *
 * Pixel data is expected to be premultiplied ARGB8888 with the provided
 * stride.
 */
struct swc_decor_part {
	uint32_t width, height;
	uint32_t stride;
	const void *data;
};

/**
 * Describes optional pixel blocks for the outer frame of a decor.
 *
 * corner parts are drawn once at their matching corner
 * edge parts are tiled to fill the remaining edge area between corners.
 */
struct swc_decor_parts {
	struct swc_decor_part top_left;
	struct swc_decor_part top;
	struct swc_decor_part top_right;
	struct swc_decor_part left;
	struct swc_decor_part right;
	struct swc_decor_part bottom_left;
	struct swc_decor_part bottom;
	struct swc_decor_part bottom_right;
};

/**
 * Describe decor around a window's edges.
 *
 * The edge sizes extend outward from the window content geometry.
 * if you put 0 it won't be visible.
 *
 * The title field controls an optional text slot rendered on one edge.
 */
enum swc_titlebar_buttons_style {
	SWC_TITLEBAR_BUTTONS_CLASSIC,
	SWC_TITLEBAR_BUTTONS_CIRCLES,
};

/* An optional solid top bar. Buttons are ordered left to right on the chosen
 * edge; count is at most four. The top edge supplies the bar's height. */
struct swc_titlebar {
	bool enabled;
	uint32_t count;
	enum swc_titlebar_action buttons[4];
	/* Classic button backgrounds; zero derives a shade from the bar colors. */
	uint32_t hover_color, pressed_color;
	enum swc_titlebar_buttons_style buttons_style;
	bool buttons_left;
	uint32_t close_color, minimize_color, fullscreen_color, pin_color;
	/* Whether this window is currently pinned, which the pin button shows. */
	bool pinned;
};

struct swc_decor {
	uint32_t color;
	uint32_t top, right, bottom, left;
	const struct swc_decor_parts *parts;
	struct swc_decor_text title;
	struct swc_titlebar titlebar;
};

/* Prepare decoration resources without changing any window. Apply consumes
 * the prepared object and cannot fail; discard releases an unused candidate. */
struct swc_prepared_decor;
struct swc_prepared_decor *swc_decor_prepare(const struct swc_decor *, uint32_t width);
void swc_decor_discard(struct swc_prepared_decor *);
void swc_window_apply_decor(struct swc_window *, struct swc_prepared_decor *);

/**
 * Set window decor around the window.
 *
 * the dimensions are independent for each edge and extend outward from the
 * window content geometry.
 *
 * swc copies any decor text string needed by the configuration.
 *
 * passing NULL disables window decor.
 */
void
swc_window_set_decor(struct swc_window *window, const struct swc_decor *decor);

/**
 * Begin an interactive move of the specified window.
 */
void
swc_window_begin_move(struct swc_window *window);

/**
 * End an interactive move of the specified window.
 */
void
swc_window_end_move(struct swc_window *window);

enum {
	SWC_WINDOW_EDGE_AUTO = 0,
	SWC_WINDOW_EDGE_TOP = (1 << 0),
	SWC_WINDOW_EDGE_BOTTOM = (1 << 1),
	SWC_WINDOW_EDGE_LEFT = (1 << 2),
	SWC_WINDOW_EDGE_RIGHT = (1 << 3)
};

/**
 * Begin an interactive resize of the specified window.
 */
void
swc_window_begin_resize(struct swc_window *window, uint32_t edges);

/**
 * End an interactive resize of the specified window.
 */
void
swc_window_end_resize(struct swc_window *window);

/**
 * returns the topmost window at any given compositor global coordinates
 *
 * returns null if there is no window at that point
 */
struct swc_window *
swc_window_at(int32_t x, int32_t y);

/**
 * move a window in the stacking order by one step
 *
 * direction < 0 moves the window towards the front (higher)
 * direction > 0 moves the window towards the back (lower)
 */
void
swc_window_stack(struct swc_window *window, int32_t direction);

/* }}} */

/* Keyboard repeat rate (characters per second) and delay (ms) definitions.
 * Exposed as extern so compositors can set them. 
 */
extern int32_t swc_repeat_rate, swc_repeat_delay;

/* Bindings {{{ */

enum {
	SWC_MOD_CTRL = 1 << 0,
	SWC_MOD_ALT = 1 << 1,
	SWC_MOD_LOGO = 1 << 2,
	SWC_MOD_SHIFT = 1 << 3,
	SWC_MOD_ANY = ~0
};

enum swc_binding_type {
	SWC_BINDING_KEY,
	SWC_BINDING_BUTTON,
};

typedef void (*swc_binding_handler)(void *data, uint32_t time, uint32_t value,
                                   uint32_t state);
typedef void (*swc_axis_binding_handler)(void *data, uint32_t time,
                                         uint32_t axis, int32_t value120);

/**
 * Register a new input binding.
 *
 * Returns 0 on success, negative error code otherwise.
 */
int
swc_add_binding(enum swc_binding_type type, uint32_t modifiers, uint32_t value,
                swc_binding_handler handler, void *data);

/**
 * Unregister a registered input binding.
 *
 */
void
swc_remove_binding(enum swc_binding_type type, uint32_t modifiers,
                   uint32_t value);

/* Allocate bindings off-list, then publish them without allocating. */
struct swc_binding_batch;
struct swc_binding_batch *swc_binding_batch_create(void);
bool swc_binding_batch_add(struct swc_binding_batch *, enum swc_binding_type,
                           uint32_t modifiers, uint32_t value, swc_binding_handler, void *);
void swc_binding_batch_commit(struct swc_binding_batch *);
void swc_binding_batch_discard(struct swc_binding_batch *);

/**
 * register a new pointer axis binding
 *
 * this will intercept axis events from clients; use swc_pointer_send_axis()
 * from the handler to forward events when appropriate
 */
int
swc_add_axis_binding(uint32_t modifiers, uint32_t axis,
                     swc_axis_binding_handler handler, void *data);

/* }}} */

/**
 * This is a user-provided structure that swc will use to notify the display
 * server of new windows, screens and input devices.
 */
struct swc_manager {
	/**
	 * Called when a new screen is created.
	 */
	void (*new_screen)(struct swc_screen *screen);

	/**
	 * Called when a new window is created.
	 */
	void (*new_window)(struct swc_window *window);

	/**
	 * Called when a new input device is detected.
	 */
	void (*new_device)(struct libinput_device *device);

	/**
	 * Called when the session gets activated (for example, startup or VT
	 * switch).
	 */
	void (*activate)(void);

	/**
	 * Called when the session gets deactivated.
	 */
	void (*deactivate)(void);

	/**
	 * Called when a desktop shell asks to activate a numbered workspace on a
	 * screen. Each screen carries its own set of numbered workspaces.
	 */
	void (*workspace_activate)(struct swc_screen *screen, uint32_t workspace);
};

/** Publish a screen's active numbered workspace (1 through 9). */
void
swc_workspace_set_active(struct swc_screen *screen, uint32_t workspace);

/**
 * Initializes the compositor using the specified display, event_loop, and
 * manager.
 */
bool
swc_initialize(struct wl_display *display, struct wl_event_loop *event_loop,
               const struct swc_manager *manager);

/**
 * Stops the compositor, releasing any used resources.
 */
void
swc_finalize(void);

#ifdef __cplusplus
}
#endif

#endif

/* vim: set fdm=marker : */
