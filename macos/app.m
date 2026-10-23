#import "app.h"

#import <QuartzCore/QuartzCore.h>

#include <locale.h>
#include <math.h>
#include <unistd.h>

#include <tllist.h>

#define LOG_MODULE "app"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "debug.h"
#include "fdm.h"
#include "ime.h"
#include "input.h"
#include "key-binding.h"
#include "render.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

/*
 * The application: NSApplication, with foot's FDM integrated into
 * the main run loop.
 *
 * The FDM's kqueue is watched by a dispatch source on the main queue;
 * when it becomes readable, pending events are dispatched without
 * blocking. foot's FDM hooks (which, among other things, render
 * terminals with pending updates) run right before the run loop goes
 * to sleep - just like they run before epoll_wait() on Linux.
 */

@interface FootAppDelegate : NSObject <NSApplicationDelegate>
@end

static struct {
    struct wayland *wayl;
    uint32_t serial;

    dispatch_source_t fdm_source;
    CFRunLoopObserverRef observer;

    volatile sig_atomic_t *aborted;
    bool keep_running;
    bool fdm_failed;
    bool running;
    bool terminating;

    bool (*new_window)(void *data, const char *cwd);
    void *new_window_data;

    bool (*settings)(void *data);
    void *settings_data;

    FootAppDelegate *delegate;
} app;

struct wayland *
app_wayland(void)
{
    return app.wayl;
}

struct seat *
app_seat(void)
{
    return tll_length(app.wayl->seats) > 0 ? &tll_front(app.wayl->seats) : NULL;
}

uint32_t
app_next_serial(void)
{
    if (++app.serial == 0)
        app.serial = 1;
    return app.serial;
}

enum option_as_alt
app_option_as_alt(void)
{
    if (tll_length(app.wayl->terms) == 0)
        return OPTION_AS_ALT_NONE;

    const struct terminal *term = tll_front(app.wayl->terms);
    return (enum option_as_alt)term->conf->macos.option_as_alt;
}

/*
 * Run loop integration
 */

static bool
should_exit(void)
{
    if (app.fdm_failed)
        return true;
    if (app.aborted != NULL && *app.aborted)
        return true;
    return !app.keep_running && tll_length(app.wayl->terms) == 0;
}

static void
stop_running(void)
{
    if (!app.running)
        return;

    app.running = false;
    [NSApp stop:nil];

    /* -stop: only takes effect after the next event has been processed */
    NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
}

void
app_run_hooks(void)
{
    if (app.wayl == NULL)
        return;

    fdm_run_hooks(app.wayl->fdm);

    if (app.running && should_exit())
        stop_running();
}

static void
before_waiting(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    app_run_hooks();
}

static void
fdm_readable(void)
{
    if (!fdm_dispatch(app.wayl->fdm)) {
        LOG_ERR("FDM failure; exiting");
        app.fdm_failed = true;
        stop_running();
    }
}

bool
wayl_run(struct wayland *wayl, volatile sig_atomic_t *aborted, bool keep_running)
{
    app.aborted = aborted;
    app.keep_running = keep_running;

    if (should_exit())
        return !app.fdm_failed;

    if (@available(macOS 14.0, *))
        [NSApp activate];

    app.running = true;
    [NSApp run];
    app.running = false;

    return !app.fdm_failed;
}

void
wayl_set_new_window_handler(struct wayland *wayl,
                            bool (*handler)(void *data, const char *cwd),
                            void *data)
{
    app.new_window = handler;
    app.new_window_data = data;
}

void
wayl_set_settings_handler(struct wayland *wayl, bool (*handler)(void *data),
                          void *data)
{
    app.settings = handler;
    app.settings_data = data;
}

static void
new_window(const char *cwd)
{
    if (app.new_window == NULL)
        return;

    if (!app.new_window(app.new_window_data, cwd))
        LOG_ERR("failed to open a new window");
}

void
wayl_prepare_environment(void)
{
    @autoreleasepool {
        /* Started from Finder/Dock: no locale in the environment */
        if (getenv("LANG") == NULL && getenv("LC_ALL") == NULL &&
            getenv("LC_CTYPE") == NULL)
        {
            NSLocale *locale = [NSLocale currentLocale];
            NSString *lang = locale.languageCode != nil && locale.countryCode != nil
                ? [NSString stringWithFormat:@"%@_%@.UTF-8",
                   locale.languageCode, locale.countryCode]
                : @"en_US.UTF-8";

            if (setlocale(LC_CTYPE, lang.UTF8String) == NULL)
                lang = @"en_US.UTF-8";

            setenv("LANG", lang.UTF8String, 1);
            setlocale(LC_CTYPE, "C");
        }

        /* foot's terminfo; in the bundle, or next to the binary (build dir) */
        NSString *terminfo = nil;
        NSString *resources = [NSBundle mainBundle].resourcePath;
        NSString *exe_dir = [NSBundle mainBundle].executablePath.stringByDeletingLastPathComponent;

        for (NSString *dir in @[[resources stringByAppendingPathComponent:@"terminfo"],
                                [exe_dir stringByAppendingPathComponent:@"terminfo"]])
        {
            BOOL is_dir = NO;
            if ([[NSFileManager defaultManager] fileExistsAtPath:dir isDirectory:&is_dir] && is_dir) {
                terminfo = dir;
                break;
            }
        }

        if (terminfo != nil) {
            /* A trailing ':' means "and the system's default locations" */
            const char *existing = getenv("TERMINFO_DIRS");
            NSString *dirs = existing != NULL && existing[0] != '\0'
                ? [NSString stringWithFormat:@"%@:%s", terminfo, existing]
                : [NSString stringWithFormat:@"%@:", terminfo];
            setenv("TERMINFO_DIRS", dirs.UTF8String, 1);
        }

        /* ... and the working directory is the root directory */
        char cwd[2];
        if (!isatty(STDIN_FILENO) && getcwd(cwd, sizeof(cwd)) != NULL &&
            strcmp(cwd, "/") == 0)
        {
            const char *home = getenv("HOME");
            if (home != NULL && chdir(home) == 0)
                setenv("PWD", home, 1);
        }
    }
}

/*
 * Monitors
 */

static void
monitor_update(struct monitor *mon, NSScreen *screen)
{
    const NSRect frame = screen.frame;
    const CGFloat scale = screen.backingScaleFactor;
    const CGSize mm = CGDisplayScreenSize(mon->display_id);

    mon->x = frame.origin.x;
    mon->y = frame.origin.y;
    mon->scale = (int)scale;
    mon->refresh = screen.maximumFramesPerSecond;
    mon->subpixel = MONITOR_SUBPIXEL_UNKNOWN;

    mon->dim.mm.width = mm.width;
    mon->dim.mm.height = mm.height;
    mon->dim.px_scaled.width = frame.size.width;
    mon->dim.px_scaled.height = frame.size.height;
    mon->dim.px_real.width = frame.size.width * scale;
    mon->dim.px_real.height = frame.size.height * scale;

    free(mon->name);
    free(mon->description);
    mon->name = xstrdup(screen.localizedName.UTF8String);
    mon->description = xstrdup(screen.localizedName.UTF8String);

    mon->inch = sqrt(pow(mm.width, 2) + pow(mm.height, 2)) * 0.03937008;

    /* PPI/DPI, as in upstream foot */
    if (mon->dim.mm.width > 0 && mon->dim.mm.height > 0) {
        const double x_inches = mon->dim.mm.width * 0.03937008;
        const double y_inches = mon->dim.mm.height * 0.03937008;

        mon->ppi.real.x = mon->dim.px_real.width / x_inches;
        mon->ppi.real.y = mon->dim.px_real.height / y_inches;
        mon->ppi.scaled.x = mon->dim.px_scaled.width / x_inches;
        mon->ppi.scaled.y = mon->dim.px_scaled.height / y_inches;

        const double px_diag_physical = sqrt(
            pow(mon->dim.px_real.width, 2) + pow(mon->dim.px_real.height, 2));
        const double px_diag_scaled = sqrt(
            pow(mon->dim.px_scaled.width, 2) + pow(mon->dim.px_scaled.height, 2));

        mon->dpi.physical = px_diag_physical / mon->inch;
        mon->dpi.scaled = px_diag_scaled / mon->inch * mon->scale;

        if (mon->dpi.physical > 1000)
            mon->dpi.physical = 96;
        if (mon->dpi.scaled > 1000)
            mon->dpi.scaled = 96;
    } else {
        mon->dpi.physical = mon->dpi.scaled = 72. * scale;
    }

    LOG_INFO("%s: %dx%d+%dx%d@%.0fHz, %.2f\" scale=%d, DPI=%.2f/%.2f",
             mon->name, mon->dim.px_real.width, mon->dim.px_real.height,
             mon->x, mon->y, mon->refresh, mon->inch, mon->scale,
             mon->dpi.physical, mon->dpi.scaled);
}

static void
monitor_destroy(struct monitor *mon)
{
    free(mon->make);
    free(mon->model);
    free(mon->name);
    free(mon->description);
}

static void
update_monitors(struct wayland *wayl)
{
    NSArray<NSScreen *> *screens = [NSScreen screens];

    /* Remove monitors that are gone */
    tll_foreach(wayl->monitors, it) {
        struct monitor *mon = &it->item;
        bool found = false;

        for (NSScreen *screen in screens) {
            if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == mon->display_id) {
                found = true;
                break;
            }
        }

        if (found)
            continue;

        tll_foreach(wayl->terms, tit) {
            struct wl_window *win = tit->item->window;
            if (win == NULL)
                continue;
            tll_foreach(win->on_outputs, oit) {
                if (oit->item == mon)
                    tll_remove(win->on_outputs, oit);
            }
        }

        monitor_destroy(mon);
        tll_remove(wayl->monitors, it);
    }

    /* Add new, update existing */
    for (NSScreen *screen in screens) {
        const uint32_t id = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        struct monitor *mon = NULL;

        tll_foreach(wayl->monitors, it) {
            if (it->item.display_id == id) {
                mon = &it->item;
                break;
            }
        }

        if (mon == NULL) {
            tll_push_back(wayl->monitors, ((struct monitor){.wayl = wayl, .display_id = id}));
            mon = &tll_back(wayl->monitors);
        }

        monitor_update(mon, screen);
    }

    tll_foreach(wayl->terms, it) {
        if (it->item->window != NULL)
            window_update_outputs(it->item->window);
    }
}

const struct monitor *
app_monitor_for_screen(NSScreen *screen)
{
    if (screen == nil || app.wayl == NULL)
        return NULL;

    const uint32_t id = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    tll_foreach(app.wayl->monitors, it) {
        if (it->item.display_id == id)
            return &it->item;
    }
    return NULL;
}

/*
 * Seat
 */

static void
load_keymap(struct seat *seat)
{
    size_t len;
    char *keymap = keyboard_keymap_create(&len);
    input_keymap(seat, keymap, len);
    free(keymap);
}

static void
layout_changed(void *data)
{
    load_keymap(data);
}

static void
seat_init(struct wayland *wayl)
{
    tll_push_back(wayl->seats, ((struct seat){
        .wayl = wayl,
        .name = xstrdup("seat0"),
        .kbd = {.repeat = {.fd = -1}},
        .pointer = {.shape = CURSOR_SHAPE_NONE},
    }));

    struct seat *seat = &tll_back(wayl->seats);

    /* Keymaps are generated, and compiled from strings */
    seat->kbd.xkb = xkb_context_new(
        XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);

    key_binding_new_for_seat(wayl->key_binding_manager, seat);
    load_keymap(seat);
    keyboard_observe_layout_changes(&layout_changed, seat);
}

static void
seat_destroy(struct seat *seat)
{
    tll_free(seat->mouse.buttons);
    key_binding_remove_seat(seat->wayl->key_binding_manager, seat);

    if (seat->kbd.xkb_compose_state != NULL)
        xkb_compose_state_unref(seat->kbd.xkb_compose_state);
    if (seat->kbd.xkb_compose_table != NULL)
        xkb_compose_table_unref(seat->kbd.xkb_compose_table);
    if (seat->kbd.xkb_keymap != NULL)
        xkb_keymap_unref(seat->kbd.xkb_keymap);
    if (seat->kbd.xkb_state != NULL)
        xkb_state_unref(seat->kbd.xkb_state);
    if (seat->kbd.xkb != NULL)
        xkb_context_unref(seat->kbd.xkb);

    ime_reset_pending(seat);
    ime_reset_preedit(seat);
    free(seat->clipboard.text);
    free(seat->primary.text);
    free(seat->pointer.last_custom_xcursor);
    free(seat->name);
}

/*
 * Appearance: with macos-follow-appearance, the system's light/dark
 * mode picks the color theme ([colors-light] or [colors-dark])
 */

static int appearance_context;

static bool
appearance_is_dark(void)
{
    NSAppearanceName name = [NSApp.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    return [name isEqualToString:NSAppearanceNameDarkAqua];
}

void
app_apply_color_theme(struct terminal *term, bool reload)
{
    if (term->shutdown.in_progress)
        return;

    /* The grid has no size yet; the initial configure applies it */
    if (term->window == NULL || !term->window->is_configured)
        return;

    /* The configurations aren't const objects (see main.c, server.c
     * and windows.c) */
    struct config *conf = (struct config *)term->conf;

    /*
     * The appearance takes the place of initial-color-theme, which
     * hard resets (term_reset()) return to
     */
    if (conf->macos.follow_appearance) {
        conf->initial_color_theme =
            appearance_is_dark() ? COLOR_THEME_DARK : COLOR_THEME_LIGHT;
    } else if (!reload)
        return;

    const bool dark = conf->initial_color_theme == COLOR_THEME_DARK;

    /* term_theme_switch_to_*() only switch to another theme */
    if (reload)
        term->colors.active_theme = dark ? COLOR_THEME_LIGHT : COLOR_THEME_DARK;

    if (dark)
        term_theme_switch_to_dark(term);
    else
        term_theme_switch_to_light(term);
}

/*
 * Menu
 */

static NSMenuItem *
menu_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = mods;
    return item;
}

static void
create_menu(void)
{
    NSMenu *bar = [[NSMenu alloc] init];
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;

    /* Application menu */
    {
        NSMenuItem *item = [[NSMenuItem alloc] init];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"foot"];

        [menu addItem:menu_item(@"About foot", @selector(orderFrontStandardAboutPanel:), @"", 0)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Settings…", @selector(openSettings:), @",", cmd)];
        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *services = menu_item(@"Services", nil, @"", 0);
        NSMenu *services_menu = [[NSMenu alloc] initWithTitle:@"Services"];
        services.submenu = services_menu;
        NSApp.servicesMenu = services_menu;
        [menu addItem:services];
        [menu addItem:[NSMenuItem separatorItem]];

        [menu addItem:menu_item(@"Hide foot", @selector(hide:), @"h", cmd)];
        [menu addItem:menu_item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                                cmd | NSEventModifierFlagOption)];
        [menu addItem:menu_item(@"Show All", @selector(unhideAllApplications:), @"", 0)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Quit foot", @selector(terminate:), @"q", cmd)];

        item.submenu = menu;
        [bar addItem:item];
    }

    /* Shell */
    {
        NSMenuItem *item = [[NSMenuItem alloc] init];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Shell"];

        [menu addItem:menu_item(@"New Window", @selector(newWindow:), @"n", cmd)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Close Window", @selector(performClose:), @"w", cmd)];

        item.submenu = menu;
        [bar addItem:item];
    }

    /* Edit */
    {
        NSMenuItem *item = [[NSMenuItem alloc] init];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Edit"];

        [menu addItem:menu_item(@"Copy", @selector(copy:), @"c", cmd)];
        [menu addItem:menu_item(@"Paste", @selector(paste:), @"v", cmd)];
        [menu addItem:menu_item(@"Paste Selection", @selector(pasteSelection:), @"V",
                                cmd | NSEventModifierFlagShift)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Find…", @selector(performFindPanelAction:), @"f", cmd)];

        item.submenu = menu;
        [bar addItem:item];
    }

    /* View */
    {
        NSMenuItem *item = [[NSMenuItem alloc] init];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"View"];

        [menu addItem:menu_item(@"Bigger", @selector(increaseFontSize:), @"+", cmd)];
        [menu addItem:menu_item(@"Bigger", @selector(increaseFontSize:), @"=", cmd)];
        menu.itemArray.lastObject.alternate = YES;
        [menu addItem:menu_item(@"Smaller", @selector(decreaseFontSize:), @"-", cmd)];
        [menu addItem:menu_item(@"Default Font Size", @selector(resetFontSize:), @"0", cmd)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:themes_menu_item()];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Show URLs", @selector(showURLs:), @"u",
                                cmd | NSEventModifierFlagShift)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f",
                                cmd | NSEventModifierFlagControl)];

        item.submenu = menu;
        [bar addItem:item];
    }

    /* Window */
    {
        NSMenuItem *item = [[NSMenuItem alloc] init];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Window"];

        [menu addItem:menu_item(@"Minimize", @selector(performMiniaturize:), @"m", cmd)];
        [menu addItem:menu_item(@"Zoom", @selector(performZoom:), @"", 0)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:menu_item(@"Bring All to Front", @selector(arrangeInFront:), @"", 0)];

        item.submenu = menu;
        [bar addItem:item];
        NSApp.windowsMenu = menu;
    }

    NSApp.mainMenu = bar;
}

@implementation FootAppDelegate

- (void)newWindow:(id)sender
{
    new_window(NULL);
}

- (void)openSettings:(id)sender
{
    if (app.settings != NULL && !app.settings(app.settings_data))
        LOG_ERR("failed to open the settings");
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    if (!app.running || tll_length(app.wayl->terms) == 0)
        return NSTerminateNow;

    /* Shut down all terminals; the run loop exits when they're gone */
    app.terminating = true;
    app.keep_running = false;

    tll_foreach(app.wayl->terms, it)
        term_shutdown(it->item);

    app_run_hooks();
    return NSTerminateCancel;
}

- (NSMenu *)applicationDockMenu:(NSApplication *)sender
{
    NSMenu *menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"New Window" action:@selector(newWindow:) keyEquivalent:@""];
    return menu;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible
{
    if (!visible && tll_length(app.wayl->terms) == 0)
        new_window(NULL);
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)sender
{
    return YES;
}

- (void)screensChanged:(NSNotification *)notification
{
    update_monitors(app.wayl);
    app_run_hooks();
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context
{
    if (context != &appearance_context) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }

    /* The system's appearance changed */
    tll_foreach(app.wayl->terms, it)
        app_apply_color_theme(it->item, false);
    app_run_hooks();
}

@end

struct wayland *
wayl_init(struct fdm *fdm, struct key_binding_manager *key_binding_manager,
          bool presentation_timings)
{
    struct wayland *wayl = xcalloc(1, sizeof(*wayl));
    wayl->fdm = fdm;
    wayl->key_binding_manager = key_binding_manager;
    wayl->presentation_timings = presentation_timings;

    if (presentation_timings)
        LOG_WARN("presentation timings are not supported on macOS");

    app.wayl = wayl;
    app.serial = 1;

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        app.delegate = [[FootAppDelegate alloc] init];
        NSApp.delegate = app.delegate;
        create_menu();

        [[NSNotificationCenter defaultCenter]
            addObserver:app.delegate
               selector:@selector(screensChanged:)
                   name:NSApplicationDidChangeScreenParametersNotification
                 object:nil];

        [NSApp addObserver:app.delegate
                forKeyPath:@"effectiveAppearance"
                   options:0
                   context:&appearance_context];

        [NSApp finishLaunching];
    }

    update_monitors(wayl);
    if (tll_length(wayl->monitors) == 0) {
        LOG_ERR("no monitors");
        goto err;
    }

    seat_init(wayl);

    /* FDM events are dispatched from the main run loop */
    app.fdm_source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_READ, fdm_fd(fdm), 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(app.fdm_source, ^{ fdm_readable(); });
    dispatch_resume(app.fdm_source);

    /* Run FDM hooks before the run loop goes to sleep (and before
     * Core Animation commits its implicit transactions) */
    app.observer = CFRunLoopObserverCreate(
        NULL, kCFRunLoopBeforeWaiting, true, 0, &before_waiting, NULL);
    CFRunLoopAddObserver(CFRunLoopGetMain(), app.observer, kCFRunLoopCommonModes);

    return wayl;

err:
    wayl_destroy(wayl);
    return NULL;
}

void
wayl_destroy(struct wayland *wayl)
{
    if (wayl == NULL)
        return;

    tll_foreach(wayl->terms, it) {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN("there are terminals still running");
        }
        term_destroy(it->item);
    }
    tll_free(wayl->terms);

    if (app.observer != NULL) {
        CFRunLoopRemoveObserver(CFRunLoopGetMain(), app.observer, kCFRunLoopCommonModes);
        CFRelease(app.observer);
        app.observer = NULL;
    }

    if (app.fdm_source != NULL) {
        dispatch_source_cancel(app.fdm_source);
        app.fdm_source = NULL;
    }

    tll_foreach(wayl->seats, it) {
        seat_destroy(&it->item);
        tll_remove(wayl->seats, it);
    }

    tll_foreach(wayl->monitors, it) {
        monitor_destroy(&it->item);
        tll_remove(wayl->monitors, it);
    }

    [[NSNotificationCenter defaultCenter] removeObserver:app.delegate];
    [NSApp removeObserver:app.delegate
               forKeyPath:@"effectiveAppearance"
                  context:&appearance_context];

    app.wayl = NULL;
    free(wayl);
}

void
wayl_flush(struct wayland *wayl)
{
    [CATransaction flush];
}

void
wayl_roundtrip(struct wayland *wayl)
{
    [CATransaction flush];
}

bool
wayl_fractional_scaling(const struct wayland *wayl)
{
    return false;
}

bool
wayl_do_linear_blending(const struct wayland *wayl, const struct config *conf)
{
    /* TODO: linear blending, with half-float IOSurfaces */
    return false;
}

bool
wayl_get_activation_token(
    struct wayland *wayl, struct seat *seat, uint32_t serial,
    struct wl_window *win, activation_token_cb_t cb, void *cb_data)
{
    /* There are no activation tokens on macOS */
    return false;
}

void
wayl_activate(struct wayland *wayl, struct wl_window *win, const char *token)
{
    if (win == NULL || win->nswindow == NULL)
        return;

    NSWindow *nswindow = (__bridge NSWindow *)win->nswindow;
    [NSApp activateIgnoringOtherApps:YES];
    [nswindow makeKeyAndOrderFront:nil];
}

/*
 * Cursors
 */

static NSCursor *
cursor_for_name(const char *name)
{
    if (name == NULL)
        return nil;

    static const struct {
        const char *name;
        const char *sel;  /* NSCursor class method */
    } map[] = {
        {"default", "arrowCursor"},
        {"left_ptr", "arrowCursor"},
        {"text", "IBeamCursor"},
        {"xterm", "IBeamCursor"},
        {"vertical-text", "IBeamCursorForVerticalLayout"},
        {"pointer", "pointingHandCursor"},
        {"hand", "pointingHandCursor"},
        {"hand2", "pointingHandCursor"},
        {"crosshair", "crosshairCursor"},
        {"cross", "crosshairCursor"},
        {"cell", "crosshairCursor"},
        {"grab", "openHandCursor"},
        {"hand1", "openHandCursor"},
        {"grabbing", "closedHandCursor"},
        {"not-allowed", "operationNotAllowedCursor"},
        {"no-drop", "operationNotAllowedCursor"},
        {"crossed_circle", "operationNotAllowedCursor"},
        {"copy", "dragCopyCursor"},
        {"dnd-copy", "dragCopyCursor"},
        {"alias", "dragLinkCursor"},
        {"dnd-link", "dragLinkCursor"},
        {"context-menu", "contextualMenuCursor"},
        {"col-resize", "resizeLeftRightCursor"},
        {"ew-resize", "resizeLeftRightCursor"},
        {"sb_h_double_arrow", "resizeLeftRightCursor"},
        {"row-resize", "resizeUpDownCursor"},
        {"ns-resize", "resizeUpDownCursor"},
        {"sb_v_double_arrow", "resizeUpDownCursor"},
        {"e-resize", "resizeRightCursor"},
        {"right_side", "resizeRightCursor"},
        {"w-resize", "resizeLeftCursor"},
        {"left_side", "resizeLeftCursor"},
        {"n-resize", "resizeUpCursor"},
        {"top_side", "resizeUpCursor"},
        {"s-resize", "resizeDownCursor"},
        {"bottom_side", "resizeDownCursor"},
        {"move", "openHandCursor"},
        {"all-scroll", "openHandCursor"},
        {"fleur", "openHandCursor"},
        {"wait", "arrowCursor"},
        {"progress", "arrowCursor"},
        {"help", "arrowCursor"},
    };

    for (size_t i = 0; i < ALEN(map); i++) {
        if (strcmp(map[i].name, name) == 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            return [NSCursor performSelector:NSSelectorFromString(@(map[i].sel))];
#pragma clang diagnostic pop
        }
    }

    return nil;
}

bool
wayl_cursor_is_valid(const char *name)
{
    return cursor_for_name(name) != nil;
}

static NSCursor *current_cursor = nil;

NSCursor *
window_cursor(void)
{
    return current_cursor != nil ? current_cursor : [NSCursor arrowCursor];
}

void
wayl_set_cursor(struct seat *seat, enum cursor_shape shape, const char *custom_name)
{
    switch (shape) {
    case CURSOR_SHAPE_NONE:
        return;

    case CURSOR_SHAPE_HIDDEN:
        [NSCursor setHiddenUntilMouseMoves:YES];
        return;

    case CURSOR_SHAPE_CUSTOM:
        current_cursor = cursor_for_name(custom_name);
        break;

    case CURSOR_SHAPE_TEXT:
        current_cursor = [NSCursor IBeamCursor];
        break;

    case CURSOR_SHAPE_LEFT_PTR:
    default:
        current_cursor = [NSCursor arrowCursor];
        break;
    }

    [window_cursor() set];
}
