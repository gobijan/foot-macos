#pragma once

#include <stdbool.h>
#include <stdint.h>

struct seat;
struct terminal;

/*
 * Hints (View > Hints), as kitty's: what's on the screen and looks like
 * a path, a hash etc. gets a label, as URLs do in foot's URL mode (it is
 * that mode, with patterns of our own); typing one copies its text. In
 * upper case, it's typed into the terminal as well (foot.ini's
 * uppercase-regex-insert).
 */
enum hint {
    HINT_PATH,
    HINT_HASH,
    HINT_IP,
    HINT_LINE,
    HINT_WORD,
    HINT_COUNT,
};

/* False if there's nothing of that kind on the screen */
bool hints_show(struct terminal *term, enum hint hint);

/*
 * View > Open URL or Path: foot's URL mode, with the paths of files
 * (and folders) there are as well; they're opened as URLs are ([url]'s
 * launch). Relative ones are the shell's directory's (OSC 7, as the
 * shell integration reports it). Any name of a file there is counts
 * (Makefile), git's a/ and b/ before a path don't, nor does a
 * sentence's "." after it. A name may have spaces: in quotes, after
 * "\", or as it is (words with a space between them are looked for as
 * one name, the longest first). A path with a line number (a.c:12:5,
 * or Python's "a.py", line 12) is opened in $EDITOR, at that line.
 */
bool hints_show_open(struct terminal *term);

/*
 * Cmd+click: the same, without the labels, for what's at a cell of the
 * view. Returns whether there is something; 'open' opens it ('serial':
 * the click's).
 */
bool hints_open_at(struct seat *seat, struct terminal *term, int row, int col, bool open,
                   uint32_t serial);

/*
 * For url-mode.c, which opens all of them: a file's URL with a line
 * number for its fragment (file:///a.c#12, or #12:5 with the column:
 * ours, and hyperlinks as kitty has them) is opened in $EDITOR, at that
 * line, in a new window, instead of with [url]'s launch. False: it
 * isn't one.
 */
bool hints_open_line(struct terminal *term, const char *url);
