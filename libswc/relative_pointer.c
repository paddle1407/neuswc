/* swc: libswc/relative_pointer.c
 *
 * zwp_relative_pointer_manager_v1
 *
 * Reports pointer motion as unbounded deltas rather than a position. Games
 * that drive a camera need this: with only absolute motion the pointer stops
 * at the edge of the screen and the camera stops turning with it.
 */

#include "relative_pointer.h"
#include "internal.h"
#include "pointer.h"
#include "util.h"

#include "relative-pointer-unstable-v1-server-protocol.h"

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>

struct relative_pointer {
	struct wl_resource *resource;
	struct pointer *pointer;
	struct wl_listener pointer_destroy;
	struct wl_list link;
};

static struct wl_list relative_pointers = {
	&relative_pointers,
	&relative_pointers,
};

static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
	.destroy = destroy_resource,
};

static void
destroy_relative_pointer(struct wl_resource *resource)
{
	struct relative_pointer *relative = wl_resource_get_user_data(resource);

	wl_list_remove(&relative->link);
	wl_list_remove(&relative->pointer_destroy.link);
	free(relative);
}

static void
handle_pointer_destroy(struct wl_listener *listener, void *data)
{
	struct relative_pointer *relative = wl_container_of(listener, relative, pointer_destroy);
	wl_list_remove(&relative->pointer_destroy.link);
	wl_list_init(&relative->pointer_destroy.link);
	relative->pointer = NULL;
}

static void
get_relative_pointer(struct wl_client *client, struct wl_resource *resource,
                     uint32_t id, struct wl_resource *pointer_resource)
{
	struct relative_pointer *relative;

	if (!(relative = malloc(sizeof(*relative)))) {
		wl_client_post_no_memory(client);
		return;
	}

	relative->resource =
	    wl_resource_create(client, &zwp_relative_pointer_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!relative->resource) {
		free(relative);
		wl_client_post_no_memory(client);
		return;
	}

	relative->pointer = wl_resource_get_user_data(pointer_resource);
	wl_list_init(&relative->pointer_destroy.link);
	if (relative->pointer) {
		relative->pointer_destroy.notify = handle_pointer_destroy;
		wl_signal_add(&relative->pointer->destroy_signal, &relative->pointer_destroy);
	}
	wl_resource_set_implementation(relative->resource, &relative_pointer_impl,
	                               relative, &destroy_relative_pointer);
	wl_list_insert(&relative_pointers, &relative->link);
}

/**
 * Report motion to whichever client currently holds pointer focus.
 *
 * 'time' is in microseconds; the protocol carries it as a 64-bit value split
 * across two 32-bit arguments.
 */
void
relative_pointer_send_motion(struct pointer *pointer, uint64_t time,
                             wl_fixed_t dx, wl_fixed_t dy,
                             wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
	struct relative_pointer *relative;
	bool sent = false;

	if (!pointer->focus.client)
		return;

	wl_list_for_each (relative, &relative_pointers, link) {
		if (relative->pointer != pointer ||
		    wl_resource_get_client(relative->resource) != pointer->focus.client)
			continue;

		zwp_relative_pointer_v1_send_relative_motion(
		    relative->resource, time >> 32, time & 0xffffffff, dx, dy,
		    dx_unaccel, dy_unaccel);
		sent = true;
	}

	/* Relative events are independent of wl_pointer.frame. Keep the existing
	 * frame notification for clients that group physical input that way. */
	if (sent)
		pointer->client_handler.pending = true;
}

static const struct zwp_relative_pointer_manager_v1_interface manager_impl = {
	.destroy = destroy_resource,
	.get_relative_pointer = get_relative_pointer,
};

static void
bind_relative_pointer_manager(struct wl_client *client, void *data,
                              uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(
	    client, &zwp_relative_pointer_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *
relative_pointer_manager_create(struct wl_display *display)
{
	return wl_global_create(display,
	                        &zwp_relative_pointer_manager_v1_interface, 1, NULL,
	                        &bind_relative_pointer_manager);
}
