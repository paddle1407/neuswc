/* swc: libswc/xdg_shell.h
 *
 * Copyright (c) 2018-2019 Michael Forney
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

#ifndef SWC_XDG_SHELL_H
#define SWC_XDG_SHELL_H

#include <stdbool.h>

struct compositor_view;
struct wl_display;
struct wl_resource;

struct wl_global *
xdg_shell_create(struct wl_display *display);

/* Attach an xdg_popup created with a NULL parent to a non-XDG shell surface. */
bool
xdg_popup_set_parent(struct wl_resource *popup_resource,
                     struct compositor_view *parent);

/*
 * Popup grabs (xdg_popup.grab). While one is held the topmost grabbing popup
 * has the keyboard.
 *
 * filter_keyboard_focus: the view keyboard focus should really go to. The
 * grabbing client's own surfaces give way to its topmost popup; any other
 * view ends the grab.
 *
 * defer_window_focus: true if the grab keeps the keyboard and the window
 * manager's choice of window is to be applied when it ends instead.
 *
 * focus_owner: the view that owns the keyboard behind a grabbing popup, for
 * callers that remember the focus to give it back later.
 */
struct compositor_view *
xdg_popup_grab_filter_keyboard_focus(struct compositor_view *view);
bool
xdg_popup_grab_defer_window_focus(struct compositor_view *view);
struct compositor_view *
xdg_popup_grab_focus_owner(struct compositor_view *view);

#endif
