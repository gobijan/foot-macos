#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Keyboard support.
 *
 * foot's keyboard handling (key bindings, the legacy and the kitty
 * keyboard protocols) is built on an XKB keymap and state. We feed
 * it a keymap generated from the active macOS keyboard layout, with
 * four levels per key: none, Shift, Option and Shift+Option. Option
 * selects the third level (like AltGr), unless configured to act as
 * Alt.
 *
 * XKB keycodes are macOS virtual keycodes + 8.
 */

struct seat;

enum option_as_alt {
    OPTION_AS_ALT_NONE,
    OPTION_AS_ALT_LEFT,
    OPTION_AS_ALT_RIGHT,
    OPTION_AS_ALT_BOTH,
};

/* Returns an XKB keymap (text v1) for the current layout. Caller frees */
char *keyboard_keymap_create(size_t *len);

/* Calls 'cb' whenever the user switches keyboard layout */
void keyboard_observe_layout_changes(void (*cb)(void *data), void *data);

static inline uint32_t
keyboard_xkb_keycode(uint16_t macos_keycode)
{
    return (uint32_t)macos_keycode + 8;
}

/* Is the Option key (left/right, as indicated by 'ns_flags') acting as Alt? */
bool keyboard_option_is_alt(uint64_t ns_flags, enum option_as_alt option_as_alt);

/* Translates NSEvent modifier flags to XKB modifier masks */
void keyboard_modifiers(
    const struct seat *seat, uint64_t ns_flags, enum option_as_alt option_as_alt,
    uint32_t *depressed, uint32_t *locked);

/*
 * For modifier keys (flagsChanged events): returns true if the key
 * is now held down.
 */
bool keyboard_modifier_key_is_down(uint16_t macos_keycode, uint64_t ns_flags);
