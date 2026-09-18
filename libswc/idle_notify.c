/* swc: libswc/idle_notify.c
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

#include "idle_notify.h"
#include "idle_inhibit.h"
#include "internal.h"
#include "util.h"

#include "ext-idle-notify-v1-server-protocol.h"

#include <stdlib.h>

/*
 * ext-idle-notify lets a client -- a screen locker, a dimmer -- ask to be told
 * when the user has been inactive for a given time, instead of polling. Each
 * notification owns a timer; there is no global idle clock, because two clients
 * asking for 30s and 10 minutes must be told at their own times.
 */
struct idle_notification {
	struct wl_resource *resource;
	struct wl_event_source *timer;
	uint32_t timeout_ms;
	/* From get_input_idle_notification: report raw input inactivity and pay
	 * no attention to idle inhibitors. A dimmer wants the inhibitor-aware
	 * form; something drawing an "away" indicator wants this one. */
	bool ignore_inhibitors;
	bool idled;
	struct wl_list link;
};

static struct wl_list notifications;
static bool initialized;

static void
set_idle(struct idle_notification *notification, bool idled)
{
	if (notification->idled == idled) {
		return;
	}
	notification->idled = idled;
	if (idled) {
		ext_idle_notification_v1_send_idled(notification->resource);
	} else {
		ext_idle_notification_v1_send_resumed(notification->resource);
	}
}

static void
arm(struct idle_notification *notification)
{
	if (!notification->ignore_inhibitors && idle_inhibit_active()) {
		/* An inhibitor holds the session awake, so stop the clock and treat
		 * the user as present for as long as it lasts. */
		wl_event_source_timer_update(notification->timer, 0);
		set_idle(notification, false);
		return;
	}

	if (notification->timeout_ms == 0) {
		/* A zero timeout means idle right away, and a zero-delay timer would
		 * instead disarm the source. */
		set_idle(notification, true);
		return;
	}

	set_idle(notification, false);
	wl_event_source_timer_update(notification->timer,
	                             (int)notification->timeout_ms);
}

static int
handle_timeout(void *data)
{
	struct idle_notification *notification = data;

	set_idle(notification, true);
	return 0;
}

void
idle_notify_activity(void)
{
	struct idle_notification *notification;

	if (!initialized) {
		return;
	}

	wl_list_for_each(notification, &notifications, link) arm(notification);
}

void
idle_notify_inhibit_changed(void)
{
	struct idle_notification *notification;

	if (!initialized) {
		return;
	}

	wl_list_for_each(notification, &notifications, link)
	{
		if (!notification->ignore_inhibitors) {
			arm(notification);
		}
	}
}

static void
destroy_notification(struct wl_resource *resource)
{
	struct idle_notification *notification =
	    wl_resource_get_user_data(resource);

	if (!notification) {
		return;
	}
	if (notification->timer) {
		wl_event_source_remove(notification->timer);
	}
	wl_list_remove(&notification->link);
	free(notification);
}

static const struct ext_idle_notification_v1_interface notification_impl = {
	.destroy = destroy_resource,
};

static void
get_notification(struct wl_client *client, struct wl_resource *manager,
                 uint32_t id, uint32_t timeout, struct wl_resource *seat,
                 bool ignore_inhibitors)
{
	struct idle_notification *notification;

	(void)seat;
	notification = calloc(1, sizeof(*notification));
	if (!notification) {
		wl_client_post_no_memory(client);
		return;
	}

	notification->resource = wl_resource_create(
	    client, &ext_idle_notification_v1_interface,
	    wl_resource_get_version(manager), id);
	if (!notification->resource) {
		free(notification);
		wl_client_post_no_memory(client);
		return;
	}

	notification->timeout_ms = timeout;
	notification->ignore_inhibitors = ignore_inhibitors;
	notification->timer =
	    wl_event_loop_add_timer(swc.event_loop, handle_timeout, notification);
	if (!notification->timer) {
		wl_resource_destroy(notification->resource);
		free(notification);
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_insert(&notifications, &notification->link);
	wl_resource_set_implementation(notification->resource, &notification_impl,
	                               notification, destroy_notification);

	/* The clock starts when the notification is created, not at the next
	 * keypress: a client asking about a 30 second timeout after the user has
	 * already walked away still wants to hear about it. */
	arm(notification);
}

static void
get_idle_notification(struct wl_client *client, struct wl_resource *manager,
                      uint32_t id, uint32_t timeout, struct wl_resource *seat)
{
	get_notification(client, manager, id, timeout, seat, false);
}

static void
get_input_idle_notification(struct wl_client *client,
                            struct wl_resource *manager, uint32_t id,
                            uint32_t timeout, struct wl_resource *seat)
{
	get_notification(client, manager, id, timeout, seat, true);
}

static const struct ext_idle_notifier_v1_interface notifier_impl = {
	.destroy = destroy_resource,
	.get_idle_notification = get_idle_notification,
	.get_input_idle_notification = get_input_idle_notification,
};

static void
bind_notifier(struct wl_client *client, void *data, uint32_t version,
              uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &ext_idle_notifier_v1_interface,
	                              version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &notifier_impl, NULL, NULL);
}

struct wl_global *
idle_notifier_create(struct wl_display *display)
{
	struct wl_global *global;

	wl_list_init(&notifications);
	global = wl_global_create(display, &ext_idle_notifier_v1_interface, 2,
	                          NULL, &bind_notifier);
	if (global) {
		initialized = true;
	}
	return global;
}

void
idle_notifier_finish(void)
{
	struct idle_notification *notification, *tmp;

	if (!initialized) {
		return;
	}
	wl_list_for_each_safe(notification, tmp, &notifications, link)
	    wl_resource_destroy(notification->resource);
	initialized = false;
}
