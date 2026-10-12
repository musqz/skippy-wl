/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fnmatch.h>
#include <string.h>

#include "filter.h"

static const struct {
	const char *name;
	unsigned bit;
} status_names[] = {
	{ "minimized", STATUS_MINIMIZED }, { "maximized", STATUS_MAXIMIZED },
	{ "fullscreen", STATUS_FULLSCREEN }, { "activated", STATUS_ACTIVATED },
};

static unsigned status_bit(const char *name)
{
	for (size_t i = 0; i < sizeof(status_names) / sizeof(status_names[0]); i++)
		if (!strcmp(name, status_names[i].name))
			return status_names[i].bit;
	return 0;
}

static int term_glob(const char *term, const void *value)
{
	return fnmatch(term, (const char *)value, FNM_CASEFOLD) == 0;
}

static int term_status(const char *term, const void *status)
{
	return (*(const unsigned *)status & status_bit(term)) != 0;
}

/*
 * Copies the next term of a comma list into out (FILTER_LEN bytes), without
 * surrounding spaces; "\," is a literal comma. Returns where to continue; the
 * list is done when that points at the end.
 */
static const char *next_term(const char *p, char *out)
{
	size_t n = 0;

	while (*p == ' ')
		p++;
	while (*p && *p != ',') {
		if (p[0] == '\\' && p[1] == ',') {
			out[n++] = ',';
			p += 2;
		} else {
			out[n++] = *p++;
		}
	}
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = 0;
	return *p == ',' ? p + 1 : p;
}

static int list_match(const char *list, int (*term_fn)(const char *, const void *), const void *value)
{
	char term[FILTER_LEN];
	const char *p = list;
	int has_plain = 0, plain_ok = 0;

	do {
		const char *tok = term;

		p = next_term(p, term);
		if (!*tok)
			continue;
		if (*tok == '!') {
			if (term_fn(tok + 1, value))
				return 0;
		} else {
			has_plain = 1;
			plain_ok |= term_fn(tok, value);
		}
	} while (*p);
	return !has_plain || plain_ok;
}

int filter_set(struct filter *f, const char *key, const char *value)
{
	char term[FILTER_LEN];
	const char *p;

	if (strlen(value) >= FILTER_LEN)
		return -1;
	if (!strcmp(key, "app-id")) {
		memcpy(f->app_id, value, strlen(value) + 1);
		return 0;
	}
	if (!strcmp(key, "title")) {
		memcpy(f->title, value, strlen(value) + 1);
		return 0;
	}
	if (strcmp(key, "status"))
		return -1;
	p = value;
	do {
		p = next_term(p, term);
		if (term[0] && !status_bit(term + (term[0] == '!')))
			return -1;
	} while (*p);
	memcpy(f->status, value, strlen(value) + 1);
	return 0;
}

int filter_match(const struct filter *f, const char *app_id, const char *title, unsigned status)
{
	/* an unpaired window has no known state: a status filter cannot say yes or no */
	if ((status & STATUS_UNKNOWN) && f->status[0])
		return 0;
	return list_match(f->app_id, term_glob, app_id ? app_id : "") &&
		list_match(f->title, term_glob, title ? title : "") &&
		list_match(f->status, term_status, &status);
}
