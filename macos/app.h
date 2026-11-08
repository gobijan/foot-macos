#pragma once

/*
 * Internal interface of the macOS platform layer (the .m files in macos/).
 */

#import <AppKit/AppKit.h>

#include "keyboard.h"
#include "splits.h"
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

/* notifications.m: before the application has finished launching */
void notifications_init(void);

/* themes.m: View > Theme */
NSMenuItem *themes_menu_item(void);

/* updates.m: foot > Check for Updates… (nil without Sparkle.framework) */
NSMenuItem *updates_menu_item(void);

/*
 * A new window, or tab, on another machine ('host', as for ssh; nil:
 * this one): a session there (macos/session.h)
 */
void app_open_on(NSString *host, bool tab);

/* Shell > Remove footd from: footd is removed there, once confirmed */
void app_remove_footd(NSString *host);

/*
 * connections.m: Shell > Connections, the other machines (for sessions
 * there). They're known by their address (as for ssh: [user@]host, or
 * a Host of ~/.ssh/config), and may have a name given there.
 */
void connections_show(void);

/* Those for Shell > New Window on (addresses), by title */
NSArray<NSString *> *connections_addresses(void);

/* The name given (nil: none); and that, or else the address */
NSString *connections_name(NSString *address);
NSString *connections_title(NSString *address);

/* A window was opened there (Other…): it's in the list from now on */
void connections_remember(NSString *address);

/* 'text', if it's an address (nil: it isn't, e.g. an option, or empty) */
NSString *connections_address(NSString *text);

/* While a new tab's window is created: the window it joins (else nil) */
NSWindow *app_new_tab_parent(void);

/*
 * A new terminal, in 'cwd' (NULL: the home directory): in a window of
 * its own, or a tab of 'tab_parent', or a split of 'split_parent' (in
 * 'dir'). Returns it, or NULL.
 */
struct terminal *app_new_terminal(const char *cwd, NSWindow *tab_parent,
                                  struct wl_window *split_parent,
                                  enum split_dir dir);

/*
 * Splits 'win': a new terminal, in its working directory, goes in 'dir'
 * (macos/split-view.m). While it's created, app_new_split_parent()
 * returns 'win' (else NULL), and the direction in '*dir' (may be NULL).
 */
void app_new_split(struct wl_window *win, enum split_dir dir);
struct wl_window *app_new_split_parent(enum split_dir *dir);

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

/* The quake terminal, if there is one */
struct terminal *quake_terminal(void);
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
 * asks first. Shuts the pane's terminal down, now or once confirmed.
 */
void window_close(struct wl_window *win);

/* As above, for all panes of 'windows' (e.g. tabs), asking (once) in 'window' */
void window_close_all(NSWindow *window, NSArray<NSWindow *> *windows);

/*
 * Whether to go ahead with quitting; with 'end_sessions', sessions end
 * too (else they keep running: nothing to ask about them)
 */
bool app_confirm_quit(bool end_sessions);

/* window.m */
void window_update_outputs(struct wl_window *win);
void window_resized(struct wl_window *win);
void window_backing_changed(struct wl_window *win);

/* The pane gets, or loses, the keyboard focus (its window is key) */
void window_focus_in(struct wl_window *win);
void window_focus_out(struct wl_window *win);

/* The window (or a divider) is no longer being resized: the final size */
void window_live_resize_ended(struct wl_window *win);

/* The terminal's background color changed (e.g. OSC 11, a color theme) */
void window_colors_changed(struct wl_window *win);

/* The current mouse cursor (app.m) */
NSCursor *window_cursor(void);
