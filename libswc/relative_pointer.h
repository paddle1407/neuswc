#ifndef SWC_RELATIVE_POINTER_H
#define SWC_RELATIVE_POINTER_H

#include <stdint.h>
#include <wayland-server.h>

struct pointer;

struct wl_global *
relative_pointer_manager_create(struct wl_display *display);

/**
 * Send a relative motion event to the client holding pointer focus.
 *
 * 'time' is in microseconds.
 */
void
relative_pointer_send_motion(struct pointer *pointer, uint64_t time,
                             wl_fixed_t dx, wl_fixed_t dy,
                             wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel);

#endif
