/* Real selection handlers and serial/focus policy, with resource transport
 * replaced so no server, socket or desktop session is needed. */
#include "../libswc/input.c"
#include "../libswc/data_device.c"
#define handle_selection_destroy primary_handle_selection_destroy
#include "../libswc/primary_selection.c"
#undef handle_selection_destroy
#include <assert.h>
#include <stdio.h>

struct swc swc;
struct resource {
	void *data;
	unsigned cancelled;
	struct wl_signal destroyed;
};
struct observer { struct wl_listener listener; unsigned changes; };
static unsigned internal_cancelled;

void *wl_resource_get_user_data(struct wl_resource *r) { return ((struct resource *)r)->data; }
void wl_resource_add_destroy_listener(struct wl_resource *r, struct wl_listener *l)
{ wl_signal_add(&((struct resource *)r)->destroyed, l); }
void wl_resource_post_event(struct wl_resource *r, uint32_t opcode, ...)
{ ++((struct resource *)r)->cancelled; }
struct data *data_from_source(struct wl_resource *r)
{ return r ? wl_resource_get_user_data(r) : NULL; }
static void cancel_internal(void *data) { ++internal_cancelled; }
bool data_internal_owner(struct data *data, const struct data_source_impl **impl, void **user)
{
	static const struct data_source_impl owner = { .cancelled=cancel_internal };
	if (!data) return false;
	*impl = &owner; *user = NULL;
	return true;
}
static void changed(struct wl_listener *listener, void *data)
{
	struct observer *observer = wl_container_of(listener, observer, listener);
	++observer->changes;
}
static void init_resource(struct resource *r, void *data)
{
	*r = (struct resource){ .data=data };
	wl_signal_init(&r->destroyed);
}

int main(void)
{
	int focused_tag, background_tag, content;
	struct wl_client *focused = (struct wl_client *)&focused_tag;
	struct wl_client *background = (struct wl_client *)&background_tag;
	struct keyboard keyboard = {0};
	struct swc_seat seat = { .keyboard=&keyboard };
	swc.seat = &seat; keyboard.focus.client = focused;
	input_record_serial(focused, 10);
	input_record_serial(background, 20);
	assert(!input_can_set_selection(NULL, 0));
	assert(!input_can_set_selection(background, 20));
	assert(!input_can_set_selection(focused, 20));
	assert(!input_can_set_selection(focused, 0));
	assert(input_can_set_selection(focused, 10));
	for (uint32_t serial = 30; serial < 38; ++serial) input_record_serial(focused, serial);
	assert(!input_can_set_selection(focused, 10));
	assert(input_can_set_selection(focused, 37));

	struct data_device *clipboard = data_device_create();
	struct primary_selection_device *primary = primary_selection_device_create();
	assert(clipboard && primary);
	struct resource clip_request, primary_request, old_clip, old_primary, next_clip, next_primary;
	init_resource(&clip_request, clipboard); init_resource(&primary_request, primary);
	init_resource(&old_clip, &content); init_resource(&old_primary, NULL);
	init_resource(&next_clip, &content); init_resource(&next_primary, NULL);
	clipboard->selection = (struct wl_resource *)&old_clip;
	clipboard->selection_data = (struct data *)&content;
	primary->selection = (struct wl_resource *)&old_primary;
	wl_signal_add(&old_clip.destroyed, &clipboard->selection_destroy_listener);
	wl_signal_add(&old_primary.destroyed, &primary->selection_destroy_listener);
	struct observer clip_events = { .listener.notify=changed }, primary_events = { .listener.notify=changed };
	wl_signal_add(&clipboard->event_signal, &clip_events.listener);
	wl_signal_add(&primary->event_signal, &primary_events.listener);
	struct wl_client *clients[] = { background, focused, focused };
	uint32_t serials[] = { 20, 10, 20 };
	for (unsigned i = 0; i < 3; ++i) {
		set_selection(clients[i], (struct wl_resource *)&clip_request, NULL, serials[i]);
		device_set_selection(clients[i], (struct wl_resource *)&primary_request, NULL, serials[i]);
		assert(clipboard->selection == (struct wl_resource *)&old_clip);
		assert(primary->selection == (struct wl_resource *)&old_primary);
		assert(!old_clip.cancelled && !old_primary.cancelled);
		assert(!clip_events.changes && !primary_events.changes);
	}
	set_selection(focused, (struct wl_resource *)&clip_request, (struct wl_resource *)&next_clip, 37);
	device_set_selection(focused, (struct wl_resource *)&primary_request, (struct wl_resource *)&next_primary, 37);
	assert(clipboard->selection == (struct wl_resource *)&next_clip && old_clip.cancelled == 1);
	assert(primary->selection == (struct wl_resource *)&next_primary && old_primary.cancelled == 1);
	assert(clip_events.changes == 1 && primary_events.changes == 1);
	set_selection(focused, (struct wl_resource *)&clip_request, NULL, 37);
	device_set_selection(focused, (struct wl_resource *)&primary_request, NULL, 37);
	assert(!clipboard->selection && !clipboard->selection_data && next_clip.cancelled == 1);
	assert(!primary->selection && next_primary.cancelled == 1);
	assert(clip_events.changes == 2 && primary_events.changes == 2);
	/* Clearing must also reach a compositor-owned Xwayland selection. */
	clipboard->selection_data = (struct data *)&content;
	set_selection(focused, (struct wl_resource *)&clip_request, NULL, 37);
	assert(!clipboard->selection_data && internal_cancelled == 1 && clip_events.changes == 3);
	keyboard.focus.client = NULL;
	assert(!input_can_set_selection(focused, 37));
	swc.seat = NULL;
	assert(!input_can_set_selection(focused, 37));
	data_device_destroy(clipboard); primary_selection_device_destroy(primary);
	puts("Selection: focus, serial ownership, expiry, replacement and clearing passed");
	return 0;
}
