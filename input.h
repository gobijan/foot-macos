#pragma once

#include <stdint.h>

#include "cursor-shape.h"
#include "key-binding.h"
#include "misc.h"
#include "wayland.h"

/*
 * Custom defines for mouse wheel left/right buttons.
 *
 * Mouse buttons use the Linux evdev codes (BTN_LEFT etc.) as foot's
 * internal vocabulary. All scroll events (both vertical and
 * horizontal) are reported as 'axis' events, not as buttons.
 *
 * BTN_BACK and BTN_FORWARD are used for vertical scroll events. But
 * for horizontal scroll events, there aren't any pre-defined mouse
 * buttons.
 *
 * Mouse buttons are in the range 0x110 - 0x11f, with joystick defines
 * starting at 0x120.
 */
#define BTN_WHEEL_BACK 0x11c
#define BTN_WHEEL_FORWARD 0x11d
#define BTN_WHEEL_LEFT 0x11e
#define BTN_WHEEL_RIGHT 0x11f

enum key_state {
    KEY_STATE_RELEASED,
    KEY_STATE_PRESSED,
};

enum button_state {
    BUTTON_STATE_RELEASED,
    BUTTON_STATE_PRESSED,
};

enum scroll_axis {
    SCROLL_AXIS_VERTICAL,
    SCROLL_AXIS_HORIZONTAL,
};

/*
 * Keyboard input, fed by the platform layer.
 *
 * Keys are XKB keycodes in the seat's current keymap (see
 * macos/keyboard.m); modifiers are XKB modifier masks.
 */
void input_keymap(struct seat *seat, const char *keymap, size_t size);
void input_kbd_enter(struct seat *seat, struct terminal *term, uint32_t serial);
void input_kbd_leave(struct seat *seat);
void input_key(struct seat *seat, uint32_t serial, uint32_t key,
               enum key_state state, bool repeat);
void input_modifiers(struct seat *seat, uint32_t mods_depressed,
                     uint32_t mods_latched, uint32_t mods_locked,
                     uint32_t group);

void input_repeat(struct seat *seat, uint32_t key);

/* Executes a key binding action (e.g. from the application menu) */
bool input_execute_action(struct seat *seat, struct terminal *term,
                          enum bind_action_normal action);

/*
 * Pointer input. Coordinates are surface local, in logical pixels.
 *
 * input_pointer_scroll() is for smooth scrolling (trackpads), with
 * the value in logical pixels; input_pointer_scroll_discrete() is for
 * mouse wheels, with the value in steps. Positive values scroll down
 * (or right).
 */
void input_pointer_enter(struct seat *seat, struct terminal *term,
                         uint32_t serial, double x, double y);
void input_pointer_leave(struct seat *seat);
void input_pointer_motion(struct seat *seat, double x, double y);
void input_pointer_button(struct seat *seat, uint32_t serial, int button,
                          enum button_state state);
void input_pointer_scroll(struct seat *seat, enum scroll_axis axis, double value);
void input_pointer_scroll_discrete(struct seat *seat, enum scroll_axis axis,
                                   int steps);
void input_pointer_scroll_stop(struct seat *seat, enum scroll_axis axis);

void get_current_modifiers(const struct seat *seat,
                           xkb_mod_mask_t *effective,
                           xkb_mod_mask_t *consumed,
                           uint32_t key, bool filter_locked);

void mouse_coord_pixel_to_cell(struct seat *seat, const struct terminal *term,
                               int x, int y);
