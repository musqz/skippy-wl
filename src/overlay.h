/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_OVERLAY_H
#define SKIPPY_OVERLAY_H

#include <stdint.h>

#include "filter.h"
#include "globals.h"

/*
 * Capture the toplevels that match filter, show the overlay on every output and block until the
 * user picks a window (it is activated) or cancels. dir 1/-1 starts on the previous/last window
 * of the MRU order (switch mode), 0 on the first. Returns -1 on setup failure.
 *
 * multi: clicks and Space toggle windows, Enter confirms, and the identifiers of the toggled
 * windows are printed to stdout, one per line, and nothing is activated or closed.
 * Returns 1 when cancelled or when nothing was toggled.
 */
int overlay_run(struct globals *g, const struct filter *filter, int dir, int multi);

/* --next/--prev (dir 1/-1) again while the overlay is up: move the selection one step */
void overlay_cycle(int dir);

/*
 * Focus the previous window (second in the usage order, or the first if no window has focus)
 * among those matching filter. Draws nothing. Returns -1 on setup failure.
 */
int overlay_switch(struct globals *g, const struct filter *filter);

/* close the overlay without picking; no-op when it is not shown */
void overlay_cancel(void);
int overlay_active(void);

/* input events, called from input.c */
/* text: the key's UTF-8 as typed, "" when it has none or Ctrl/Alt/Super is held */
void overlay_key(uint32_t keysym, const char *text);
void overlay_modifiers(uint32_t depressed);
void overlay_pointer_motion(struct wl_surface *surface, double x, double y);
void overlay_pointer_leave(void);
void overlay_pointer_button(uint32_t button);

#endif
