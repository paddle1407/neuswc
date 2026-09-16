#ifndef SWC_IDLE_INHIBIT_H
#define SWC_IDLE_INHIBIT_H

#include <wayland-server.h>

struct wl_global *idle_inhibit_manager_create(struct wl_display *display);
void idle_inhibit_manager_finish(void);

#endif
