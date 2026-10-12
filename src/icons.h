/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_ICONS_H
#define SKIPPY_ICONS_H

#include <cairo.h>
#include <stddef.h>

/* Icon= of a .desktop file; -1 if the file or the key is missing */
int desktop_icon_name(const char *path, char *out, size_t len);

/*
 * Icon of an application as a size x size surface, or NULL if none is found. Looked up through
 * the .desktop file named after app_id, then the icon theme and its parents, then hicolor.
 * The surface belongs to a cache: do not destroy it, icons_clear() frees everything.
 */
cairo_surface_t *icon_get(const char *app_id, const char *theme, int size);
void icons_clear(void);

#endif
