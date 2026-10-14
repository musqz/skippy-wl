/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libheif/heif.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "bgimage.h"

/* 8-bit RGB or RGBA rows to a premultiplied ARGB32 surface */
static cairo_surface_t *from_rgb(const uint8_t *src, int stride, int w, int h, int channels)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	uint8_t *dst;
	int dstride;

	if (cairo_surface_status(s)) {
		cairo_surface_destroy(s);
		return NULL;
	}
	dst = cairo_image_surface_get_data(s);
	dstride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		const uint8_t *p = src + (size_t)y * stride;
		uint32_t *q = (uint32_t *)(dst + (size_t)y * dstride);

		for (int x = 0; x < w; x++, p += channels) {
			uint32_t a = channels == 4 ? p[3] : 255;

			q[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | p[2] * a / 255;
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

static cairo_surface_t *load_heif(const char *path)
{
	struct heif_context *ctx = heif_context_alloc();
	struct heif_image_handle *hd = NULL;
	struct heif_image *img = NULL;
	cairo_surface_t *s = NULL;
	const uint8_t *data;
	int stride;

	if (heif_context_read_from_file(ctx, path, NULL).code != heif_error_Ok ||
	    heif_context_get_primary_image_handle(ctx, &hd).code != heif_error_Ok ||
	    heif_decode_image(hd, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL).code != heif_error_Ok)
		goto out;
	data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
	if (data)
		s = from_rgb(data, stride, heif_image_get_width(img, heif_channel_interleaved),
			heif_image_get_height(img, heif_channel_interleaved), 4);
out:
	heif_image_release(img);
	heif_image_handle_release(hd);
	heif_context_free(ctx);
	return s;
}

static cairo_surface_t *load_pixbuf(const char *path)
{
	GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL), *rot;
	cairo_surface_t *s;

	if (!pb)
		return NULL;
	rot = gdk_pixbuf_apply_embedded_orientation(pb);
	g_object_unref(pb);
	if (!rot)
		return NULL;
	s = from_rgb(gdk_pixbuf_read_pixels(rot), gdk_pixbuf_get_rowstride(rot), gdk_pixbuf_get_width(rot),
		gdk_pixbuf_get_height(rot), gdk_pixbuf_get_n_channels(rot));
	g_object_unref(rot);
	return s;
}

cairo_surface_t *bgimage_load(const char *path)
{
	const char *ext = strrchr(path, '.');
	cairo_surface_t *s;
	int heif = ext && (!strcasecmp(ext, ".avif") || !strcasecmp(ext, ".heic") || !strcasecmp(ext, ".heif"));

	/* a wrong or missing extension: try the other decoder */
	s = heif ? load_heif(path) : load_pixbuf(path);
	if (!s)
		s = heif ? load_pixbuf(path) : load_heif(path);
	return s;
}
