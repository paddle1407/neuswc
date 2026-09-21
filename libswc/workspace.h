#ifndef SWC_WORKSPACE_H
#define SWC_WORKSPACE_H

#include <wayland-server.h>

struct output;
struct screen;

struct wl_global *workspace_manager_create(struct wl_display *display);
void workspace_manager_finish(void);
void workspace_output_bound(struct output *output, struct wl_resource *resource);
void workspace_output_removed(struct output *output);
void workspace_screen_added(struct screen *screen);
void workspace_screen_removed(struct screen *screen);

#endif
