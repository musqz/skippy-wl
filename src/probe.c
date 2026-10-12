/* SPDX-License-Identifier: GPL-3.0-or-later */
/* skippy-wl-probe: print ext and wlr toplevel lists in event order. */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct ext_top {
	struct ext_foreign_toplevel_handle_v1 *h;
	char *id, *title, *app_id;
	int seq, closed;
};

struct wlr_top {
	struct zwlr_foreign_toplevel_handle_v1 *h;
	char *title, *app_id, state[96], outputs[64];
	int seq, closed;
};

#define MAX 256
static struct ext_top ext[MAX];
static struct wlr_top wlr[MAX];
static int n_ext, n_wlr, seq, watch;
static volatile sig_atomic_t quit;

struct out { struct wl_output *o; char name[32]; } outs[8];
static int n_out;

static void set(char **dst, const char *s)
{
	free(*dst);
	*dst = strdup(s);
}

static void ev(const char *k, int idx, const char *what, const char *val)
{
	if (watch)
		printf("[%3d] %s#%d %s %s\n", seq, k, idx, what, val ? val : "");
}

/* ext handle */
static struct ext_top *ext_of(void *d) { return d; }
static void e_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{ (void)h; ext_of(d)->closed = 1; ev("EXT", (int)(ext_of(d) - ext), "closed", NULL); seq++; }
static void e_done(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{ (void)h; ev("EXT", (int)(ext_of(d) - ext), "done", NULL); seq++; }
static void e_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; set(&ext_of(d)->title, s); ev("EXT", (int)(ext_of(d) - ext), "title", s); seq++; }
static void e_app(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; set(&ext_of(d)->app_id, s); ev("EXT", (int)(ext_of(d) - ext), "app_id", s); seq++; }
static void e_ident(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; set(&ext_of(d)->id, s); ev("EXT", (int)(ext_of(d) - ext), "identifier", s); seq++; }
static const struct ext_foreign_toplevel_handle_v1_listener ext_h_l = {
	e_closed, e_done, e_title, e_app, e_ident };

static void l_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l,
		struct ext_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)l;
	if (n_ext >= MAX)
		return;
	ext[n_ext].h = h;
	ext[n_ext].seq = seq;
	ev("EXT", n_ext, "new", NULL);
	seq++;
	ext_foreign_toplevel_handle_v1_add_listener(h, &ext_h_l, &ext[n_ext++]);
}
static void l_finished(void *d, struct ext_foreign_toplevel_list_v1 *l) { (void)d; (void)l; }
static const struct ext_foreign_toplevel_list_v1_listener ext_l = { l_toplevel, l_finished };

/* wlr handle */
static struct wlr_top *wlr_of(void *d) { return d; }
#define WI(d) ((int)(wlr_of(d) - wlr))
static void w_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; set(&wlr_of(d)->title, s); ev("WLR", WI(d), "title", s); seq++; }
static void w_app(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; set(&wlr_of(d)->app_id, s); ev("WLR", WI(d), "app_id", s); seq++; }
static const char *oname(struct wl_output *o)
{
	for (int i = 0; i < n_out; i++)
		if (outs[i].o == o)
			return outs[i].name;
	return "?";
}
static void w_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
	struct wlr_top *t = wlr_of(d);
	(void)h;
	snprintf(t->outputs + strlen(t->outputs), sizeof(t->outputs) - strlen(t->outputs),
		"%s ", oname(o));
	ev("WLR", WI(d), "output_enter", oname(o));
	seq++;
}
static void w_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{ (void)h; ev("WLR", WI(d), "output_leave", oname(o)); seq++; }
static void w_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a)
{
	struct wlr_top *t = wlr_of(d);
	static const char *names[] = { "max", "min", "act", "full" };
	uint32_t *s;
	(void)h;
	t->state[0] = 0;
	wl_array_for_each(s, a)
		if (*s < 4)
			snprintf(t->state + strlen(t->state), sizeof(t->state) - strlen(t->state),
				"%s ", names[*s]);
	ev("WLR", WI(d), "state", t->state);
	seq++;
}
static void w_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{ (void)h; ev("WLR", WI(d), "done", NULL); seq++; }
static void w_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{ (void)h; wlr_of(d)->closed = 1; ev("WLR", WI(d), "closed", NULL); seq++; }
static void w_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
		struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)h; (void)p; ev("WLR", WI(d), "parent", NULL); seq++; }
static const struct zwlr_foreign_toplevel_handle_v1_listener wlr_h_l = {
	w_title, w_app, w_enter, w_leave, w_state, w_done, w_closed, w_parent };

static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
		struct zwlr_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)m;
	if (n_wlr >= MAX)
		return;
	wlr[n_wlr].h = h;
	wlr[n_wlr].seq = seq;
	ev("WLR", n_wlr, "new", NULL);
	seq++;
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &wlr_h_l, &wlr[n_wlr++]);
}
static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener wlr_m_l = { m_toplevel, m_finished };

/* outputs */
static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t w, int32_t h,
		int32_t sp, const char *mk, const char *md, int32_t t)
{ (void)d; (void)o; (void)x; (void)y; (void)w; (void)h; (void)sp; (void)mk; (void)md; (void)t; }
static void o_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{ (void)d; (void)o; (void)f; (void)w; (void)h; (void)r; }
static void o_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void o_scale(void *d, struct wl_output *o, int32_t s) { (void)d; (void)o; (void)s; }
static void o_name(void *d, struct wl_output *o, const char *s)
{ (void)o; snprintf(((struct out *)d)->name, sizeof(((struct out *)d)->name), "%s", s); }
static void o_desc(void *d, struct wl_output *o, const char *s) { (void)d; (void)o; (void)s; }
static const struct wl_output_listener out_l = { o_geometry, o_mode, o_done, o_scale, o_name, o_desc };

/* registry */
static struct wl_seat *wl_seat;
static struct ext_foreign_toplevel_list_v1 *ext_list;
static struct zwlr_foreign_toplevel_manager_v1 *wlr_mgr;

static void r_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	(void)d;
	if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name)) {
		ext_list = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(ext_list, &ext_l, NULL);
	} else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		wlr_mgr = wl_registry_bind(r, name,
			&zwlr_foreign_toplevel_manager_v1_interface, v < 3 ? v : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(wlr_mgr, &wlr_m_l, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		wl_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name) && n_out < 8) {
		outs[n_out].o = wl_registry_bind(r, name, &wl_output_interface, v < 4 ? v : 4);
		wl_output_add_listener(outs[n_out].o, &out_l, &outs[n_out]);
		n_out++;
	}
}
static void r_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { r_global, r_remove };

#define S(x) ((x) ? (x) : "-")

static void print_tables(void)
{
	printf("\nEXT list (%d)\n", n_ext);
	for (int i = 0; i < n_ext; i++)
		printf("  #%d seq=%d%s id=%s app=%s title=%s\n", i, ext[i].seq,
			ext[i].closed ? " CLOSED" : "", S(ext[i].id), S(ext[i].app_id), S(ext[i].title));
	printf("WLR list (%d)\n", n_wlr);
	for (int i = 0; i < n_wlr; i++)
		printf("  #%d seq=%d%s app=%s title=%s state=[%s] out=[%s]\n", i, wlr[i].seq,
			wlr[i].closed ? " CLOSED" : "", S(wlr[i].app_id), S(wlr[i].title),
			wlr[i].state, wlr[i].outputs);
	printf("\nSide by side (same index = paired by order)\n");
	for (int i = 0; i < n_ext || i < n_wlr; i++)
		printf("  %2d | %-32s %-16.16s | %-10.10s %-16.16s %s%s\n", i,
			i < n_ext ? S(ext[i].id) : "-", i < n_ext ? S(ext[i].title) : "",
			i < n_wlr ? S(wlr[i].app_id) : "-", i < n_wlr ? S(wlr[i].title) : "",
			i < n_wlr ? wlr[i].state : "",
			(i < n_ext && ext[i].closed) || (i < n_wlr && wlr[i].closed) ? "CLOSED" : "");
}

static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
	watch = argc > 1 && !strcmp(argv[1], "--watch");
	const char *op = NULL;
	int target = -1;
	if (argc > 1 && !watch) {
		char *end;
		op = argv[1];
		if (argc < 3 || (strcmp(op, "--act") && strcmp(op, "--min") && strcmp(op, "--unmin"))
				|| (target = (int)strtol(argv[2], &end, 10), *end || end == argv[2])) {
			fprintf(stderr, "usage: %s [--watch | --act N | --min N | --unmin N]\n", argv[0]);
			return 1;
		}
	}
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot connect to wayland display\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!ext_list || !wlr_mgr) {
		fprintf(stderr, "missing protocol: ext_foreign_toplevel_list=%d wlr_foreign_toplevel=%d\n",
			!!ext_list, !!wlr_mgr);
		return 1;
	}
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (op && (target < 0 || target >= n_wlr || !wl_seat)) {
		fprintf(stderr, "no wlr toplevel #%d (or no seat)\n", target);
		return 1;
	}
	if (op) {
		if (!strcmp(op, "--min"))
			zwlr_foreign_toplevel_handle_v1_set_minimized(wlr[target].h);
		else if (!strcmp(op, "--unmin"))
			zwlr_foreign_toplevel_handle_v1_unset_minimized(wlr[target].h);
		else if (!strcmp(op, "--act"))
			zwlr_foreign_toplevel_handle_v1_activate(wlr[target].h, wl_seat);
		wl_display_roundtrip(dpy);
		wl_display_roundtrip(dpy);
	}
	int err = 0;
	if (watch) {
		signal(SIGINT, on_sig);
		while (!quit) {
			wl_display_flush(dpy);
			struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
			if (poll(&p, 1, -1) < 0 && !quit)
				err = 1;
			else if (!quit && wl_display_dispatch(dpy) < 0)
				err = 1;
			if (err) {
				fprintf(stderr, "display error: %d\n", wl_display_get_error(dpy));
				break;
			}
		}
	}
	print_tables();
	return err;
}
