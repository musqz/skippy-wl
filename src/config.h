/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_CONFIG_H
#define SKIPPY_CONFIG_H

#include <stdint.h>

enum action {
	ACT_SELECT, ACT_CANCEL, ACT_CLOSE,
	ACT_LEFT, ACT_RIGHT, ACT_UP, ACT_DOWN, ACT_HOME, ACT_END,
	ACT_NEXT, ACT_PREV,
	ACT_COUNT
};

#define MAX_BINDS 4

struct config {
	int margin, spacing, label_h;
	char font[96];
	double bg[4], highlight[4]; /* r, g, b, a in 0..1 */
	uint32_t keys[ACT_COUNT][MAX_BINDS]; /* xkb keysyms, 0 = unused */
};

void config_defaults(struct config *c);
/* defaults, then ~/.config/skippy-wl/config; problems go to stderr and are skipped */
void config_load(struct config *c);
/* one "key = value" setting; -1 if the key or value is invalid */
int config_set(struct config *c, const char *key, const char *value);
/* action bound to keysym, or -1 */
int config_action(const struct config *c, uint32_t keysym);

#endif
