#ifndef SWC_POINTER_CONSTRAINTS_H
#define SWC_POINTER_CONSTRAINTS_H

#include <stdbool.h>
#include <wayland-util.h>

struct pointer;
struct wl_display;
struct wl_global;

struct wl_global *
pointer_constraints_create(struct wl_display *display);

/**
 * Whether the pointer is currently locked in place.
 *
 * The pointer keeps reporting relative motion while locked, but must not move.
 */
bool
pointer_constraints_pointer_locked(void);

/**
 * Re-evaluate which constraint applies after a pointer focus change.
 */
void
pointer_constraints_update_focus(struct pointer *pointer);

/* Clip physical motion to the active confinement region. */
void
pointer_constraints_confine(struct pointer *pointer, wl_fixed_t *x, wl_fixed_t *y);

#endif
