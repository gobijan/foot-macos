#pragma once

/*
 * Internal interface of the macOS platform layer (the .m files in macos/).
 */

#import <AppKit/AppKit.h>

#include "keyboard.h"
#include "wayland.h"

/* app.m */
struct wayland *app_wayland(void);
struct seat *app_seat(void);
uint32_t app_next_serial(void);
enum option_as_alt app_option_as_alt(void);

/* Runs foot's FDM hooks (e.g. rendering), after handling AppKit events */
void app_run_hooks(void);

/* The monitor corresponding to 'screen' (may be NULL) */
const struct monitor *app_monitor_for_screen(NSScreen *screen);

/* window.m */
void window_update_outputs(struct wl_window *win);
void window_resized(struct wl_window *win);
void window_backing_changed(struct wl_window *win);

/* The current mouse cursor (app.m) */
NSCursor *window_cursor(void);
