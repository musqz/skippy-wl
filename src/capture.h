/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_CAPTURE_H
#define SKIPPY_CAPTURE_H

#include <cairo.h>
#include <stdint.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

enum capture_state { CAPTURE_PENDING, CAPTURE_READY, CAPTURE_FAILED };

/* fail_reason: 0..2 come from the compositor, these are ours */
enum { CAPTURE_ERR_FORMAT = 100, CAPTURE_ERR_ALLOC, CAPTURE_ERR_STOPPED };

struct capture {
	enum capture_state state;
	uint32_t fail_reason;
	uint32_t width, height, stride;
	uint32_t format; /* wl_shm format */
	int rank;       /* preference of the chosen shm format, 0 = none yet */
	int swap;       /* BGR order, fixed up when ready */
	int requested;  /* buffer allocated; later constraint events are ignored */

	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
};

void capture_init(struct wl_shm *shm,
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr,
	struct ext_image_copy_capture_manager_v1 *copy_mgr);
struct capture *capture_start(struct ext_foreign_toplevel_handle_v1 *toplevel);
/* surface is valid until capture_destroy() */
cairo_surface_t *capture_surface(struct capture *c);
void capture_destroy(struct capture *c);

#endif
