/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_OVERLAY_H
#define SKIPPY_OVERLAY_H

#include <stdint.h>

#include "globals.h"

/*
 * Capture all toplevels, show the overlay on every output and block until the
 * user picks a window or cancels. *selected is the picked window's identifier
 * (malloc'd) or NULL when cancelled. Returns -1 on setup failure.
 */
int overlay_run(struct globals *g, char **selected);

/* input events, called from input.c */
void overlay_key(uint32_t keysym);
void overlay_pointer_motion(struct wl_surface *surface, double x, double y);
void overlay_pointer_leave(void);
void overlay_pointer_button(uint32_t button);

#endif
