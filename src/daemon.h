/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_DAEMON_H
#define SKIPPY_DAEMON_H

#include "globals.h"

/* keep tracking windows and show the overlay on "show" / "toggle" from the control socket */
int daemon_run(struct globals *g);

#endif
