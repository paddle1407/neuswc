/* swc: libswc/output_management.c
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

/*
 * wlr-output-management-unstable-v1. A head is a connected monitor, lit or
 * not; its modes are the connector's. Heads are kept per client and compared
 * against the connectors on every update, so a client is only told what
 * changed. A head whose mode list changed (another monitor on the same
 * connector) is finished and announced again.
 *
 * Applying goes through drm_configure_heads, which builds a screen again for
 * a new mode or position, just as an unplug and replug would. Transforms,
 * scales other than 1 and adaptive sync are refused: the compositor has none
 * of them.
 */

#include "output_management.h"
#include "drm.h"
#include "internal.h"
#include "screen.h"
#include "util.h"

#include "wlr-output-management-unstable-v1-server-protocol.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct om_manager {
	struct wl_resource *resource;
	struct wl_list heads;
	struct wl_list link;
};

struct om_head {
	struct om_manager *manager;
	struct wl_resource *resource;
	uint32_t connector;
	struct wl_list modes;
	struct wl_list link;

	/* What the client was last told. */
	bool announced;
	bool enabled;
	struct om_mode *current;
	int32_t x, y;
};

struct om_mode {
	struct om_head *head;
	struct wl_resource *resource;
	drmModeModeInfo info;
	struct wl_list link;
};

struct om_config {
	struct wl_resource *resource;
	uint32_t serial;
	bool used;
	struct wl_list heads;
};

struct om_config_head {
	struct om_config *config;
	/* NULL for a disabled head, which gets no object. */
	struct wl_resource *resource;
	uint32_t connector;
	bool enabled;

	bool mode_set;
	bool has_mode;
	drmModeModeInfo mode;
	bool has_custom_mode;
	int32_t custom_width, custom_height, custom_refresh;
	bool position_set;
	int32_t x, y;
	bool transform_set, scale_set, adaptive_sync_set;
	/* Asked for something this compositor cannot do. */
	bool unsupported;
	struct wl_list link;
};

static struct {
	struct wl_list managers;
	uint32_t serial;
	bool initialized;
} om;

/* Snapshot {{{ */

struct snapshot {
	drmModeRes *resources;
	drmModeConnector **connectors;
	int count;
};

static bool
snapshot_take(struct snapshot *snapshot)
{
	int i;

	snapshot->count = 0;
	snapshot->connectors = NULL;
	if (!(snapshot->resources = drmModeGetResources(swc.drm->fd))) {
		return false;
	}
	snapshot->connectors = calloc(snapshot->resources->count_connectors,
	                              sizeof(*snapshot->connectors));
	if (!snapshot->connectors) {
		drmModeFreeResources(snapshot->resources);
		return false;
	}
	snapshot->count = snapshot->resources->count_connectors;
	/* The kernel's last answer: the hotplug code is what probes. */
	for (i = 0; i < snapshot->count; ++i) {
		snapshot->connectors[i] = drmModeGetConnectorCurrent(
		    swc.drm->fd, snapshot->resources->connectors[i]);
	}
	return true;
}

static void
snapshot_release(struct snapshot *snapshot)
{
	int i;

	for (i = 0; i < snapshot->count; ++i)
		drmModeFreeConnector(snapshot->connectors[i]);
	free(snapshot->connectors);
	drmModeFreeResources(snapshot->resources);
}

static drmModeConnector *
snapshot_find(struct snapshot *snapshot, uint32_t id)
{
	int i;

	for (i = 0; i < snapshot->count; ++i) {
		if (snapshot->connectors[i] &&
		    snapshot->connectors[i]->connector_id == id &&
		    snapshot->connectors[i]->connection == DRM_MODE_CONNECTED &&
		    snapshot->connectors[i]->count_modes > 0) {
			return snapshot->connectors[i];
		}
	}
	return NULL;
}

/* }}} */

/* Modes {{{ */

/* In mHz, as the protocol has it; vrefresh alone cannot tell 59.94 from 60. */
static int32_t
mode_refresh(const drmModeModeInfo *info)
{
	uint64_t refresh;

	if (!info->htotal || !info->vtotal) {
		return info->vrefresh * 1000;
	}
	refresh = ((uint64_t)info->clock * 1000000 / info->htotal +
	           info->vtotal / 2) / info->vtotal;
	if (info->flags & DRM_MODE_FLAG_INTERLACE) {
		refresh *= 2;
	}
	if (info->flags & DRM_MODE_FLAG_DBLSCAN) {
		refresh /= 2;
	}
	if (info->vscan > 1) {
		refresh /= info->vscan;
	}
	return (int32_t)refresh;
}

static void
mode_handle_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwlr_output_mode_v1_interface mode_impl = {
    .release = mode_handle_release,
};

static void
mode_free(struct om_mode *mode)
{
	wl_list_remove(&mode->link);
	free(mode);
}

static void
mode_handle_destroy(struct wl_resource *resource)
{
	struct om_mode *mode = wl_resource_get_user_data(resource);

	if (mode) {
		if (mode->head->current == mode) {
			mode->head->current = NULL;
		}
		mode_free(mode);
	}
}

/* }}} */

/* Heads {{{ */

static void
head_handle_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwlr_output_head_v1_interface head_impl = {
    .release = head_handle_release,
};

/* The client's objects stay until it lets go of them, inert. */
static void
head_free(struct om_head *head)
{
	struct om_mode *mode, *tmp;

	wl_list_for_each_safe(mode, tmp, &head->modes, link)
	{
		wl_resource_set_user_data(mode->resource, NULL);
		mode_free(mode);
	}
	if (head->resource) {
		wl_resource_set_user_data(head->resource, NULL);
	}
	wl_list_remove(&head->link);
	free(head);
}

static void
head_handle_destroy(struct wl_resource *resource)
{
	struct om_head *head = wl_resource_get_user_data(resource);

	if (head) {
		head->resource = NULL;
		head_free(head);
	}
}

static void
head_finish(struct om_head *head)
{
	struct om_mode *mode;

	wl_list_for_each(mode, &head->modes, link)
		zwlr_output_mode_v1_send_finished(mode->resource);
	if (head->resource) {
		zwlr_output_head_v1_send_finished(head->resource);
	}
	head_free(head);
}

static bool
head_modes_match(struct om_head *head, const drmModeConnector *connector)
{
	struct om_mode *mode;
	int i = 0;

	wl_list_for_each_reverse(mode, &head->modes, link)
	{
		if (i >= connector->count_modes ||
		    !drm_mode_info_equal(&mode->info, &connector->modes[i])) {
			return false;
		}
		++i;
	}
	return i == connector->count_modes;
}

static struct om_head *
head_create(struct om_manager *manager, const drmModeConnector *connector)
{
	struct wl_client *client = wl_resource_get_client(manager->resource);
	uint32_t version = wl_resource_get_version(manager->resource);
	struct om_head *head;
	struct om_mode *mode;
	char name[32];
	int i;

	if (!(head = calloc(1, sizeof(*head)))) {
		goto error0;
	}
	head->manager = manager;
	head->connector = connector->connector_id;
	wl_list_init(&head->modes);
	wl_list_insert(manager->heads.prev, &head->link);

	head->resource =
	    wl_resource_create(client, &zwlr_output_head_v1_interface, version, 0);
	if (!head->resource) {
		wl_list_remove(&head->link);
		free(head);
		goto error0;
	}
	wl_resource_set_implementation(head->resource, &head_impl, head,
	                               &head_handle_destroy);
	zwlr_output_manager_v1_send_head(manager->resource, head->resource);

	drm_connector_name(connector, name, sizeof(name));
	zwlr_output_head_v1_send_name(head->resource, name);
	zwlr_output_head_v1_send_description(head->resource, name);
	if (connector->mmWidth > 0 && connector->mmHeight > 0) {
		zwlr_output_head_v1_send_physical_size(
		    head->resource, connector->mmWidth, connector->mmHeight);
	}

	for (i = 0; i < connector->count_modes; ++i) {
		if (!(mode = calloc(1, sizeof(*mode)))) {
			goto error1;
		}
		mode->resource = wl_resource_create(
		    client, &zwlr_output_mode_v1_interface,
		    MIN(version, (uint32_t)zwlr_output_mode_v1_interface.version), 0);
		if (!mode->resource) {
			free(mode);
			goto error1;
		}
		mode->head = head;
		mode->info = connector->modes[i];
		/* Kept in reverse, as wl_list_insert at the head builds it. */
		wl_list_insert(&head->modes, &mode->link);
		wl_resource_set_implementation(mode->resource, &mode_impl, mode,
		                               &mode_handle_destroy);
		zwlr_output_head_v1_send_mode(head->resource, mode->resource);
		zwlr_output_mode_v1_send_size(mode->resource, mode->info.hdisplay,
		                              mode->info.vdisplay);
		zwlr_output_mode_v1_send_refresh(mode->resource,
		                                 mode_refresh(&mode->info));
		if (mode->info.type & DRM_MODE_TYPE_PREFERRED) {
			zwlr_output_mode_v1_send_preferred(mode->resource);
		}
	}

	return head;

error1:
	wl_resource_post_no_memory(manager->resource);
	return head;
error0:
	wl_resource_post_no_memory(manager->resource);
	return NULL;
}

static struct om_mode *
head_find_mode(struct om_head *head, const drmModeModeInfo *info)
{
	struct om_mode *mode;

	wl_list_for_each(mode, &head->modes, link)
	{
		if (drm_mode_info_equal(&mode->info, info)) {
			return mode;
		}
	}
	return NULL;
}

/* Sends what differs from what the client knows. Returns whether anything
 * did. */
static bool
head_send_state(struct om_head *head)
{
	struct screen *screen = drm_screen_for_connector(head->connector);
	bool enabled = screen != NULL, all = !head->announced;
	struct om_mode *current;
	bool sent = false;

	if (!head->resource) {
		return false;
	}
	if (all || enabled != head->enabled) {
		zwlr_output_head_v1_send_enabled(head->resource, enabled);
		head->enabled = enabled;
		/* Nothing said while it was off holds any more. */
		all = true;
		sent = true;
	}
	head->announced = true;
	if (!enabled) {
		return sent;
	}

	current = head_find_mode(head, &screen->planes.primary.mode.info);
	if (current && (all || current != head->current)) {
		zwlr_output_head_v1_send_current_mode(head->resource,
		                                      current->resource);
		head->current = current;
		sent = true;
	}
	if (all || screen->base.geometry.x != head->x ||
	    screen->base.geometry.y != head->y) {
		head->x = screen->base.geometry.x;
		head->y = screen->base.geometry.y;
		zwlr_output_head_v1_send_position(head->resource, head->x, head->y);
		sent = true;
	}
	if (all) {
		zwlr_output_head_v1_send_transform(head->resource,
		                                   WL_OUTPUT_TRANSFORM_NORMAL);
		zwlr_output_head_v1_send_scale(head->resource, wl_fixed_from_int(1));
		if (wl_resource_get_version(head->resource) >=
		    ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_SINCE_VERSION) {
			zwlr_output_head_v1_send_adaptive_sync(
			    head->resource,
			    ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
		}
	}
	return sent;
}

static struct om_head *
manager_find_head(struct om_manager *manager, uint32_t connector)
{
	struct om_head *head;

	wl_list_for_each(head, &manager->heads, link)
	{
		if (head->connector == connector) {
			return head;
		}
	}
	return NULL;
}

/* Brings one client up to date. Returns whether it was told anything. */
static bool
manager_sync(struct om_manager *manager, struct snapshot *snapshot)
{
	struct om_head *head, *tmp;
	drmModeConnector *connector;
	bool changed = false;
	int i;

	wl_list_for_each_safe(head, tmp, &manager->heads, link)
	{
		connector = snapshot_find(snapshot, head->connector);
		if (!connector || !head_modes_match(head, connector)) {
			head_finish(head);
			changed = true;
		}
	}
	for (i = 0; i < snapshot->count; ++i) {
		connector = snapshot->connectors[i];
		if (!connector ||
		    !snapshot_find(snapshot, connector->connector_id)) {
			continue;
		}
		if (!(head = manager_find_head(manager, connector->connector_id))) {
			if (!(head = head_create(manager, connector))) {
				continue;
			}
			changed = true;
		}
		changed |= head_send_state(head);
	}
	return changed;
}

void
output_management_update(void)
{
	struct snapshot snapshot;
	struct om_manager *manager;
	bool changed = false;

	if (!om.initialized || wl_list_empty(&om.managers) ||
	    !snapshot_take(&snapshot)) {
		return;
	}
	wl_list_for_each(manager, &om.managers, link)
		changed |= manager_sync(manager, &snapshot);
	snapshot_release(&snapshot);

	/* Configurations made against the old state are cancelled by this. */
	if (changed) {
		if (++om.serial == 0) {
			om.serial = 1;
		}
		wl_list_for_each(manager, &om.managers, link)
			zwlr_output_manager_v1_send_done(manager->resource, om.serial);
	}
}

/* }}} */

/* Configuration heads {{{ */

static struct om_config_head *
config_head_get(struct wl_resource *resource)
{
	return wl_resource_get_user_data(resource);
}

static bool
config_head_check_unset(struct wl_resource *resource, bool *set)
{
	if (*set) {
		wl_resource_post_error(resource,
		                       ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET,
		                       "property has already been set");
		return false;
	}
	*set = true;
	return true;
}

static void
config_head_set_mode(struct wl_client *client, struct wl_resource *resource,
                     struct wl_resource *mode_resource)
{
	struct om_config_head *config_head = config_head_get(resource);
	struct om_mode *mode = wl_resource_get_user_data(mode_resource);

	if (!config_head ||
	    !config_head_check_unset(resource, &config_head->mode_set)) {
		return;
	}
	/* A mode of a head since finished: the serial has moved on, and the
	 * configuration will be cancelled. */
	if (!mode) {
		return;
	}
	if (mode->head->connector != config_head->connector) {
		wl_resource_post_error(resource,
		                       ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE,
		                       "mode does not belong to this head");
		return;
	}
	config_head->has_mode = true;
	config_head->mode = mode->info;
}

static void
config_head_set_custom_mode(struct wl_client *client,
                            struct wl_resource *resource, int32_t width,
                            int32_t height, int32_t refresh)
{
	struct om_config_head *config_head = config_head_get(resource);

	if (!config_head ||
	    !config_head_check_unset(resource, &config_head->mode_set)) {
		return;
	}
	if (width <= 0 || height <= 0 || refresh < 0) {
		wl_resource_post_error(
		    resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE,
		    "invalid custom mode");
		return;
	}
	config_head->has_custom_mode = true;
	config_head->custom_width = width;
	config_head->custom_height = height;
	config_head->custom_refresh = refresh;
}

static void
config_head_set_position(struct wl_client *client, struct wl_resource *resource,
                         int32_t x, int32_t y)
{
	struct om_config_head *config_head = config_head_get(resource);

	if (!config_head ||
	    !config_head_check_unset(resource, &config_head->position_set)) {
		return;
	}
	config_head->x = x;
	config_head->y = y;
}

static void
config_head_set_transform(struct wl_client *client,
                          struct wl_resource *resource, int32_t transform)
{
	struct om_config_head *config_head = config_head_get(resource);

	if (!config_head ||
	    !config_head_check_unset(resource, &config_head->transform_set)) {
		return;
	}
	if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
	    transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(
		    resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM,
		    "invalid transform");
		return;
	}
	if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		config_head->unsupported = true;
	}
}

static void
config_head_set_scale(struct wl_client *client, struct wl_resource *resource,
                      wl_fixed_t scale)
{
	struct om_config_head *config_head = config_head_get(resource);

	if (!config_head ||
	    !config_head_check_unset(resource, &config_head->scale_set)) {
		return;
	}
	if (scale <= 0) {
		wl_resource_post_error(
		    resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE,
		    "invalid scale");
		return;
	}
	if (scale != wl_fixed_from_int(1)) {
		config_head->unsupported = true;
	}
}

static void
config_head_set_adaptive_sync(struct wl_client *client,
                              struct wl_resource *resource, uint32_t state)
{
	struct om_config_head *config_head = config_head_get(resource);

	if (!config_head ||
	    !config_head_check_unset(resource, &config_head->adaptive_sync_set)) {
		return;
	}
	switch (state) {
	case ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED:
		break;
	case ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED:
		config_head->unsupported = true;
		break;
	default:
		wl_resource_post_error(
		    resource,
		    ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE,
		    "invalid adaptive sync state");
		break;
	}
}

static const struct zwlr_output_configuration_head_v1_interface
    config_head_impl = {
        .set_mode = config_head_set_mode,
        .set_custom_mode = config_head_set_custom_mode,
        .set_position = config_head_set_position,
        .set_transform = config_head_set_transform,
        .set_scale = config_head_set_scale,
        .set_adaptive_sync = config_head_set_adaptive_sync,
};

static void
config_head_free(struct om_config_head *config_head)
{
	if (config_head->resource) {
		wl_resource_set_user_data(config_head->resource, NULL);
		/* No destructor request: it goes with its configuration. */
		wl_resource_destroy(config_head->resource);
	}
	wl_list_remove(&config_head->link);
	free(config_head);
}

static void
config_head_handle_destroy(struct wl_resource *resource)
{
	struct om_config_head *config_head = wl_resource_get_user_data(resource);

	if (config_head) {
		config_head->resource = NULL;
	}
}

/* }}} */

/* Configurations {{{ */

static void
config_free_heads(struct om_config *config)
{
	struct om_config_head *config_head, *tmp;

	wl_list_for_each_safe(config_head, tmp, &config->heads, link)
		config_head_free(config_head);
}

static struct om_config_head *
config_add_head(struct om_config *config, struct wl_resource *head_resource,
                bool enabled)
{
	struct om_head *head = wl_resource_get_user_data(head_resource);
	struct om_config_head *config_head;

	if (config->used) {
		wl_resource_post_error(config->resource,
		                       ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
		                       "configuration has already been used");
		return NULL;
	}
	/* A head that has since been finished has no connector to compare:
	 * the serial has moved on, and the configuration will be cancelled. */
	if (head) {
		wl_list_for_each(config_head, &config->heads, link)
		{
			if (config_head->connector == head->connector) {
				wl_resource_post_error(
				    config->resource,
				    ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD,
				    "head has already been configured");
				return NULL;
			}
		}
	}
	if (!(config_head = calloc(1, sizeof(*config_head)))) {
		wl_resource_post_no_memory(config->resource);
		return NULL;
	}
	config_head->config = config;
	config_head->connector = head ? head->connector : 0;
	config_head->enabled = enabled;
	wl_list_insert(config->heads.prev, &config_head->link);
	return config_head;
}

static void
config_enable_head(struct wl_client *client, struct wl_resource *resource,
                   uint32_t id, struct wl_resource *head_resource)
{
	struct om_config *config = wl_resource_get_user_data(resource);
	struct om_config_head *config_head;
	struct wl_resource *config_head_resource;

	config_head_resource =
	    wl_resource_create(client, &zwlr_output_configuration_head_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!config_head_resource) {
		wl_client_post_no_memory(client);
		return;
	}
	config_head = config_add_head(config, head_resource, true);
	wl_resource_set_implementation(config_head_resource, &config_head_impl,
	                               config_head, &config_head_handle_destroy);
	if (config_head) {
		config_head->resource = config_head_resource;
	}
}

static void
config_disable_head(struct wl_client *client, struct wl_resource *resource,
                    struct wl_resource *head_resource)
{
	struct om_config *config = wl_resource_get_user_data(resource);

	config_add_head(config, head_resource, false);
}

/* Some mode of the monitor's that fits; refresh 0 takes any. */
static bool
find_custom_mode(const drmModeConnector *connector,
                 const struct om_config_head *config_head,
                 drmModeModeInfo *found)
{
	const drmModeModeInfo *best = NULL;
	int32_t diff, best_diff = INT32_MAX;
	int i;

	for (i = 0; i < connector->count_modes; ++i) {
		const drmModeModeInfo *info = &connector->modes[i];

		if (info->hdisplay != config_head->custom_width ||
		    info->vdisplay != config_head->custom_height) {
			continue;
		}
		diff = config_head->custom_refresh
		           ? abs(mode_refresh(info) - config_head->custom_refresh)
		           : !(info->type & DRM_MODE_TYPE_PREFERRED);
		if (diff < best_diff) {
			best = info;
			best_diff = diff;
		}
	}
	/* Within 1Hz: rounding differs between tools. */
	if (!best || (config_head->custom_refresh && best_diff > 1000)) {
		return false;
	}
	*found = *best;
	return true;
}

static void
config_finish(struct wl_resource *resource, bool test)
{
	struct om_config *config = wl_resource_get_user_data(resource);
	struct om_config_head *config_head;
	struct drm_head_config *heads = NULL;
	drmModeModeInfo *modes = NULL;
	struct snapshot snapshot;
	drmModeConnector *connector;
	size_t count = 0, i = 0;
	bool ok = false;

	if (config->used) {
		wl_resource_post_error(resource,
		                       ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
		                       "configuration has already been used");
		return;
	}
	config->used = true;

	if (config->serial != om.serial) {
		zwlr_output_configuration_v1_send_cancelled(resource);
		config_free_heads(config);
		return;
	}

	count = wl_list_length(&config->heads);
	if (count && (!(heads = calloc(count, sizeof(*heads))) ||
	              !(modes = calloc(count, sizeof(*modes))))) {
		goto done;
	}
	if (!snapshot_take(&snapshot)) {
		goto done;
	}
	wl_list_for_each(config_head, &config->heads, link)
	{
		struct drm_head_config *head = &heads[i];

		if (config_head->unsupported || !config_head->connector) {
			goto release;
		}
		head->connector = config_head->connector;
		head->enabled = config_head->enabled;
		if (config_head->enabled) {
			if (config_head->has_mode) {
				modes[i] = config_head->mode;
				head->mode = &modes[i];
			} else if (config_head->has_custom_mode) {
				connector = snapshot_find(&snapshot, config_head->connector);
				if (!connector ||
				    !find_custom_mode(connector, config_head, &modes[i])) {
					goto release;
				}
				head->mode = &modes[i];
			}
			head->has_position = config_head->position_set;
			head->x = config_head->x;
			head->y = config_head->y;
		}
		++i;
	}
	ok = drm_configure_heads(heads, count, test);

release:
	snapshot_release(&snapshot);
done:
	free(modes);
	free(heads);
	config_free_heads(config);
	if (ok) {
		zwlr_output_configuration_v1_send_succeeded(resource);
	} else {
		zwlr_output_configuration_v1_send_failed(resource);
	}
}

static void
config_apply(struct wl_client *client, struct wl_resource *resource)
{
	config_finish(resource, false);
}

static void
config_test(struct wl_client *client, struct wl_resource *resource)
{
	config_finish(resource, true);
}

static void
config_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwlr_output_configuration_v1_interface config_impl = {
    .enable_head = config_enable_head,
    .disable_head = config_disable_head,
    .apply = config_apply,
    .test = config_test,
    .destroy = config_destroy,
};

static void
config_handle_destroy(struct wl_resource *resource)
{
	struct om_config *config = wl_resource_get_user_data(resource);

	config_free_heads(config);
	free(config);
}

/* }}} */

/* Manager {{{ */

static void
manager_create_configuration(struct wl_client *client,
                             struct wl_resource *resource, uint32_t id,
                             uint32_t serial)
{
	struct om_config *config;

	if (!(config = calloc(1, sizeof(*config)))) {
		wl_client_post_no_memory(client);
		return;
	}
	config->resource =
	    wl_resource_create(client, &zwlr_output_configuration_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!config->resource) {
		free(config);
		wl_client_post_no_memory(client);
		return;
	}
	config->serial = serial;
	wl_list_init(&config->heads);
	wl_resource_set_implementation(config->resource, &config_impl, config,
	                               &config_handle_destroy);
}

static void
manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	zwlr_output_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct zwlr_output_manager_v1_interface manager_impl = {
    .create_configuration = manager_create_configuration,
    .stop = manager_stop,
};

static void
manager_free(struct om_manager *manager)
{
	struct om_head *head, *tmp;

	wl_list_for_each_safe(head, tmp, &manager->heads, link)
		head_free(head);
	wl_list_remove(&manager->link);
	free(manager);
}

static void
manager_handle_destroy(struct wl_resource *resource)
{
	struct om_manager *manager = wl_resource_get_user_data(resource);

	if (manager) {
		manager_free(manager);
	}
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct om_manager *manager;
	struct snapshot snapshot;

	if (!(manager = calloc(1, sizeof(*manager)))) {
		wl_client_post_no_memory(client);
		return;
	}
	manager->resource =
	    wl_resource_create(client, &zwlr_output_manager_v1_interface, version, id);
	if (!manager->resource) {
		free(manager);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&manager->heads);
	wl_list_init(&manager->link);
	wl_resource_set_implementation(manager->resource, &manager_impl, manager,
	                               &manager_handle_destroy);

	/*
	 * Look at the connectors now. Someone running wlr-randr to find a
	 * monitor that did not come up gets it found, and lit, even if the
	 * uevent for it never arrived.
	 */
	drm_rescan();

	wl_list_insert(&om.managers, &manager->link);
	if (snapshot_take(&snapshot)) {
		manager_sync(manager, &snapshot);
		snapshot_release(&snapshot);
	}
	zwlr_output_manager_v1_send_done(manager->resource, om.serial);
}

struct wl_global *
output_management_create(struct wl_display *display)
{
	struct wl_global *global;

	wl_list_init(&om.managers);
	/* Never 0: wlr-randr takes a done with serial 0 for none at all. */
	om.serial = 1;
	global = wl_global_create(display, &zwlr_output_manager_v1_interface, 4,
	                          NULL, &bind_manager);
	om.initialized = global != NULL;
	return global;
}

void
output_management_finish(void)
{
	struct om_manager *manager, *tmp;

	if (!om.initialized) {
		return;
	}
	wl_list_for_each_safe(manager, tmp, &om.managers, link)
	{
		wl_resource_set_user_data(manager->resource, NULL);
		manager_free(manager);
	}
	om.initialized = false;
}

/* }}} */
