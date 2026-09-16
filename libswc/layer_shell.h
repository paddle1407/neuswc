#ifndef SWC_LAYER_SHELL_H
#define SWC_LAYER_SHELL_H

struct compositor_view;
struct wl_display;
struct wl_global;

struct wl_global *
layer_shell_create(struct wl_display *display);

/* Apply layer-shell click-to-focus semantics to a compositor view. */
void
layer_shell_handle_pointer_press(struct compositor_view *view);

#endif
