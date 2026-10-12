/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_INPUT_H
#define SKIPPY_INPUT_H

#include "globals.h"

void input_init(struct globals *g);
void input_finish(void);
struct wl_seat *input_seat(void);
int input_has_pointer(void);

#endif
