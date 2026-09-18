/* swc: libswc/text_input.h
 *
 * Copyright (c) 2025 charaWC contributors
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

#ifndef SWC_TEXT_INPUT_H
#define SWC_TEXT_INPUT_H

#include <stdbool.h>
#include <wayland-server.h>

struct keyboard;
struct key;
struct compositor_view;

struct wl_global *
text_input_manager_create(struct wl_display *display);
struct wl_global *
input_method_manager_create(struct wl_display *display);
void
text_input_finish(void);

/* Keyboard focus moved; hand the text inputs of each client their enter and
 * leave events, and activate or deactivate the input method to match. */
void
text_input_handle_focus(struct compositor_view *view);

#endif
