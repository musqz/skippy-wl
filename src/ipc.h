/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SKIPPY_IPC_H
#define SKIPPY_IPC_H

#include <stddef.h>

/* daemon: listening socket fd, -1 on error, -2 if another daemon already owns the socket */
int ipc_listen(void);
/* daemon: accept one client and read its command line into buf; returns the client fd or -1 */
int ipc_accept(int listen_fd, char *buf, size_t len);
/* daemon: answer and close the client */
void ipc_reply(int client_fd, const char *reply);
void ipc_close(int listen_fd);

/* client: 0 on "ok"; -1 if no daemon is listening; -2 on any other reply (printed to stderr) */
int ipc_send(const char *cmd);

#endif
