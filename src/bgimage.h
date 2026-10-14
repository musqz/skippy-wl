/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_BGIMAGE_H
#define SKIPPY_BGIMAGE_H

#include <cairo.h>

/* PNG, JPEG or AVIF/HEIC file as an ARGB32 surface; NULL if it cannot be read. Caller destroys it. */
cairo_surface_t *bgimage_load(const char *path);

#endif
