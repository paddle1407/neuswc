/* swc: libswc/xdg_activation.c
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

#include "xdg_activation.h"
#include "compositor.h"
#include "internal.h"
#include "surface.h"
#include "util.h"
#include "window.h"

#include "xdg-activation-v1-server-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * xdg-activation is how one program hands focus to another: a mail client
 * about to open a link asks for a token, passes it to the browser in
 * XDG_ACTIVATION_TOKEN, and the browser presents it back here to raise itself.
 * Without it a click on a link opens a window that never comes forward.
 *
 * The token is the whole security story, so it is random, single use, and does
 * not last: anything that can guess or replay one can steal focus at will.
 */

#define TOKEN_BYTES 16                    /* 128 bits, hex encoded */
#define TOKEN_CHARS (TOKEN_BYTES * 2)
#define TOKEN_LIFETIME_MS 30000
/* A client that asks for tokens and never spends them must not be able to grow
 * this list without bound. */
#define TOKEN_MAX 64

struct activation_token {
	struct wl_resource *resource; /* NULL once committed */
	char token[TOKEN_CHARS + 1];
	struct wl_event_source *expiry;
	bool committed;
	struct wl_list link;
};

static struct wl_list tokens;
static bool initialized;

static void
token_free(struct activation_token *token)
{
	if (token->expiry) {
		wl_event_source_remove(token->expiry);
	}
	wl_list_remove(&token->link);
	free(token);
}

static int
handle_expiry(void *data)
{
	struct activation_token *token = data;

	/* The window it would have raised is no longer the one the user was
	 * thinking about, so let it lapse rather than raise something stale. */
	if (token->resource) {
		wl_resource_set_user_data(token->resource, NULL);
	}
	token_free(token);
	return 0;
}

/* Fill dst with TOKEN_CHARS hex digits of kernel randomness. */
static bool
generate_token(char *dst)
{
	unsigned char bytes[TOKEN_BYTES];
	size_t got = 0;
	ssize_t n;
	int fd;

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	while (got < sizeof(bytes)) {
		n = read(fd, bytes + got, sizeof(bytes) - got);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			close(fd);
			return false;
		}
		got += (size_t)n;
	}
	close(fd);

	for (size_t i = 0; i < sizeof(bytes); ++i) {
		snprintf(dst + i * 2, 3, "%02x", bytes[i]);
	}
	return true;
}

/* --------------------------------------------------------------- token */

static void
token_set_serial(struct wl_client *client, struct wl_resource *resource,
                 uint32_t serial, struct wl_resource *seat)
{
	/* Recorded by compositors that refuse tokens without a recent input
	 * event. charaWC lets any client raise a window it owns, so the serial
	 * carries no extra weight here. */
	(void)client;
	(void)resource;
	(void)serial;
	(void)seat;
}

static void
token_set_app_id(struct wl_client *client, struct wl_resource *resource,
                 const char *app_id)
{
	/* Only useful for startup notification, which charaWC does not draw. */
	(void)client;
	(void)resource;
	(void)app_id;
}

static void
token_set_surface(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *surface)
{
	(void)client;
	(void)resource;
	(void)surface;
}

static void
token_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct activation_token *token = wl_resource_get_user_data(resource);

	(void)client;
	if (!token) {
		return;
	}
	if (token->committed) {
		wl_resource_post_error(resource,
		                       XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
		                       "the activation token has already been used");
		return;
	}

	if (!generate_token(token->token)) {
		wl_resource_post_no_memory(resource);
		return;
	}
	token->committed = true;
	xdg_activation_token_v1_send_done(resource, token->token);
}

static void
destroy_token_resource(struct wl_resource *resource)
{
	struct activation_token *token = wl_resource_get_user_data(resource);

	if (!token) {
		return;
	}
	token->resource = NULL;
	/* An uncommitted token can never be redeemed, so it goes with its
	 * resource. A committed one outlives it: the client is expected to
	 * destroy the object and pass the string on. */
	if (!token->committed) {
		token_free(token);
	}
}

static const struct xdg_activation_token_v1_interface token_impl = {
	.set_serial = token_set_serial,
	.set_app_id = token_set_app_id,
	.set_surface = token_set_surface,
	.commit = token_commit,
	.destroy = destroy_resource,
};

/* ------------------------------------------------------------- activation */

static void
get_activation_token(struct wl_client *client, struct wl_resource *manager,
                     uint32_t id)
{
	struct activation_token *token;
	unsigned count = 0;

	wl_list_for_each(token, &tokens, link) {
		++count;
	}

	if (count >= TOKEN_MAX) {
		/* Drop the oldest rather than refusing: the list is ordered by
		 * creation, and the oldest is the one closest to expiring anyway. */
		token = wl_container_of(tokens.next, token, link);
		if (token->resource) {
			wl_resource_set_user_data(token->resource, NULL);
		}
		token_free(token);
	}

	token = calloc(1, sizeof(*token));
	if (!token) {
		wl_client_post_no_memory(client);
		return;
	}
	token->resource = wl_resource_create(
	    client, &xdg_activation_token_v1_interface,
	    wl_resource_get_version(manager), id);
	if (!token->resource) {
		free(token);
		wl_client_post_no_memory(client);
		return;
	}
	token->expiry =
	    wl_event_loop_add_timer(swc.event_loop, handle_expiry, token);
	if (!token->expiry) {
		wl_resource_destroy(token->resource);
		free(token);
		wl_client_post_no_memory(client);
		return;
	}
	wl_event_source_timer_update(token->expiry, TOKEN_LIFETIME_MS);
	wl_list_insert(tokens.prev, &token->link);
	wl_resource_set_implementation(token->resource, &token_impl, token,
	                               destroy_token_resource);
}

static struct activation_token *
find_token(const char *needle)
{
	struct activation_token *token;

	wl_list_for_each(token, &tokens, link)
	{
		if (token->committed && strcmp(token->token, needle) == 0) {
			return token;
		}
	}
	return NULL;
}

static void
activate(struct wl_client *client, struct wl_resource *manager,
         const char *token_string, struct wl_resource *surface_resource)
{
	struct activation_token *token;
	struct surface *surface;
	struct compositor_view *view;
	struct window *window;

	(void)client;
	(void)manager;

	token = find_token(token_string);
	if (!token) {
		/* An unknown or expired token is not an error: the protocol says to
		 * ignore the request, so that a stale token merely fails to raise. */
		return;
	}

	/* Single use, spent whether or not it names a window we can raise. */
	if (token->resource) {
		wl_resource_set_user_data(token->resource, NULL);
	}
	token_free(token);

	surface = surface_from_resource(surface_resource);
	if (!surface || !surface->view) {
		return;
	}
	view = compositor_view(surface->view);
	if (!view || !view->window) {
		return;
	}
	window = view->window;

	/* The window manager decides what raising means -- it may have to switch
	 * workspace or un-minimize first -- so hand it the same request a taskbar
	 * click produces rather than reaching into the stacking order here. */
	if (window->handler && window->handler->request_activate) {
		window->handler->request_activate(window->handler_data);
	}
}

static const struct xdg_activation_v1_interface activation_impl = {
	.destroy = destroy_resource,
	.get_activation_token = get_activation_token,
	.activate = activate,
};

static void
bind_activation(struct wl_client *client, void *data, uint32_t version,
                uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource =
	    wl_resource_create(client, &xdg_activation_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &activation_impl, NULL, NULL);
}

struct wl_global *
xdg_activation_create(struct wl_display *display)
{
	struct wl_global *global;

	wl_list_init(&tokens);
	global = wl_global_create(display, &xdg_activation_v1_interface, 1, NULL,
	                          &bind_activation);
	if (global) {
		initialized = true;
	}
	return global;
}

void
xdg_activation_finish(void)
{
	struct activation_token *token, *tmp;

	if (!initialized) {
		return;
	}
	wl_list_for_each_safe(token, tmp, &tokens, link)
	{
		if (token->resource) {
			wl_resource_set_user_data(token->resource, NULL);
		}
		token_free(token);
	}
	initialized = false;
}
