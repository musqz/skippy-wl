/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_LAYOUT_H
#define SKIPPY_LAYOUT_H

struct size {
	int w, h;
};

struct rect {
	int x, y, w, h;
};

/*
 * Place n thumbnails (source sizes in src[]) on a grid inside area, aspect
 * preserved, never upscaled. out[i] is the thumbnail rect; label_h pixels
 * below each rect are reserved for a title. Returns the column count (0 if
 * n == 0).
 */
int layout_grid(const struct size *src, int n, struct size area, int spacing,
	int label_h, struct rect *out);

#endif
