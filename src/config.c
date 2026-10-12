/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"

static const char *const action_names[ACT_COUNT] = {
	"select", "cancel", "close", "minimize", "left", "right", "up", "down", "home", "end", "next", "prev",
};

/* all-or-nothing: an invalid or over-long list leaves the current binds untouched */
static int set_keys(struct config *c, enum action a, const char *names)
{
	uint32_t keys[MAX_BINDS] = { 0 };
	char buf[128];
	char *tok, *save;
	int n = 0;

	if (strlen(names) >= sizeof(buf))
		return -1;
	memcpy(buf, names, strlen(names) + 1);
	for (tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
		if (n == MAX_BINDS)
			return -1;
		keys[n] = xkb_keysym_from_name(tok, XKB_KEYSYM_NO_FLAGS);
		if (keys[n] == XKB_KEY_NoSymbol)
			return -1;
		n++;
	}
	if (!n)
		return -1;
	/* a key bound here is taken away from whatever action had it */
	for (int other = 0; other < ACT_COUNT; other++)
		for (int i = 0; i < MAX_BINDS; i++)
			for (int j = 0; j < n && other != (int)a; j++)
				if (c->keys[other][i] == keys[j])
					c->keys[other][i] = 0;
	memcpy(c->keys[a], keys, sizeof(keys));
	return 0;
}

void config_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	c->margin = 48;
	c->spacing = 28;
	c->label_h = 28;
	c->icon = 1;
	c->icon_size = 48;
	snprintf(c->icon_theme, sizeof(c->icon_theme), "hicolor");
	c->pivot = 1;
	c->switch_wait = 100;
	snprintf(c->font, sizeof(c->font), "Sans 11");
	c->bg[3] = 1.0;
	c->highlight[0] = 0.35;
	c->highlight[1] = 0.6;
	c->highlight[2] = 1.0;
	c->highlight[3] = 1.0;
	c->border[0] = c->border[1] = c->border[2] = 1.0;
	c->border[3] = 0.18;
	set_keys(c, ACT_SELECT, "Return, KP_Enter");
	set_keys(c, ACT_CANCEL, "Escape");
	set_keys(c, ACT_CLOSE, "Delete");
	set_keys(c, ACT_MINIMIZE, "m, M");
	set_keys(c, ACT_LEFT, "Left");
	set_keys(c, ACT_RIGHT, "Right");
	set_keys(c, ACT_UP, "Up");
	set_keys(c, ACT_DOWN, "Down");
	set_keys(c, ACT_HOME, "Home");
	set_keys(c, ACT_END, "End");
	set_keys(c, ACT_NEXT, "Tab");
	set_keys(c, ACT_PREV, "ISO_Left_Tab");
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || *end || v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

/* #RRGGBB or #RRGGBBAA */
static int parse_color(const char *s, double out[4])
{
	unsigned int v[4] = { 0, 0, 0, 255 };
	size_t len = strlen(s);

	if (s[0] != '#' || (len != 7 && len != 9))
		return -1;
	for (size_t i = 1; i < len; i++)
		if (!isxdigit((unsigned char)s[i]))
			return -1;
	for (size_t i = 0; i < (len - 1) / 2; i++) {
		char byte[3] = { s[1 + 2 * i], s[2 + 2 * i], 0 };

		v[i] = (unsigned int)strtoul(byte, NULL, 16);
	}
	for (int i = 0; i < 4; i++)
		out[i] = v[i] / 255.0;
	return 0;
}

int config_set(struct config *c, const char *key, const char *value)
{
	if (!strcmp(key, "margin"))
		return parse_int(value, 0, 1000, &c->margin);
	if (!strcmp(key, "spacing"))
		return parse_int(value, 0, 1000, &c->spacing);
	if (!strcmp(key, "label_height"))
		return parse_int(value, 8, 200, &c->label_h);
	if (!strcmp(key, "icon_size"))
		return parse_int(value, 16, 256, &c->icon_size);
	if (!strcmp(key, "icon")) {
		if (!strcmp(value, "true"))
			c->icon = 1;
		else if (!strcmp(value, "false"))
			c->icon = 0;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "icon_theme")) {
		if (!*value || strlen(value) >= sizeof(c->icon_theme) || strchr(value, '/'))
			return -1;
		snprintf(c->icon_theme, sizeof(c->icon_theme), "%s", value);
		return 0;
	}
	if (!strcmp(key, "switch_wait"))
		return parse_int(value, 0, 5000, &c->switch_wait);
	if (!strcmp(key, "pivot")) {
		if (!strcmp(value, "true"))
			c->pivot = 1;
		else if (!strcmp(value, "false"))
			c->pivot = 0;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "label")) {
		if (!strcmp(value, "title"))
			c->label = LABEL_TITLE;
		else if (!strcmp(value, "app_id"))
			c->label = LABEL_APP_ID;
		else if (!strcmp(value, "both"))
			c->label = LABEL_BOTH;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "monitors")) {
		if (!strcmp(value, "each"))
			c->monitors = MON_EACH;
		else if (!strcmp(value, "pointer"))
			c->monitors = MON_POINTER;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "font")) {
		if (!*value || strlen(value) >= sizeof(c->font))
			return -1;
		snprintf(c->font, sizeof(c->font), "%s", value);
		return 0;
	}
	if (!strcmp(key, "background"))
		return parse_color(value, c->bg);
	if (!strcmp(key, "highlight")) {
		if (parse_color(value, c->highlight) < 0)
			return -1;
		c->highlight_set = 1;
		return 0;
	}
	if (!strcmp(key, "border")) {
		if (parse_color(value, c->border) < 0)
			return -1;
		c->border_set = 1;
		return 0;
	}
	if (!strcmp(key, "theme_file")) {
		if (strlen(value) >= sizeof(c->theme_file))
			return -1;
		snprintf(c->theme_file, sizeof(c->theme_file), "%s", value);
		return 0;
	}
	if (!strncmp(key, "bind.", 5)) {
		for (int a = 0; a < ACT_COUNT; a++) {
			if (strcmp(key + 5, action_names[a]))
				continue;
			return set_keys(c, (enum action)a, value);
		}
	}
	return -1;
}

int config_action(const struct config *c, uint32_t keysym)
{
	for (int a = 0; a < ACT_COUNT; a++)
		for (int i = 0; i < MAX_BINDS; i++)
			if (c->keys[a][i] && c->keys[a][i] == keysym)
				return a;
	return -1;
}

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = 0;
	return s;
}

/* "window.active.border.color: #rrggbb" lines of a labwc theme */
static void load_theme(struct config *c)
{
	const char *home = getenv("HOME");
	char path[PATH_MAX], line[256];
	FILE *f;

	if (!c->theme_file[0])
		return;
	if (c->theme_file[0] == '~' && c->theme_file[1] == '/' && home)
		snprintf(path, sizeof(path), "%s%s", home, c->theme_file + 1);
	else
		snprintf(path, sizeof(path), "%s", c->theme_file);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "skippy-wl: cannot read theme_file %s\n", path);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':'), *val;
		double rgba[4];

		if (!colon)
			continue;
		*colon = 0;
		val = trim(colon + 1);
		if (parse_color(val, rgba) < 0)
			continue;
		if (!c->highlight_set && !strcmp(trim(line), "window.active.border.color"))
			memcpy(c->highlight, rgba, sizeof(rgba));
		else if (!c->border_set && !strcmp(trim(line), "window.inactive.border.color")) {
			memcpy(c->border, rgba, sizeof(rgba));
			c->border[3] = 0.6; /* the inactive border is quieter than the selected one */
		}
	}
	fclose(f);
}

void config_load(struct config *c)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char path[PATH_MAX], line[256];
	FILE *f;
	int n = 0;

	config_defaults(c);
	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/skippy-wl/config", xdg);
	else if (home)
		snprintf(path, sizeof(path), "%s/.config/skippy-wl/config", home);
	else
		return;
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		int too_long = !strchr(line, '\n') && !feof(f);
		char *s = trim(line), *eq;

		n++;
		while (too_long) {
			int ch = fgetc(f);

			too_long = ch != '\n' && ch != EOF;
		}
		if (!*s || *s == '#')
			continue;
		eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "skippy-wl: %s:%d: expected key = value\n", path, n);
			continue;
		}
		*eq = 0;
		if (config_set(c, trim(s), trim(eq + 1)) < 0)
			fprintf(stderr, "skippy-wl: %s:%d: invalid setting '%s'\n", path, n, trim(s));
	}
	fclose(f);
	load_theme(c);
}
