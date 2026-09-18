#include "foreign_toplevel.h"
#include "compositor.h"
#include "internal.h"
#include "keyboard.h"
#include "output.h"
#include "screen.h"
#include "seat.h"
#include "util.h"
#include "window.h"

#include "wlr-foreign-toplevel-management-unstable-v1-server-protocol.h"

#include <stdlib.h>

struct foreign_manager;
struct foreign_toplevel;

struct foreign_handle {
	struct wl_resource *resource;
	struct foreign_manager *manager;
	struct foreign_toplevel *toplevel;
	struct wl_list manager_link;
	struct wl_list toplevel_link;
};

struct foreign_manager {
	struct wl_resource *resource;
	bool stopped;
	/* Set while foreign_toplevel_manager_finish() is tearing this manager
	 * down, so that freeing its last handle does not free the manager out
	 * from under the loop walking it. workspace.c uses a refcount for the
	 * same hazard. */
	bool finishing;
	struct wl_list handles;
	struct wl_list link;
};

struct foreign_toplevel {
	struct window *window;
	struct wl_list handles;
	struct wl_list link;
};

static struct wl_list managers;
static struct wl_list toplevels;

static void maybe_free_manager(struct foreign_manager *manager)
{
	if (manager->finishing || manager->resource ||
	    !wl_list_empty(&manager->handles))
		return;
	wl_list_remove(&manager->link);
	free(manager);
}

static void handle_resource_destroy(struct wl_resource *resource)
{
	struct foreign_handle *handle = wl_resource_get_user_data(resource);
	struct foreign_manager *manager = handle->manager;
	wl_list_remove(&handle->manager_link);
	if (handle->toplevel)
		wl_list_remove(&handle->toplevel_link);
	free(handle);
	maybe_free_manager(manager);
}

static void manager_resource_destroy(struct wl_resource *resource)
{
	struct foreign_manager *manager = wl_resource_get_user_data(resource);
	manager->resource = NULL;
	maybe_free_manager(manager);
}

static struct foreign_toplevel *foreign_from_window(struct window *window)
{
	return window->foreign_toplevel;
}

static struct wl_resource *parent_resource(struct foreign_handle *handle)
{
	struct foreign_handle *candidate;
	struct swc_window *parent;
	if (!handle->toplevel)
		return NULL;
	parent = handle->toplevel->window->base.parent;
	if (!parent)
		return NULL;
	wl_list_for_each(candidate, &handle->manager->handles, manager_link) {
		if (candidate->toplevel &&
		    &candidate->toplevel->window->base == parent)
			return candidate->resource;
	}
	return NULL;
}

static void send_state(struct foreign_handle *handle)
{
	struct wl_array states;
	struct window *window;
	uint32_t *state;
	if (!handle->toplevel)
		return;
	window = handle->toplevel->window;
	wl_array_init(&states);
#define ADD_STATE(value)                                                       \
	do {                                                                       \
		state = wl_array_add(&states, sizeof(*state));                           \
		if (state)                                                              \
			*state = (value);                                                     \
	} while (0)
	if (window->mode == WINDOW_MODE_TILED)
		ADD_STATE(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED);
	if (window->minimized)
		ADD_STATE(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED);
	if (swc.seat && swc.seat->keyboard &&
	    swc.seat->keyboard->focus.view == window->view)
		ADD_STATE(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED);
	if (window->mode == WINDOW_MODE_FULLSCREEN &&
	    wl_resource_get_version(handle->resource) >= 2)
		ADD_STATE(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN);
#undef ADD_STATE
	zwlr_foreign_toplevel_handle_v1_send_state(handle->resource, &states);
	wl_array_release(&states);
}

static void send_output(struct foreign_handle *handle, struct output *output,
		bool entered)
{
	struct wl_resource *resource;
	if (!handle->toplevel)
		return;
	resource = wl_resource_find_for_client(
		&output->resources, wl_resource_get_client(handle->resource));
	if (!resource)
		return;
	if (entered)
		zwlr_foreign_toplevel_handle_v1_send_output_enter(handle->resource,
		                                                      resource);
	else
		zwlr_foreign_toplevel_handle_v1_send_output_leave(handle->resource,
		                                                      resource);
}

static void send_mask_outputs(struct foreign_handle *handle, uint32_t mask,
		bool entered)
{
	struct screen *screen;
	wl_list_for_each(screen, &swc.screens, link) {
		struct output *output;
		if (!(mask & screen_mask(screen)))
			continue;
		wl_list_for_each(output, &screen->outputs, link)
			send_output(handle, output, entered);
	}
}

/*
 * A charaWC addition to the wlr protocol. The workspace a window is on is the only
 * way a taskbar can tell a window hidden on another workspace from one it is
 * simply not showing, because a hidden window enters no output at all.
 */
static void send_workspace(struct foreign_handle *handle)
{
	if (!handle->toplevel || wl_resource_get_version(handle->resource) < 4)
		return;
	zwlr_foreign_toplevel_handle_v1_send_workspace(
		handle->resource, handle->toplevel->window->workspace);
}

static void request_maximized(struct foreign_handle *handle, bool enabled)
{
	if (handle->toplevel &&
	    handle->toplevel->window->handler->request_maximized)
		handle->toplevel->window->handler->request_maximized(
			handle->toplevel->window->handler_data, enabled);
}

static void set_maximized(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	request_maximized(wl_resource_get_user_data(resource), true);
}

static void unset_maximized(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	request_maximized(wl_resource_get_user_data(resource), false);
}

static void request_minimized(struct foreign_handle *handle, bool enabled)
{
	if (handle->toplevel &&
	    handle->toplevel->window->handler->request_minimized)
		handle->toplevel->window->handler->request_minimized(
			handle->toplevel->window->handler_data, enabled);
}

static void set_minimized(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	request_minimized(wl_resource_get_user_data(resource), true);
}

static void unset_minimized(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	request_minimized(wl_resource_get_user_data(resource), false);
}

static void activate(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat)
{
	struct foreign_handle *handle = wl_resource_get_user_data(resource);
	(void)client;
	(void)seat;
	if (handle->toplevel &&
	    handle->toplevel->window->handler->request_activate)
		handle->toplevel->window->handler->request_activate(
			handle->toplevel->window->handler_data);
}

static void close_toplevel(struct wl_client *client,
		struct wl_resource *resource)
{
	struct foreign_handle *handle = wl_resource_get_user_data(resource);
	(void)client;
	if (handle->toplevel)
		swc_window_close(&handle->toplevel->window->base);
}

static void set_rectangle(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *surface, int32_t x,
		int32_t y, int32_t width, int32_t height)
{
	(void)client;
	(void)surface;
	(void)x;
	(void)y;
	if (width < 0 || height < 0 || (!!width != !!height))
		wl_resource_post_error(
			resource,
			ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_ERROR_INVALID_RECTANGLE,
			"invalid toplevel rectangle %dx%d", width, height);
}

static void destroy_handle(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void request_fullscreen(struct foreign_handle *handle, bool enabled,
		struct wl_resource *output_resource)
{
	struct swc_screen *screen = NULL;
	if (!handle->toplevel ||
	    !handle->toplevel->window->handler->request_fullscreen)
		return;
	if (output_resource) {
		struct output *output = wl_resource_get_user_data(output_resource);
		if (output && output->screen)
			screen = &output->screen->base;
	}
	handle->toplevel->window->handler->request_fullscreen(
		handle->toplevel->window->handler_data, enabled, screen);
}

static void set_fullscreen(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *output)
{
	(void)client;
	request_fullscreen(wl_resource_get_user_data(resource), true, output);
}

static void unset_fullscreen(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	request_fullscreen(wl_resource_get_user_data(resource), false, NULL);
}

static const struct zwlr_foreign_toplevel_handle_v1_interface handle_impl = {
	.set_maximized = set_maximized,
	.unset_maximized = unset_maximized,
	.set_minimized = set_minimized,
	.unset_minimized = unset_minimized,
	.activate = activate,
	.close = close_toplevel,
	.set_rectangle = set_rectangle,
	.destroy = destroy_handle,
	.set_fullscreen = set_fullscreen,
	.unset_fullscreen = unset_fullscreen,
};

static bool create_handle(struct foreign_manager *manager,
		struct foreign_toplevel *toplevel)
{
	struct foreign_handle *handle = calloc(1, sizeof(*handle));
	struct window *window = toplevel->window;
	if (!handle)
		return false;
	handle->resource = wl_resource_create(
		wl_resource_get_client(manager->resource),
		&zwlr_foreign_toplevel_handle_v1_interface,
		wl_resource_get_version(manager->resource), 0);
	if (!handle->resource) {
		free(handle);
		return false;
	}
	handle->manager = manager;
	handle->toplevel = toplevel;
	wl_list_insert(&manager->handles, &handle->manager_link);
	wl_list_insert(&toplevel->handles, &handle->toplevel_link);
	wl_resource_set_implementation(handle->resource, &handle_impl, handle,
	                               handle_resource_destroy);
	zwlr_foreign_toplevel_manager_v1_send_toplevel(manager->resource,
	                                                handle->resource);
	zwlr_foreign_toplevel_handle_v1_send_title(handle->resource,
	                                             window->base.title ?: "");
	zwlr_foreign_toplevel_handle_v1_send_app_id(handle->resource,
	                                              window->base.app_id ?: "");
	send_mask_outputs(handle, window->view->base.screens, true);
	send_state(handle);
	send_workspace(handle);
	if (wl_resource_get_version(handle->resource) >= 3)
		zwlr_foreign_toplevel_handle_v1_send_parent(handle->resource,
		                                                  parent_resource(handle));
	zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	return true;
}

static void manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	struct foreign_manager *manager = wl_resource_get_user_data(resource);
	(void)client;
	if (manager->stopped)
		return;
	manager->stopped = true;
	zwlr_foreign_toplevel_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct zwlr_foreign_toplevel_manager_v1_interface manager_impl = {
	.stop = manager_stop,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct foreign_manager *manager = calloc(1, sizeof(*manager));
	struct foreign_toplevel *toplevel;
	(void)data;
	if (!manager)
		goto no_memory;
	manager->resource = wl_resource_create(
		client, &zwlr_foreign_toplevel_manager_v1_interface, version, id);
	if (!manager->resource) {
		free(manager);
		goto no_memory;
	}
	wl_list_init(&manager->handles);
	wl_list_insert(&managers, &manager->link);
	wl_resource_set_implementation(manager->resource, &manager_impl, manager,
	                               manager_resource_destroy);
	wl_list_for_each(toplevel, &toplevels, link) {
		if (!create_handle(manager, toplevel)) {
			wl_client_post_no_memory(client);
			return;
		}
	}
	return;
no_memory:
	wl_client_post_no_memory(client);
}

struct wl_global *foreign_toplevel_manager_create(struct wl_display *display)
{
	wl_list_init(&managers);
	wl_list_init(&toplevels);
	return wl_global_create(display,
	                        &zwlr_foreign_toplevel_manager_v1_interface, 4,
	                        NULL, bind_manager);
}

void foreign_toplevel_manager_finish(void)
{
	while (!wl_list_empty(&toplevels)) {
		struct foreign_toplevel *toplevel =
			wl_container_of(toplevels.next, toplevel, link);
		foreign_toplevel_window_unmanage(toplevel->window);
	}
	while (!wl_list_empty(&managers)) {
		struct foreign_manager *manager =
			wl_container_of(managers.next, manager, link);
		/*
		 * A client that already destroyed its manager resource leaves the
		 * manager here with resource == NULL and its handles still alive.
		 * Destroying the last of those handles would then free the manager
		 * inside maybe_free_manager, and the loop below would read
		 * manager->handles afterwards. Hold it until we are done.
		 */
		manager->finishing = true;
		while (!wl_list_empty(&manager->handles)) {
			struct foreign_handle *handle =
				wl_container_of(manager->handles.next, handle, manager_link);
			wl_resource_destroy(handle->resource);
		}
		if (manager->resource) {
			zwlr_foreign_toplevel_manager_v1_send_finished(manager->resource);
			wl_resource_destroy(manager->resource);
		}
		wl_list_remove(&manager->link);
		free(manager);
	}
}

void foreign_toplevel_window_manage(struct window *window)
{
	struct foreign_toplevel *toplevel;
	struct foreign_manager *manager;
	if (window->foreign_toplevel)
		return;
	toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel)
		return;
	toplevel->window = window;
	wl_list_init(&toplevel->handles);
	wl_list_insert(&toplevels, &toplevel->link);
	window->foreign_toplevel = toplevel;
	wl_list_for_each(manager, &managers, link) {
		if (!manager->stopped && manager->resource &&
		    !create_handle(manager, toplevel))
			wl_client_post_no_memory(wl_resource_get_client(manager->resource));
	}
}

void foreign_toplevel_window_unmanage(struct window *window)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle, *tmp;
	if (!toplevel)
		return;
	wl_list_for_each_safe(handle, tmp, &toplevel->handles, toplevel_link) {
		zwlr_foreign_toplevel_handle_v1_send_closed(handle->resource);
		wl_list_remove(&handle->toplevel_link);
		wl_list_init(&handle->toplevel_link);
		handle->toplevel = NULL;
	}
	wl_list_remove(&toplevel->link);
	window->foreign_toplevel = NULL;
	free(toplevel);
}

void foreign_toplevel_window_title(struct window *window)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle;
	if (!toplevel)
		return;
	wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
		zwlr_foreign_toplevel_handle_v1_send_title(
			handle->resource, window->base.title ?: "");
		zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	}
}

void foreign_toplevel_window_app_id(struct window *window)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle;
	if (!toplevel)
		return;
	wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
		zwlr_foreign_toplevel_handle_v1_send_app_id(
			handle->resource, window->base.app_id ?: "");
		zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	}
}

void foreign_toplevel_window_parent(struct window *window)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle;
	if (!toplevel)
		return;
	wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
		if (wl_resource_get_version(handle->resource) >= 3)
			zwlr_foreign_toplevel_handle_v1_send_parent(
				handle->resource, parent_resource(handle));
		zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	}
}

void foreign_toplevel_window_state(struct window *window)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle;
	if (!toplevel)
		return;
	wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
		send_state(handle);
		zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	}
}

void foreign_toplevel_window_workspace(struct window *window)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle;
	if (!toplevel)
		return;
	wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
		if (wl_resource_get_version(handle->resource) < 4)
			continue;
		send_workspace(handle);
		zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	}
}

void foreign_toplevel_window_screens(struct window *window, uint32_t entered,
		uint32_t left)
{
	struct foreign_toplevel *toplevel = foreign_from_window(window);
	struct foreign_handle *handle;
	if (!toplevel)
		return;
	wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
		send_mask_outputs(handle, left, false);
		send_mask_outputs(handle, entered, true);
		zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
	}
}

void foreign_toplevel_output_bound(struct output *output,
		struct wl_resource *resource)
{
	struct foreign_toplevel *toplevel;
	if (!toplevels.next)
		return;
	wl_list_for_each(toplevel, &toplevels, link) {
		struct foreign_handle *handle;
		if (!(toplevel->window->view->base.screens &
		      screen_mask(output->screen)))
			continue;
		wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
			if (wl_resource_get_client(handle->resource) ==
			    wl_resource_get_client(resource)) {
				zwlr_foreign_toplevel_handle_v1_send_output_enter(
					handle->resource, resource);
				zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
			}
		}
	}
}

void foreign_toplevel_output_removed(struct output *output)
{
	struct foreign_toplevel *toplevel;
	if (!toplevels.next)
		return;
	wl_list_for_each(toplevel, &toplevels, link) {
		struct foreign_handle *handle;
		if (!(toplevel->window->view->base.screens &
		      screen_mask(output->screen)))
			continue;
		wl_list_for_each(handle, &toplevel->handles, toplevel_link) {
			send_output(handle, output, false);
			zwlr_foreign_toplevel_handle_v1_send_done(handle->resource);
		}
	}
}
