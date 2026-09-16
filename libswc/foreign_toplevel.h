#ifndef SWC_FOREIGN_TOPLEVEL_H
#define SWC_FOREIGN_TOPLEVEL_H

#include <stdint.h>
#include <wayland-server.h>

struct output;
struct window;

struct wl_global *foreign_toplevel_manager_create(struct wl_display *display);
void foreign_toplevel_manager_finish(void);

void foreign_toplevel_window_manage(struct window *window);
void foreign_toplevel_window_unmanage(struct window *window);
void foreign_toplevel_window_title(struct window *window);
void foreign_toplevel_window_app_id(struct window *window);
void foreign_toplevel_window_parent(struct window *window);
void foreign_toplevel_window_state(struct window *window);
void foreign_toplevel_window_workspace(struct window *window);
void foreign_toplevel_window_screens(struct window *window, uint32_t entered,
                                     uint32_t left);
void foreign_toplevel_output_bound(struct output *output,
                                   struct wl_resource *resource);
void foreign_toplevel_output_removed(struct output *output);

#endif
