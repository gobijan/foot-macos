#pragma once

#include <stdbool.h>

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
    HINT_OPEN_PATH,     /* Opened, as Finder would (those there are) */
    HINT_HASH,
    HINT_IP,
    HINT_LINE,
    HINT_WORD,
    HINT_COUNT,
};

/* False if there's nothing of that kind on the screen */
bool hints_show(struct terminal *term, enum hint hint);
