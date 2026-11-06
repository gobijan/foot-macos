#pragma once

/*
 * A terminal's state, as control sequences (macos/footd.c).
 *
 * When a window attaches to a session, footd sends it the state of the
 * session's terminal first. A new terminal of the same size (and with
 * the same configuration) that parses it ends up in the same state:
 * the scrollback, the screen (or the alternate screen, with the normal
 * one behind it), the cursor, the modes, the colors and the title.
 * Nothing in it makes the terminal reply to the session's programs.
 *
 * Not included: sixel images, the title stack, the color stack, and
 * XTSAVE'd modes.
 */

#include <stddef.h>

struct terminal;

/* Returns a malloc:ed buffer of '*len' bytes */
char *snapshot_create(const struct terminal *term, size_t *len);
