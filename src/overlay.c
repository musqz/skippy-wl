/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cairo.h>
#include <glib.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "bgimage.h"
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
#define BAR_Y 16 /* type-to-filter bar */
#define BAR_H 44
#define BAR_BOTTOM (BAR_Y + BAR_H + 8) /* the grid starts below this while filtering */

struct tile {
	struct toplevel *t; /* valid until its closed event removes this tile */
	char *app_id, *title;
	char *app_id_cf, *title_cf; /* case-folded, for the type-to-filter match */
	struct capture *cap;
	cairo_surface_t *full; /* wraps cap->data, NULL when the capture failed */
	struct size size;
	int picked; /* multi-select: 1-based toggle order, 0 = not picked */
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
	cairo_surface_t *bg; /* background image scaled to cover this surface */
	int cols;
};

static struct config cfg;
static int active;
static PangoFontDescription *font, *font_big;
#define LABEL_GAP 4 /* between the label box and the tile frame */

static int label_space(void);
static void label_rect(const struct rect *r, int *x, int *y, int *w, int *h);

static struct {
	struct globals *g;
	struct tile *tiles;
	int n;
	struct surf *surfs;
	cairo_surface_t *bg_img; /* background.image_cmd or background.image as loaded; NULL = none or not tried yet */
	int bg_tried;
	int sel;
	struct surf *ptr_surf;
	struct surf *target; /* monitors.mode = pointer: the output that shows every window */
	int quit;
	int multi, done; /* multi-select; confirmed */
	int pick_seq;
	struct toplevel *pick; /* window to activate after teardown */
	int dir; /* 1/-1: opened by --next/--prev */
	int hidden; /* switch wait: surfaces are mapped but transparent, tiles not captured */
	int mods_seen, ptr_seen, pivot_done, scrolled;
	uint32_t cur_mods; /* modifiers down now */
	double t_last; /* ms of the last key press or step once the overlay is drawn */
	double anim_t0; /* ms when the tiles first showed; 0 = not yet */
	uint32_t held; /* modifiers down when the keyboard got focus */
	const struct filter *filter;
	char query[96]; /* typed filter: tiles whose app id or title lacks it are hidden */
	char *query_cf; /* query, case-folded */
	int qlen, filtering;
} st;

static void draw(struct surf *s);
static void relayout(struct surf *s);
static void set_target(struct surf *s);
static void relayout_all(void);
static void refilter(void);
static struct surf *active_surf(void);
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

/* [hide] in the config: never shown, not even by --switch */
static int hidden(const struct toplevel *t)
{
	return config_hidden(&cfg, t->app_id, t->title, toplevel_status(t));
}

static int wanted(const struct toplevel *t)
{
	return !hidden(t) && filter_match(st.filter, t->app_id, t->title, toplevel_status(t));
}

/* case-insensitive substring of the app id or title; the empty query matches all */
static int tile_matches(const struct tile *tl)
{
	return !st.qlen || strstr(tl->title_cf, st.query_cf) || strstr(tl->app_id_cf, st.query_cf);
}

/* Unicode case folding; text that is not valid UTF-8 only gets its ASCII letters folded */
static char *fold(const char *s)
{
	return g_utf8_validate(s, -1, NULL) ? g_utf8_casefold(s, -1) : g_ascii_strdown(s, -1);
}

static int any_match(void)
{
	for (int i = 0; i < st.n; i++)
		if (tile_matches(&st.tiles[i]))
			return 1;
	return 0;
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
		tl->app_id_cf = fold(tl->app_id);
		tl->title_cf = fold(tl->title);
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
	g_free(tl->app_id_cf);
	g_free(tl->title_cf);
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
	refilter();
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
	int top;

	layout_free(s);
	if (!st.n || st.hidden)
		return;
	s->vis = calloc((size_t)st.n, sizeof(*s->vis));
	if (!s->vis)
		return;
	for (int i = 0; i < st.n; i++)
		if (tile_matches(&st.tiles[i]) &&
				(cfg.monitors == MON_POINTER ? s == st.target : toplevel_on_output(st.tiles[i].t, s->output)))
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
	/* the filter bar gets its own strip when the margin is too small for it */
	top = st.filtering && cfg.margin < BAR_BOTTOM ? BAR_BOTTOM : cfg.margin;
	s->cols = layout_grid(src, s->nvis, (struct size){ s->w - 2 * cfg.margin, s->h - top - cfg.margin },
		cfg.spacing, label_space(), s->rects);
	free(src);

	for (int p = 0; p < s->nvis; p++) {
		struct rect *r = &s->rects[p];
		struct tile *tl = &st.tiles[s->vis[p]];
		cairo_t *cr;

		r->x += cfg.margin;
		r->y += top;
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

		int lx, ly, lw, lh;

		if (x >= r->x - pad && x < r->x + r->w + pad &&
				y >= r->y - pad && y < r->y + r->h + label_space() + pad)
			return s->vis[p];
		label_rect(r, &lx, &ly, &lw, &lh);
		if (cfg.lbl.show && x >= lx && x < lx + lw && y >= ly && y < ly + lh)
			return s->vis[p];
	}
	return -1;
}

/* ---- drawing ---- */

static void fonts_init(void)
{
	font = pango_font_description_from_string(cfg.lbl.font);
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

	switch (cfg.lbl.option) {
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

static void draw_text_e(cairo_t *cr, const char *text, int x, int y, int w, int big,
		int center_v, int h, double alpha, PangoEllipsizeMode ellipsize)
{
	PangoLayout *pl = pango_cairo_create_layout(cr);
	int tw, th;

	pango_layout_set_font_description(pl, big ? font_big : font);
	pango_layout_set_text(pl, text, -1);
	pango_layout_set_width(pl, w * PANGO_SCALE);
	pango_layout_set_ellipsize(pl, ellipsize);
	pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
	pango_layout_get_pixel_size(pl, &tw, &th);
	cairo_set_source_rgba(cr, 1, 1, 1, alpha);
	cairo_move_to(cr, x, center_v ? y + (h - th) / 2.0 : y);
	pango_cairo_show_layout(cr, pl);
	g_object_unref(pl);
}

static void draw_text(cairo_t *cr, const char *text, int x, int y, int w, int big,
		int center_v, int h, double alpha)
{
	draw_text_e(cr, text, x, y, w, big, center_v, h, alpha, PANGO_ELLIPSIZE_END);
}

/* height reserved under each tile; labels inside the tile need none */
static int label_space(void)
{
	return cfg.lbl.show && cfg.lbl.anchor_v == ANCHOR_V_BELOW ? cfg.lbl.height + LABEL_GAP : 0;
}

/* start of an item of length item in a span from origin: anchor 0 = start, 1 = middle, 2 = end, gap off the edge */
static int anchor_pos(int anchor, int origin, int len, int item, int gap)
{
	return anchor == 0 ? origin + gap : anchor == 1 ? origin + (len - item) / 2 : origin + len - item - gap;
}

/* where the label box goes for tile r */
static void label_rect(const struct rect *r, int *x, int *y, int *w, int *h)
{
	const struct label_cfg *l = &cfg.lbl;

	*w = (int)(r->w * l->width);
	*h = l->height;
	/* a side anchor keeps the gap on its side; the box never overhangs the other */
	if (l->anchor_h != ANCHOR_H_CENTER && *w > r->w - LABEL_GAP)
		*w = r->w - LABEL_GAP;
	*x = anchor_pos(l->anchor_h, r->x, r->w, *w, LABEL_GAP);
	*y = l->anchor_v == ANCHOR_V_BELOW ? r->y + r->h + LABEL_GAP : anchor_pos(l->anchor_v, r->y, r->h, *h, LABEL_GAP);
	*x += l->offset_x;
	*y += l->offset_y;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	if (w <= 0 || h <= 0)
		return;
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	if (r < 0)
		r = 0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* tile outline grown by grow pixels on every side, the radius growing with it; a square tile stays square */
static void tile_path(cairo_t *cr, const struct rect *r, double grow)
{
	rounded_rect(cr, r->x - grow, r->y - grow, r->w + 2 * grow, r->h + 2 * grow,
		cfg.tile_radius ? cfg.tile_radius + grow : 0);
}

static void draw_label(cairo_t *cr, const struct rect *r, const char *text, int selected)
{
	const struct label_cfg *l = &cfg.lbl;
	const double *bg = selected && l->bg_hl[3] >= 0 ? l->bg_hl : l->bg;
	const double *border = l->border;
	double k = l->opacity / 255.0, dim = selected ? 1.0 : 0.7;
	int bw, bh, pad = l->radius / 2 > 4 ? l->radius / 2 : 4, tw, th, x, y, lw;
	PangoLayout *pl;

	/* no label colors configured: a dark tint of the tile frame color with the frame color as outline */
	if (!l->bg_set) {
		static double fill[4], edge[4];
		const double *frame = selected ? cfg.highlight : cfg.border;

		for (int i = 0; i < 3; i++) {
			fill[i] = 0.05 + 0.25 * frame[i];
			edge[i] = frame[i];
		}
		fill[3] = edge[3] = k > 0 ? 1.0 / k : 0; /* opaque whatever the opacity */
		bg = fill;
		border = edge;
	}
	label_rect(r, &x, &y, &bw, &bh);
	if (bg[3] > 0) {
		cairo_set_source_rgba(cr, bg[0], bg[1], bg[2], bg[3] * k);
		rounded_rect(cr, x, y, bw, bh, l->radius);
		cairo_fill(cr);
	}
	if (border[3] > 0) {
		cairo_set_source_rgba(cr, border[0], border[1], border[2], border[3] * k);
		cairo_set_line_width(cr, 1);
		rounded_rect(cr, x + 0.5, y + 0.5, bw - 1, bh - 1, l->radius - 0.5);
		cairo_stroke(cr);
	}

	pl = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(pl, font);
	pango_layout_set_text(pl, text, -1);
	lw = bw - 2 * pad;
	pango_layout_set_width(pl, (lw > 1 ? lw : 1) * PANGO_SCALE);
	pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
	pango_layout_get_pixel_size(pl, &tw, &th);
	if (l->outline[3] > 0) {
		cairo_move_to(cr, x + pad, y + (bh - th) / 2.0);
		pango_cairo_layout_path(cr, pl);
		cairo_set_source_rgba(cr, l->outline[0], l->outline[1], l->outline[2], l->outline[3] * dim);
		cairo_set_line_width(cr, 3);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
	}
	cairo_set_source_rgba(cr, l->text[0], l->text[1], l->text[2], l->text[3] * dim);
	cairo_move_to(cr, x + pad, y + (bh - th) / 2.0);
	pango_cairo_show_layout(cr, pl);
	g_object_unref(pl);
}

static void paint_icon(cairo_t *cr, cairo_surface_t *ic, int x, int y)
{
	cairo_set_source_surface(cr, ic, x, y);
	cairo_paint(cr);
}

/* 0..1, eased out; 1 once the open animation is over or switched off */
static double anim_progress(void)
{
	double t;

	if (!cfg.anim || !cfg.anim_ms)
		return 1;
	if (!st.anim_t0)
		st.anim_t0 = now_ms();
	t = (now_ms() - st.anim_t0) / cfg.anim_ms;
	if (t >= 1)
		return 1;
	t = 1 - t;
	return 1 - t * t * t;
}

/* first line printed by background.image_cmd, trimmed and ~/ expanded; 0 on failure, timeout or a non-zero exit */
static int bg_cmd_path(char *path, size_t len)
{
	char line[PATH_MAX], *s;
	const char *home = getenv("HOME");
	int fd[2], status, got = 0, ok = 0;
	pid_t pid;
	struct pollfd pf;

	if (pipe(fd))
		return 0;
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return 0;
	}
	if (!pid) {
		signal(SIGPIPE, SIG_DFL);
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		close(fd[1]);
		execl("/bin/sh", "sh", "-c", cfg.bg_image_cmd, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	pf.fd = fd[0];
	pf.events = POLLIN;
	/* a hung command must not freeze the overlay */
	while (got < (int)sizeof(line) - 1 && poll(&pf, 1, 1000) > 0) {
		ssize_t n = read(fd[0], line + got, sizeof(line) - 1 - got);

		if (n <= 0) {
			ok = n == 0;
			break;
		}
		got += n;
	}
	close(fd[0]);
	if (!ok)
		kill(pid, SIGKILL);
	if (waitpid(pid, &status, 0) < 0 || !ok || !WIFEXITED(status) || WEXITSTATUS(status))
		return 0;
	line[got] = 0;
	line[strcspn(line, "\r\n")] = 0;
	for (s = line; *s == ' ' || *s == '\t'; s++)
		;
	for (char *e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); )
		*--e = 0;
	if (s[0] == '~' && s[1] == '/' && home)
		snprintf(path, len, "%s%s", home, s + 1);
	else
		snprintf(path, len, "%s", s);
	return path[0] != 0;
}

/* loaded on the first draw, so a quick tap that never draws does not pay for the decode */
static cairo_surface_t *bg_image(void)
{
	char path[PATH_MAX];

	if (st.bg_tried)
		return st.bg_img;
	st.bg_tried = 1;
	if (cfg.bg_image_cmd[0] && bg_cmd_path(path, sizeof(path))) {
		st.bg_img = bgimage_load(path);
		if (!st.bg_img)
			fprintf(stderr, "skippy-wl: cannot read background image %s\n", path);
	} else if (cfg.bg_image_cmd[0]) {
		fprintf(stderr, "skippy-wl: background.image_cmd gave no path\n");
	}
	if (!st.bg_img && cfg.bg_image[0]) {
		st.bg_img = bgimage_load(cfg.bg_image);
		if (!st.bg_img)
			fprintf(stderr, "skippy-wl: cannot read background image %s\n", cfg.bg_image);
	}
	return st.bg_img;
}

/* scale and center the image to cover the surface, once per surface size */
static cairo_surface_t *bg_cover(struct surf *s)
{
	cairo_surface_t *out;
	cairo_t *cr;
	double iw, ih, sc;

	if (s->bg && cairo_image_surface_get_width(s->bg) == s->w && cairo_image_surface_get_height(s->bg) == s->h)
		return s->bg;
	cairo_surface_destroy(s->bg);
	s->bg = NULL;
	iw = cairo_image_surface_get_width(st.bg_img);
	ih = cairo_image_surface_get_height(st.bg_img);
	sc = s->w / iw > s->h / ih ? s->w / iw : s->h / ih;
	out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s->w, s->h);
	cr = cairo_create(out);
	cairo_translate(cr, (s->w - iw * sc) / 2, (s->h - ih * sc) / 2);
	cairo_scale(cr, sc, sc);
	cairo_set_source_surface(cr, st.bg_img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_destroy(cr);
	s->bg = out;
	return out;
}

/* the open animation: fade and grow from 94% around the tile's center, clipped to the tile and its label */
static void tile_anim_begin(cairo_t *cr, const struct rect *r, double fade, int lp)
{
	double sc = 0.94 + 0.06 * fade, cx = r->x + r->w / 2.0, cy = r->y + (r->h + label_space()) / 2.0;

	cairo_save(cr);
	cairo_rectangle(cr, r->x - 8 - lp, r->y - 8 - lp, r->w + 16 + 2 * lp, r->h + 2 * label_space() + 24 + 2 * lp);
	cairo_clip(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, sc, sc);
	cairo_translate(cr, -cx, -cy);
	cairo_push_group(cr);
}

static void tile_anim_end(cairo_t *cr, double fade)
{
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, fade);
	cairo_restore(cr);
}

/* returns 1 while the open animation is still running */
static int render(struct surf *s, cairo_t *cr)
{
	/* monitors.mode = pointer: nothing to show until the pointer picks the output */
	int waiting = cfg.monitors == MON_POINTER && !st.target;
	double fade = waiting ? 0 : anim_progress();
	/* the open animation clips each tile; leave room for a label moved or sized beyond it */
	int lp = (abs(cfg.lbl.offset_x) > abs(cfg.lbl.offset_y) ? abs(cfg.lbl.offset_x) : abs(cfg.lbl.offset_y)) + cfg.lbl.height;
	int animating = fade < 1 && !waiting;

	if (st.hidden) {
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		return 0;
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	if (bg_image()) {
		/* colors.background, when set, is a tint over the image */
		cairo_set_source_surface(cr, bg_cover(s), 0, 0);
		cairo_paint_with_alpha(cr, fade);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	if (!st.bg_img || cfg.bg_set) {
		cairo_set_source_rgba(cr, cfg.bg[0], cfg.bg[1], cfg.bg[2], cfg.bg[3] * fade);
		cairo_paint(cr);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (int p = 0; p < s->nvis && s->rects && s->scaled; p++) {
		int i = s->vis[p];
		struct rect *r = &s->rects[p];
		struct tile *tl = &st.tiles[i];
		int selected = i == st.sel;

		if (animating)
			tile_anim_begin(cr, r, fade, lp);
		if (s->scaled[p]) {
			cairo_save(cr);
			tile_path(cr, r, 0);
			cairo_clip(cr);
			cairo_set_source_surface(cr, s->scaled[p], r->x, r->y);
			cairo_paint(cr);
			/* corner icon, unless the tile is too small to carry one; clipped with the preview */
			if (cfg.icon && r->w >= 3 * cfg.icon_size && r->h >= 3 * cfg.icon_size) {
				cairo_surface_t *ic = icon_get(tl->app_id, cfg.icon_theme, cfg.icon_size);

				if (ic)
					paint_icon(cr, ic, anchor_pos(cfg.icon_h, r->x, r->w, cfg.icon_size, 6),
						anchor_pos(cfg.icon_v, r->y, r->h, cfg.icon_size, 6));
			}
			cairo_restore(cr);
		} else {
			int isz = (r->w < r->h ? r->w : r->h) / 2;
			cairo_surface_t *ic = NULL;

			/* a few fixed sizes, so a relayout reuses the cached icons */
			isz = isz >= 256 ? 256 : isz >= 128 ? 128 : isz >= 96 ? 96 : isz >= 64 ? 64 :
				isz >= 48 ? 48 : isz >= 32 ? 32 : 0;
			cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
			tile_path(cr, r, 0);
			cairo_fill(cr);
			if (cfg.icon && isz)
				ic = icon_get(tl->app_id, cfg.icon_theme, isz);
			if (ic)
				paint_icon(cr, ic, r->x + (r->w - isz) / 2, r->y + (r->h - isz) / 2);
			else
				draw_text(cr, tl->app_id[0] ? tl->app_id : "?", r->x, r->y, r->w, 1, 1, r->h, 0.8);
		}

		if (selected) {
			cairo_set_source_rgba(cr, cfg.highlight[0], cfg.highlight[1], cfg.highlight[2],
				cfg.highlight[3]);
			cairo_set_line_width(cr, 4);
			/* the 4 px stroke is centered on the path: its inner edge keeps the tile radius */
			tile_path(cr, r, 2);
		} else {
			cairo_set_source_rgba(cr, cfg.border[0], cfg.border[1], cfg.border[2], cfg.border[3]);
			cairo_set_line_width(cr, 1);
			tile_path(cr, r, -0.5);
		}
		cairo_stroke(cr);
		if (tl->picked) {
			cairo_set_source_rgba(cr, cfg.highlight[0], cfg.highlight[1], cfg.highlight[2],
				cfg.highlight[3] * 0.35);
			tile_path(cr, r, 0);
			cairo_fill(cr);
		}

		if (animating)
			tile_anim_end(cr, fade);
	}
	/* labels last, the selected one on top, so a label that leaves its tile is not covered by the next preview */
	for (int pass = 0; cfg.lbl.show && pass < 2; pass++) {
		for (int p = 0; p < s->nvis && s->rects && s->scaled; p++) {
			int i = s->vis[p];
			struct rect *r = &s->rects[p];
			char label[256];

			if ((i == st.sel) != pass)
				continue;
			if (animating)
				tile_anim_begin(cr, r, fade, lp);
			draw_label(cr, r, label_text(&st.tiles[i], label, sizeof(label)), i == st.sel);
			if (animating)
				tile_anim_end(cr, fade);
		}
	}
	if (st.filtering && !waiting) {
		int bw = s->w < 520 ? s->w - 40 : 480, bx = (s->w - bw) / 2, nm = any_match() ? 0 : 120;

		cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
		cairo_rectangle(cr, bx, BAR_Y, bw, BAR_H);
		cairo_fill(cr);
		draw_text_e(cr, "/", bx + 12, BAR_Y, 20, 1, 1, BAR_H, 1.0, PANGO_ELLIPSIZE_END);
		/* a long query keeps its newest characters visible */
		draw_text_e(cr, st.query, bx + 36, BAR_Y, bw - 48 - nm, 1, 1, BAR_H, 1.0, PANGO_ELLIPSIZE_START);
		if (nm)
			draw_text_e(cr, "(no match)", bx + bw - nm - 12, BAR_Y, nm, 0, 1, BAR_H, 0.6, PANGO_ELLIPSIZE_END);
	}
	return animating;
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
	int animating;

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
	animating = render(s, cr);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	b->busy = 1;
	s->dirty = animating; /* the next frame callback draws the next step */
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
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer, cfg.panels_show ? 0 : -1);
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
	cairo_surface_destroy(s->bg);
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
	if (st.quit || i < 0 || i >= st.n || !tile_matches(&st.tiles[i]))
		return;
	if (st.multi) {
		st.tiles[i].picked = st.tiles[i].picked ? 0 : ++st.pick_seq;
		redraw_all();
		return;
	}
	st.pick = st.tiles[i].t;
	st.quit = 1;
}

static void close_tile(int i)
{
	if (st.quit || st.multi || i < 0 || i >= st.n || !tile_matches(&st.tiles[i]))
		return;
	/* the tile goes away when the window's closed event arrives */
	if (toplevel_close(st.tiles[i].t) < 0)
		fprintf(stderr, "skippy-wl: cannot close this window (no wlr handle paired)\n");
}

static void minimize_tile(int i)
{
	if (st.quit || st.multi || i < 0 || i >= st.n || !tile_matches(&st.tiles[i]))
		return;
	if (toplevel_set_minimized(st.tiles[i].t, !(toplevel_status(st.tiles[i].t) & STATUS_MINIMIZED)) < 0)
		fprintf(stderr, "skippy-wl: cannot minimize this window (no wlr handle paired)\n");
}

static void set_sel(int to)
{
	if (to != st.sel) {
		st.sel = to;
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
	int to = st.sel;

	for (int k = 0; k < st.n; k++) {
		to = (to + dir + st.n) % st.n;
		if (tile_matches(&st.tiles[to])) {
			set_sel(to);
			return;
		}
	}
}

/* the query changed: re-flow and keep the selection on a visible tile */
static void refilter(void)
{
	struct surf *a;

	g_free(st.query_cf);
	st.query_cf = fold(st.query);

	for (struct surf *s = st.surfs; s; s = s->next)
		if (s->w)
			relayout(s);
	if (st.sel < 0 || st.sel >= st.n || !tile_matches(&st.tiles[st.sel])) {
		/* prefer a tile on the output the user is working on */
		a = active_surf();
		if (a && a->nvis) {
			st.sel = a->vis[0];
		} else {
			for (int i = 0; i < st.n; i++)
				if (tile_matches(&st.tiles[i])) {
					st.sel = i;
					break;
				}
		}
	}
	redraw_all();
}

/* the filter bar opened or closed: tiles move only if the bar needs its own strip */
static void bar_toggled(void)
{
	if (cfg.margin < BAR_BOTTOM)
		relayout_all();
	else
		redraw_all();
}

static int printable(const char *text)
{
	return (unsigned char)text[0] >= 0x20 && text[0] != 0x7f;
}

static void query_add(const char *text)
{
	size_t n = strlen(text);

	st.filtering = 1;
	if (st.qlen + n < sizeof(st.query)) {
		memcpy(st.query + st.qlen, text, n + 1);
		st.qlen += (int)n;
	}
	refilter();
}

static void query_pop(void)
{
	if (!st.qlen) {
		st.filtering = 0;
		bar_toggled();
		return;
	}
	do
		st.qlen--;
	while (st.qlen && ((unsigned char)st.query[st.qlen] & 0xc0) == 0x80);
	st.query[st.qlen] = 0;
	refilter();
}

static void query_clear(void)
{
	st.qlen = 0;
	st.query[0] = 0;
	st.filtering = 0;
	refilter();
}

void overlay_key(uint32_t sym, const char *text)
{
	struct surf *s;
	int act;

	if (st.quit)
		return;
	st.t_last = now_ms();
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
	if (st.multi && sym == XKB_KEY_space) {
		choose(st.sel);
		return;
	}
	if (st.filtering) {
		if (sym == XKB_KEY_BackSpace) {
			query_pop();
			return;
		}
		if (act == ACT_CANCEL && !printable(text)) {
			query_clear();
			return;
		}
		if (printable(text)) {
			query_add(text);
			return;
		}
	} else if (act < 0 && printable(text)) {
		/* "/" only opens the filter; any other unbound key also starts it */
		if (text[0] == '/' && !text[1]) {
			st.filtering = 1;
			bar_toggled();
		} else {
			query_add(text);
		}
		return;
	}
	switch (act) {
	case ACT_CANCEL:
		st.quit = 1;
		break;
	case ACT_SELECT:
		if (st.multi)
			st.done = st.quit = 1;
		else
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

	st.cur_mods = depressed;
	if (!st.dir || !cfg.pivot || st.pivot_done || st.quit)
		return;
	st.mods_seen = 1;
	if (first)
		st.held = depressed;
	if (st.held && (depressed & st.held))
		return;
	/* a pause with the overlay up: the user is looking around, so it stays open */
	if (st.held && !st.hidden && cfg.switch_hold && now_ms() - st.t_last > cfg.switch_hold) {
		st.pivot_done = 1;
		return;
	}
	/* nothing held at open: the key was already released */
	choose(st.sel);
}

void overlay_cycle(int dir)
{
	if (st.quit)
		return;
	/* the keybind again after the overlay stayed open: this release picks */
	if (st.pivot_done && st.cur_mods) {
		st.pivot_done = 0;
		st.held = st.cur_mods;
	}
	st.t_last = now_ms();
	cycle(dir);
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
	/* the parked cursor's enter event must not replace the --next/--prev or multi-select start */
	if ((st.dir || st.multi) && !st.ptr_seen) {
		st.ptr_seen = 1;
		ptr_x = x;
		ptr_y = y;
		return;
	}
	ptr_x = x;
	ptr_y = y;
	st.scrolled = 0;
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
	/* after the wheel the cursor is not on the selection: clicks act on it until the cursor moves */
	hit = st.scrolled ? st.sel : hit_test(st.ptr_surf, ptr_x, ptr_y);
	if (button == BTN_LEFT) {
		if (hit >= 0)
			choose(hit);
		else if (!st.multi)
			st.quit = 1;
	} else if (button == BTN_MIDDLE) {
		close_tile(hit);
	}
}

void overlay_pointer_scroll(int dir)
{
	st.scrolled = 1;
	overlay_cycle(dir);
}

/* ---- run ---- */

static int quit_requested(void *arg)
{
	(void)arg;
	return st.quit;
}

int overlay_run(struct globals *g, const struct filter *filter, int dir, int multi)
{
	struct output *o;
	int rc = 0, input_up = 0;

	memset(&st, 0, sizeof(st));
	st.g = g;
	st.filter = filter;
	st.dir = multi ? 0 : dir;
	st.multi = multi;
	config_load(&cfg);
	fonts_init();
	active = 1;
	toplevels_set_closed_cb(on_toplevel_closed);
	if (tiles_build() < 0) {
		rc = -1;
		goto out;
	}
	if (!st.n)
		goto out;
	/* the first tile is the current window only if it has focus (a hidden or unlisted window may have it) */
	st.sel = st.dir > 0 && st.n > 1 && (toplevel_status(st.tiles[0].t) & STATUS_ACTIVATED) ? 1 :
		st.dir < 0 ? st.n - 1 : 0;
	/* this process has no usage history, so the focused window may not be the first tile */
	if (multi)
		for (int i = 0; i < st.n; i++)
			if (toplevel_status(st.tiles[i].t) & STATUS_ACTIVATED) {
				st.sel = i;
				break;
			}
	/* a quick tap then picks the window without the capture delay or any drawing */
	st.hidden = st.dir && cfg.pivot && cfg.switch_wait > 0;
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
	st.t_last = now_ms();
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
	cairo_surface_destroy(st.bg_img);
	/* print only after the overlay is gone, so a consumer that focuses windows does not race it */
	if (st.multi)
		wl_display_roundtrip(g->display);
	if (st.multi && !rc) {
		int any = 0;

		/* toggle order; a cancel prints nothing */
		for (int seq = 1; st.done && seq <= st.pick_seq; seq++)
			for (int i = 0; i < st.n; i++) {
				if (st.tiles[i].picked != seq || !tile_matches(&st.tiles[i]))
					continue;
				any = 1;
				printf("%s\n", st.tiles[i].t->identifier);
			}
		if (fflush(stdout) == EOF)
			rc = -1;
		else if (!st.done || !any)
			rc = 1;
	}
	g_free(st.query_cf);
	st.query_cf = NULL;
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
	tiles_finish();
	icons_clear();
	wl_display_roundtrip(g->display);
	pango_font_description_free(font);
	pango_font_description_free(font_big);
	active = 0;
	return rc;
}

int overlay_switch(struct globals *g, const struct filter *filter)
{
	int rc = 0, skip;

	memset(&st, 0, sizeof(st));
	st.g = g;
	st.filter = filter;
	config_load(&cfg);
	/* before the window list: its roundtrips may deliver a closed event that frees a toplevel */
	input_init(g);
	if (tiles_build() < 0) {
		rc = -1;
		goto out;
	}
	/* the first window is the current one, unless nothing has focus */
	skip = st.n && (toplevel_status(st.tiles[0].t) & STATUS_ACTIVATED) ? 1 : 0;
	if (st.n <= skip)
		goto out;
	if (toplevel_activate(st.tiles[skip].t, input_seat()) < 0)
		fprintf(stderr, "skippy-wl: cannot activate this window (no wlr handle paired)\n");
	wl_display_roundtrip(g->display);
out:
	input_finish();
	tiles_finish();
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
