#pragma once

struct terminal;

/* Removes the scrollback and everything above the prompt, which moves
 * to the top */
void clear_to_start(struct terminal *term);

/* Removes only the scrollback */
void clear_scrollback(struct terminal *term);
