/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "toplevels.h"

static struct toplevel *head;

static void set(char **dst, const char *s)
{
	free(*dst);
	*dst = strdup(s);
}

static void h_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = d, **p;

	for (p = &head; *p != t; p = &(*p)->next)
		;
	*p = t->next;
	ext_foreign_toplevel_handle_v1_destroy(h);
	free(t->identifier);
	free(t->title);
	free(t->app_id);
	free(t);
}

static void h_done(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)h;
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
}

static void l_finished(void *d, struct ext_foreign_toplevel_list_v1 *l)
{
	(void)d; (void)l;
}

static const struct ext_foreign_toplevel_list_v1_listener list_l = { l_toplevel, l_finished };

void toplevels_init(struct ext_foreign_toplevel_list_v1 *list)
{
	ext_foreign_toplevel_list_v1_add_listener(list, &list_l, NULL);
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
