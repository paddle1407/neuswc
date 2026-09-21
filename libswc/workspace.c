#include "workspace.h"
#include "internal.h"
#include "output.h"
#include "screen.h"
#include "util.h"

#include "ext-workspace-v1-server-protocol.h"

#include <stdio.h>
#include <stdlib.h>

#define WORKSPACE_COUNT 9

/*
 * Each screen owns an independent set of numbered workspaces, published as one
 * ext_workspace_group_handle_v1 per screen. A panel therefore sees which
 * workspace is active on the monitor it is drawn on, rather than one shared
 * state for the whole desktop.
 */

struct workspace_manager;
struct workspace_group;

struct workspace_ref {
	struct workspace_group *group;
	struct wl_resource *resource;
	uint32_t index;
};

struct workspace_group {
	struct workspace_manager *manager;
	struct screen *screen;
	struct wl_resource *resource;
	struct workspace_ref workspaces[WORKSPACE_COUNT];
	int32_t pending_active;
	unsigned live_resources;
	struct wl_list link;
};

struct workspace_manager {
	struct wl_resource *resource;
	struct wl_list groups;
	unsigned live_resources;
	bool stopped;
	struct wl_list link;
};

static struct wl_list managers;

static void maybe_free_manager(struct workspace_manager *manager)
{
	if (manager->live_resources)
		return;
	wl_list_remove(&manager->link);
	free(manager);
}

static void maybe_free_group(struct workspace_group *group)
{
	struct workspace_manager *manager = group->manager;
	if (group->live_resources)
		return;
	wl_list_remove(&group->link);
	free(group);
	--manager->live_resources;
	maybe_free_manager(manager);
}

static void manager_resource_destroy(struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	manager->resource = NULL;
	--manager->live_resources;
	maybe_free_manager(manager);
}

static void group_resource_destroy(struct wl_resource *resource)
{
	struct workspace_group *group = wl_resource_get_user_data(resource);
	group->resource = NULL;
	--group->live_resources;
	maybe_free_group(group);
}

static void workspace_resource_destroy(struct wl_resource *resource)
{
	struct workspace_ref *workspace = wl_resource_get_user_data(resource);
	struct workspace_group *group = workspace->group;
	workspace->resource = NULL;
	--group->live_resources;
	maybe_free_group(group);
}

static void group_create_workspace(struct wl_client *client,
		struct wl_resource *resource, const char *name)
{
	(void)client;
	(void)resource;
	(void)name;
}

static void group_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_workspace_group_handle_v1_interface group_impl = {
	.create_workspace = group_create_workspace,
	.destroy = group_destroy,
};

static void workspace_destroy(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void workspace_activate(struct wl_client *client,
		struct wl_resource *resource)
{
	struct workspace_ref *workspace = wl_resource_get_user_data(resource);
	(void)client;
	if (!workspace->group->manager->stopped)
		workspace->group->pending_active = (int32_t)workspace->index;
}

static void workspace_ignore(struct wl_client *client,
		struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void workspace_assign(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *group)
{
	(void)client;
	(void)resource;
	(void)group;
}

static const struct ext_workspace_handle_v1_interface workspace_impl = {
	.destroy = workspace_destroy,
	.activate = workspace_activate,
	.deactivate = workspace_ignore,
	.assign = workspace_assign,
	.remove = workspace_ignore,
};

/*
 * Tear a group down. 'announce' sends the protocol removal events first, which
 * is what a client needs when the group's screen goes away; a client that is
 * shutting down or has already stopped the manager does not.
 */
static void destroy_group_resources(struct workspace_group *group, bool announce)
{
	unsigned i;
	announce = announce && !group->manager->stopped;
	for (i = 0; i < WORKSPACE_COUNT; ++i) {
		if (!group->workspaces[i].resource)
			continue;
		if (announce)
			ext_workspace_handle_v1_send_removed(group->workspaces[i].resource);
		wl_resource_destroy(group->workspaces[i].resource);
	}
	if (group->resource) {
		if (announce)
			ext_workspace_group_handle_v1_send_removed(group->resource);
		wl_resource_destroy(group->resource);
	}
}

static void destroy_manager_resources(struct workspace_manager *manager)
{
	struct workspace_group *group, *next;
	/* Hold the manager alive across the teardown. Its last group going away
	 * would otherwise free the struct holding the list head being walked, and
	 * the manager resource read afterwards. */
	++manager->live_resources;
	wl_list_for_each_safe(group, next, &manager->groups, link)
		destroy_group_resources(group, false);
	if (manager->resource)
		wl_resource_destroy(manager->resource);
	--manager->live_resources;
	maybe_free_manager(manager);
}

/* A screen is still live only while swc lists it. */
static bool screen_is_live(const struct screen *screen)
{
	struct screen *item;
	wl_list_for_each(item, &swc.screens, link) {
		if (item == screen)
			return true;
	}
	return false;
}

static void manager_commit(struct wl_client *client,
		struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	struct workspace_group *group, *next;
	(void)client;
	wl_list_for_each_safe(group, next, &manager->groups, link) {
		int32_t index = group->pending_active;
		group->pending_active = -1;
		if (index < 0 || index >= WORKSPACE_COUNT)
			continue;
		if (!swc.manager->workspace_activate || !screen_is_live(group->screen))
			continue;
		swc.manager->workspace_activate(&group->screen->base,
		                                (uint32_t)index + 1);
	}
}

static void manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	(void)client;
	if (manager->stopped)
		return;
	manager->stopped = true;
	ext_workspace_manager_v1_send_finished(resource);
	destroy_manager_resources(manager);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = manager_commit,
	.stop = manager_stop,
};

static void send_output(struct workspace_group *group,
		struct wl_resource *output_resource, bool entered)
{
	if (!group->resource || group->manager->stopped ||
	    wl_resource_get_client(group->resource) !=
	        wl_resource_get_client(output_resource))
		return;
	if (entered)
		ext_workspace_group_handle_v1_send_output_enter(group->resource,
		                                                output_resource);
	else
		ext_workspace_group_handle_v1_send_output_leave(group->resource,
		                                                output_resource);
}

static struct workspace_group *find_group(struct workspace_manager *manager,
		const struct screen *screen)
{
	struct workspace_group *group;
	wl_list_for_each(group, &manager->groups, link) {
		if (group->screen == screen)
			return group;
	}
	return NULL;
}

/* Publish one group, with its own numbered workspaces, for a single screen. */
static bool create_group(struct workspace_manager *manager,
		struct wl_client *client, uint32_t version, struct screen *screen)
{
	struct workspace_group *group = calloc(1, sizeof(*group));
	struct output *output;
	unsigned i;

	if (!group)
		return false;
	group->manager = manager;
	group->screen = screen;
	group->pending_active = -1;
	group->resource = wl_resource_create(
		client, &ext_workspace_group_handle_v1_interface, version, 0);
	if (!group->resource) {
		free(group);
		return false;
	}
	group->live_resources = 1;
	wl_list_insert(manager->groups.prev, &group->link);
	++manager->live_resources;
	wl_resource_set_implementation(group->resource, &group_impl, group,
	                               group_resource_destroy);
	ext_workspace_manager_v1_send_workspace_group(manager->resource,
	                                              group->resource);
	ext_workspace_group_handle_v1_send_capabilities(group->resource, 0);

	wl_list_for_each(output, &screen->outputs, link) {
		struct wl_resource *output_resource =
			wl_resource_find_for_client(&output->resources, client);
		if (output_resource)
			send_output(group, output_resource, true);
	}

	for (i = 0; i < WORKSPACE_COUNT; ++i) {
		struct workspace_ref *workspace = &group->workspaces[i];
		struct wl_array coordinates;
		uint32_t *coordinate;
		const char *name = swc_screen_get_name(&screen->base);
		char label[12], stable_id[64];

		workspace->group = group;
		workspace->index = i;
		workspace->resource = wl_resource_create(
			client, &ext_workspace_handle_v1_interface, version, 0);
		if (!workspace->resource)
			return false;
		++group->live_resources;
		wl_resource_set_implementation(workspace->resource, &workspace_impl,
		                               workspace, workspace_resource_destroy);
		ext_workspace_manager_v1_send_workspace(manager->resource,
		                                        workspace->resource);
		snprintf(label, sizeof(label), "%u", i + 1);
		/* The id has to stay unique across groups, so it carries the
		 * connector name of the screen the workspace belongs to. */
		snprintf(stable_id, sizeof(stable_id), "charawc-workspace-%s-%u",
		         name ? name : "screen", i + 1);
		ext_workspace_handle_v1_send_id(workspace->resource, stable_id);
		ext_workspace_handle_v1_send_name(workspace->resource, label);
		wl_array_init(&coordinates);
		coordinate = wl_array_add(&coordinates, sizeof(*coordinate));
		if (coordinate) {
			*coordinate = i;
			ext_workspace_handle_v1_send_coordinates(workspace->resource,
			                                         &coordinates);
		}
		wl_array_release(&coordinates);
		ext_workspace_handle_v1_send_state(
			workspace->resource,
			i + 1 == screen->active_workspace ?
				EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0);
		ext_workspace_handle_v1_send_capabilities(
			workspace->resource,
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		ext_workspace_group_handle_v1_send_workspace_enter(
			group->resource, workspace->resource);
	}
	return true;
}

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct workspace_manager *manager = calloc(1, sizeof(*manager));
	struct screen *screen;
	(void)data;
	if (!manager) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&manager->groups);
	manager->resource = wl_resource_create(
		client, &ext_workspace_manager_v1_interface, version, id);
	if (!manager->resource) {
		free(manager);
		wl_client_post_no_memory(client);
		return;
	}
	manager->live_resources = 1;
	wl_list_insert(&managers, &manager->link);
	wl_resource_set_implementation(manager->resource, &manager_impl, manager,
	                               manager_resource_destroy);

	wl_list_for_each(screen, &swc.screens, link) {
		if (!create_group(manager, client, version, screen)) {
			wl_client_post_no_memory(client);
			destroy_manager_resources(manager);
			return;
		}
	}
	ext_workspace_manager_v1_send_done(manager->resource);
}

struct wl_global *workspace_manager_create(struct wl_display *display)
{
	wl_list_init(&managers);
	return wl_global_create(display, &ext_workspace_manager_v1_interface, 1,
	                        NULL, bind_manager);
}

void workspace_manager_finish(void)
{
	while (!wl_list_empty(&managers)) {
		struct workspace_manager *manager =
			wl_container_of(managers.next, manager, link);
		destroy_manager_resources(manager);
	}
}

void workspace_output_bound(struct output *output,
		struct wl_resource *resource)
{
	struct workspace_manager *manager;
	struct workspace_group *group;
	if (!managers.next)
		return;
	wl_list_for_each(manager, &managers, link) {
		group = find_group(manager, output->screen);
		if (group)
			send_output(group, resource, true);
	}
}

void workspace_output_removed(struct output *output)
{
	struct workspace_manager *manager;
	struct workspace_group *group;
	struct wl_resource *resource;
	if (!managers.next)
		return;
	wl_list_for_each(resource, &output->resources, link) {
		wl_list_for_each(manager, &managers, link) {
			group = find_group(manager, output->screen);
			if (group)
				send_output(group, resource, false);
		}
	}
}

void workspace_screen_added(struct screen *screen)
{
	struct workspace_manager *manager, *manager_next;
	if (!managers.next)
		return;
	wl_list_for_each_safe(manager, manager_next, &managers, link) {
		if (!manager->resource || manager->stopped ||
		    find_group(manager, screen))
			continue;
		++manager->live_resources;
		if (!create_group(manager, wl_resource_get_client(manager->resource),
		                  wl_resource_get_version(manager->resource), screen))
			wl_client_post_no_memory(
				wl_resource_get_client(manager->resource));
		else
			ext_workspace_manager_v1_send_done(manager->resource);
		--manager->live_resources;
		maybe_free_manager(manager);
	}
}

void workspace_screen_removed(struct screen *screen)
{
	struct workspace_manager *manager, *manager_next;
	struct workspace_group *group;
	if (!managers.next)
		return;
	wl_list_for_each_safe(manager, manager_next, &managers, link) {
		group = find_group(manager, screen);
		if (!group)
			continue;
		++manager->live_resources;
		destroy_group_resources(group, true);
		if (manager->resource && !manager->stopped)
			ext_workspace_manager_v1_send_done(manager->resource);
		--manager->live_resources;
		maybe_free_manager(manager);
	}
}

EXPORT void swc_workspace_set_active(struct swc_screen *base, uint32_t workspace)
{
	struct screen *screen = (struct screen *)base;
	struct workspace_manager *manager;
	struct workspace_group *group;
	unsigned i;

	if (!screen || workspace < 1 || workspace > WORKSPACE_COUNT)
		return;
	screen->active_workspace = workspace;
	/* Screens exist before the workspace global does, so an early call has no
	 * manager list to walk; the state it records still reaches every later
	 * bind. */
	if (!managers.next)
		return;
	wl_list_for_each(manager, &managers, link) {
		if (manager->stopped || !manager->resource)
			continue;
		group = find_group(manager, screen);
		if (!group)
			continue;
		for (i = 0; i < WORKSPACE_COUNT; ++i) {
			if (group->workspaces[i].resource)
				ext_workspace_handle_v1_send_state(
					group->workspaces[i].resource,
					i + 1 == workspace ?
						EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0);
		}
		ext_workspace_manager_v1_send_done(manager->resource);
	}
}
