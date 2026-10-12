/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_TOPLEVELS_H
#define SKIPPY_TOPLEVELS_H

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct wtop;

/* One window: the ext handle (thumbnails) paired with its wlr handle (actions). */
struct toplevel {
	struct toplevel *next;
	struct ext_foreign_toplevel_handle_v1 *ext;
	char *identifier, *title, *app_id;
	struct wtop *w; /* NULL until paired, or when pairing failed */
	int ext_done, no_pair;
};

/* mgr may be NULL: windows then stay unpaired and cannot be acted on */
void toplevels_init(struct ext_foreign_toplevel_list_v1 *list,
	struct zwlr_foreign_toplevel_manager_v1 *mgr);
struct toplevel *toplevels_first(void);
struct toplevel *toplevels_find(const char *identifier);

/* called just before a toplevel is freed */
void toplevels_set_closed_cb(void (*cb)(struct toplevel *));

/* 0 on success, -1 when the window has no trusted wlr pair. Activate also unminimizes and switches workspace on labwc. */
int toplevel_activate(struct toplevel *t, struct wl_seat *seat);
int toplevel_close(struct toplevel *t);
/* called after a window's state (minimized, maximized, ...) changed */
void toplevels_set_changed_cb(void (*cb)(void));
/* 0 on success, -1 when the window has no trusted wlr pair */
int toplevel_set_minimized(struct toplevel *t, int minimized);
/* STATUS_* bits; STATUS_UNKNOWN for an unpaired window or when pairing is not trusted */
unsigned toplevel_status(const struct toplevel *t);
/* forget an output that went away */
void toplevels_output_removed(const struct wl_output *out);
/* 1 if the window is on the output, or if its outputs are unknown */
int toplevel_on_output(const struct toplevel *t, const struct wl_output *out);

#endif
