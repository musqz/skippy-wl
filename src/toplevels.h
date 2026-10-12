/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_TOPLEVELS_H
#define SKIPPY_TOPLEVELS_H

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"

struct toplevel {
	struct toplevel *next;
	struct ext_foreign_toplevel_handle_v1 *ext;
	char *identifier, *title, *app_id;
};

void toplevels_init(struct ext_foreign_toplevel_list_v1 *list);
struct toplevel *toplevels_first(void);
struct toplevel *toplevels_find(const char *identifier);

#endif
