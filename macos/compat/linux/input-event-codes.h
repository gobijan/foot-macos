#pragma once

/*
 * foot uses the Linux evdev codes as its vocabulary for mouse buttons
 * (e.g. in mouse bindings), and a handful of key codes in unit
 * tests. The values are those of <linux/input-event-codes.h>.
 */

#define KEY_TAB         15
#define KEY_Y           21
#define KEY_ENTER       28
#define KEY_A           30
#define KEY_APOSTROPHE  40
#define KEY_SPACE       57
#define KEY_LEFT        105
#define KEY_2           3
#define KEY_BACKSPACE   14

#define BTN_MOUSE       0x110
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114
#define BTN_FORWARD     0x115
#define BTN_BACK        0x116
#define BTN_TASK        0x117
