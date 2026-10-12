/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "capture.h"
#include "toplevels.h"

#define WAIT_MS 5000

static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr;
static struct ext_image_copy_capture_manager_v1 *copy_mgr;

static void r_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d; (void)v;
	if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name))
		list = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
	else if (!strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
		source_mgr = wl_registry_bind(r, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name))
		copy_mgr = wl_registry_bind(r, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
}

static void r_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}

static const struct wl_registry_listener reg_l = { r_global, r_remove };

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* dispatch until fn() is true; -1 on timeout or display error */
static int wait_until(struct wl_display *dpy, int (*fn)(void *), void *arg)
{
	double end = now_ms() + WAIT_MS;

	while (!fn(arg)) {
		struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
		int left = (int)(end - now_ms()), pr;

		while (wl_display_prepare_read(dpy) != 0)
			wl_display_dispatch_pending(dpy);
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				return -1;
			}
			p.events |= POLLOUT;
		}
		pr = left > 0 ? poll(&p, 1, left) : 0;
		if (pr < 0 && errno == EINTR) {
			wl_display_cancel_read(dpy);
			continue;
		}
		if (pr <= 0) {
			wl_display_cancel_read(dpy);
			return -1;
		}
		if (p.revents & POLLIN) {
			if (wl_display_read_events(dpy) < 0)
				return -1;
		} else {
			wl_display_cancel_read(dpy);
		}
		if (wl_display_dispatch_pending(dpy) < 0)
			return -1;
	}
	return 0;
}

struct job {
	char app_id[32], title[32]; /* copied: the toplevel may close while we wait */
	struct capture *c;
	double ms;
};

struct jobs {
	struct job *v;
	int n;
	double t0, total;
};

static int all_done(void *arg)
{
	struct jobs *j = arg;
	int pending = 0;

	for (int i = 0; i < j->n; i++) {
		if (j->v[i].c->state == CAPTURE_PENDING)
			pending = 1;
		else if (!j->v[i].ms)
			j->v[i].ms = now_ms() - j->t0;
	}
	return !pending;
}

static int save(struct capture *c, const char *path)
{
	cairo_surface_t *s = capture_surface(c);
	int ok;

	if (!s)
		return -1;
	ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS ? 0 : -1;
	cairo_surface_destroy(s);
	return ok;
}

static int run(struct wl_display *dpy, struct toplevel **ts, int n, const char *dir, const char *out)
{
	struct jobs j = { calloc((size_t)n, sizeof(struct job)), n, 0, 0 };
	int rc = 0;

	if (!j.v)
		return 1;
	j.t0 = now_ms();
	for (int i = 0; i < n; i++) {
		snprintf(j.v[i].app_id, sizeof(j.v[i].app_id), "%s", ts[i]->app_id ? ts[i]->app_id : "-");
		snprintf(j.v[i].title, sizeof(j.v[i].title), "%s", ts[i]->title ? ts[i]->title : "-");
		j.v[i].c = capture_start(ts[i]->ext);
		if (!j.v[i].c) {
			fprintf(stderr, "out of memory\n");
			while (i--)
				capture_destroy(j.v[i].c);
			free(j.v);
			return 1;
		}
	}
	if (wait_until(dpy, all_done, &j) < 0)
		fprintf(stderr, "timeout or display error while capturing\n");
	j.total = now_ms() - j.t0;
	for (int i = 0; i < n; i++) {
		struct capture *c = j.v[i].c;
		char path[512];

		snprintf(path, sizeof(path), "%s/%d.png", dir ? dir : "", i);
		if (c->state != CAPTURE_READY) {
			printf("%2d %-8s %-16.16s  FAILED (%s %u)\n", i, j.v[i].app_id, j.v[i].title,
				c->state == CAPTURE_PENDING ? "timeout, pending" :
				c->fail_reason >= CAPTURE_ERR_FORMAT ? "internal error" : "compositor reason",
				c->fail_reason);
			rc = 1;
		} else if (save(c, out ? out : path) < 0) {
			fprintf(stderr, "cannot write %s\n", out ? out : path);
			rc = 1;
		} else {
			printf("%2d %-8s %-16.16s  %ux%u  %.1f ms\n", i, j.v[i].app_id, j.v[i].title,
				c->width, c->height, j.v[i].ms);
		}
	}
	if (n > 1)
		printf("total %.1f ms for %d windows\n", j.total, n);
	for (int i = 0; i < n; i++)
		capture_destroy(j.v[i].c);
	free(j.v);
	return rc;
}

static int usage(const char *argv0)
{
	fprintf(stderr, "skippy-wl %s\nusage: %s --capture <identifier> <out.png>\n"
		"       %s --capture-all <dir>\n       %s --version\n", SKIPPY_VERSION, argv0, argv0, argv0);
	return 1;
}

int main(int argc, char **argv)
{
	struct wl_display *dpy;
	struct toplevel *ts[256], *t;
	int n = 0, one = argc == 4 && !strcmp(argv[1], "--capture");

	if (argc == 2 && !strcmp(argv[1], "--version")) {
		printf("skippy-wl %s\n", SKIPPY_VERSION);
		return 0;
	}
	if (!one && !(argc == 3 && !strcmp(argv[1], "--capture-all")))
		return usage(argv[0]);
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot connect to wayland display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!shm || !list || !source_mgr || !copy_mgr) {
		fprintf(stderr, "compositor lacks ext-foreign-toplevel-list, ext-image-capture-source or ext-image-copy-capture\n");
		return 1;
	}
	toplevels_init(list);
	capture_init(shm, source_mgr, copy_mgr);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);

	if (one) {
		t = toplevels_find(argv[2]);
		if (!t) {
			fprintf(stderr, "no toplevel with identifier %s\n", argv[2]);
			return 1;
		}
		ts[n++] = t;
		return run(dpy, ts, n, NULL, argv[3]);
	}
	for (t = toplevels_first(); t && n < 256; t = t->next)
		ts[n++] = t;
	if (t)
		fprintf(stderr, "only capturing the first 256 windows\n");
	if (!n) {
		fprintf(stderr, "no toplevels\n");
		return 1;
	}
	return run(dpy, ts, n, argv[2], NULL);
}
