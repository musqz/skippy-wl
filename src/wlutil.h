/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_WLUTIL_H
#define SKIPPY_WLUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

double now_ms(void);

/* w*h*4 shm buffer; *data is the mapping (unmap with munmap(*data, *size)). NULL on failure. */
struct wl_buffer *shm_buffer_create(struct wl_shm *shm, int w, int h, uint32_t format,
	void **data, size_t *size);

/* dispatch until done(arg) is true; -1 on timeout or display error (timeout_ms < 0: no timeout) */
int display_wait(struct wl_display *dpy, int (*done)(void *), void *arg, int timeout_ms);

#endif
