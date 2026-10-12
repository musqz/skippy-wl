/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>

#include "layout.h"

#define MAXN 40

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

static int overlap(struct rect a, struct rect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void check_layout(int n, struct size area, int spacing, int label_h, struct size sz)
{
	struct size src[MAXN];
	struct rect r[MAXN];
	int i, j, cols;

	for (i = 0; i < n; i++) {
		src[i] = sz;
		/* mix aspect ratios */
		if (i % 3 == 1) {
			src[i].w = sz.w / 2 + 1;
		}
		if (i % 3 == 2) {
			src[i].h = sz.h / 2 + 1;
		}
	}
	cols = layout_grid(src, n, area, spacing, label_h, r);
	CHECK(cols >= 1 && cols <= n, "n=%d cols=%d", n, cols);
	for (i = 0; i < n; i++) {
		CHECK(r[i].w >= 1 && r[i].h >= 1, "n=%d i=%d empty rect", n, i);
		CHECK(r[i].x >= 0 && r[i].y >= 0 &&
			r[i].x + r[i].w <= area.w && r[i].y + r[i].h + label_h <= area.h,
			"n=%d i=%d outside area: %d,%d %dx%d", n, i, r[i].x, r[i].y, r[i].w, r[i].h);
		CHECK(r[i].w <= src[i].w && r[i].h <= src[i].h, "n=%d i=%d upscaled", n, i);
		/* aspect within rounding */
		CHECK(abs(r[i].w * src[i].h - r[i].h * src[i].w) <= src[i].w + src[i].h,
			"n=%d i=%d aspect %dx%d from %dx%d", n, i, r[i].w, r[i].h, src[i].w, src[i].h);
		for (j = i + 1; j < n; j++)
			CHECK(!overlap(r[i], r[j]), "n=%d tiles %d and %d overlap", n, i, j);
	}
}

int main(void)
{
	struct size areas[] = { {1920, 1080}, {1366, 768}, {800, 600}, {3840, 2160}, {600, 900} };
	struct size win = { 1022, 681 };
	struct size src = { 1000, 500 };
	struct rect r;
	unsigned a;
	int n;

	CHECK(layout_grid(&src, 0, areas[0], 20, 24, &r) == 0, "n=0");

	/* a window smaller than the area is centred at its own size */
	CHECK(layout_grid(&src, 1, areas[0], 20, 24, &r) == 1, "n=1 cols");
	CHECK(r.w == 1000 && r.h == 500, "n=1 size %dx%d", r.w, r.h);
	CHECK(r.x == (1920 - 1000) / 2, "n=1 x=%d", r.x);

	for (a = 0; a < sizeof(areas) / sizeof(areas[0]); a++)
		for (n = 1; n <= MAXN; n++)
			check_layout(n, areas[a], 20, 24, win);

	/* area too small for the spacing: tiles overflow but stay non-empty */
	{
		struct size tiny_src[12];
		struct rect tiny_r[12];
		int i;

		for (i = 0; i < 12; i++)
			tiny_src[i] = win;
		CHECK(layout_grid(tiny_src, 12, (struct size){ 100, 80 }, 20, 24, tiny_r) >= 1, "tiny cols");
		for (i = 0; i < 12; i++)
			CHECK(tiny_r[i].w >= 1 && tiny_r[i].h >= 1, "tiny i=%d empty", i);
	}

	if (failures)
		fprintf(stderr, "%d failures\n", failures);
	return failures ? 1 : 0;
}
