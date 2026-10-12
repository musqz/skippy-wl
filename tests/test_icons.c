/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "icons.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

int main(void)
{
	char root[] = "/tmp/skippy-icons-XXXXXX", path[512], name[64];
	cairo_surface_t *png, *ic;

	if (!mkdtemp(root))
		return 1;
	snprintf(path, sizeof(path), "%s/applications", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/hicolor", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/hicolor/48x48", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/hicolor/48x48/apps", root);
	mkdir(path, 0700);

	snprintf(path, sizeof(path), "%s/applications/Demo.desktop", root);
	write_file(path, "[Other]\nIcon=wrong\n[Desktop Entry]\nName=Demo\nIcon=demo-icon\n");
	CHECK(desktop_icon_name(path, name, sizeof(name)) == 0 && !strcmp(name, "demo-icon"), "got '%s'", name);
	snprintf(path, sizeof(path), "%s/applications/none.desktop", root);
	write_file(path, "[Desktop Entry]\nName=None\n");
	CHECK(desktop_icon_name(path, name, sizeof(name)) < 0, "no Icon key");
	CHECK(desktop_icon_name("/nonexistent.desktop", name, sizeof(name)) < 0, "missing file");

	png = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 24);
	snprintf(path, sizeof(path), "%s/icons/hicolor/48x48/apps/demo-icon.png", root);
	cairo_surface_write_to_png(png, path);
	cairo_surface_destroy(png);

	setenv("HOME", root, 1);
	setenv("XDG_DATA_HOME", root, 1);
	setenv("XDG_DATA_DIRS", "/nonexistent", 1);
	ic = icon_get("Demo", "hicolor", 200);
	CHECK(ic && cairo_image_surface_get_width(ic) == 200 && cairo_image_surface_get_height(ic) == 200,
		"icon found and scaled to the requested size");
	CHECK(icon_get("Demo", "hicolor", 200) == ic, "second lookup is served from the cache");
	CHECK(icon_get("nothing-here-xyz", "hicolor", 48) == NULL, "unknown app");
	CHECK(icon_get("", "hicolor", 48) == NULL, "empty app id");

	/* theme "child" inherits "base"; the icon is an SVG in a "48/apps" layout, named with an extension in Icon= */
	snprintf(path, sizeof(path), "%s/icons/child", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/child/index.theme", root);
	write_file(path, "[Icon Theme]\nInherits=missing, base\n");
	snprintf(path, sizeof(path), "%s/icons/base", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/base/48", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/base/48/apps", root);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/icons/base/48/apps/vec.svg", root);
	write_file(path, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"48\" height=\"48\">"
		"<rect width=\"48\" height=\"48\" fill=\"red\"/></svg>");
	snprintf(path, sizeof(path), "%s/applications/vec-app.desktop", root);
	write_file(path, "[Desktop Entry]\nIcon=vec.svg\n");
	ic = icon_get("vec-app", "child", 64);
	CHECK(ic != NULL, "svg through Inherits and 48/apps, extension stripped");
	CHECK(icon_get("vec-app", "hicolor", 64) == NULL, "another theme is a separate cache entry");

	/* a client-chosen app id must not name a file */
	snprintf(path, sizeof(path), "%s/icons/base/48/apps/vec.svg", root);
	CHECK(icon_get(path, "child", 64) == NULL, "absolute app id is refused");
	CHECK(icon_get("../x", "child", 64) == NULL, "app id with a slash is refused");
	icons_clear();

	snprintf(path, sizeof(path), "rm -rf %s", root);
	if (system(path))
		failures++;
	return failures ? 1 : 0;
}
