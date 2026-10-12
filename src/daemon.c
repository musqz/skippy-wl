/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "daemon.h"
#include "filter.h"
#include "ipc.h"
#include "overlay.h"
#include "wlutil.h"

static int listen_fd;
static int want_show, want_quit, want_switch;
static struct filter pending; /* filter of the next overlay */
static int pending_dir;

/* "verb\tkey=value\t...": fills f and *dir (dir=next|prev), returns the verb; NULL if invalid */
static char *split_command(char *cmd, struct filter *f, int *dir)
{
	char *save, *tok, *verb = strtok_r(cmd, "\t", &save);

	memset(f, 0, sizeof(*f));
	*dir = 0;
	while (verb && (tok = strtok_r(NULL, "\t", &save))) {
		char *eq = strchr(tok, '=');

		if (!eq)
			return NULL;
		*eq = 0;
		if (!strcmp(tok, "dir")) {
			if (strcmp(eq + 1, "next") && strcmp(eq + 1, "prev"))
				return NULL;
			*dir = eq[1] == 'n' ? 1 : -1;
			continue;
		}
		if (filter_set(f, tok, eq + 1) < 0)
			return NULL;
	}
	return verb ? verb : "";
}

static void on_command(void)
{
	char cmd[IPC_CMD_LEN];
	struct filter f;
	char *verb;
	int dir;
	int client = ipc_accept(listen_fd, cmd, sizeof(cmd));

	if (client < 0)
		return;
	verb = split_command(cmd, &f, &dir);
	if (!verb) {
		ipc_reply(client, "invalid filter\n");
		return;
	}
	if (!strcmp(verb, "toggle") || !strcmp(verb, "show")) {
		if (overlay_active() && dir) {
			/* the keybind fired again while the modifier is held: step the selection */
			overlay_cycle(dir);
		} else if (overlay_active()) {
			if (verb[0] == 't')
				overlay_cancel();
		} else {
			pending = f;
			pending_dir = dir;
			want_show = 1;
		}
	} else if (!strcmp(verb, "switch")) {
		if (!overlay_active()) {
			pending = f;
			want_switch = 1;
		}
	} else if (!strcmp(verb, "quit")) {
		want_quit = 1;
		overlay_cancel();
	} else {
		ipc_reply(client, "unknown command\n");
		return;
	}
	ipc_reply(client, "ok\n");
}

static int idle_done(void *arg)
{
	(void)arg;
	return want_show || want_quit || want_switch;
}

static void on_signal(int sig)
{
	(void)sig;
	display_interrupt();
}

int daemon_run(struct globals *g)
{
	struct sigaction sa;
	sigset_t block, old;
	int rc = 0;

	listen_fd = ipc_listen();
	if (listen_fd == -2) {
		fprintf(stderr, "skippy-wl: daemon already running\n");
		return 1;
	}
	if (listen_fd < 0) {
		fprintf(stderr, "skippy-wl: cannot create $XDG_RUNTIME_DIR/skippy-wl.sock\n");
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal; /* no SA_RESTART: poll() must wake up */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	/* blocked except inside ppoll(), so a signal cannot slip in before the wait */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigprocmask(SIG_BLOCK, &block, &old);
	display_set_wait_mask(&old);

	display_set_extra_fd(listen_fd, on_command);
	while (!want_quit) {
		if (display_wait(g->display, idle_done, NULL, -1) < 0) {
			if (!display_interrupted()) {
				fprintf(stderr, "skippy-wl: lost the wayland connection\n");
				rc = 1;
			}
			break;
		}
		if (want_quit)
			break;
		if (want_switch) {
			want_switch = 0;
			if (overlay_switch(g, &pending) < 0 && !display_interrupted())
				fprintf(stderr, "skippy-wl: switch failed\n");
			continue;
		}
		want_show = 0;
		if (overlay_run(g, &pending, pending_dir) < 0 && !display_interrupted())
			fprintf(stderr, "skippy-wl: overlay failed\n");
	}
	display_set_extra_fd(-1, NULL);
	display_set_wait_mask(NULL);
	ipc_close(listen_fd);
	return rc;
}
