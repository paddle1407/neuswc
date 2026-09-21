/* swc: libswc/util.c
 *
 * Copyright (c) 2013, 2014 Michael Forney
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

#include "util.h"
#include "internal.h"

#include <wayland-server.h>

pixman_box32_t infinite_extents = {
    .x1 = INT32_MIN,
    .y1 = INT32_MIN,
    .x2 = INT32_MAX,
    .y2 = INT32_MAX,
};

void
remove_resource(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

void
destroy_resource(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

void
orphan_resources(struct wl_list *resources)
{
	struct wl_resource *resource, *tmp;

	wl_resource_for_each_safe(resource, tmp, resources) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
		wl_resource_set_user_data(resource, NULL);
	}
}

/* Retired globals {{{ */

/*
 * How long a withdrawn global stays bindable. A client reads global_remove
 * only when it next dispatches, and one that sent a bind before then must
 * still find the global there, or libwayland disconnects it for naming a
 * global that does not exist.
 */
#define GLOBAL_RETIRE_DELAY_MS 5000

struct retired_global {
	struct wl_global *global;
	struct wl_event_source *timer;
	struct wl_list link;
};

static struct wl_list retired_globals = {&retired_globals, &retired_globals};

static void
retired_global_destroy(struct retired_global *retired)
{
	wl_list_remove(&retired->link);
	if (retired->timer) {
		wl_event_source_remove(retired->timer);
	}
	wl_global_destroy(retired->global);
	free(retired);
}

static int
handle_retire_timeout(void *data)
{
	retired_global_destroy(data);
	return 0;
}

void
global_retire(struct wl_global *global)
{
	struct retired_global *retired;

	/* The object behind it is about to go: a late bind gets an inert
	 * resource rather than a pointer to freed memory. */
	wl_global_set_user_data(global, NULL);
	wl_global_remove(global);

	if (!(retired = malloc(sizeof(*retired)))) {
		wl_global_destroy(global);
		return;
	}
	retired->global = global;
	retired->timer = wl_event_loop_add_timer(swc.event_loop,
	                                         &handle_retire_timeout, retired);
	wl_list_insert(&retired_globals, &retired->link);
	if (!retired->timer ||
	    wl_event_source_timer_update(retired->timer,
	                                 GLOBAL_RETIRE_DELAY_MS) < 0) {
		retired_global_destroy(retired);
	}
}

void
retired_globals_finish(void)
{
	struct retired_global *retired, *tmp;

	wl_list_for_each_safe(retired, tmp, &retired_globals, link)
		retired_global_destroy(retired);
}

/* }}} */

/* Descriptor accounting {{{ */

#include <dirent.h>
#include <errno.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/*
 * Enough kinds that a machine's input devices, which are one kind each, cannot
 * crowd out the one that matters, plus a bucket for whatever still does not
 * fit so the numbers always add up.
 */
#define FD_KINDS 64
#define FD_KINDS_SHOWN 8
#define FD_NEWEST_SHOWN 4

struct fd_kind {
	char name[64];
	unsigned count;
};

static uint64_t
monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec;
}

/*
 * Group descriptors by what they point at rather than by number: the numbers
 * are meaningless, while "33000 of these are anon_inode:sync_file" names the
 * leaking call site directly. Serial numbers inside socket:[...] and pipe:[...]
 * are dropped so those collapse into one line each.
 */
static void
fd_kind_of(const char *target, char *out, size_t size)
{
	static const char *const anonymous[] = {"socket:", "pipe:"};
	size_t i;

	for (i = 0; i < ARRAY_LENGTH(anonymous); ++i) {
		size_t length = strlen(anonymous[i]);

		if (strncmp(target, anonymous[i], length) == 0) {
			snprintf(out, size, "%s[]", anonymous[i]);
			return;
		}
	}
	snprintf(out, size, "%s", target);
}

unsigned
fd_limit(void)
{
	struct rlimit limit;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0 ||
	    limit.rlim_cur == RLIM_INFINITY) {
		return 0;
	}
	return (unsigned)limit.rlim_cur;
}

/*
 * Held open for the life of the process and rewound for each scan.
 *
 * Opening it per scan is what a report is most needed for and least able to do:
 * a process that has run out of descriptors cannot open a directory to find out
 * why, and reports "0 open" at exactly the moment the answer matters.
 */
static DIR *fd_dir;

static bool
fd_dir_ready(void)
{
	if (!fd_dir) {
		fd_dir = opendir("/proc/self/fd");
	} else {
		rewinddir(fd_dir);
	}
	return fd_dir != NULL;
}

/*
 * Counting and classifying share one directory walk. 'kinds' may be NULL when
 * only the total is wanted. 'unclassified' receives the descriptors that found
 * no slot, so a caller can say so rather than quietly dropping them.
 */
static unsigned
fd_scan(struct fd_kind *kinds, unsigned capacity, unsigned *kind_count,
        unsigned *unclassified, int *newest, unsigned newest_capacity)
{
	struct dirent *entry;
	unsigned total = 0, found = 0;
	int self;

	if (kind_count) {
		*kind_count = 0;
	}
	if (unclassified) {
		*unclassified = 0;
	}
	for (; found < newest_capacity; ++found) {
		newest[found] = -1;
	}
	if (!fd_dir_ready()) {
		return 0;
	}
	/* The walk lists the descriptor it is walking with. */
	self = dirfd(fd_dir);

	while ((entry = readdir(fd_dir))) {
		char path[64], target[256], name[64];
		int number;
		ssize_t length;
		unsigned i;

		if (entry->d_name[0] == '.') {
			continue;
		}
		number = atoi(entry->d_name);
		if (number == self) {
			continue;
		}
		++total;

		/* Keep the highest numbers seen: a leak accumulates at the top, so
		 * these name what the process opened most recently. */
		for (i = 0; i < newest_capacity; ++i) {
			if (number > newest[i]) {
				memmove(&newest[i + 1], &newest[i],
				        (newest_capacity - i - 1) * sizeof(*newest));
				newest[i] = number;
				break;
			}
		}

		if (!kinds) {
			continue;
		}
		snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
		length = readlink(path, target, sizeof(target) - 1);
		if (length < 0) {
			if (unclassified) {
				++*unclassified;
			}
			continue;
		}
		target[length] = '\0';
		fd_kind_of(target, name, sizeof(name));
		for (i = 0; i < *kind_count; ++i) {
			if (strcmp(kinds[i].name, name) == 0) {
				++kinds[i].count;
				break;
			}
		}
		if (i < *kind_count) {
			continue;
		}
		if (*kind_count < capacity) {
			snprintf(kinds[i].name, sizeof(kinds[i].name), "%s", name);
			kinds[i].count = 1;
			++*kind_count;
		} else if (unclassified) {
			++*unclassified;
		}
	}

	return total;
}

unsigned
fd_count(void)
{
	int newest[1];

	return fd_scan(NULL, 0, NULL, NULL, newest, 0);
}

void
fd_report(const char *reason)
{
	static uint64_t last_report;
	static unsigned reports;
	struct fd_kind kinds[FD_KINDS];
	int newest[FD_NEWEST_SHOWN];
	unsigned kind_count = 0, unclassified = 0, total, limit, shown, i;
	uint64_t now = monotonic_seconds();

	/* An exhausted process hits its error paths every frame. */
	if (reports >= 16 || (last_report && now - last_report < 10)) {
		return;
	}
	last_report = now;
	++reports;

	total = fd_scan(kinds, ARRAY_LENGTH(kinds), &kind_count, &unclassified,
	                newest, ARRAY_LENGTH(newest));
	limit = fd_limit();
	fprintf(stderr, "WARNING: descriptor report (%s): %u open", reason, total);
	if (limit) {
		fprintf(stderr, " of %u", limit);
	}

	/* Largest first: the leak is the one with an implausible count, and it is
	 * of no help to be told about the four input devices instead. */
	for (shown = 0; shown < FD_KINDS_SHOWN; ++shown) {
		unsigned best = 0;

		for (i = 1; i < kind_count; ++i) {
			if (kinds[i].count > kinds[best].count) {
				best = i;
			}
		}
		if (best >= kind_count || kinds[best].count == 0) {
			break;
		}
		fprintf(stderr, "; %u %s", kinds[best].count, kinds[best].name);
		kinds[best].count = 0;
	}
	if (unclassified) {
		fprintf(stderr, "; %u unclassified", unclassified);
	}

	for (i = 0; i < ARRAY_LENGTH(newest); ++i) {
		char path[64], target[256];
		ssize_t length;

		if (newest[i] < 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/proc/self/fd/%d", newest[i]);
		length = readlink(path, target, sizeof(target) - 1);
		if (length < 0) {
			continue;
		}
		target[length] = '\0';
		fprintf(stderr, "%s[%d]=%s", i == 0 ? "; newest " : " ", newest[i],
		        target);
	}
	fputc('\n', stderr);
}

void
fd_pressure_check(void)
{
	static uint64_t last_sample;
	static unsigned reported_at;
	uint64_t now = monotonic_seconds();
	unsigned total, limit;

	if (last_sample && now - last_sample < 5) {
		return;
	}
	last_sample = now;

	limit = fd_limit();
	if (!limit) {
		return;
	}
	total = fd_count();
	if (total < limit / 2) {
		return;
	}
	/* Report on the way up, not once per sample while it sits there. */
	if (reported_at && total < reported_at + reported_at / 2) {
		return;
	}
	reported_at = total;
	fd_report("descriptor use is over half the limit");
}

/* }}} */
