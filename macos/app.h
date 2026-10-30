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

/*
 * Applies the color theme: with macos-follow-appearance, the one
 * matching the system's appearance. 'reload': the configuration's
 * themes changed; re-apply, and use initial-color-theme otherwise.
 */
void app_apply_color_theme(struct terminal *term, bool reload);

/* themes.m: View > Theme */
NSMenuItem *themes_menu_item(void);

/* updates.m: foot > Check for Updates… (nil without Sparkle.framework) */
NSMenuItem *updates_menu_item(void);

/* The monitor corresponding to 'screen' (may be NULL) */
const struct monitor *app_monitor_for_screen(NSScreen *screen);

/*
 * A window (not the quake terminal's) was opened or closed: in server
 * mode, foot is in the Dock (and the application switcher) only while
 * there are windows.
 */
void app_windows_changed(void);

/* quake.m: the quake terminal's window, a panel (see wayl_quake()) */
bool quake_owns_config(const struct config *conf);
NSWindow *quake_window_create(NSRect content_rect);

/* The initial configure: its frame (before the terminal is sized), and shown */
void quake_window_place(struct wl_window *win);
void quake_window_configured(struct wl_window *win);

void quake_window_became_key(struct wl_window *win);
void quake_window_resigned_key(struct wl_window *win);
void quake_window_destroyed(struct wl_window *win);

/* themes.m: the quake terminal's color themes were replaced */
void quake_theme_changed(void);

/* Shown, and has the keyboard focus (a toggle hides it) */
bool quake_is_focused(void);

/*
 * hotkey.m: the quake terminal's global hotkey (macos-quake-hotkey);
 * sym 0 (XKB_KEY_NoSymbol): none
 */
void hotkey_register(uint32_t sym, unsigned mods, void (*pressed)(void));
void hotkey_layout_changed(void);

/* Shows the hotkey in the menu */
void hotkey_set_key_equivalent(NSMenuItem *item, uint32_t sym, unsigned mods);

/*
 * close.m: with something running in the terminal (macos-confirm-close),
 * asks first. Shuts the window's terminal down, now or once confirmed.
 */
void window_close(struct wl_window *win);

/* Whether to go ahead with quitting */
bool app_confirm_quit(void);

/* window.m */
void window_update_outputs(struct wl_window *win);
void window_resized(struct wl_window *win);
void window_backing_changed(struct wl_window *win);

/* The current mouse cursor (app.m) */
NSCursor *window_cursor(void);
