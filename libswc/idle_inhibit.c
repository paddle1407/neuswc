#include "idle_inhibit.h"
#include "idle_notify.h"
#include "surface.h"
#include "util.h"

#include "idle-inhibit-unstable-v1-server-protocol.h"

#include <stdlib.h>

struct inhibitor {
	struct wl_resource *resource;
	struct wl_listener surface_destroy;
	struct wl_list link;
};

static struct wl_list inhibitors;
static bool inhibitors_ready;

bool
idle_inhibit_active(void)
{
	return inhibitors_ready && !wl_list_empty(&inhibitors);
}

static void inhibitor_resource_destroy(struct wl_resource *resource)
{
	struct inhibitor *inhibitor = wl_resource_get_user_data(resource);
	if (!inhibitor)
		return;
	wl_list_remove(&inhibitor->surface_destroy.link);
	wl_list_remove(&inhibitor->link);
	free(inhibitor);
	idle_notify_inhibit_changed();
}

static void inhibitor_destroy(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
	.destroy = inhibitor_destroy,
};

static void surface_destroyed(struct wl_listener *listener, void *data)
{
	struct inhibitor *inhibitor =
		wl_container_of(listener, inhibitor, surface_destroy);
	(void)data;
	wl_resource_destroy(inhibitor->resource);
}

static void create_inhibitor(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	struct wl_resource *resource;

	(void)manager_resource;
	if (!inhibitor) {
		wl_client_post_no_memory(client);
		return;
	}
	resource = wl_resource_create(client, &zwp_idle_inhibitor_v1_interface, 1,
	                              id);
	if (!resource) {
		free(inhibitor);
		wl_client_post_no_memory(client);
		return;
	}
	inhibitor->resource = resource;
	inhibitor->surface_destroy.notify = surface_destroyed;
	wl_signal_add(&surface->signal.destroy, &inhibitor->surface_destroy);
	wl_list_insert(&inhibitors, &inhibitor->link);
	wl_resource_set_implementation(resource, &inhibitor_impl, inhibitor,
	                               inhibitor_resource_destroy);
	idle_notify_inhibit_changed();
}

static const struct zwp_idle_inhibit_manager_v1_interface manager_impl = {
	.destroy = destroy_resource,
	.create_inhibitor = create_inhibitor,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_idle_inhibit_manager_v1_interface, version, id);
	(void)data;
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *idle_inhibit_manager_create(struct wl_display *display)
{
	wl_list_init(&inhibitors);
	inhibitors_ready = true;
	return wl_global_create(display, &zwp_idle_inhibit_manager_v1_interface, 1,
	                        NULL, bind_manager);
}

void idle_inhibit_manager_finish(void)
{
	struct inhibitor *inhibitor, *tmp;
	wl_list_for_each_safe(inhibitor, tmp, &inhibitors, link)
		wl_resource_destroy(inhibitor->resource);
}
