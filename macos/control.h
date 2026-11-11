#pragma once

/*
 * Remote control: `foot @ COMMAND` controls the running foot, as
 * `kitty @` does kitty. foot (the application) listens on a socket of
 * its own ($FOOT_CONTROL_SOCKET, or in its Application Support folder;
 * the user's processes only), unless macos-remote-control=no.
 *
 * A connection is one request and its reply, in the session protocol's
 * framing (session-protocol.h). The client (control-client.c) is part
 * of foot's binary, so that there's nothing else to install; the
 * commands themselves are the application's (control.m).
 */

#include <stdbool.h>

/* strv arguments (the command first), blob data (send-text's) */
#define CONTROL_MSG_REQUEST 1

/* i32 exit status, blob output (stdout's), str error (stderr's; may be empty) */
#define CONTROL_MSG_REPLY 2

/* Where foot listens; malloc:ed (NULL: nowhere) */
char *control_socket_path(void);

/* foot @ ...: 'argv' is what follows the @. Returns the exit status */
int control_client(int argc, char *const *argv);

/*
 * The application's side: listens, if 'enabled' (or $FOOT_CONTROL_SOCKET
 * is set: e.g. a test's foot)
 */
struct fdm;
struct wayland;
void control_init(struct fdm *fdm, struct wayland *wayl, bool enabled);
void control_fini(void);
