#pragma once

/*
 * The windows' layout, kept by footd for persistent sessions, and
 * restored at the next start (macos/layout.m).
 */

#import <AppKit/AppKit.h>

#include <stdbool.h>
#include <stdint.h>

struct fdm;

void session_layout_init(struct fdm *fdm);

/* No more changes are sent (foot is quitting: the windows are going) */
void session_layout_fini(void);

/* Something changed (windows, tabs, splits, frames, focus): sent soon */
void session_layout_changed(void);

/* Sends it now (e.g. before quitting), if it changed */
void session_layout_save(void);

/*
 * At startup: opens windows for the sessions no window is attached to,
 * as they were. Returns whether there were any.
 */
bool session_layout_restore(struct fdm *fdm);

/* window.m: a restored window's frame, for its initial configure */
bool session_layout_restored_frame(NSWindow *window, NSRect *frame);

/* quake.m: the quake terminal's session, to attach to (0: a new one) */
uint64_t session_layout_take_quake_session(void);
