/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "globals.h"
#include "overlay.h"
#include "toplevels.h"
#include "wlutil.h"

#define WAIT_MS 5000

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
	if (display_wait(dpy, all_done, &j, WAIT_MS) < 0)
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
	fprintf(stderr, "skippy-wl %s\nusage: %s --show\n"
		"       %s --capture <identifier> <out.png>\n"
		"       %s --capture-all <dir>\n"
		"       %s --version\n", SKIPPY_VERSION, argv0, argv0, argv0, argv0);
	return 1;
}

static int run_mode(struct globals *g, int show, int one, char **argv)
{
	struct toplevel *ts[256], *t;
	int n = 0;

	toplevels_init(g->list);
	capture_init(g->shm, g->source_mgr, g->copy_mgr);
	wl_display_roundtrip(g->display);
	wl_display_roundtrip(g->display);

	if (show) {
		char *id;
		int rc = overlay_run(g, &id);

		if (id)
			printf("%s\n", id);
		free(id);
		return rc < 0 ? 1 : 0;
	}
	if (one) {
		t = toplevels_find(argv[2]);
		if (!t) {
			fprintf(stderr, "no toplevel with identifier %s\n", argv[2]);
			return 1;
		}
		ts[n++] = t;
		return run(g->display, ts, n, NULL, argv[3]);
	}
	for (t = toplevels_first(); t && n < 256; t = t->next)
		ts[n++] = t;
	if (t)
		fprintf(stderr, "only capturing the first 256 windows\n");
	if (!n) {
		fprintf(stderr, "no toplevels\n");
		return 1;
	}
	return run(g->display, ts, n, argv[2], NULL);
}

int main(int argc, char **argv)
{
	struct globals g;
	int rc, show = argc == 2 && !strcmp(argv[1], "--show");
	int one = argc == 4 && !strcmp(argv[1], "--capture");

	if (argc == 2 && !strcmp(argv[1], "--version")) {
		printf("skippy-wl %s\n", SKIPPY_VERSION);
		return 0;
	}
	if (!show && !one && !(argc == 3 && !strcmp(argv[1], "--capture-all")))
		return usage(argv[0]);
	if (globals_init(&g) < 0) {
		fprintf(stderr, "cannot connect to wayland display\n");
		return 1;
	}
	if (!g.shm || !g.list || !g.source_mgr || !g.copy_mgr) {
		fprintf(stderr, "compositor lacks ext-foreign-toplevel-list, ext-image-capture-source or ext-image-copy-capture\n");
		rc = 1;
	} else if (show && (!g.compositor || !g.layer_shell || !g.seat_name || !g.outputs)) {
		fprintf(stderr, "compositor lacks wlr-layer-shell, a seat (v5+) or an output\n");
		rc = 1;
	} else {
		rc = run_mode(&g, show, one, argv);
	}
	globals_finish(&g);
	return rc;
}
