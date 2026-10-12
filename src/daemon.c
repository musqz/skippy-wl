/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "daemon.h"
#include "ipc.h"
#include "overlay.h"
#include "wlutil.h"

static int listen_fd;
static int want_show, want_quit;

static void on_command(void)
{
	char cmd[64];
	int client = ipc_accept(listen_fd, cmd, sizeof(cmd));

	if (client < 0)
		return;
	if (!strcmp(cmd, "toggle")) {
		if (overlay_active())
			overlay_cancel();
		else
			want_show = 1;
	} else if (!strcmp(cmd, "show")) {
		if (!overlay_active())
			want_show = 1;
	} else if (!strcmp(cmd, "quit")) {
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
	return want_show || want_quit;
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
		want_show = 0;
		if (overlay_run(g) < 0 && !display_interrupted())
			fprintf(stderr, "skippy-wl: overlay failed\n");
	}
	display_set_extra_fd(-1, NULL);
	display_set_wait_mask(NULL);
	ipc_close(listen_fd);
	return rc;
}
