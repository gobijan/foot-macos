#pragma once

/*
 * foot's terminal core, without a window (macos/footd.c).
 *
 * footd keeps a terminal of each session's, fed with everything the
 * session's programs write, so that it has the session's state
 * (macos/snapshot.c) when a window attaches again. The platform layer
 * the core calls into does nothing here, as in upstream's PGO harness
 * (pgo/pgo.c); its fonts are stand-ins, with the cell size of the
 * window that's attached.
 */

#include <stdbool.h>

struct config;
struct fdm;
struct reaper;
struct terminal;

/* Once, before the first terminal */
bool headless_init(struct fdm *fdm, struct reaper *reaper);
void headless_fini(void);

/*
 * A new terminal. With 'spawn', 'argv' (or the shell) runs in a new
 * PTY, whose master the caller reads from, and writes to: '*pty'.
 * Without it, there's no PTY ('*pty' is -1).
 *
 * The terminal doesn't write to the PTY itself: its replies (e.g. to a
 * DA query) can be read from '*replies', for the caller to forward (or
 * not). 'shutdown_cb' is called when it's gone (see term_shutdown()).
 */
struct terminal *headless_term_new(
    const struct config *conf, const char *cwd,
    int argc, char *const *argv, const char *const *envp, bool spawn,
    int *pty, int *replies,
    void (*shutdown_cb)(void *data, int exit_code), void *shutdown_data);

/*
 * Feeds the terminal what its programs wrote (as the PTY reader of a
 * window would).
 */
void headless_term_feed(struct terminal *term, const void *data, size_t len);

/*
 * Resizes the terminal, as a window's resize (render_resize()) does:
 * the grid is reflowed. The cell size is the window's, in pixels.
 * Doesn't resize the PTY.
 */
void headless_term_resize(
    struct terminal *term, int cols, int rows, int cell_width, int cell_height);
