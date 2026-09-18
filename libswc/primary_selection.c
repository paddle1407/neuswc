/* swc: libswc/primary_selection.c
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

#include "primary_selection.h"
#include "event.h"
#include "internal.h"
#include "seat.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "primary-selection-unstable-v1-server-protocol.h"

/*
 * One offered selection. The source resource owns this; each offer the
 * compositor hands to a client points back at it, and is neutered when the
 * source goes away -- the same lifetime dance data.c documents, for the same
 * reason: a client may legitimately call destroy on an offer whose source has
 * already gone, but must not be able to make us dereference it.
 */
struct primary_source {
	struct wl_array mime_types;
	struct wl_resource *resource;
	struct wl_list offers;
};

/* ------------------------------------------------------------------ offer */

static void
offer_receive(struct wl_client *client, struct wl_resource *resource,
              const char *mime_type, int fd)
{
	struct primary_source *source = wl_resource_get_user_data(resource);

	(void)client;
	/* Guard against an offer outliving its source. */
	if (!source) {
		close(fd);
		return;
	}

	zwp_primary_selection_source_v1_send_send(source->resource, mime_type, fd);
	close(fd);
}

static const struct zwp_primary_selection_offer_v1_interface offer_impl = {
	.receive = offer_receive,
	.destroy = destroy_resource,
};

static struct wl_resource *
offer_new(struct wl_client *client, struct wl_resource *source_resource,
          uint32_t version)
{
	struct primary_source *source = wl_resource_get_user_data(source_resource);
	struct wl_resource *offer;

	offer = wl_resource_create(
	    client, &zwp_primary_selection_offer_v1_interface, version, 0);
	if (!offer) {
		return NULL;
	}
	wl_resource_set_implementation(offer, &offer_impl, source,
	                               &remove_resource);
	wl_list_insert(&source->offers, wl_resource_get_link(offer));

	return offer;
}

/* ----------------------------------------------------------------- source */

static void
source_offer(struct wl_client *client, struct wl_resource *resource,
             const char *mime_type)
{
	struct primary_source *source = wl_resource_get_user_data(resource);
	char *copy, **slot;

	(void)client;
	copy = strdup(mime_type);
	if (!copy) {
		goto error0;
	}
	slot = wl_array_add(&source->mime_types, sizeof(*slot));
	if (!slot) {
		goto error1;
	}
	*slot = copy;
	return;

error1:
	free(copy);
error0:
	wl_resource_post_no_memory(resource);
}

static const struct zwp_primary_selection_source_v1_interface source_impl = {
	.offer = source_offer,
	.destroy = destroy_resource,
};

static void
source_destroy(struct wl_resource *resource)
{
	struct primary_source *source = wl_resource_get_user_data(resource);
	struct wl_resource *offer;
	char **mime_type;

	wl_array_for_each(mime_type, &source->mime_types) free(*mime_type);
	wl_array_release(&source->mime_types);

	/* Offers handed out for this source now point at memory about to be
	 * freed. Destroying them here would make the client's own destroy call
	 * fault, so neuter them instead, exactly as data.c does. */
	wl_resource_for_each(offer, &source->offers)
	{
		wl_resource_set_user_data(offer, NULL);
		wl_resource_set_destructor(offer, NULL);
	}

	free(source);
}

static void
source_send_mime_types(struct wl_resource *source_resource,
                       struct wl_resource *offer)
{
	struct primary_source *source = wl_resource_get_user_data(source_resource);
	char **mime_type;

	wl_array_for_each(mime_type, &source->mime_types)
	    zwp_primary_selection_offer_v1_send_offer(offer, *mime_type);
}

/* ----------------------------------------------------------------- device */

static void
handle_selection_destroy(struct wl_listener *listener, void *data)
{
	struct primary_selection_device *device =
	    wl_container_of(listener, device, selection_destroy_listener);

	(void)data;
	device->selection = NULL;
	send_event(&device->event_signal,
	           PRIMARY_SELECTION_EVENT_SELECTION_CHANGED, NULL);
}

static void
device_set_selection(struct wl_client *client, struct wl_resource *resource,
                     struct wl_resource *source, uint32_t serial)
{
	struct primary_selection_device *device =
	    wl_resource_get_user_data(resource);

	(void)client;
	(void)serial;

	if (source == device->selection) {
		return;
	}

	if (device->selection) {
		zwp_primary_selection_source_v1_send_cancelled(device->selection);
		wl_list_remove(&device->selection_destroy_listener.link);
	}

	device->selection = source;

	if (source) {
		wl_resource_add_destroy_listener(source,
		                                 &device->selection_destroy_listener);
	}

	send_event(&device->event_signal,
	           PRIMARY_SELECTION_EVENT_SELECTION_CHANGED, NULL);
}

static const struct zwp_primary_selection_device_v1_interface device_impl = {
	.set_selection = device_set_selection,
	.destroy = destroy_resource,
};

struct primary_selection_device *
primary_selection_device_create(void)
{
	struct primary_selection_device *device;

	device = malloc(sizeof(*device));
	if (!device) {
		return NULL;
	}
	device->selection = NULL;
	device->selection_destroy_listener.notify = &handle_selection_destroy;
	wl_signal_init(&device->event_signal);
	wl_list_init(&device->resources);

	return device;
}

void
primary_selection_device_destroy(struct primary_selection_device *device)
{
	struct wl_resource *resource, *tmp;

	wl_list_for_each_safe(resource, tmp, &device->resources, link)
	    wl_resource_destroy(resource);
	if (device->selection) {
		wl_list_remove(&device->selection_destroy_listener.link);
	}
	free(device);
}

void
primary_selection_device_offer(struct primary_selection_device *device,
                               struct wl_client *client)
{
	struct wl_resource *resource, *offer = NULL;

	resource = wl_resource_find_for_client(&device->resources, client);
	if (!resource) {
		return;
	}

	if (device->selection) {
		offer = offer_new(client, device->selection,
		                  wl_resource_get_version(resource));
		if (offer) {
			zwp_primary_selection_device_v1_send_data_offer(resource, offer);
			source_send_mime_types(device->selection, offer);
		}
	}

	zwp_primary_selection_device_v1_send_selection(resource, offer);
}

/* ---------------------------------------------------------------- manager */

static void
create_source(struct wl_client *client, struct wl_resource *resource,
              uint32_t id)
{
	struct primary_source *source;

	source = malloc(sizeof(*source));
	if (!source) {
		goto error0;
	}
	wl_array_init(&source->mime_types);
	wl_list_init(&source->offers);

	source->resource = wl_resource_create(
	    client, &zwp_primary_selection_source_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!source->resource) {
		goto error1;
	}
	wl_resource_set_implementation(source->resource, &source_impl, source,
	                               &source_destroy);
	return;

error1:
	free(source);
error0:
	wl_resource_post_no_memory(resource);
}

static void
get_device(struct wl_client *client, struct wl_resource *resource, uint32_t id,
           struct wl_resource *seat_resource)
{
	struct swc_seat *seat = wl_resource_get_user_data(seat_resource);
	struct primary_selection_device *device = seat->primary_selection;
	struct wl_resource *device_resource;

	device_resource = wl_resource_create(
	    client, &zwp_primary_selection_device_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!device_resource) {
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(device_resource, &device_impl, device,
	                               &remove_resource);
	wl_list_insert(&device->resources, wl_resource_get_link(device_resource));

	/* A client binding after a selection was already set still needs it. */
	primary_selection_device_offer(device, client);
}

static const struct zwp_primary_selection_device_manager_v1_interface
    manager_impl = {
        .create_source = create_source,
        .get_device = get_device,
        .destroy = destroy_resource,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(
	    client, &zwp_primary_selection_device_manager_v1_interface, version,
	    id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *
primary_selection_device_manager_create(struct wl_display *display)
{
	return wl_global_create(
	    display, &zwp_primary_selection_device_manager_v1_interface, 1, NULL,
	    &bind_manager);
}
