#ifndef SWC_IDLE_INHIBIT_H
#define SWC_IDLE_INHIBIT_H

#include <wayland-server.h>

#include <stdbool.h>

struct wl_global *idle_inhibit_manager_create(struct wl_display *display);
void idle_inhibit_manager_finish(void);

/* Whether any client currently holds the session awake. */
bool idle_inhibit_active(void);

#endif
