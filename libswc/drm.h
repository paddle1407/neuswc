#ifndef SWC_DRM_H
#define SWC_DRM_H

#include "backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_list;
struct wld_buffer;

struct drm_handler {
	void (*page_flip)(struct drm_handler *handler, uint32_t time);
};

struct swc_drm {
	struct swc_backend backend;
	int fd;
	uint32_t cursor_w, cursor_h;
	struct wld_context *context;
	struct wld_renderer *renderer;
};

bool
drm_initialize(void);
void
drm_finalize(void);

bool
drm_create_screens(struct wl_list *screens);

/* Output management {{{ */

struct screen;
/* xf86drmMode.h's drmModeModeInfo and drmModeConnector. Not included here:
 * it includes <drm.h>, which is this file to anything in libswc. */
struct _drmModeModeInfo;
struct _drmModeConnector;

/* What a configuration asks of one connected monitor. */
struct drm_head_config {
	uint32_t connector;
	bool enabled;
	/* One of the connector's own modes; NULL keeps the current one. */
	const struct _drmModeModeInfo *mode;
	bool has_position;
	int32_t x, y;
};

/* Light, turn off and place monitors as asked; monitors not named keep
 * their state. With test, only says whether it could be done. */
bool
drm_configure_heads(const struct drm_head_config *heads, size_t count,
                    bool test);
/* Look at the connectors now rather than wait for a uevent. */
void
drm_rescan(void);
struct screen *
drm_screen_for_connector(uint32_t connector);
void
drm_connector_name(const struct _drmModeConnector *connector, char *name,
                   size_t size);
bool
drm_mode_info_equal(const struct _drmModeModeInfo *a,
                    const struct _drmModeModeInfo *b);

/* }}} */
uint32_t
drm_get_framebuffer(struct wld_buffer *buffer);

#endif
