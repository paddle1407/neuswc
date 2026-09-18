/* swc: xselection.h
 *
 * The X11 clipboard bridge. X selections and Wayland selections are two
 * separate worlds; without something joining them, copying in an X client
 * and pasting in a Wayland one does nothing, because the window manager is
 * the only party that sees both.
 */

#ifndef SWC_XSELECTION_H
#define SWC_XSELECTION_H

#include <stdbool.h>
#include <xcb/xcb.h>

bool
xselection_initialize(xcb_connection_t *connection, xcb_window_t window);
void
xselection_finalize(void);
/* Whether the event belonged to the selection machinery. */
bool
xselection_handle_event(xcb_generic_event_t *event);
/* The Wayland selection changed, so the X side may need a new owner. */
void
xselection_wayland_selection_changed(void);

#endif
