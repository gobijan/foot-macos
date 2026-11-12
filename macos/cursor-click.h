#pragma once

#include <stdbool.h>

struct terminal;

/*
 * Option and a click at the shell's prompt moves the cursor there, as
 * in Terminal.app and Ghostty: the shell gets as many arrow keys as it
 * takes. It needs the shell's integration (the prompt is known by its
 * mark, OSC 133), and isn't done while a command runs, in a program
 * with the alternate screen or one that has the mouse.
 *
 * 'row' and 'col' are the cell (on the screen). False if the click
 * isn't one of those (it's the terminal's, as other clicks).
 */
bool cursor_click(struct terminal *term, int row, int col);

/* How many keys that takes: to the right (> 0), or to the left (< 0) */
int cursor_click_moves(struct terminal *term, int row, int col);
