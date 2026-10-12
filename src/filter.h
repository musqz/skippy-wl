/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_FILTER_H
#define SKIPPY_FILTER_H

enum {
	STATUS_MINIMIZED = 1,
	STATUS_MAXIMIZED = 2,
	STATUS_FULLSCREEN = 4,
	STATUS_ACTIVATED = 8,
	STATUS_UNKNOWN = 16, /* no wlr handle paired: state not known */
};

#define FILTER_LEN 256

/*
 * Each field is a comma separated list of glob patterns ("\\," is a literal
 * comma, spaces around terms are ignored): a window matches if it matches any
 * plain term and none of the "!" terms. Empty = match all.
 */
struct filter {
	char app_id[FILTER_LEN], title[FILTER_LEN];
	char status[FILTER_LEN]; /* minimized, maximized, fullscreen, activated */
};

/* key is "app-id", "title" or "status"; -1 if the key or a status name is invalid */
int filter_set(struct filter *f, const char *key, const char *value);
int filter_match(const struct filter *f, const char *app_id, const char *title, unsigned status);

#endif
