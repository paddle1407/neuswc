/* swc: xselection.c
 *
 * Joining the X11 CLIPBOARD selection to the Wayland one.
 *
 * The two never meet on their own. An X client that copies talks ICCCM
 * directly to the X client that pastes, and the window manager is not part
 * of that conversation -- which is why X to X has always worked here while
 * X to Wayland did nothing at all. The bridge below makes the window
 * manager play both roles:
 *
 *   X owns the clipboard   -> claim it on the Wayland side with a source
 *                             the compositor answers for itself, converting
 *                             the selection on demand when a Wayland client
 *                             asks to receive it.
 *   Wayland owns it        -> own CLIPBOARD in X, and answer SelectionRequest
 *                             by reading from the Wayland source's pipe.
 *
 * Both directions are asynchronous end to end. Nothing here may block the
 * compositor: a client on either side can be slow, wedged, or gone, so
 * every read and write runs from the event loop and every transfer has an
 * owner that can free it.
 */

#include "xselection.h"
#include "data.h"
#include "data_device.h"
#include "internal.h"
#include "seat.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xfixes.h>

/* One property write has to fit in one request. Anything larger would need
 * INCR on the way out, which is not implemented; such a request is refused
 * rather than silently truncated. */
#define MAX_PROPERTY_SIZE (256 * 1024)
/* A peer that never finishes must not grow the buffer without end. */
#define MAX_TRANSFER_SIZE (16 * 1024 * 1024)

enum {
	ATOM_CLIPBOARD,
	ATOM_TARGETS,
	ATOM_TIMESTAMP,
	ATOM_INCR,
	ATOM_UTF8_STRING,
	ATOM_TEXT,
	ATOM_WL_SELECTION,
	ATOM_TEXT_PLAIN_UTF8,
	ATOM_TEXT_PLAIN,
	ATOM_LAST,
};

static const char *const atom_names[ATOM_LAST] = {
	[ATOM_CLIPBOARD] = "CLIPBOARD",
	[ATOM_TARGETS] = "TARGETS",
	[ATOM_TIMESTAMP] = "TIMESTAMP",
	[ATOM_INCR] = "INCR",
	[ATOM_UTF8_STRING] = "UTF8_STRING",
	[ATOM_TEXT] = "TEXT",
	[ATOM_WL_SELECTION] = "_SWC_SELECTION",
	[ATOM_TEXT_PLAIN_UTF8] = "text/plain;charset=utf-8",
	[ATOM_TEXT_PLAIN] = "text/plain",
};

/* A Wayland client asked to receive the X selection. */
struct incoming {
	struct wl_list link;
	char *mime_type;
	int fd;
	/* Data collected from the X side, and how much has reached the fd. */
	char *data;
	size_t size, sent;
	/* A conversion has been asked for and the reply has not arrived. */
	bool converting;
	bool reading_incr;
	struct wl_event_source *writable;
};

/* An X client asked to receive the Wayland selection. */
struct outgoing {
	struct wl_list link;
	xcb_window_t requestor;
	xcb_atom_t property, target;
	uint32_t time;
	int fd;
	char *data;
	size_t size;
	struct wl_event_source *readable;
};

static struct {
	xcb_connection_t *connection;
	xcb_window_t window;
	xcb_atom_t atoms[ATOM_LAST];
	const xcb_query_extension_reply_t *xfixes;

	/* Set while an X client owns CLIPBOARD and we mirror it to Wayland. */
	struct data *source;
	/* Requests from Wayland clients: the head is the one in flight. */
	struct wl_list incoming;

	/* Set while we own CLIPBOARD on behalf of a Wayland source. */
	bool owning;
	struct wl_list outgoing;

	/* The most recent timestamp seen, for selection ownership. */
	xcb_timestamp_t timestamp;
	bool initialized;
} xs;

static void start_next_incoming(void);

/* ------------------------------------------------------------- helpers */

static struct data_device *
selection_device(void)
{
	return swc.seat ? swc.seat->data_device : NULL;
}

/*
 * X names its text targets with atoms that predate MIME types, so those get
 * translated; everything else an X client advertises is already a MIME type
 * spelled as an atom name.
 */
static xcb_atom_t
atom_for_mime_type(const char *mime_type)
{
	xcb_intern_atom_reply_t *reply;
	xcb_atom_t atom = XCB_ATOM_NONE;

	if (strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
		return xs.atoms[ATOM_UTF8_STRING];
	}
	if (strcmp(mime_type, "text/plain") == 0) {
		return XCB_ATOM_STRING;
	}
	reply = xcb_intern_atom_reply(
	    xs.connection,
	    xcb_intern_atom(xs.connection, 0, strlen(mime_type), mime_type), NULL);
	if (reply) {
		atom = reply->atom;
		free(reply);
	}

	return atom;
}

/* Caller frees. NULL for a target that carries no data of its own. */
static char *
mime_type_for_atom(xcb_atom_t atom)
{
	xcb_get_atom_name_reply_t *reply;
	char *name;
	int length;

	if (atom == xs.atoms[ATOM_UTF8_STRING]) {
		return strdup("text/plain;charset=utf-8");
	}
	if (atom == XCB_ATOM_STRING || atom == xs.atoms[ATOM_TEXT]) {
		return strdup("text/plain");
	}
	if (atom == xs.atoms[ATOM_TARGETS] || atom == xs.atoms[ATOM_TIMESTAMP] ||
	    atom == XCB_ATOM_NONE) {
		return NULL;
	}

	reply = xcb_get_atom_name_reply(
	    xs.connection, xcb_get_atom_name(xs.connection, atom), NULL);
	if (!reply) {
		return NULL;
	}
	length = xcb_get_atom_name_name_length(reply);
	name = xcb_get_atom_name_name(reply);
	/* Only an atom that looks like a MIME type is one. */
	if (length == 0 || !memchr(name, '/', length)) {
		free(reply);
		return NULL;
	}
	name = strndup(name, length);
	free(reply);

	return name;
}

/* ------------------------------------------------- X to Wayland: receive */

static void
incoming_destroy(struct incoming *transfer)
{
	wl_list_remove(&transfer->link);
	if (transfer->writable) {
		wl_event_source_remove(transfer->writable);
	}
	if (transfer->fd >= 0) {
		close(transfer->fd);
	}
	free(transfer->mime_type);
	free(transfer->data);
	free(transfer);
}

static int
handle_writable(int fd, uint32_t mask, void *user)
{
	struct incoming *transfer = user;
	ssize_t written;

	(void)mask;
	while (transfer->sent < transfer->size) {
		written = write(fd, transfer->data + transfer->sent,
		                transfer->size - transfer->sent);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 0; /* come back when there is room */
			}
			break; /* the reader is gone */
		}
		transfer->sent += (size_t)written;
	}

	incoming_destroy(transfer);
	start_next_incoming();

	return 0;
}

/* Hand what we collected to the Wayland client, without blocking on it. */
static void
incoming_deliver(struct incoming *transfer)
{
	if (transfer->size == 0) {
		incoming_destroy(transfer);
		start_next_incoming();
		return;
	}
	transfer->writable = wl_event_loop_add_fd(
	    swc.event_loop, transfer->fd, WL_EVENT_WRITABLE, handle_writable,
	    transfer);
	if (!transfer->writable) {
		incoming_destroy(transfer);
		start_next_incoming();
		return;
	}
	/* Try once straight away; most selections fit in a pipe buffer. */
	handle_writable(transfer->fd, WL_EVENT_WRITABLE, transfer);
}

static bool
incoming_append(struct incoming *transfer, const void *data, size_t size)
{
	char *grown;

	if (size == 0) {
		return true;
	}
	if (transfer->size + size > MAX_TRANSFER_SIZE) {
		return false;
	}
	grown = realloc(transfer->data, transfer->size + size);
	if (!grown) {
		return false;
	}
	memcpy(grown + transfer->size, data, size);
	transfer->data = grown;
	transfer->size += size;

	return true;
}

/*
 * Ask the X owner to convert the selection for the transfer at the head.
 * A loop rather than recursion: a client can queue as many receives as it
 * likes, and every one of them could name a type X cannot produce.
 */
static void
start_next_incoming(void)
{
	while (!wl_list_empty(&xs.incoming)) {
		struct incoming *transfer =
		    wl_container_of(xs.incoming.next, transfer, link);
		xcb_atom_t target;

		if (transfer->converting || transfer->reading_incr) {
			return; /* the head is already under way */
		}
		target = atom_for_mime_type(transfer->mime_type);
		if (target == XCB_ATOM_NONE) {
			/* Nothing on the X side answers to this type. */
			incoming_destroy(transfer);
			continue;
		}
		transfer->converting = true;
		xcb_convert_selection(xs.connection, xs.window,
		                      xs.atoms[ATOM_CLIPBOARD], target,
		                      xs.atoms[ATOM_WL_SELECTION], xs.timestamp);
		xcb_flush(xs.connection);
		return;
	}
}

/* The compositor-owned source: a Wayland client wants the X selection. */
static void
source_send(void *user, const char *mime_type, int fd)
{
	struct incoming *transfer;
	bool idle;

	(void)user;
	if (!xs.initialized || !xs.source) {
		close(fd);
		return;
	}
	transfer = calloc(1, sizeof(*transfer));
	if (!transfer) {
		close(fd);
		return;
	}
	transfer->mime_type = strdup(mime_type);
	if (!transfer->mime_type) {
		free(transfer);
		close(fd);
		return;
	}
	/* The pipe must never stall the compositor. */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	transfer->fd = fd;

	idle = wl_list_empty(&xs.incoming);
	wl_list_insert(xs.incoming.prev, &transfer->link);
	/* One conversion at a time: they all land on the same property. */
	if (idle) {
		start_next_incoming();
	}
}

static void
source_cancelled(void *user)
{
	struct data *source = user;

	/*
	 * Something else became the selection. That something may be a newer
	 * source of ours -- one X copy replacing another -- so only forget
	 * xs.source when it is this one, and never destroy the newcomer.
	 */
	if (xs.source == source) {
		xs.source = NULL;
	}
	data_destroy_internal(source);
}

static const struct data_source_impl source_impl = {
	.send = source_send,
	.cancelled = source_cancelled,
};

/* The X owner answered our TARGETS request: publish what it offers. */
static void
publish_targets(void)
{
	xcb_get_property_reply_t *reply;
	struct data *source;
	xcb_atom_t *targets;
	int count, i;
	bool any = false;

	reply = xcb_get_property_reply(
	    xs.connection,
	    xcb_get_property(xs.connection, 1, xs.window,
	                     xs.atoms[ATOM_WL_SELECTION], XCB_GET_PROPERTY_TYPE_ANY,
	                     0, 4096),
	    NULL);
	if (!reply) {
		return;
	}
	if (reply->type != XCB_ATOM_ATOM) {
		free(reply);
		return;
	}
	source = data_create_internal(&source_impl, NULL);
	if (!source) {
		free(reply);
		return;
	}
	/* So the cancelled hook knows which source it is being told about. */
	data_set_internal_user(source, source);
	targets = xcb_get_property_value(reply);
	count = xcb_get_property_value_length(reply) / sizeof(*targets);
	for (i = 0; i < count; ++i) {
		char *mime_type = mime_type_for_atom(targets[i]);

		if (!mime_type) {
			continue;
		}
		any |= data_add_mime_type(source, mime_type);
		free(mime_type);
	}
	free(reply);

	if (!any) {
		/* Nothing we could hand to a Wayland client. */
		data_destroy_internal(source);
		return;
	}

	/*
	 * Set before publishing, not after: publishing sends a selection-changed
	 * event, and the handler for it has to recognise this source as ours or
	 * it will turn round and claim CLIPBOARD in X for a selection that came
	 * from X. source_cancelled keys off the source it is handed, so the old
	 * one is still dropped correctly.
	 */
	xs.source = source;
	data_device_set_internal_selection(selection_device(), source);
}

static void
handle_selection_notify(xcb_selection_notify_event_t *event)
{
	struct incoming *transfer;
	xcb_get_property_reply_t *reply;

	if (event->property != xs.atoms[ATOM_WL_SELECTION]) {
		return; /* refused, or not ours */
	}
	if (event->target == xs.atoms[ATOM_TARGETS]) {
		publish_targets();
		return;
	}
	if (wl_list_empty(&xs.incoming)) {
		return;
	}
	transfer = wl_container_of(xs.incoming.next, transfer, link);
	transfer->converting = false;

	reply = xcb_get_property_reply(
	    xs.connection,
	    xcb_get_property(xs.connection, 1, xs.window,
	                     xs.atoms[ATOM_WL_SELECTION], XCB_GET_PROPERTY_TYPE_ANY,
	                     0, MAX_TRANSFER_SIZE / 4),
	    NULL);
	if (!reply) {
		incoming_destroy(transfer);
		start_next_incoming();
		return;
	}
	if (reply->type == xs.atoms[ATOM_INCR]) {
		/*
		 * Too large for one property. The owner will now write it in
		 * pieces, each announced by a PropertyNotify, ending with an empty
		 * one. Deleting the property above asked for the first piece.
		 */
		transfer->reading_incr = true;
		free(reply);
		xcb_flush(xs.connection);
		return;
	}
	if (!incoming_append(transfer, xcb_get_property_value(reply),
	                     xcb_get_property_value_length(reply))) {
		free(reply);
		incoming_destroy(transfer);
		start_next_incoming();
		return;
	}
	free(reply);
	incoming_deliver(transfer);
}

/* A chunk of an INCR transfer arrived. */
static void
handle_incr_chunk(void)
{
	struct incoming *transfer;
	xcb_get_property_reply_t *reply;
	int length;

	if (wl_list_empty(&xs.incoming)) {
		return;
	}
	transfer = wl_container_of(xs.incoming.next, transfer, link);
	if (!transfer->reading_incr) {
		return;
	}
	reply = xcb_get_property_reply(
	    xs.connection,
	    xcb_get_property(xs.connection, 1, xs.window,
	                     xs.atoms[ATOM_WL_SELECTION], XCB_GET_PROPERTY_TYPE_ANY,
	                     0, MAX_TRANSFER_SIZE / 4),
	    NULL);
	if (!reply) {
		incoming_destroy(transfer);
		start_next_incoming();
		return;
	}
	length = xcb_get_property_value_length(reply);
	if (length == 0) {
		/* An empty piece ends the transfer. */
		free(reply);
		transfer->reading_incr = false;
		incoming_deliver(transfer);
		return;
	}
	if (!incoming_append(transfer, xcb_get_property_value(reply), length)) {
		free(reply);
		incoming_destroy(transfer);
		start_next_incoming();
		return;
	}
	free(reply);
	xcb_flush(xs.connection); /* the delete asks for the next piece */
}

/* ------------------------------------------------- Wayland to X: serving */

static void
send_selection_notify(xcb_window_t requestor, xcb_atom_t selection,
                      xcb_atom_t target, xcb_atom_t property, uint32_t time)
{
	xcb_selection_notify_event_t notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = time,
		.requestor = requestor,
		.selection = selection,
		.target = target,
		.property = property,
	};

	xcb_send_event(xs.connection, 0, requestor, XCB_EVENT_MASK_NO_EVENT,
	               (const char *)&notify);
	xcb_flush(xs.connection);
}

static void
outgoing_destroy(struct outgoing *transfer)
{
	wl_list_remove(&transfer->link);
	if (transfer->readable) {
		wl_event_source_remove(transfer->readable);
	}
	if (transfer->fd >= 0) {
		close(transfer->fd);
	}
	free(transfer->data);
	free(transfer);
}

static void
outgoing_finish(struct outgoing *transfer, bool ok)
{
	if (ok && transfer->size <= MAX_PROPERTY_SIZE) {
		xcb_change_property(xs.connection, XCB_PROP_MODE_REPLACE,
		                    transfer->requestor, transfer->property,
		                    transfer->target, 8, transfer->size,
		                    transfer->data);
		send_selection_notify(transfer->requestor, xs.atoms[ATOM_CLIPBOARD],
		                      transfer->target, transfer->property,
		                      transfer->time);
	} else {
		/* Refused: either the source failed, or it is larger than one
		 * property and this does not speak INCR on the way out. */
		send_selection_notify(transfer->requestor, xs.atoms[ATOM_CLIPBOARD],
		                      transfer->target, XCB_ATOM_NONE,
		                      transfer->time);
	}
	outgoing_destroy(transfer);
}

static int
handle_readable(int fd, uint32_t mask, void *user)
{
	struct outgoing *transfer = user;
	char buffer[4096];
	ssize_t got;

	(void)mask;
	for (;;) {
		got = read(fd, buffer, sizeof(buffer));
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 0; /* more later */
			}
			outgoing_finish(transfer, false);
			return 0;
		}
		if (got == 0) {
			outgoing_finish(transfer, true); /* the source is done */
			return 0;
		}
		if (transfer->size + (size_t)got > MAX_TRANSFER_SIZE) {
			outgoing_finish(transfer, false);
			return 0;
		}
		char *grown = realloc(transfer->data, transfer->size + (size_t)got);
		if (!grown) {
			outgoing_finish(transfer, false);
			return 0;
		}
		memcpy(grown + transfer->size, buffer, (size_t)got);
		transfer->data = grown;
		transfer->size += (size_t)got;
	}
}

static void
answer_targets(xcb_selection_request_event_t *request, struct data *selection)
{
	struct wl_array *mime_types = data_mime_types(selection);
	xcb_atom_t *atoms;
	unsigned count = 0;
	char **mime_type;

	atoms = calloc(2 + (mime_types ? mime_types->size / sizeof(char *) : 0),
	               sizeof(*atoms));
	if (!atoms) {
		send_selection_notify(request->requestor, request->selection,
		                      request->target, XCB_ATOM_NONE, request->time);
		return;
	}
	atoms[count++] = xs.atoms[ATOM_TARGETS];
	atoms[count++] = xs.atoms[ATOM_TIMESTAMP];
	if (mime_types) {
		wl_array_for_each(mime_type, mime_types)
		{
			xcb_atom_t atom = atom_for_mime_type(*mime_type);

			if (atom != XCB_ATOM_NONE) {
				atoms[count++] = atom;
			}
		}
	}
	xcb_change_property(xs.connection, XCB_PROP_MODE_REPLACE,
	                    request->requestor, request->property, XCB_ATOM_ATOM,
	                    32, count, atoms);
	free(atoms);
	send_selection_notify(request->requestor, request->selection,
	                      request->target, request->property, request->time);
}

static void
handle_selection_request(xcb_selection_request_event_t *request)
{
	struct data *selection = data_device_selection(selection_device());
	struct outgoing *transfer;
	char *mime_type;
	int pipes[2];

	if (request->selection != xs.atoms[ATOM_CLIPBOARD]) {
		return;
	}
	/* An owner must answer even when it cannot: silence hangs the client. */
	if (!xs.owning || !selection || selection == xs.source) {
		send_selection_notify(request->requestor, request->selection,
		                      request->target, XCB_ATOM_NONE, request->time);
		return;
	}
	/* ICCCM allows property None for obsolete clients; use the target. */
	if (request->property == XCB_ATOM_NONE) {
		request->property = request->target;
	}
	if (request->target == xs.atoms[ATOM_TARGETS]) {
		answer_targets(request, selection);
		return;
	}
	if (request->target == xs.atoms[ATOM_TIMESTAMP]) {
		xcb_change_property(xs.connection, XCB_PROP_MODE_REPLACE,
		                    request->requestor, request->property,
		                    XCB_ATOM_INTEGER, 32, 1, &xs.timestamp);
		send_selection_notify(request->requestor, request->selection,
		                      request->target, request->property,
		                      request->time);
		return;
	}

	mime_type = mime_type_for_atom(request->target);
	if (!mime_type) {
		send_selection_notify(request->requestor, request->selection,
		                      request->target, XCB_ATOM_NONE, request->time);
		return;
	}
	if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) < 0) {
		free(mime_type);
		send_selection_notify(request->requestor, request->selection,
		                      request->target, XCB_ATOM_NONE, request->time);
		return;
	}
	transfer = calloc(1, sizeof(*transfer));
	if (!transfer) {
		close(pipes[0]);
		close(pipes[1]);
		free(mime_type);
		send_selection_notify(request->requestor, request->selection,
		                      request->target, XCB_ATOM_NONE, request->time);
		return;
	}
	transfer->requestor = request->requestor;
	transfer->property = request->property;
	transfer->target = request->target;
	transfer->time = request->time;
	transfer->fd = pipes[0];
	wl_list_insert(&xs.outgoing, &transfer->link);

	/* The source writes into the pipe; we answer X once it closes. */
	data_send(selection, mime_type, pipes[1]);
	free(mime_type);

	transfer->readable = wl_event_loop_add_fd(
	    swc.event_loop, transfer->fd, WL_EVENT_READABLE, handle_readable,
	    transfer);
	if (!transfer->readable) {
		outgoing_finish(transfer, false);
	}
}

/* --------------------------------------------------------------- events */

static void
handle_xfixes_selection_notify(xcb_xfixes_selection_notify_event_t *event)
{
	if (event->selection != xs.atoms[ATOM_CLIPBOARD]) {
		return;
	}
	xs.timestamp = event->timestamp;

	if (event->owner == xs.window) {
		return; /* our own claim, made for a Wayland source */
	}
	if (event->owner == XCB_WINDOW_NONE) {
		/* The X owner went away. If the selection was the one we published
		 * for it, there is nothing to offer any more. */
		if (xs.source) {
			data_device_set_internal_selection(selection_device(), NULL);
		}
		return;
	}
	/* Ask what it has before claiming anything on the Wayland side. */
	xcb_convert_selection(xs.connection, xs.window, xs.atoms[ATOM_CLIPBOARD],
	                      xs.atoms[ATOM_TARGETS], xs.atoms[ATOM_WL_SELECTION],
	                      event->timestamp);
	xcb_flush(xs.connection);
}

bool
xselection_handle_event(xcb_generic_event_t *event)
{
	uint8_t type = event->response_type & ~0x80;

	if (!xs.initialized) {
		return false;
	}
	if (xs.xfixes && xs.xfixes->present &&
	    type == xs.xfixes->first_event + XCB_XFIXES_SELECTION_NOTIFY) {
		handle_xfixes_selection_notify(
		    (xcb_xfixes_selection_notify_event_t *)event);
		return true;
	}

	switch (type) {
	case XCB_SELECTION_NOTIFY:
		handle_selection_notify((xcb_selection_notify_event_t *)event);
		return true;
	case XCB_SELECTION_REQUEST: {
		xcb_selection_request_event_t *request =
		    (xcb_selection_request_event_t *)event;

		if (request->time != XCB_CURRENT_TIME) {
			xs.timestamp = request->time;
		}
		handle_selection_request(request);
		return true;
	}
	case XCB_SELECTION_CLEAR: {
		xcb_selection_clear_event_t *clear =
		    (xcb_selection_clear_event_t *)event;

		if (clear->selection == xs.atoms[ATOM_CLIPBOARD]) {
			xs.owning = false;
		}
		return true;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *notify =
		    (xcb_property_notify_event_t *)event;

		xs.timestamp = notify->time;
		/* Only the pieces of an INCR transfer onto our own window. */
		if (notify->window == xs.window &&
		    notify->atom == xs.atoms[ATOM_WL_SELECTION] &&
		    notify->state == XCB_PROPERTY_NEW_VALUE) {
			handle_incr_chunk();
			return true;
		}
		return false; /* the window manager wants the rest */
	}
	default:
		return false;
	}
}

void
xselection_wayland_selection_changed(void)
{
	struct data *selection;

	if (!xs.initialized) {
		return;
	}
	selection = data_device_selection(selection_device());

	/* Our own source going in or out is not something to mirror back. */
	if (selection && selection == xs.source) {
		return;
	}
	if (!selection) {
		if (xs.owning) {
			xcb_set_selection_owner(xs.connection, XCB_WINDOW_NONE,
			                        xs.atoms[ATOM_CLIPBOARD], xs.timestamp);
			xs.owning = false;
			xcb_flush(xs.connection);
		}
		return;
	}
	/* A Wayland client owns the clipboard: stand in for it in X. */
	xcb_set_selection_owner(xs.connection, xs.window, xs.atoms[ATOM_CLIPBOARD],
	                        xs.timestamp);
	xs.owning = true;
	xcb_flush(xs.connection);
}

/* ----------------------------------------------------------- lifecycle */

bool
xselection_initialize(xcb_connection_t *connection, xcb_window_t window)
{
	xcb_intern_atom_cookie_t cookies[ATOM_LAST];
	unsigned i;

	xs.connection = connection;
	xs.window = window;
	xs.timestamp = XCB_CURRENT_TIME;
	wl_list_init(&xs.incoming);
	wl_list_init(&xs.outgoing);

	for (i = 0; i < ATOM_LAST; ++i) {
		cookies[i] = xcb_intern_atom(connection, 0, strlen(atom_names[i]),
		                             atom_names[i]);
	}
	for (i = 0; i < ATOM_LAST; ++i) {
		xcb_intern_atom_reply_t *reply =
		    xcb_intern_atom_reply(connection, cookies[i], NULL);

		if (!reply) {
			ERROR("xselection: could not intern %s\n", atom_names[i]);
			return false;
		}
		xs.atoms[i] = reply->atom;
		free(reply);
	}

	/*
	 * XFixes is the only way to be told that another client took the
	 * selection; without it the X side of the clipboard is invisible.
	 */
	xs.xfixes = xcb_get_extension_data(connection, &xcb_xfixes_id);
	if (!xs.xfixes || !xs.xfixes->present) {
		ERROR("xselection: XFixes is missing; the X clipboard will not be "
		      "shared\n");
		return false;
	}
	free(xcb_xfixes_query_version_reply(
	    connection,
	    xcb_xfixes_query_version(connection, XCB_XFIXES_MAJOR_VERSION,
	                             XCB_XFIXES_MINOR_VERSION),
	    NULL));

	xcb_xfixes_select_selection_input(
	    connection, window, xs.atoms[ATOM_CLIPBOARD],
	    XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER
	        | XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY
	        | XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
	xcb_flush(connection);

	xs.initialized = true;

	return true;
}

void
xselection_finalize(void)
{
	struct incoming *incoming, *incoming_tmp;
	struct outgoing *outgoing, *outgoing_tmp;

	if (!xs.initialized) {
		return;
	}
	wl_list_for_each_safe(incoming, incoming_tmp, &xs.incoming, link)
	    incoming_destroy(incoming);
	wl_list_for_each_safe(outgoing, outgoing_tmp, &xs.outgoing, link)
	    outgoing_destroy(outgoing);
	if (xs.source) {
		/* Drops it from the seat, and source_cancelled frees it. */
		data_device_set_internal_selection(selection_device(), NULL);
		xs.source = NULL;
	}
	xs.owning = false;
	xs.initialized = false;
}
