/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cairo.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "capture.h"
#include "config.h"
#include "filter.h"
#include "icons.h"
#include "input.h"
#include "layout.h"
#include "overlay.h"
#include "toplevels.h"
#include "wlutil.h"

#define CAPTURE_WAIT_MS 1500
#define PLACEHOLDER_W 640
#define PLACEHOLDER_H 400

struct tile {
	struct toplevel *t; /* valid until its closed event removes this tile */
	char *app_id, *title;
	struct capture *cap;
	cairo_surface_t *full; /* wraps cap->data, NULL when the capture failed */
	struct size size;
};

struct buffer {
	struct wl_buffer *buf;
	void *data;
	size_t size;
	int busy;
	struct surf *owner;
};

struct surf {
	struct surf *next;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	int w, h;
	struct buffer buf[2];
	struct wl_callback *frame_cb;
	int dirty;
	struct wl_output *output;
	/* tiles shown on this output (indexes into st.tiles); rects/scaled are indexed by position in vis */
	int *vis;
	int nvis;
	struct rect *rects;
	cairo_surface_t **scaled;
	int cols;
};

static struct config cfg;
static int active;
static PangoFontDescription *font, *font_big;

static struct {
	struct globals *g;
	struct tile *tiles;
	int n;
	struct surf *surfs;
	int sel;
	struct surf *ptr_surf;
	struct surf *target; /* monitors.mode = pointer: the output that shows every window */
	int quit;
	struct toplevel *pick; /* window to activate after teardown */
	int dir; /* 1/-1: opened by --next/--prev */
	int last_req, rev; /* last --next/--prev given, and whether repeats of it currently step backwards */
	int hidden; /* switch wait: surfaces are mapped but transparent, tiles not captured */
	int mods_seen, ptr_seen;
	uint32_t held; /* modifiers down when the keyboard got focus */
	const struct filter *filter;
} st;

static void draw(struct surf *s);
static void relayout(struct surf *s);
static void set_target(struct surf *s);
static void relayout_all(void);
static void tile_free(struct tile *tl);
static void layout_free(struct surf *s);

static char *dup_or_empty(const char *s)
{
	return strdup(s ? s : "");
}

static void redraw_all(void)
{
	struct surf *s;

	for (s = st.surfs; s; s = s->next) {
		if (s->frame_cb || !s->w)
			s->dirty = 1;
		else
			draw(s);
	}
}

/* ---- tiles ---- */

static int caps_done(void *arg)
{
	(void)arg;
	for (int i = 0; i < st.n; i++)
		if (st.tiles[i].cap->state == CAPTURE_PENDING)
			return 0;
	return 1;
}

static int wanted(const struct toplevel *t)
{
	return filter_match(st.filter, t->app_id, t->title, toplevel_status(t));
}

static int tiles_build(void)
{
	struct toplevel *t, **order;
	int i = 0, n = 0;

	for (t = toplevels_first(); t; t = t->next)
		n += wanted(t);
	if (!n)
		return 0;
	order = malloc((size_t)n * sizeof(*order));
	if (!order)
		return -1;
	for (t = toplevels_first(); t; t = t->next)
		if (wanted(t))
			order[i++] = t;
	/* most recently active first; stable, so never-activated windows keep list order */
	for (i = 1; i < n; i++) {
		struct toplevel *k = order[i];
		int j = i;

		for (; j > 0 && toplevel_mru(order[j - 1]) < toplevel_mru(k); j--)
			order[j] = order[j - 1];
		order[j] = k;
	}
	st.tiles = calloc((size_t)n, sizeof(*st.tiles));
	if (!st.tiles) {
		free(order);
		return -1;
	}
	st.n = n;
	for (i = 0; i < n; i++) {
		struct tile *tl = &st.tiles[i];

		t = order[i];
		tl->t = t;
		tl->app_id = dup_or_empty(t->app_id);
		tl->title = dup_or_empty(t->title);
		if (!tl->app_id || !tl->title) {
			free(order);
			return -1;
		}
	}
	free(order);
	return 0;
}

static int tiles_capture(struct globals *g)
{
	int i;

	for (i = 0; i < st.n; i++) {
		st.tiles[i].cap = capture_start(st.tiles[i].t->ext);
		if (!st.tiles[i].cap)
			return -1;
	}
	/* a window that never answers just gets a placeholder tile */
	display_wait(g->display, caps_done, NULL, CAPTURE_WAIT_MS);
	for (i = 0; i < st.n; i++) {
		struct tile *tl = &st.tiles[i];

		tl->full = capture_surface(tl->cap);
		tl->size = tl->full ? (struct size){ (int)tl->cap->width, (int)tl->cap->height }
			: (struct size){ PLACEHOLDER_W, PLACEHOLDER_H };
	}
	return 0;
}

static void tiles_finish(void)
{
	for (int i = 0; i < st.n; i++)
		tile_free(&st.tiles[i]);
	free(st.tiles);
	st.tiles = NULL;
	st.n = 0;
}

static void tile_free(struct tile *tl)
{
	if (tl->full)
		cairo_surface_destroy(tl->full);
	if (tl->cap)
		capture_destroy(tl->cap);
	free(tl->app_id);
	free(tl->title);
}

/* a window closed while the overlay is up: drop its tile and re-flow */
static void on_toplevel_closed(struct toplevel *t)
{
	struct surf *s;
	int i;

	if (st.pick == t)
		st.pick = NULL;
	for (i = 0; i < st.n && st.tiles[i].t != t; i++)
		;
	if (i == st.n)
		return;
	tile_free(&st.tiles[i]);
	memmove(&st.tiles[i], &st.tiles[i + 1], (size_t)(st.n - i - 1) * sizeof(*st.tiles));
	st.n--;
	if (!st.n) {
		for (s = st.surfs; s; s = s->next)
			layout_free(s);
		st.quit = 1;
		return;
	}
	if (st.sel > i)
		st.sel--;
	else if (st.sel >= st.n)
		st.sel = st.n - 1;
	relayout_all();
}

/* ---- per-output layout ---- */

static void relayout_all(void)
{
	for (struct surf *s = st.surfs; s; s = s->next)
		if (s->w)
			relayout(s);
	redraw_all();
}

static void layout_free(struct surf *s)
{
	for (int p = 0; p < s->nvis && s->scaled; p++)
		if (s->scaled[p])
			cairo_surface_destroy(s->scaled[p]);
	free(s->scaled);
	free(s->rects);
	free(s->vis);
	s->scaled = NULL;
	s->rects = NULL;
	s->vis = NULL;
	s->nvis = 0;
	s->cols = 0;
}

static void relayout(struct surf *s)
{
	struct size *src;

	layout_free(s);
	if (!st.n || st.hidden)
		return;
	s->vis = calloc((size_t)st.n, sizeof(*s->vis));
	if (!s->vis)
		return;
	for (int i = 0; i < st.n; i++)
		if (cfg.monitors == MON_POINTER ? s == st.target : toplevel_on_output(st.tiles[i].t, s->output))
			s->vis[s->nvis++] = i;
	if (!s->nvis) {
		layout_free(s);
		return;
	}
	src = calloc((size_t)s->nvis, sizeof(*src));
	s->scaled = calloc((size_t)s->nvis, sizeof(*s->scaled));
	s->rects = calloc((size_t)s->nvis, sizeof(*s->rects));
	if (!src || !s->scaled || !s->rects) {
		free(src);
		layout_free(s);
		return;
	}
	for (int p = 0; p < s->nvis; p++)
		src[p] = st.tiles[s->vis[p]].size;
	s->cols = layout_grid(src, s->nvis, (struct size){ s->w - 2 * cfg.margin, s->h - 2 * cfg.margin },
		cfg.spacing, cfg.label_h, s->rects);
	free(src);

	for (int p = 0; p < s->nvis; p++) {
		struct rect *r = &s->rects[p];
		struct tile *tl = &st.tiles[s->vis[p]];
		cairo_t *cr;

		r->x += cfg.margin;
		r->y += cfg.margin;
		if (!tl->full)
			continue;
		/* scale once to tile size; drawing then only blits */
		s->scaled[p] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, r->w, r->h);
		cr = cairo_create(s->scaled[p]);
		cairo_scale(cr, (double)r->w / tl->size.w, (double)r->h / tl->size.h);
		cairo_set_source_surface(cr, tl->full, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_destroy(cr);
	}
}

/* tile index under the point, or -1 */
static int hit_test(struct surf *s, double x, double y)
{
	int pad = cfg.spacing / 2;

	if (!s->rects)
		return -1;
	for (int p = 0; p < s->nvis; p++) {
		struct rect *r = &s->rects[p];

		if (x >= r->x - pad && x < r->x + r->w + pad &&
				y >= r->y - pad && y < r->y + r->h + cfg.label_h + pad)
			return s->vis[p];
	}
	return -1;
}

/* ---- drawing ---- */

static void fonts_init(void)
{
	font = pango_font_description_from_string(cfg.font);
	/* a family-only font string has no size */
	if (pango_font_description_get_size(font) == 0)
		pango_font_description_set_size(font, 11 * PANGO_SCALE);
	font_big = pango_font_description_copy(font);
	if (pango_font_description_get_size_is_absolute(font))
		pango_font_description_set_absolute_size(font_big,
			pango_font_description_get_size(font) * 1.8);
	else
		pango_font_description_set_size(font_big,
			(gint)(pango_font_description_get_size(font) * 1.8));
}

/* label under a tile; falls back to the other field when the chosen one is empty */
static const char *label_text(const struct tile *tl, char *buf, size_t len)
{
	const char *title = tl->title, *app = tl->app_id;

	switch (cfg.label) {
	case LABEL_APP_ID:
		return app[0] ? app : title;
	case LABEL_BOTH:
		if (app[0] && title[0]) {
			const char *bad;

			snprintf(buf, len, "%s: %s", app, title);
			/* a cut inside a multibyte character would make pango complain on every redraw */
			if (!g_utf8_validate(buf, -1, &bad))
				buf[bad - buf] = 0;
			return buf;
		}
		return title[0] ? title : app;
	default:
		return title[0] ? title : app;
	}
}

static void draw_text(cairo_t *cr, const char *text, int x, int y, int w, int big,
		int center_v, int h, double alpha)
{
	PangoLayout *pl = pango_cairo_create_layout(cr);
	int tw, th;

	pango_layout_set_font_description(pl, big ? font_big : font);
	pango_layout_set_text(pl, text, -1);
	pango_layout_set_width(pl, w * PANGO_SCALE);
	pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
	pango_layout_get_pixel_size(pl, &tw, &th);
	cairo_set_source_rgba(cr, 1, 1, 1, alpha);
	cairo_move_to(cr, x, center_v ? y + (h - th) / 2.0 : y);
	pango_cairo_show_layout(cr, pl);
	g_object_unref(pl);
}

static void paint_icon(cairo_t *cr, cairo_surface_t *ic, int x, int y, double alpha)
{
	cairo_set_source_surface(cr, ic, x, y);
	cairo_paint_with_alpha(cr, alpha);
}

static void render(struct surf *s, cairo_t *cr)
{
	if (st.hidden) {
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		return;
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, cfg.bg[0], cfg.bg[1], cfg.bg[2], cfg.bg[3]);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (int p = 0; p < s->nvis && s->rects && s->scaled; p++) {
		int i = s->vis[p];
		struct rect *r = &s->rects[p];
		struct tile *tl = &st.tiles[i];
		int selected = i == st.sel;

		/* minimized windows are dimmed */
		double dim = toplevel_status(tl->t) & STATUS_MINIMIZED ? 0.45 : 1.0;
		char label[256];

		if (s->scaled[p]) {
			cairo_set_source_surface(cr, s->scaled[p], r->x, r->y);
			cairo_paint_with_alpha(cr, dim);
			/* corner icon, unless the tile is too small to carry one */
			if (cfg.icon && r->w >= 3 * cfg.icon_size && r->h >= 3 * cfg.icon_size) {
				cairo_surface_t *ic = icon_get(tl->app_id, cfg.icon_theme, cfg.icon_size);

				if (ic)
					paint_icon(cr, ic, r->x + r->w - cfg.icon_size - 6,
						r->y + r->h - cfg.icon_size - 6, dim);
			}
		} else {
			int isz = (r->w < r->h ? r->w : r->h) / 2;
			cairo_surface_t *ic = NULL;

			/* a few fixed sizes, so a relayout reuses the cached icons */
			isz = isz >= 256 ? 256 : isz >= 128 ? 128 : isz >= 96 ? 96 : isz >= 64 ? 64 :
				isz >= 48 ? 48 : isz >= 32 ? 32 : 0;
			cairo_set_source_rgba(cr, 0.16, 0.16, 0.18, dim);
			cairo_rectangle(cr, r->x, r->y, r->w, r->h);
			cairo_fill(cr);
			if (cfg.icon && isz)
				ic = icon_get(tl->app_id, cfg.icon_theme, isz);
			if (ic)
				paint_icon(cr, ic, r->x + (r->w - isz) / 2, r->y + (r->h - isz) / 2, dim);
			else
				draw_text(cr, tl->app_id[0] ? tl->app_id : "?", r->x, r->y, r->w, 1, 1, r->h, 0.8 * dim);
		}

		if (selected) {
			cairo_set_source_rgba(cr, cfg.highlight[0], cfg.highlight[1], cfg.highlight[2],
				cfg.highlight[3]);
			cairo_set_line_width(cr, 4);
			cairo_rectangle(cr, r->x - 2, r->y - 2, r->w + 4, r->h + 4);
		} else {
			cairo_set_source_rgba(cr, cfg.border[0], cfg.border[1], cfg.border[2], cfg.border[3]);
			cairo_set_line_width(cr, 1);
			cairo_rectangle(cr, r->x + 0.5, r->y + 0.5, r->w - 1, r->h - 1);
		}
		cairo_stroke(cr);

		draw_text(cr, label_text(tl, label, sizeof(label)), r->x, r->y + r->h + 6, r->w, 0,
			0, 0, (selected ? 1.0 : 0.7) * dim);
	}
}

/* ---- buffers ---- */

static void buf_release(void *d, struct wl_buffer *b)
{
	struct buffer *buf = d;

	(void)b;
	buf->busy = 0;
	if (buf->owner->dirty && !buf->owner->frame_cb)
		draw(buf->owner);
}

static const struct wl_buffer_listener buf_l = { buf_release };

static int buf_alloc(struct surf *s, struct buffer *b)
{
	b->buf = shm_buffer_create(st.g->shm, s->w, s->h, WL_SHM_FORMAT_ARGB8888, &b->data, &b->size);
	if (!b->buf)
		return -1;
	b->owner = s;
	wl_buffer_add_listener(b->buf, &buf_l, b);
	return 0;
}

static void buf_free(struct buffer *b)
{
	if (b->buf)
		wl_buffer_destroy(b->buf);
	if (b->data)
		munmap(b->data, b->size);
	memset(b, 0, sizeof(*b));
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
	struct surf *s = d;

	(void)time;
	wl_callback_destroy(cb);
	s->frame_cb = NULL;
	if (s->dirty)
		draw(s);
}

static const struct wl_callback_listener frame_l = { frame_done };

static void draw(struct surf *s)
{
	struct buffer *b = NULL;
	cairo_surface_t *cs;
	cairo_t *cr;

	for (int i = 0; i < 2; i++)
		if (!s->buf[i].busy) {
			b = &s->buf[i];
			break;
		}
	if (!b || !b->data) {
		s->dirty = 1;
		return;
	}
	cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, s->w, s->h, s->w * 4);
	cr = cairo_create(cs);
	render(s, cr);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	b->busy = 1;
	s->dirty = 0;
	wl_surface_attach(s->surface, b->buf, 0, 0);
	wl_surface_damage(s->surface, 0, 0, s->w, s->h);
	s->frame_cb = wl_surface_frame(s->surface);
	wl_callback_add_listener(s->frame_cb, &frame_l, s);
	wl_surface_commit(s->surface);
}

/* ---- layer surfaces ---- */

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		uint32_t w, uint32_t h)
{
	struct surf *s = d;

	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (!w || !h)
		return;
	if ((int)w != s->w || (int)h != s->h) {
		s->w = (int)w;
		s->h = (int)h;
		for (int i = 0; i < 2; i++) {
			buf_free(&s->buf[i]);
			if (buf_alloc(s, &s->buf[i]) < 0) {
				st.quit = 1;
				return;
			}
		}
		relayout(s);
	}
	/* without a pointer no enter event will pick the output */
	if (!input_has_pointer())
		set_target(s);
	if (s->frame_cb)
		s->dirty = 1;
	else
		draw(s);
}

static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls)
{
	(void)d; (void)ls;
	st.quit = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_l = { ls_configure, ls_closed };

static void surf_create(struct output *o)
{
	struct surf *s = calloc(1, sizeof(*s));

	if (!s)
		return;
	s->output = o->wl;
	s->surface = wl_compositor_create_surface(st.g->compositor);
	s->layer = zwlr_layer_shell_v1_get_layer_surface(st.g->layer_shell, s->surface, o->wl,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "skippy-wl");
	zwlr_layer_surface_v1_add_listener(s->layer, &layer_l, s);
	zwlr_layer_surface_v1_set_size(s->layer, 0, 0);
	zwlr_layer_surface_v1_set_anchor(s->layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(s->surface);
	s->next = st.surfs;
	st.surfs = s;
}

static void surf_destroy(struct surf *s)
{
	if (s->frame_cb)
		wl_callback_destroy(s->frame_cb);
	layout_free(s);
	buf_free(&s->buf[0]);
	buf_free(&s->buf[1]);
	zwlr_layer_surface_v1_destroy(s->layer);
	wl_surface_destroy(s->surface);
	free(s);
}

/* ---- input ---- */

/* monitors.mode = pointer: show every window on s, the output the pointer first entered */
static void set_target(struct surf *s)
{
	if (cfg.monitors != MON_POINTER || st.target || !s)
		return;
	st.target = s;
	relayout_all();
}

/* the output the user is working on: the pointer's, else the first with windows */
static struct surf *active_surf(void)
{
	struct surf *s;

	if (st.ptr_surf && st.ptr_surf->nvis)
		return st.ptr_surf;
	for (s = st.surfs; s; s = s->next)
		if (s->nvis)
			return s;
	return NULL;
}

static int pos_of(const struct surf *s, int tile)
{
	for (int p = 0; p < s->nvis; p++)
		if (s->vis[p] == tile)
			return p;
	return -1;
}

static void choose(int i)
{
	if (st.quit || i < 0 || i >= st.n)
		return;
	st.pick = st.tiles[i].t;
	st.quit = 1;
}

static void close_tile(int i)
{
	if (st.quit || i < 0 || i >= st.n)
		return;
	/* the tile goes away when the window's closed event arrives */
	if (toplevel_close(st.tiles[i].t) < 0)
		fprintf(stderr, "skippy-wl: cannot close this window (no wlr handle paired)\n");
}

static void minimize_tile(int i)
{
	if (st.quit || i < 0 || i >= st.n)
		return;
	/* the tile dims when the new state arrives */
	if (toplevel_set_minimized(st.tiles[i].t, !(toplevel_status(st.tiles[i].t) & STATUS_MINIMIZED)) < 0)
		fprintf(stderr, "skippy-wl: cannot minimize this window (no wlr handle paired)\n");
}

static void set_sel(int to)
{
	if (to != st.sel) {
		st.sel = to;
		st.last_req = st.rev = 0; /* moved by hand: a repeated command starts over */
		redraw_all();
	}
}

/* move to position pos on the active output; no-op when out of range */
static void select_pos(struct surf *s, int pos)
{
	if (pos >= 0 && pos < s->nvis)
		set_sel(s->vis[pos]);
}

static void move_horiz(int delta)
{
	struct surf *s = active_surf();
	int pos;

	if (!s)
		return;
	pos = pos_of(s, st.sel);
	select_pos(s, pos < 0 ? 0 : pos + delta);
}

/* a vertical step into a shorter last row lands on its last tile */
static void move_vert(int dir)
{
	struct surf *s = active_surf();
	int cols, pos, to;

	if (!s)
		return;
	pos = pos_of(s, st.sel);
	if (pos < 0) {
		select_pos(s, 0);
		return;
	}
	cols = s->cols ? s->cols : 1;
	to = pos + dir * cols;
	if (to >= s->nvis && pos / cols < (s->nvis - 1) / cols)
		to = s->nvis - 1;
	select_pos(s, to);
}

static void cycle(int dir)
{
	if (st.n)
		set_sel((st.sel + dir + st.n) % st.n);
}

void overlay_key(uint32_t sym)
{
	struct surf *s;
	int act;

	if (st.quit)
		return;
	if (cfg.monitors == MON_POINTER && !st.target && !st.hidden) {
		/* nothing is shown yet: only cancel is safe */
		if (config_action(&cfg, sym) == ACT_CANCEL)
			st.quit = 1;
		return;
	}
	act = config_action(&cfg, sym);
	/* nothing is drawn yet: close, minimize and the like would act on a window the user cannot see */
	if (st.hidden && act != ACT_CANCEL && act != ACT_SELECT && act != ACT_NEXT && act != ACT_PREV)
		return;
	switch (act) {
	case ACT_CANCEL:
		st.quit = 1;
		break;
	case ACT_SELECT:
		choose(st.sel);
		break;
	case ACT_CLOSE:
		close_tile(st.sel);
		break;
	case ACT_MINIMIZE:
		minimize_tile(st.sel);
		break;
	case ACT_LEFT:
		move_horiz(-1);
		break;
	case ACT_RIGHT:
		move_horiz(1);
		break;
	case ACT_UP:
		move_vert(-1);
		break;
	case ACT_DOWN:
		move_vert(1);
		break;
	case ACT_HOME:
		if ((s = active_surf()))
			select_pos(s, 0);
		break;
	case ACT_END:
		if ((s = active_surf()))
			select_pos(s, s->nvis - 1);
		break;
	case ACT_NEXT:
		cycle(1);
		break;
	case ACT_PREV:
		cycle(-1);
		break;
	}
}

void overlay_modifiers(uint32_t depressed)
{
	int first = !st.mods_seen;

	if (!st.dir || !cfg.pivot || st.quit)
		return;
	st.mods_seen = 1;
	if (first)
		st.held = depressed;
	/* nothing held at open: the key was already released */
	if (!st.held || !(depressed & st.held))
		choose(st.sel);
}

void overlay_cycle(int dir)
{
	/* the same command again turns around; the other one steps in its own direction */
	int rev = dir == st.last_req ? !st.rev : 0;

	cycle(rev ? -dir : dir);
	/* after cycle(): moving the selection resets the state */
	st.last_req = dir;
	st.rev = rev;
}

static double ptr_x, ptr_y;

void overlay_pointer_motion(struct wl_surface *surface, double x, double y)
{
	int hit;

	if (st.quit)
		return;
	if (surface) {
		struct surf *s;

		st.ptr_surf = NULL;
		for (s = st.surfs; s; s = s->next)
			if (s->surface == surface)
				st.ptr_surf = s;
		set_target(st.ptr_surf);
	}
	/* the parked cursor's enter event must not replace the --next/--prev selection */
	if (st.dir && !st.ptr_seen) {
		st.ptr_seen = 1;
		ptr_x = x;
		ptr_y = y;
		return;
	}
	ptr_x = x;
	ptr_y = y;
	if (!st.ptr_surf)
		return;
	hit = hit_test(st.ptr_surf, x, y);
	if (hit >= 0)
		set_sel(hit);
}

void overlay_pointer_leave(void)
{
	st.ptr_surf = NULL;
}

void overlay_pointer_button(uint32_t button)
{
	int hit;

	if (st.quit || !st.ptr_surf)
		return;
	hit = hit_test(st.ptr_surf, ptr_x, ptr_y);
	if (button == BTN_LEFT) {
		if (hit >= 0)
			choose(hit);
		else
			st.quit = 1;
	} else if (button == BTN_MIDDLE) {
		close_tile(hit);
	}
}

/* ---- run ---- */

static int quit_requested(void *arg)
{
	(void)arg;
	return st.quit;
}

int overlay_run(struct globals *g, const struct filter *filter, int dir)
{
	struct output *o;
	int rc = 0, input_up = 0;

	memset(&st, 0, sizeof(st));
	st.g = g;
	st.filter = filter;
	st.dir = dir;
	st.last_req = dir;
	config_load(&cfg);
	fonts_init();
	active = 1;
	toplevels_set_closed_cb(on_toplevel_closed);
	toplevels_set_changed_cb(redraw_all);
	if (tiles_build() < 0) {
		rc = -1;
		goto out;
	}
	if (!st.n)
		goto out;
	st.sel = dir > 0 && st.n > 1 ? 1 : dir < 0 ? st.n - 1 : 0;
	/* a quick tap then picks the window without the capture delay or any drawing */
	st.hidden = dir && cfg.pivot && cfg.switch_wait > 0;
	if (!st.hidden && tiles_capture(g) < 0) {
		rc = -1;
		goto out;
	}
	input_init(g);
	input_up = 1;
	for (o = g->outputs; o; o = o->next)
		surf_create(o);
	if (st.hidden) {
		/* returns -1 on timeout: the modifiers are still held, show the overlay */
		display_wait(g->display, quit_requested, NULL, cfg.switch_wait);
		if (!st.quit && !display_interrupted()) {
			/* stays hidden until the sizes are known */
			if (tiles_capture(g) < 0) {
				rc = -1;
				goto out;
			}
			st.hidden = 0;
			relayout_all();
		}
	}
	if (display_wait(g->display, quit_requested, NULL, -1) < 0)
		rc = -1;
out:
	while (st.surfs) {
		struct surf *s = st.surfs;

		st.surfs = s->next;
		surf_destroy(s);
	}
	st.ptr_surf = NULL;
	st.target = NULL;
	if (st.pick) {
		/* focus the pick only after the overlay is gone, or the compositor hands focus back */
		wl_display_roundtrip(g->display);
		/* the window may have closed during the roundtrip */
		if (st.pick && toplevel_activate(st.pick, input_seat()) < 0)
			fprintf(stderr, "skippy-wl: cannot activate this window (no wlr handle paired)\n");
		wl_display_roundtrip(g->display);
	}
	if (input_up)
		input_finish();
	toplevels_set_closed_cb(NULL);
	toplevels_set_changed_cb(NULL);
	tiles_finish();
	icons_clear();
	wl_display_roundtrip(g->display);
	pango_font_description_free(font);
	pango_font_description_free(font_big);
	active = 0;
	return rc;
}

void overlay_cancel(void)
{
	st.quit = 1;
}

int overlay_active(void)
{
	return active;
}
