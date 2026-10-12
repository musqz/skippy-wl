/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "capture.h"
#include "wlutil.h"

#define DRM_ARGB8888 0x34325241 /* 'AR24' */
#define DRM_XRGB8888 0x34325258 /* 'XR24' */

static struct wl_shm *shm;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr;
static struct ext_image_copy_capture_manager_v1 *copy_mgr;

void capture_init(struct wl_shm *s,
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *sm,
	struct ext_image_copy_capture_manager_v1 *cm)
{
	shm = s;
	source_mgr = sm;
	copy_mgr = cm;
}

static void fail(struct capture *c, uint32_t reason)
{
	c->state = CAPTURE_FAILED;
	c->fail_reason = reason;
}

static int alloc_buffer(struct capture *c)
{
	c->stride = c->width * 4;
	c->buffer = shm_buffer_create(shm, (int)c->width, (int)c->height, c->format,
		&c->data, &c->size);
	return c->buffer ? 0 : -1;
}

static void f_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t)
{
	(void)d; (void)f; (void)t;
}

static void f_damage(void *d, struct ext_image_copy_capture_frame_v1 *f,
		int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}

static void f_time(void *d, struct ext_image_copy_capture_frame_v1 *f,
		uint32_t hi, uint32_t lo, uint32_t ns)
{
	(void)d; (void)f; (void)hi; (void)lo; (void)ns;
}

static void f_ready(void *d, struct ext_image_copy_capture_frame_v1 *f)
{
	struct capture *c = d;

	(void)f;
	if (c->swap) {
		uint32_t *p = c->data;

		for (size_t i = 0; i < c->size / 4; i++)
			p[i] = (p[i] & 0xff00ff00) | ((p[i] & 0xff) << 16) | ((p[i] >> 16) & 0xff);
	}
	c->state = CAPTURE_READY;
	ext_image_copy_capture_frame_v1_destroy(c->frame);
	c->frame = NULL;
}

static void f_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason)
{
	struct capture *c = d;

	fail(c, reason);
	ext_image_copy_capture_frame_v1_destroy(f);
	c->frame = NULL;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_l = {
	f_transform, f_damage, f_time, f_ready, f_failed };

static void s_buffer_size(void *d, struct ext_image_copy_capture_session_v1 *s,
		uint32_t w, uint32_t h)
{
	struct capture *c = d;

	(void)s;
	if (c->requested)
		return;
	c->width = w;
	c->height = h;
}

static void s_shm_format(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t fmt)
{
	struct capture *c = d;
	int rank, swap = 0;
	uint32_t want;

	(void)s;
	if (c->requested)
		return;
	/* wlroots sends DRM fourccs here; other compositors send wl_shm enum values */
	if (fmt == DRM_ARGB8888 || fmt == WL_SHM_FORMAT_ARGB8888) {
		want = WL_SHM_FORMAT_ARGB8888;
		rank = 4;
	} else if (fmt == DRM_XRGB8888 || fmt == WL_SHM_FORMAT_XRGB8888) {
		want = WL_SHM_FORMAT_XRGB8888;
		rank = 3;
	} else if (fmt == WL_SHM_FORMAT_ABGR8888 || fmt == WL_SHM_FORMAT_XBGR8888) {
		want = fmt;
		swap = 1;
		rank = fmt == WL_SHM_FORMAT_ABGR8888 ? 2 : 1;
	} else {
		return;
	}
	if (rank > c->rank) {
		c->format = want;
		c->swap = swap;
		c->rank = rank;
	}
}

static void s_dmabuf_device(void *d, struct ext_image_copy_capture_session_v1 *s,
		struct wl_array *dev)
{
	(void)d; (void)s; (void)dev;
}

static void s_dmabuf_format(void *d, struct ext_image_copy_capture_session_v1 *s,
		uint32_t fmt, struct wl_array *mods)
{
	(void)d; (void)s; (void)fmt; (void)mods;
}

static void s_done(void *d, struct ext_image_copy_capture_session_v1 *s)
{
	struct capture *c = d;

	if (c->requested)
		return;
	if (!c->rank || !c->width || !c->height) {
		fail(c, CAPTURE_ERR_FORMAT);
		return;
	}
	if (alloc_buffer(c) < 0) {
		fail(c, CAPTURE_ERR_ALLOC);
		return;
	}
	c->requested = 1;
	c->frame = ext_image_copy_capture_session_v1_create_frame(s);
	ext_image_copy_capture_frame_v1_add_listener(c->frame, &frame_l, c);
	ext_image_copy_capture_frame_v1_attach_buffer(c->frame, c->buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(c->frame, 0, 0,
		(int32_t)c->width, (int32_t)c->height);
	ext_image_copy_capture_frame_v1_capture(c->frame);
}

static void s_stopped(void *d, struct ext_image_copy_capture_session_v1 *s)
{
	struct capture *c = d;

	(void)s;
	if (c->state == CAPTURE_PENDING)
		fail(c, CAPTURE_ERR_STOPPED);
}

static const struct ext_image_copy_capture_session_v1_listener session_l = {
	s_buffer_size, s_shm_format, s_dmabuf_device, s_dmabuf_format, s_done, s_stopped };

struct capture *capture_start(struct ext_foreign_toplevel_handle_v1 *toplevel)
{
	struct ext_image_capture_source_v1 *src;
	struct capture *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	src = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
		source_mgr, toplevel);
	c->session = ext_image_copy_capture_manager_v1_create_session(copy_mgr, src, 0);
	ext_image_capture_source_v1_destroy(src);
	ext_image_copy_capture_session_v1_add_listener(c->session, &session_l, c);
	return c;
}

cairo_surface_t *capture_surface(struct capture *c)
{
	if (c->state != CAPTURE_READY)
		return NULL;
	return cairo_image_surface_create_for_data(c->data,
		c->format == WL_SHM_FORMAT_ARGB8888 || c->format == WL_SHM_FORMAT_ABGR8888
			? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
		(int)c->width, (int)c->height, (int)c->stride);
}

void capture_destroy(struct capture *c)
{
	if (c->frame)
		ext_image_copy_capture_frame_v1_destroy(c->frame);
	if (c->session)
		ext_image_copy_capture_session_v1_destroy(c->session);
	if (c->buffer)
		wl_buffer_destroy(c->buffer);
	if (c->data)
		munmap(c->data, c->size);
	free(c);
}
