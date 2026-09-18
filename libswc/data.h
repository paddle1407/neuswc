/* swc: data.h
 *
 * Copyright (c) 2013 Michael Forney
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

#ifndef SWC_DATA_H
#define SWC_DATA_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

struct wl_client;
struct data;

/*
 * A selection normally belongs to a client: the source is that client's
 * wl_data_source, and receive() is answered by sending it an event. The X11
 * clipboard bridge has no client to send to -- the compositor itself holds
 * the data -- so it supplies these instead.
 */
struct data_source_impl {
	/* Write the selection to fd as mime_type. Takes ownership of fd. */
	void (*send)(void *user, const char *mime_type, int fd);
	/* Something else became the selection; this source is no longer it. */
	void (*cancelled)(void *user);
};

#define DATA_DND_ACTION_ALL                   \
	(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY   \
	 | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE \
	 | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)

struct wl_resource *
data_source_new(struct wl_client *client, uint32_t version, uint32_t id);
/* A source the compositor answers for itself, with no client behind it. */
struct data *
data_create_internal(const struct data_source_impl *impl, void *user);
void
data_set_internal_user(struct data *data, void *user);
bool
data_add_mime_type(struct data *data, const char *mime_type);
void
data_destroy_internal(struct data *data);
/* Hand the selection to fd as mime_type, whoever owns it. Takes fd. */
void
data_send(struct data *data, const char *mime_type, int fd);
/* The offered types, as an array of char *. */
struct wl_array *
data_mime_types(struct data *data);
bool
data_internal_owner(struct data *data, const struct data_source_impl **impl,
                    void **user);
struct data *
data_from_source(struct wl_resource *source);
struct wl_resource *
data_offer_new_for(struct wl_client *client, struct data *data,
                   uint32_t version);
void
data_send_mime_types_for(struct data *data, struct wl_resource *offer);
struct wl_resource *
data_offer_new(struct wl_client *client, struct wl_resource *source,
               uint32_t version);
void
data_send_mime_types(struct wl_resource *source, struct wl_resource *offer);
/* The drag-and-drop actions the source said it would take part in. */
uint32_t
data_source_actions(struct wl_resource *source);

#endif
