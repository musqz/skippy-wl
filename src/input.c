/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "input.h"
#include "overlay.h"

static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_state;


static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)serial;
	overlay_pointer_motion(s, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)p; (void)serial; (void)s;
	overlay_pointer_leave();
}

static void p_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)time;
	overlay_pointer_motion(NULL, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)serial; (void)time;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		overlay_pointer_button(button);
}

static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }

int input_has_pointer(void)
{
	return pointer != NULL;
}

static const struct wl_pointer_listener pointer_l = {
	.enter = p_enter,
	.leave = p_leave,
	.motion = p_motion,
	.button = p_button,
		.axis = p_axis,
		.frame = p_frame,
		.axis_source = p_axis_source,
		.axis_stop = p_axis_stop,
		.axis_discrete = p_axis_discrete,
};

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
	char *map;

	(void)d; (void)k;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	keymap = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	xkb_state = keymap ? xkb_state_new(keymap) : NULL;
}

static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state)
{
	char text[8] = "";

	(void)d; (void)k; (void)serial; (void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_state)
		return;
	if (!xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED) &&
			!xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_DEPRESSED) &&
			!xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_DEPRESSED))
		xkb_state_key_get_utf8(xkb_state, key + 8, text, sizeof(text));
	overlay_key(xkb_state_key_get_one_sym(xkb_state, key + 8), text);
}

static void k_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
		uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)d; (void)k; (void)serial;
	if (xkb_state)
		xkb_state_update_mask(xkb_state, depressed, latched, locked, 0, 0, group);
	overlay_modifiers(depressed);
}

static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		struct wl_array *keys)
{ (void)d; (void)k; (void)serial; (void)s; (void)keys; }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)k; (void)serial; (void)s; }
static void k_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)d; (void)k; (void)rate; (void)delay; }
static void s_name(void *d, struct wl_seat *s, const char *name) { (void)d; (void)s; (void)name; }

static const struct wl_keyboard_listener keyboard_l = {
	.keymap = k_keymap,
		.enter = k_enter,
		.leave = k_leave,
	.key = k_key,
	.modifiers = k_modifiers,
		.repeat_info = k_repeat_info,
};

static void s_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_l, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_release(pointer);
		pointer = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_l, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
		wl_keyboard_release(keyboard);
		keyboard = NULL;
	}
}

static const struct wl_seat_listener seat_l = {
	.capabilities = s_caps,
		.name = s_name,
};

void input_init(struct globals *g)
{
	seat = wl_registry_bind(g->registry, g->seat_name, &wl_seat_interface, g->seat_version);
	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	wl_seat_add_listener(seat, &seat_l, NULL);
	/* capabilities -> pointer/keyboard objects -> keymap */
	wl_display_roundtrip(g->display);
	wl_display_roundtrip(g->display);
}

struct wl_seat *input_seat(void)
{
	return seat;
}

void input_finish(void)
{
	if (pointer)
		wl_pointer_release(pointer);
	if (keyboard)
		wl_keyboard_release(keyboard);
	pointer = NULL;
	keyboard = NULL;
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	xkb_context_unref(xkb_ctx);
	xkb_state = NULL;
	keymap = NULL;
	xkb_ctx = NULL;
	if (seat)
		wl_seat_release(seat);
	seat = NULL;
}
