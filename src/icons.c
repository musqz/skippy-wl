/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <librsvg/rsvg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "icons.h"

#define MAX_DIRS 16
#define MAX_THEMES 12

static const int sizes[] = { 16, 22, 24, 32, 48, 64, 96, 128, 256, 512 };
#define N_SIZES (int)(sizeof(sizes) / sizeof(sizes[0]))

static struct entry {
	char *app_id;
	char theme[64];
	int size;
	cairo_surface_t *surf; /* NULL: looked up, nothing found */
} *cache;
static int n_cache;

/* ---- .desktop files ---- */

int desktop_icon_name(const char *path, char *out, size_t len)
{
	char line[512];
	FILE *f = fopen(path, "r");
	int in_entry = 0, rc = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[')
			in_entry = !strcmp(line, "[Desktop Entry]");
		else if (in_entry && !strncmp(line, "Icon=", 5) && line[5] && strlen(line + 5) < len) {
			memcpy(out, line + 5, strlen(line + 5) + 1);
			rc = 0;
			break;
		}
	}
	fclose(f);
	return rc;
}

/* XDG data directories: $XDG_DATA_HOME, then $XDG_DATA_DIRS */
static int data_dirs(char dirs[][512])
{
	const char *home = getenv("HOME"), *xh = getenv("XDG_DATA_HOME");
	const char *xd = getenv("XDG_DATA_DIRS");
	char list[1024];
	char *tok, *save;
	int n = 0;

	if (xh && *xh)
		snprintf(dirs[n++], 512, "%s", xh);
	else if (home)
		snprintf(dirs[n++], 512, "%s/.local/share", home);
	snprintf(list, sizeof(list), "%s", xd && *xd ? xd : "/usr/local/share:/usr/share");
	for (tok = strtok_r(list, ":", &save); tok && n < MAX_DIRS; tok = strtok_r(NULL, ":", &save))
		snprintf(dirs[n++], 512, "%s", tok);
	return n;
}

static int desktop_icon(const char *app_id, char *out, size_t len)
{
	char dirs[MAX_DIRS][512], lower[256], path[1024];
	int n = data_dirs(dirs);

	snprintf(lower, sizeof(lower), "%s", app_id);
	for (char *p = lower; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	for (int i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%.511s/applications/%.255s.desktop", dirs[i], app_id);
		if (desktop_icon_name(path, out, len) == 0)
			return 0;
		snprintf(path, sizeof(path), "%.511s/applications/%.255s.desktop", dirs[i], lower);
		if (desktop_icon_name(path, out, len) == 0)
			return 0;
	}
	return -1;
}

/* ---- theme lookup ---- */

static int in_chain(char names[][64], int n, const char *s)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(names[i], s))
			return 1;
	return 0;
}

/* the theme, its Inherits= (breadth first) and hicolor */
static int theme_chain(char names[][64], char bases[][512], int n_bases, const char *theme)
{
	int n = 0;

	snprintf(names[n++], 64, "%s", theme);
	for (int i = 0; i < n; i++) {
		for (int b = 0; b < n_bases; b++) {
			char path[1100], line[1024];
			FILE *f;

			snprintf(path, sizeof(path), "%.511s/%.63s/index.theme", bases[b], names[i]);
			f = fopen(path, "r");
			if (!f)
				continue;
			while (fgets(line, sizeof(line), f)) {
				char *tok, *save;

				if (strncmp(line, "Inherits=", 9))
					continue;
				line[strcspn(line, "\r\n")] = 0;
				for (tok = strtok_r(line + 9, ", \t", &save); tok && n < MAX_THEMES;
						tok = strtok_r(NULL, ", \t", &save))
					if (strlen(tok) < 64 && !in_chain(names, n, tok))
						snprintf(names[n++], 64, "%s", tok);
				break;
			}
			fclose(f);
			break;
		}
	}
	if (n < MAX_THEMES && !in_chain(names, n, "hicolor"))
		snprintf(names[n++], 64, "hicolor");
	return n;
}

/* sizes[] indexes, nearest first, preferring a larger source over a smaller one */
static void size_order(int want, int *order)
{
	int k = 0;

	for (int i = 0; i < N_SIZES; i++)
		if (sizes[i] >= want)
			order[k++] = i;
	for (int i = N_SIZES - 1; i >= 0; i--)
		if (sizes[i] < want)
			order[k++] = i;
}

static int try_file(char *path, size_t len, const char *base, const char *theme, const char *sub,
		const char *name, const char *ext)
{
	snprintf(path, len, "%.511s/%.63s/%.63s/%.255s.%s", base, theme, sub, name, ext);
	return access(path, R_OK) == 0;
}

static int find_in_theme(const char *base, const char *theme, const char *name, int want,
		char *path, size_t len)
{
	static const char *const exts[] = { "svg", "png" };
	int order[N_SIZES];

	if (try_file(path, len, base, theme, "scalable/apps", name, "svg") ||
			try_file(path, len, base, theme, "apps/scalable", name, "svg"))
		return 0;
	size_order(want, order);
	for (int i = 0; i < N_SIZES; i++)
		for (int e = 0; e < 2; e++) {
			char sub[64];

			snprintf(sub, sizeof(sub), "%dx%d/apps", sizes[order[i]], sizes[order[i]]);
			if (try_file(path, len, base, theme, sub, name, exts[e]))
				return 0;
			snprintf(sub, sizeof(sub), "apps/%d", sizes[order[i]]);
			if (try_file(path, len, base, theme, sub, name, exts[e]))
				return 0;
			snprintf(sub, sizeof(sub), "%d/apps", sizes[order[i]]);
			if (try_file(path, len, base, theme, sub, name, exts[e]))
				return 0;
		}
	return -1;
}

static int find_icon(const char *name, const char *theme, int want, char *path, size_t len)
{
	char dirs[MAX_DIRS][512], bases[MAX_DIRS + 1][512], names[MAX_THEMES][64];
	int n = data_dirs(dirs), nb = 0, nt;
	const char *home = getenv("HOME");

	if (name[0] == '/') {
		snprintf(path, len, "%s", name);
		return access(path, R_OK) == 0 ? 0 : -1;
	}
	if (home)
		snprintf(bases[nb++], 512, "%s/.icons", home);
	for (int i = 0; i < n; i++)
		snprintf(bases[nb++], 512, "%.500s/icons", dirs[i]);
	nt = theme_chain(names, bases, nb, theme);
	for (int t = 0; t < nt; t++)
		for (int b = 0; b < nb; b++)
			if (find_in_theme(bases[b], names[t], name, want, path, len) == 0)
				return 0;
	for (int i = 0; i < n; i++)
		for (int e = 0; e < 2; e++) {
			snprintf(path, len, "%.511s/pixmaps/%.255s.%s", dirs[i], name, e ? "svg" : "png");
			if (access(path, R_OK) == 0)
				return 0;
		}
	return -1;
}

/* ---- loading ---- */

static int ends_with(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && !strcmp(s + a - b, suffix);
}

static cairo_surface_t *load_path(const char *path, int size)
{
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(out);
	double w = size, h = size, scale;
	int ok = 0;

	if (ends_with(path, ".svg")) {
		RsvgHandle *svg = rsvg_handle_new_from_file(path, NULL);

		if (svg) {
			if (!rsvg_handle_get_intrinsic_size_in_pixels(svg, &w, &h) || w <= 0 || h <= 0)
				w = h = size;
			scale = size / (w > h ? w : h);
			RsvgRectangle vp = { (size - w * scale) / 2, (size - h * scale) / 2, w * scale, h * scale };

			ok = rsvg_handle_render_document(svg, cr, &vp, NULL);
			g_object_unref(svg);
		}
	} else if (ends_with(path, ".png")) {
		cairo_surface_t *src = cairo_image_surface_create_from_png(path);

		if (cairo_surface_status(src) == CAIRO_STATUS_SUCCESS) {
			w = cairo_image_surface_get_width(src);
			h = cairo_image_surface_get_height(src);
			scale = size / (w > h ? w : h);
			cairo_translate(cr, (size - w * scale) / 2, (size - h * scale) / 2);
			cairo_scale(cr, scale, scale);
			cairo_set_source_surface(cr, src, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			ok = 1;
		}
		cairo_surface_destroy(src);
	}
	cairo_destroy(cr);
	if (!ok) {
		cairo_surface_destroy(out);
		return NULL;
	}
	return out;
}

static cairo_surface_t *load_named(const char *icon, const char *theme, int size)
{
	char name[256], path[1100];
	size_t len;

	snprintf(name, sizeof(name), "%s", icon);
	len = strlen(name);
	/* Icon= is a theme name, but some files put the file name there */
	if (name[0] != '/' && (ends_with(name, ".svg") || ends_with(name, ".png")))
		name[len - 4] = 0;
	if (find_icon(name, theme, size, path, sizeof(path)) < 0)
		return NULL;
	return load_path(path, size);
}

static cairo_surface_t *lookup(const char *app_id, const char *theme, int size)
{
	char name[256], lower[256];
	cairo_surface_t *s = NULL;

	/* an absolute path is only taken from a .desktop file, never from the app id */
	if (desktop_icon(app_id, name, sizeof(name)) == 0)
		s = load_named(name, theme, size);
	if (!s)
		s = load_named(app_id, theme, size);
	if (!s) {
		snprintf(lower, sizeof(lower), "%s", app_id);
		for (char *p = lower; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		if (strcmp(lower, app_id))
			s = load_named(lower, theme, size);
	}
	return s;
}

cairo_surface_t *icon_get(const char *app_id, const char *theme, int size)
{
	struct entry *e;
	char *copy;

	/* the app id is chosen by the client: it must not name a path */
	if (!app_id || !*app_id || size <= 0 || strchr(app_id, '/') || strlen(app_id) >= 256 ||
			strlen(theme) >= sizeof(cache->theme))
		return NULL;
	for (int i = 0; i < n_cache; i++)
		if (cache[i].size == size && !strcmp(cache[i].app_id, app_id) && !strcmp(cache[i].theme, theme))
			return cache[i].surf;
	copy = strdup(app_id);
	e = realloc(cache, (size_t)(n_cache + 1) * sizeof(*cache));
	if (!copy || !e) {
		free(copy);
		if (e)
			cache = e;
		return NULL;
	}
	cache = e;
	cache[n_cache].app_id = copy;
	cache[n_cache].size = size;
	snprintf(cache[n_cache].theme, sizeof(cache->theme), "%s", theme);
	cache[n_cache].surf = lookup(app_id, theme, size);
	return cache[n_cache++].surf;
}

void icons_clear(void)
{
	for (int i = 0; i < n_cache; i++) {
		if (cache[i].surf)
			cairo_surface_destroy(cache[i].surf);
		free(cache[i].app_id);
	}
	free(cache);
	cache = NULL;
	n_cache = 0;
}
