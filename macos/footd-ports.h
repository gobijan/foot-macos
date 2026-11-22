#pragma once

/*
 * Another machine's TCP ports, as ports of this one (FORWARD; see
 * session-protocol.h): a web server started in a session there is
 * opened in the browser here, a database there is reached by the
 * programs here. footd listens here, for this machine only, and each
 * connection is a channel of its connection to that machine
 * (session-mux.h), where footd --bridge connects to the port.
 *
 * These are the sockets of both sides, and which ports there are.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fdm.h"

/*
 * Listens on 'port' of this machine, for this machine only: 127.0.0.1
 * ('fds[0]'), and ::1 if there is IPv6 ('fds[1]', else -1). Not if a
 * program listens on it already, on either, or on all addresses: it
 * would no longer be reached as localhost. 0: a free port. Returns the
 * port, or 0 if it's taken (or it failed: logged).
 */
uint16_t ports_listen(uint16_t port, int fds[static 2]);

/* A connection to one of those (non-blocking), or -1: none (any more) */
int ports_accept(int listen_fd);

/*
 * Connects to 'port' of 'host': "localhost" (127.0.0.1, or else ::1:
 * some listen on that one only), or an address (names aren't looked up:
 * that would block). 'done' is told once, maybe before this returns:
 * the connection (its own now; non-blocking), or -1 and why.
 */
typedef void (*ports_connected_t)(int fd, const char *error, void *data);
void ports_connect(struct fdm *fdm, const char *host, uint16_t port,
                   ports_connected_t done, void *data);

/* Those not told yet aren't any more: with 'data', or all of them (NULL) */
void ports_connect_cancel(const void *data);

/* A TCP port a program listens on */
struct listening_port {
    uint16_t port;
    char *program;  /* Its name; NULL: unknown (another user's) */
    bool own;       /* The user's own program */
};

/*
 * The ports programs listen on here: the user's own programs' first,
 * each by port. Found when asked for (nothing is watched). malloc:ed
 * (ports_listening_free()); NULL: none, or unknown.
 */
struct listening_port *ports_listening(size_t *count);
void ports_listening_free(struct listening_port *ports, size_t count);
