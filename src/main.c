/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "daemon.h"
#include "filter.h"
#include "ipc.h"
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
	fprintf(stderr, "skippy-wl %s\nusage: %s --daemon\n"
		"       %s --toggle | --show [--app-id LIST] [--title LIST] [--status LIST]\n"
		"                          [--next | --prev]\n"
		"       %s --show --multi-select [--app-id LIST] [--title LIST] [--status LIST]\n"
		"       %s --switch [--app-id LIST] [--title LIST] [--status LIST]\n"
		"       %s --quit\n"
		"       %s --capture <identifier> <out.png>\n"
		"       %s --capture-all <dir>\n"
		"       %s --version\n", SKIPPY_VERSION, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
	return 1;
}

enum mode { MODE_SHOW, MODE_DAEMON, MODE_CAPTURE, MODE_CAPTURE_ALL };

static int run_mode(struct globals *g, enum mode mode, char **argv, const struct filter *filter, int dir,
	int multi)
{
	struct toplevel *ts[256], *t;
	int n = 0;

	toplevels_init(g->list, g->toplevel_mgr);
	capture_init(g->shm, g->source_mgr, g->copy_mgr);
	wl_display_roundtrip(g->display);
	wl_display_roundtrip(g->display);

	if (mode == MODE_DAEMON)
		return daemon_run(g);
	if (mode == MODE_SHOW) {
		int rc = overlay_run(g, filter, dir, multi);

		return rc < 0 ? 1 : rc;
	}
	if (mode == MODE_CAPTURE) {
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
	struct filter filter;
	enum mode mode;
	int rc, dir = 0, multi = 0;

	memset(&filter, 0, sizeof(filter));

	if (argc == 2 && !strcmp(argv[1], "--version")) {
		printf("skippy-wl %s\n", SKIPPY_VERSION);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "--daemon")) {
		mode = MODE_DAEMON;
	} else if (argc >= 2 && (!strcmp(argv[1], "--show") || !strcmp(argv[1], "--toggle") ||
			!strcmp(argv[1], "--switch"))) {
		char cmd[IPC_CMD_LEN];
		size_t len = (size_t)snprintf(cmd, sizeof(cmd), "%s", argv[1] + 2);

		for (int i = 2; i < argc; i += 2) {
			const char *key, *val;
			int n;

			if (!strcmp(argv[i], "--next") || !strcmp(argv[i], "--prev")) {
				if (!strcmp(argv[1], "--switch")) {
					fprintf(stderr, "skippy-wl: --switch does not take %s\n", argv[i]);
					return 1;
				}
				dir = argv[i][2] == 'n' ? 1 : -1;
				n = snprintf(cmd + len, sizeof(cmd) - len, "\tdir=%s", argv[i] + 2);
				if (n < 0 || (size_t)n >= sizeof(cmd) - len)
					return usage(argv[0]);
				len += (size_t)n;
				i--;
				continue;
			}

			if (!strcmp(argv[i], "--multi-select")) {
				if (strcmp(argv[1], "--show")) {
					fprintf(stderr, "skippy-wl: --multi-select needs --show\n");
					return 1;
				}
				multi = 1;
				i--;
				continue;
			}

			if (strncmp(argv[i], "--", 2)) {
				fprintf(stderr, "skippy-wl: unexpected argument '%s'\n", argv[i]);
				return usage(argv[0]);
			}
			key = argv[i] + 2;
			if (i + 1 >= argc) {
				fprintf(stderr, "skippy-wl: %s needs a value\n", argv[i]);
				return 1;
			}
			val = argv[i + 1];
			if (strpbrk(val, "\t\r\n")) {
				fprintf(stderr, "skippy-wl: %s: tabs and line breaks are not allowed\n", argv[i]);
				return 1;
			}
			if (filter_set(&filter, key, val) < 0) {
				fprintf(stderr, "skippy-wl: invalid %s: unknown option, unknown status name or value too long\n",
					argv[i]);
				return 1;
			}
			n = snprintf(cmd + len, sizeof(cmd) - len, "\t%s=%s", key, val);
			if (n < 0 || (size_t)n >= sizeof(cmd) - len) {
				fprintf(stderr, "skippy-wl: filters too long\n");
				return 1;
			}
			len += (size_t)n;
		}
		if (multi && dir) {
			fprintf(stderr, "skippy-wl: --multi-select does not take --next or --prev\n");
			return 1;
		}
		/* the picks go to this process's stdout, so the daemon cannot run it */
		int sent = multi ? -1 : ipc_send(cmd);

		if (multi)
			signal(SIGPIPE, SIG_IGN);

		if (sent != -1)
			return sent == 0 ? 0 : 1;
		if (!strcmp(argv[1], "--switch")) {
			fprintf(stderr, "skippy-wl: --switch needs the daemon (start one with --daemon)\n");
			return 1;
		}
		/* no daemon: show once in this process */
		if (!strcmp(argv[1], "--toggle"))
			fprintf(stderr, "skippy-wl: no daemon running, showing once (start one with --daemon)\n");
		mode = MODE_SHOW;
	} else if (argc == 2 && !strcmp(argv[1], "--quit")) {
		int sent = ipc_send("quit");

		if (sent != -1)
			return sent == 0 ? 0 : 1;
		fprintf(stderr, "skippy-wl: no daemon running\n");
		return 1;
	} else if (argc == 4 && !strcmp(argv[1], "--capture")) {
		mode = MODE_CAPTURE;
	} else if (argc == 3 && !strcmp(argv[1], "--capture-all")) {
		mode = MODE_CAPTURE_ALL;
	} else {
		return usage(argv[0]);
	}

	if (globals_init(&g) < 0) {
		fprintf(stderr, "cannot connect to wayland display\n");
		return 1;
	}
	if (!g.shm || !g.list || !g.source_mgr || !g.copy_mgr) {
		fprintf(stderr, "compositor lacks ext-foreign-toplevel-list, ext-image-capture-source or ext-image-copy-capture\n");
		rc = 1;
	} else if ((mode == MODE_SHOW || mode == MODE_DAEMON) &&
			(!g.compositor || !g.layer_shell || !g.seat_name || !g.outputs || !g.toplevel_mgr)) {
		fprintf(stderr, "compositor lacks wlr-layer-shell, wlr-foreign-toplevel-management, a seat (v5+) or an output\n");
		rc = 1;
	} else {
		rc = run_mode(&g, mode, argv, &filter, dir, multi);
	}
	globals_finish(&g);
	return rc;
}
