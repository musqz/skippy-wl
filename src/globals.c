/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "globals.h"
#include "toplevels.h"

static void r_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	struct globals *g = d;

	if (!strcmp(iface, wl_shm_interface.name))
		g->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_compositor_interface.name))
		g->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name) && v >= 5) {
		g->seat_name = name;
		g->seat_version = 5;
	}
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		g->layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name))
		g->list = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
	else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		g->mgr_name = name;
		g->mgr_version = v < 3 ? v : 3;
	}
	else if (!strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
		g->source_mgr = wl_registry_bind(r, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name))
		g->copy_mgr = wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name)) {
		struct output *o = calloc(1, sizeof(*o));

		if (!o)
			return;
		o->name = name;
		o->wl = wl_registry_bind(r, name, &wl_output_interface, 1);
		o->next = g->outputs;
		g->outputs = o;
	}
}

static void r_remove(void *d, struct wl_registry *r, uint32_t name)
{
	struct globals *g = d;
	struct output **p;

	(void)r;
	for (p = &g->outputs; *p; p = &(*p)->next) {
		struct output *o = *p;

		if (o->name != name)
			continue;
		*p = o->next;
		toplevels_output_removed(o->wl);
		wl_output_destroy(o->wl);
		free(o);
		return;
	}
}

static const struct wl_registry_listener reg_l = { r_global, r_remove };

int globals_init(struct globals *g)
{
	memset(g, 0, sizeof(*g));
	g->display = wl_display_connect(NULL);
	if (!g->display)
		return -1;
	g->registry = wl_display_get_registry(g->display);
	wl_registry_add_listener(g->registry, &reg_l, g);
	wl_display_roundtrip(g->display);
	/* after the outputs are bound, or the initial output_enter events are not sent */
	if (g->mgr_name)
		g->toplevel_mgr = wl_registry_bind(g->registry, g->mgr_name,
			&zwlr_foreign_toplevel_manager_v1_interface, g->mgr_version);
	return 0;
}

void globals_finish(struct globals *g)
{
	while (g->outputs) {
		struct output *o = g->outputs;

		g->outputs = o->next;
		wl_output_destroy(o->wl);
		free(o);
	}
	wl_display_disconnect(g->display);
}
