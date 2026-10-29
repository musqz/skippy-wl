/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_CONFIG_H
#define SKIPPY_CONFIG_H

#include <stdint.h>

#include "filter.h"

enum action {
	ACT_SELECT, ACT_CANCEL, ACT_CLOSE, ACT_MINIMIZE,
	ACT_LEFT, ACT_RIGHT, ACT_UP, ACT_DOWN, ACT_HOME, ACT_END,
	ACT_NEXT, ACT_PREV,
	ACT_COUNT
};

#define MAX_BINDS 4

enum label_mode { LABEL_TITLE, LABEL_APP_ID, LABEL_BOTH };
enum { ANCHOR_H_LEFT, ANCHOR_H_CENTER, ANCHOR_H_RIGHT };
enum { ANCHOR_V_TOP, ANCHOR_V_MIDDLE, ANCHOR_V_BOTTOM, ANCHOR_V_BELOW };

struct label_cfg {
	int show;
	enum label_mode option;
	int anchor_h, anchor_v; /* ANCHOR_V_BELOW sits under the tile and reserves height; the rest sit inside it */
	int offset_x, offset_y; /* from the anchor; may leave the tile */
	int height; /* box height, and the room reserved under the tile */
	double width; /* box width as a fraction of the tile */
	int radius; /* corner radius of the box in pixels */
	int opacity; /* 0-255, scales border and background */
	double text[4], outline[4], border[4], bg[4], bg_hl[4]; /* r, g, b, a in 0..1 */
	char font[96]; /* Pango description */
};
enum monitor_mode { MON_EACH, MON_POINTER };

struct config {
	int margin, spacing;
	struct label_cfg lbl;
	enum monitor_mode monitors;
	int icon, icon_size; /* app icon on each tile */
	int icon_h, icon_v; /* ANCHOR_*: where the icon sits on the tile; never ANCHOR_V_BELOW */
	char icon_theme[64];
	int anim, anim_ms; /* fade and grow when the overlay opens */
	int pivot; /* release of the modifiers held at open picks the selection */
	int switch_wait; /* ms before a --next/--prev overlay is drawn; 0 = at once */
	int switch_hold; /* ms of pause after which releasing the modifiers leaves the overlay open; 0 = always pick */
	double bg[4], highlight[4], border[4]; /* r, g, b, a in 0..1 */
	int bg_set, highlight_set, border_set; /* set explicitly: a theme file must not override them */
	int panels_show; /* leave room for panels and docks instead of covering them */
	char bg_image[256]; /* PNG, JPEG, AVIF, HEIC or HEIF shown behind the tiles; empty = none */
	char bg_image_cmd[256]; /* shell command that prints the image path, run at each open; wins over bg_image */
	char theme_file[256]; /* labwc themerc style file the border colors are read from */
	struct filter hide_app, hide_title, hide_status; /* windows matching any are never shown; an empty field hides nothing */
	uint32_t keys[ACT_COUNT][MAX_BINDS]; /* xkb keysyms, 0 = unused */
};

void config_defaults(struct config *c);
/* defaults, then ~/.config/skippy-wl/skippy-wl.rc; problems go to stderr and are skipped */
void config_load(struct config *c);
/* true if a [hide] rule matches the window */
int config_hidden(const struct config *c, const char *app_id, const char *title, unsigned status);
/* one setting as "section.key" (for example "layout.margin"); -1 if the key or value is invalid */
int config_set(struct config *c, const char *key, const char *value);
/* action bound to keysym, or -1 */
int config_action(const struct config *c, uint32_t keysym);

#endif
