#pragma once

/*
 * footd --bridge: run over SSH, by footd on another machine (see
 * session-protocol.h). Its stdin and stdout are the stream to that
 * footd; it passes the sessions on to the footd here, which it starts
 * if there's none (a daemon: it outlives the SSH connection, and its
 * sessions with it).
 *
 * The sessions created that way get this machine's environment (the
 * SSH login's), and its user's shell, configuration and home, not the
 * window's. TERM is the window's, unless there's no terminfo entry for
 * it here (then xterm-256color).
 *
 * 'socket_path': footd's (NULL: the default). Returns the exit code.
 */
int footd_bridge(const char *socket_path);

/*
 * What this footd --bridge can do: `footd --bridge-level` prints it.
 * footd on the other machine is replaced (installed anew) when its
 * level is lower, or it has none (see footd.c).
 */
#define FOOTD_BRIDGE_LEVEL 1

/* This footd's executable (NULL: unknown) */
char *footd_own_path(void);

/*
 * footd as ssh's SSH_ASKPASS (footd-askpass.c): asks footd (listening on
 * 'socket_path') for what ssh's 'prompt' asks, for the ssh footd gave
 * 'token' (hex). Prints the answer. Returns the exit code.
 */
int footd_askpass(const char *socket_path, const char *token, const char *prompt);
