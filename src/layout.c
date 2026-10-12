/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "layout.h"

static double fit_scale(struct size s, int cw, int ch)
{
	double sx = (double)cw / s.w, sy = (double)ch / s.h;
	double k = sx < sy ? sx : sy;

	return k > 1.0 ? 1.0 : k;
}

static void cell_size(int n, int cols, struct size area, int spacing, int label_h,
		int *cw, int *ch)
{
	int rows = (n + cols - 1) / cols;

	*cw = (area.w - (cols + 1) * spacing) / cols;
	*ch = (area.h - (rows + 1) * spacing) / rows - label_h;
}

int layout_grid(const struct size *src, int n, struct size area, int spacing,
		int label_h, struct rect *out)
{
	int best_cols = 1, cols, rows, i, cw, ch;
	double best = -1.0;

	if (n <= 0)
		return 0;

	/* the column count that gives the largest total thumbnail area wins */
	for (cols = 1; cols <= n; cols++) {
		double total = 0.0;

		cell_size(n, cols, area, spacing, label_h, &cw, &ch);
		if (cw < 1 || ch < 1)
			continue;
		for (i = 0; i < n; i++) {
			double k = fit_scale(src[i], cw, ch);

			total += (src[i].w * k) * (src[i].h * k);
		}
		if (total > best) {
			best = total;
			best_cols = cols;
		}
	}

	cols = best_cols;
	rows = (n + cols - 1) / cols;
	cell_size(n, cols, area, spacing, label_h, &cw, &ch);
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;

	{
		int grid_h = rows * (ch + label_h) + (rows - 1) * spacing;
		int top = (area.h - grid_h) / 2;

		if (top < 0)
			top = 0;
		for (i = 0; i < n; i++) {
			int row = i / cols, col = i % cols;
			int in_row = row == rows - 1 ? n - row * cols : cols;
			int row_w = in_row * cw + (in_row - 1) * spacing;
			int left = (area.w - row_w) / 2;
			double k = fit_scale(src[i], cw, ch);
			int w = (int)(src[i].w * k), h = (int)(src[i].h * k);
			int cx = left + col * (cw + spacing);
			int cy = top + row * (ch + label_h + spacing);

			if (w < 1)
				w = 1;
			if (h < 1)
				h = 1;
			/* thumbnail and its title are centred in the cell as one block */
			out[i].x = cx + (cw - w) / 2;
			out[i].y = cy + (ch - h) / 2;
			out[i].w = w;
			out[i].h = h;
		}
	}
	return cols;
}
