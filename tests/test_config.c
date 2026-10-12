/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"

static int failures;

static double dabs(double v)
{
	return v < 0 ? -v : v;
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

int main(void)
{
	struct config c;

	config_defaults(&c);
	CHECK(c.margin == 48 && c.spacing == 28);
	CHECK(dabs(c.bg[3] - 1.0) < 1e-6);
	CHECK(c.monitors == MON_EACH);
	CHECK(config_set(&c, "monitors", "pointer") == 0);
	CHECK(c.monitors == MON_POINTER);
	CHECK(config_set(&c, "monitors", "all") < 0);
	CHECK(config_action(&c, XKB_KEY_Escape) == ACT_CANCEL);
	CHECK(config_action(&c, XKB_KEY_KP_Enter) == ACT_SELECT);
	CHECK(config_action(&c, XKB_KEY_x) == -1);

	CHECK(config_set(&c, "margin", "10") == 0 && c.margin == 10);
	CHECK(config_set(&c, "margin", "-1") < 0 && c.margin == 10);
	CHECK(config_set(&c, "margin", "12px") < 0 && c.margin == 10);
	CHECK(config_set(&c, "spacing", "0") == 0 && c.spacing == 0);
	CHECK(c.label == LABEL_TITLE);
	CHECK(config_set(&c, "label", "app_id") == 0 && c.label == LABEL_APP_ID);
	CHECK(config_set(&c, "label", "both") == 0 && c.label == LABEL_BOTH);
	CHECK(config_set(&c, "label", "class") < 0 && c.label == LABEL_BOTH);
	CHECK(config_action(&c, XKB_KEY_m) == ACT_MINIMIZE && config_action(&c, XKB_KEY_M) == ACT_MINIMIZE);
	CHECK(config_set(&c, "font", "Monospace 9") == 0);
	CHECK(config_set(&c, "font", "") < 0);
	CHECK(config_set(&c, "nonsense", "1") < 0);

	CHECK(config_set(&c, "background", "#102030") == 0);
	CHECK(dabs(c.bg[0] - 16 / 255.0) < 1e-6 && dabs(c.bg[3] - 1.0) < 1e-6);
	CHECK(config_set(&c, "highlight", "#ff000080") == 0);
	CHECK(dabs(c.highlight[0] - 1.0) < 1e-6 && dabs(c.highlight[3] - 128 / 255.0) < 1e-6);
	CHECK(config_set(&c, "background", "red") < 0);
	CHECK(config_set(&c, "background", "#12345") < 0);
	CHECK(config_set(&c, "background", "#12345g") < 0);

	/* a bind line replaces the defaults for that action */
	CHECK(config_set(&c, "bind.close", "x, q") == 0);
	CHECK(config_action(&c, XKB_KEY_x) == ACT_CLOSE && config_action(&c, XKB_KEY_q) == ACT_CLOSE);
	CHECK(config_action(&c, XKB_KEY_Delete) == -1);
	CHECK(config_set(&c, "bind.close", "NoSuchKeysym") < 0);
	/* a failed bind keeps the previous keys */
	CHECK(config_action(&c, XKB_KEY_x) == ACT_CLOSE);
	CHECK(config_set(&c, "bind.cancel", "Escpae") < 0 && config_action(&c, XKB_KEY_Escape) == ACT_CANCEL);
	CHECK(config_set(&c, "bind.close", "q, Bogus") < 0 && config_action(&c, XKB_KEY_x) == ACT_CLOSE);
	CHECK(config_set(&c, "bind.close", "a,b,c,d,e") < 0 && config_action(&c, XKB_KEY_a) == -1);
	CHECK(config_set(&c, "bind.nothing", "x") < 0);

	/* a key moves to its new action */
	config_defaults(&c);
	CHECK(config_set(&c, "bind.close", "Tab") == 0);
	CHECK(config_action(&c, XKB_KEY_Tab) == ACT_CLOSE);
	CHECK(config_set(&c, "bind.close", "Delete, Delete, Delete, Delete, Delete") < 0);
	CHECK(config_set(&c, "bind.cancel", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") < 0);

	/* config_load reads every line, skips bad ones and over-long ones */
	{
		char dir[] = "/tmp/skippy-wl-test-XXXXXX";
		char path[192], sub[160];
		FILE *f;
		int i;

		CHECK(mkdtemp(dir) != NULL);
		snprintf(sub, sizeof(sub), "%s/skippy-wl", dir);
		snprintf(path, sizeof(path), "%s/config", sub);
		CHECK(mkdir(sub, 0700) == 0);
		f = fopen(path, "w");
		CHECK(f != NULL);
		fprintf(f, "# comment\nmargin = 11\nspacing = 22\nbogus\nlabel_height = 33\n# ");
		for (i = 0; i < 400; i++)
			fputc('x', f);
		fprintf(f, "\nfont = Mono 9\nhighlight = #00ff00");
		fclose(f);
		setenv("XDG_CONFIG_HOME", dir, 1);
		config_load(&c);
		CHECK(c.margin == 11 && c.spacing == 22 && c.label_h == 33);
		CHECK(strcmp(c.font, "Mono 9") == 0);
		CHECK(dabs(c.highlight[1] - 1.0) < 1e-6);
		unlink(path);
		rmdir(sub);
		rmdir(dir);
	}

	/* theme_file: border colors come from a labwc theme, explicit settings win */
	{
		char dir[] = "/tmp/skippy-wl-theme-XXXXXX";
		char theme[160], cfgdir[160], cfg[192];
		FILE *f;

		CHECK(mkdtemp(dir) != NULL);
		snprintf(theme, sizeof(theme), "%s/theme", dir);
		snprintf(cfgdir, sizeof(cfgdir), "%s/skippy-wl", dir);
		snprintf(cfg, sizeof(cfg), "%s/config", cfgdir);
		CHECK(mkdir(cfgdir, 0700) == 0);
		f = fopen(theme, "w");
		CHECK(f != NULL);
		if (!f)
			return 1;
		fprintf(f, "# comment\nwindow.active.border.color: #112233\nwindow.inactive.border.color: #445566\nwindow.active.title.bg.color: #ffffff\n");
		fclose(f);
		setenv("XDG_CONFIG_HOME", dir, 1);

		f = fopen(cfg, "w");
		CHECK(f != NULL);
		if (!f)
			return 1;
		fprintf(f, "theme_file = %s\n", theme);
		fclose(f);
		config_load(&c);
		CHECK(dabs(c.highlight[0] - 0x11 / 255.0) < 1e-6 && dabs(c.highlight[2] - 0x33 / 255.0) < 1e-6);
		CHECK(dabs(c.border[0] - 0x44 / 255.0) < 1e-6 && c.border[3] < 1.0);

		f = fopen(cfg, "w");
		fprintf(f, "theme_file = %s\nhighlight = #ff0000\n", theme);
		fclose(f);
		config_load(&c);
		CHECK(dabs(c.highlight[0] - 1.0) < 1e-6 && dabs(c.highlight[1]) < 1e-6);
		CHECK(dabs(c.border[0] - 0x44 / 255.0) < 1e-6);

		/* an explicit value stays explicit even if a later line for it is invalid */
		f = fopen(cfg, "w");
		fprintf(f, "theme_file = %s\nhighlight = #00ff00\nhighlight = bogus\nborder = #0000ff\n", theme);
		fclose(f);
		config_load(&c);
		CHECK(dabs(c.highlight[1] - 1.0) < 1e-6 && dabs(c.highlight[0]) < 1e-6);
		CHECK(dabs(c.border[2] - 1.0) < 1e-6 && dabs(c.border[0]) < 1e-6);

		/* "~/" in theme_file expands to $HOME */
		setenv("HOME", dir, 1);
		f = fopen(cfg, "w");
		fprintf(f, "theme_file = ~/theme\n");
		fclose(f);
		config_load(&c);
		CHECK(dabs(c.highlight[0] - 0x11 / 255.0) < 1e-6);

		f = fopen(cfg, "w");
		fprintf(f, "theme_file = %s/missing\n", dir);
		fclose(f);
		config_load(&c); /* warns on stderr, keeps the defaults */
		CHECK(dabs(c.highlight[0] - 0.35) < 1e-6);

		unlink(theme);
		unlink(cfg);
		rmdir(cfgdir);
		rmdir(dir);
	}

	return failures ? 1 : 0;
}
