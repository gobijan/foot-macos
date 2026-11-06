#pragma once

/*
 * Persistent sessions: the windows' side (macos/session.c).
 *
 * A window's terminal is a session of footd's (macos/footd.c): the PTY
 * and the programs running in it are footd's, and keep running when
 * foot quits (or crashes). The terminal gets a socket instead of a PTY,
 * through term_init()'s --pty path: it's opened as /dev/fd/N (a dup()
 * of it, on macOS), and nothing is spawned.
 *
 * The session is created (or attached to, see sessions_restore()) once
 * the terminal knows its size: the first TIOCSWINSZ (render.c).
 * Closing the window ends the session; quitting foot only detaches it
 * (sessions_detach_all()).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct config;
struct fdm;
struct session_link;
struct terminal;

/*
 * Whether this foot's terminals are sessions: when it's the
 * application (or $FOOT_SESSIONS_SOCKET is set), unless it's a server
 * (footclient's terminals are footclient's), and not with
 * macos-sessions=no.
 */
void sessions_enable(bool enable);

/*
 * term_init(): with a PTY of footd's, sets '*pty_path' (to open) and
 * returns the link to the session. NULL: the terminal has a PTY of its
 * own (not a session, or footd failed).
 */
struct session_link *session_link_new(
    struct fdm *fdm, const struct config *conf, const char *cwd,
    int argc, char *const *argv, const char *const *envp,
    const char **pty_path);

/* term_init(): the terminal has opened the PTY (or failed to) */
void session_link_set_term(struct session_link *link, struct terminal *term);

/* render.c's TIOCSWINSZ */
void session_link_resize(struct session_link *link, int cols, int rows,
                         int cell_width, int cell_height);

/*
 * term_destroy(): the window is gone. Its session ends, unless foot is
 * quitting, or another window has taken over.
 */
void session_link_destroy(struct session_link *link);

/*
 * What runs in the terminal: footd's session's process (its shell),
 * and its terminal's foreground process group. 0 while unknown.
 */
pid_t session_link_pid(const struct session_link *link);
pid_t session_link_foreground_pgrp(const struct session_link *link);

/* The session's ID (0 while it's not created) */
uint64_t session_link_id(const struct session_link *link);

/* Whether it runs a command of its own (not just the shell) */
bool session_link_has_command(const struct session_link *link);

/*
 * What runs in a terminal, a session's or not: its first process (the
 * shell), and its foreground process group (0 if unknown)
 */
pid_t terminal_pid(const struct terminal *term);
pid_t terminal_foreground_pgrp(const struct terminal *term);

/* foot is quitting (or exiting): its windows' sessions keep running */
void sessions_detach_all(void);

/* foot exits: the connection to footd is closed (once the terminals are gone) */
void sessions_fini(void);

/*
 * Restoring: the sessions no window is attached to (and the layout of
 * the windows, from the last foot)
 */
struct session_info {
    uint64_t id;
    bool attached;
    pid_t pid;
    char *title;
};

struct session_list {
    struct session_info *sessions;
    size_t count;
    void *layout;
    size_t layout_len;
};

/* Asks footd (if it's running; it isn't started for this) */
bool sessions_list(struct fdm *fdm, struct session_list *list);
void session_list_free(struct session_list *list);

/*
 * The next terminal (session_link_new()) attaches to the session 'id',
 * instead of creating one. With 'hold', it doesn't before
 * sessions_release(): until the window has its final size (it isn't
 * resized after it got the session's state).
 */
void sessions_attach_next(uint64_t id, bool hold);
void sessions_release(void);

/* The windows' layout, for the next foot (sessions_list()) */
void sessions_set_layout(const void *layout, size_t len);
