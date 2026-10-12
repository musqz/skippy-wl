/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc.h"

static int sock_path(struct sockaddr_un *addr)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");

	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	if (!dir || snprintf(addr->sun_path, sizeof(addr->sun_path), "%s/skippy-wl.sock", dir)
			>= (int)sizeof(addr->sun_path))
		return -1;
	return 0;
}

static int connect_daemon(void)
{
	struct sockaddr_un addr;
	int fd;

	if (sock_path(&addr) < 0)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int ipc_listen(void)
{
	struct sockaddr_un addr;
	int fd, probe;

	if (sock_path(&addr) < 0)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno != EADDRINUSE) {
			close(fd);
			return -1;
		}
		probe = connect_daemon();
		if (probe >= 0) {
			close(probe);
			close(fd);
			return -2;
		}
		unlink(addr.sun_path); /* stale socket of a dead daemon */
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd);
			return -1;
		}
	}
	if (listen(fd, 4) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int ipc_accept(int listen_fd, char *buf, size_t len)
{
	struct timeval tv = { 0, 200 * 1000 };
	ssize_t n;
	int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);

	if (fd < 0)
		return -1;
	/* a client that connects and says nothing must not stall the daemon */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	n = read(fd, buf, len - 1);
	if (n <= 0) {
		close(fd);
		return -1;
	}
	buf[n] = 0;
	buf[strcspn(buf, "\r\n")] = 0;
	return fd;
}

void ipc_reply(int client_fd, const char *reply)
{
	ssize_t w = write(client_fd, reply, strlen(reply));

	(void)w;
	close(client_fd);
}

void ipc_close(int listen_fd)
{
	struct sockaddr_un addr;

	close(listen_fd);
	if (sock_path(&addr) == 0)
		unlink(addr.sun_path);
}

int ipc_send(const char *cmd)
{
	struct timeval tv = { 2, 0 };
	char reply[64];
	ssize_t n;
	int fd;

	signal(SIGPIPE, SIG_IGN);
	fd = connect_daemon();
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (dprintf(fd, "%s\n", cmd) < 0) {
		close(fd);
		fprintf(stderr, "skippy-wl: cannot talk to the daemon\n");
		return -2;
	}
	n = read(fd, reply, sizeof(reply) - 1);
	close(fd);
	if (n <= 0) {
		fprintf(stderr, "skippy-wl: no reply from the daemon\n");
		return -2;
	}
	reply[n] = 0;
	reply[strcspn(reply, "\r\n")] = 0;
	if (strcmp(reply, "ok")) {
		fprintf(stderr, "skippy-wl: %s\n", reply);
		return -2;
	}
	return 0;
}
