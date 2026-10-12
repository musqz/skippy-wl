/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_GLOBALS_H
#define SKIPPY_GLOBALS_H

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct output {
	struct wl_output *wl;
	struct output *next;
};

struct globals {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_registry *registry;
	uint32_t mgr_name, mgr_version;
	uint32_t seat_name, seat_version; /* bound later by input_init() so no event is missed */
	struct zwlr_layer_shell_v1 *layer_shell;
	struct ext_foreign_toplevel_list_v1 *list;
	struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr; /* NULL: no activate/close */
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr;
	struct ext_image_copy_capture_manager_v1 *copy_mgr;
	struct output *outputs;
};

/* connect and bind; returns -1 if there is no display */
int globals_init(struct globals *g);

/* release what globals_init() allocated */
void globals_finish(struct globals *g);

#endif
