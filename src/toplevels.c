/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"
#include "toplevels.h"

#define MAX_OUTS 8

/*
 * ext and wlr handles share no id. labwc creates both in one call per view,
 * so both lists come in the same order: the oldest unpaired ext handle belongs
 * to the oldest unpaired wlr handle (docs/pairing.md).
 */
struct wtop {
	struct wtop *next;
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *app_id;
	struct wl_output *outs[MAX_OUTS];
	int n_outs, done, checked, bad;
	unsigned status; /* STATUS_* bits from the wlr state event */
	unsigned shown_status; /* status at the last changed_cb */
	unsigned long mru; /* activation stamp, 0 = never activated */
	struct toplevel *t;
};

static struct toplevel *head;
static struct wtop *whead;
static void (*closed_cb)(struct toplevel *);
static void (*changed_cb)(void);
/* set when a pair disagrees: order pairing can no longer be trusted, so no window gets actions */
static int pairing_broken;
static unsigned long mru_clock;

static void set(char **dst, const char *s)
{
	free(*dst);
	*dst = strdup(s);
}

static void unlink_pair(struct toplevel *t)
{
	if (t->w) {
		t->w->t = NULL;
		t->w->bad = 1;
		t->w = NULL;
	}
	t->no_pair = 1;
}

static void try_pair(void)
{
	while (!pairing_broken) {
		struct toplevel *t;
		struct wtop *w;

		for (t = head; t && (t->w || t->no_pair); t = t->next)
			;
		for (w = whead; w && (w->t || w->bad); w = w->next)
			;
		if (!t || !w)
			return;
		t->w = w;
		w->t = t;
	}
}

/* checked once per pair, right after both handles sent their initial state */
static void check_pair(struct wtop *w)
{
	struct toplevel *t = w->t;
	const char *a, *b;

	if (!t || w->checked || !w->done || !t->ext_done)
		return;
	w->checked = 1;
	a = t->app_id ? t->app_id : "";
	b = w->app_id ? w->app_id : "";
	if (!strcmp(a, b))
		return;
	fprintf(stderr, "skippy-wl: toplevel pairing mismatch (app_id '%s' vs '%s'), "
		"window actions disabled\n", a, b);
	pairing_broken = 1;
	for (t = head; t; t = t->next)
		if (t->w)
			unlink_pair(t);
}

/* ---- ext handle ---- */

static void h_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = d, **p;

	if (closed_cb)
		closed_cb(t);
	if (t->w)
		unlink_pair(t);
	for (p = &head; *p != t; p = &(*p)->next)
		;
	*p = t->next;
	/* with no windows left, a bad pairing can be forgotten */
	if (!head && !whead)
		pairing_broken = 0;
	ext_foreign_toplevel_handle_v1_destroy(h);
	free(t->identifier);
	free(t->title);
	free(t->app_id);
	free(t);
}

static void h_done(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = d;

	(void)h;
	t->ext_done = 1;
	if (t->w)
		check_pair(t->w);
}

static void h_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s)
{
	(void)h;
	set(&((struct toplevel *)d)->title, s);
}

static void h_app_id(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s)
{
	(void)h;
	set(&((struct toplevel *)d)->app_id, s);
}

static void h_identifier(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s)
{
	(void)h;
	set(&((struct toplevel *)d)->identifier, s);
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_l = {
	h_closed, h_done, h_title, h_app_id, h_identifier };

static void l_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l,
		struct ext_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t, **p;

	(void)d; (void)l;
	t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->ext = h;
	/* append so the list keeps event order */
	for (p = &head; *p; p = &(*p)->next)
		;
	*p = t;
	ext_foreign_toplevel_handle_v1_add_listener(h, &handle_l, t);
	try_pair();
}

static void l_finished(void *d, struct ext_foreign_toplevel_list_v1 *l)
{
	(void)d; (void)l;
}

static const struct ext_foreign_toplevel_list_v1_listener list_l = { l_toplevel, l_finished };

/* ---- wlr handle ---- */

static void w_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{
	(void)d; (void)h; (void)s;
}

static void w_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{
	(void)h;
	set(&((struct wtop *)d)->app_id, s);
}

static void w_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
	struct wtop *w = d;

	(void)h;
	if (o && w->n_outs < MAX_OUTS)
		w->outs[w->n_outs++] = o;
}

static void w_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
	struct wtop *w = d;

	(void)h;
	for (int i = 0; i < w->n_outs; i++)
		if (w->outs[i] == o)
			w->outs[i] = w->outs[--w->n_outs];
}

static void w_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a)
{
	struct wtop *w = d;
	uint32_t *s;
	unsigned old = w->status;

	(void)h;
	w->status = 0;
	wl_array_for_each(s, a) {
		switch (*s) {
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
			w->status |= STATUS_MAXIMIZED;
			break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
			w->status |= STATUS_MINIMIZED;
			break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
			w->status |= STATUS_ACTIVATED;
			break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
			w->status |= STATUS_FULLSCREEN;
			break;
		}
	}
	if ((w->status & STATUS_ACTIVATED) && !(old & STATUS_ACTIVATED))
		w->mru = ++mru_clock;
}

static void w_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct wtop *w = d;

	(void)h;
	w->done = 1;
	check_pair(w);
	if (changed_cb && w->status != w->shown_status)
		changed_cb();
	w->shown_status = w->status;
}

static void w_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct wtop *w = d, **p;

	if (w->t) {
		w->t->w = NULL;
		w->t->no_pair = 1;
	}
	for (p = &whead; *p != w; p = &(*p)->next)
		;
	*p = w->next;
	zwlr_foreign_toplevel_handle_v1_destroy(h);
	free(w->app_id);
	free(w);
}

static void w_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
		struct zwlr_foreign_toplevel_handle_v1 *p)
{
	(void)d; (void)h; (void)p;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener wlr_handle_l = {
	w_title, w_app_id, w_output_enter, w_output_leave, w_state, w_done, w_closed, w_parent };

static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
		struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct wtop *w, **p;

	(void)d; (void)m;
	w = calloc(1, sizeof(*w));
	if (!w)
		return;
	w->handle = h;
	for (p = &whead; *p; p = &(*p)->next)
		;
	*p = w;
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &wlr_handle_l, w);
	try_pair();
}

static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m)
{
	(void)d; (void)m;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_l = { m_toplevel, m_finished };

/* ---- API ---- */

void toplevels_init(struct ext_foreign_toplevel_list_v1 *list,
		struct zwlr_foreign_toplevel_manager_v1 *mgr)
{
	ext_foreign_toplevel_list_v1_add_listener(list, &list_l, NULL);
	if (mgr)
		zwlr_foreign_toplevel_manager_v1_add_listener(mgr, &mgr_l, NULL);
}

struct toplevel *toplevels_first(void)
{
	return head;
}

struct toplevel *toplevels_find(const char *identifier)
{
	struct toplevel *t;

	for (t = head; t; t = t->next)
		if (t->identifier && !strcmp(t->identifier, identifier))
			return t;
	return NULL;
}

void toplevels_set_closed_cb(void (*cb)(struct toplevel *))
{
	closed_cb = cb;
}

int toplevel_activate(struct toplevel *t, struct wl_seat *seat)
{
	if (!t->w || pairing_broken)
		return -1;
	zwlr_foreign_toplevel_handle_v1_activate(t->w->handle, seat);
	return 0;
}

int toplevel_close(struct toplevel *t)
{
	if (!t->w || pairing_broken)
		return -1;
	zwlr_foreign_toplevel_handle_v1_close(t->w->handle);
	return 0;
}

int toplevel_on_output(const struct toplevel *t, const struct wl_output *out)
{
	if (!t->w || !t->w->n_outs)
		return 1;
	for (int i = 0; i < t->w->n_outs; i++)
		if (t->w->outs[i] == out)
			return 1;
	return 0;
}

void toplevels_output_removed(const struct wl_output *out)
{
	for (struct wtop *w = whead; w; w = w->next)
		for (int i = 0; i < w->n_outs; i++)
			if (w->outs[i] == out)
				w->outs[i] = w->outs[--w->n_outs];
}

unsigned toplevel_status(const struct toplevel *t)
{
	return t->w && !pairing_broken ? t->w->status : STATUS_UNKNOWN;
}

void toplevels_set_changed_cb(void (*cb)(void))
{
	changed_cb = cb;
}

int toplevel_set_minimized(struct toplevel *t, int minimized)
{
	if (!t->w || pairing_broken)
		return -1;
	if (minimized)
		zwlr_foreign_toplevel_handle_v1_set_minimized(t->w->handle);
	else
		zwlr_foreign_toplevel_handle_v1_unset_minimized(t->w->handle);
	return 0;
}

unsigned long toplevel_mru(const struct toplevel *t)
{
	return t->w ? t->w->mru : 0;
}
