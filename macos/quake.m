#import "app.h"

#import <QuartzCore/QuartzCore.h>

#include <math.h>

#define LOG_MODULE "quake"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "client-protocol.h"
#include "config.h"
#include "render.h"
#include "terminal.h"

/*
 * The quake terminal (footclient --quake): a terminal in a panel that
 * slides in from an edge of the screen, over other applications'
 * windows, on every Space, also over full screen applications. There
 * is at most one, in the server (foot --server). The first footclient
 * --quake starts it; it's then shown and hidden, until its shell exits.
 *
 * Its placement, opacity, dimming, auto-hide and animation are the
 * macos-quake-* options: the server's configuration, with each
 * footclient's overrides (-o). That's how a window manager can control
 * them.
 */

enum quake_position {
    QUAKE_TOP,
    QUAKE_BOTTOM,
    QUAKE_LEFT,
    QUAKE_RIGHT,
    QUAKE_CENTER,
};

static struct {
    struct terminal *term;
    struct config *conf;            /* The terminal's (ours) */
    struct macos_quake settings;    /* The latest footclient's */

    /* The color themes' own alpha; macos-quake-alpha takes its place */
    uint16_t alpha_dark;
    uint16_t alpha_light;

    bool visible;       /* Shown, or being shown */
    bool sliding_in;    /* The show animation is running */
    bool show_when_configured;
    uint64_t generation;  /* Of the latest show or hide */

    /* Active before the terminal was shown; re-activated when it's hidden */
    NSRunningApplication *previous;

    /* Darkens the rest of the screen (macos-quake-dim) */
    NSWindow *dim;
    CGFloat dim_alpha;      /* Darkened (or being darkened) this much */
    uint64_t dim_generation;
} quake;

/*
 * The panel: borderless, and non-activating (showing it doesn't switch
 * to the Space of foot's other windows, or out of a full screen app).
 */

@interface FootQuakePanel : NSPanel
@end

@implementation FootQuakePanel

/* Borderless windows can't become key (or main) otherwise */
- (BOOL)canBecomeKeyWindow
{
    return YES;
}

- (BOOL)canBecomeMainWindow
{
    return YES;
}

/* Close Window (Cmd+W): as other windows, the terminal shuts down */
- (void)performClose:(id)sender
{
    [self.delegate windowShouldClose:self];
}

/* A borderless window has no close button, so AppKit disables Close Window */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    return item.action == @selector(performClose:) || [super validateMenuItem:item];
}

/* Neither full screen, nor zoomed, nor minimized */
- (void)toggleFullScreen:(id)sender
{
}

- (void)performZoom:(id)sender
{
}

- (void)performMiniaturize:(id)sender
{
}

@end

bool
quake_owns_config(const struct config *conf)
{
    return conf != NULL && conf == quake.conf;
}

NSWindow *
quake_window_create(NSRect content_rect)
{
    FootQuakePanel *panel = [[FootQuakePanel alloc]
        initWithContentRect:content_rect
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];

    panel.level = NSFloatingWindowLevel;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorFullScreenAuxiliary |
                               NSWindowCollectionBehaviorIgnoresCycle;
    panel.hidesOnDeactivate = NO;
    panel.canHide = NO;
    panel.becomesKeyOnlyIfNeeded = NO;
    panel.excludedFromWindowsMenu = YES;
    panel.animationBehavior = NSWindowAnimationBehaviorNone;

    /* Window managers (Accessibility) see a floating window, and which one */
    NSString *bundle_id = NSBundle.mainBundle.bundleIdentifier;
    panel.identifier = [NSString stringWithFormat:@"%@.quake", bundle_id != nil ? bundle_id : @"foot"];
    [panel setAccessibilitySubrole:NSAccessibilityFloatingWindowSubrole];

    return panel;
}

static NSWindow *
quake_window(void)
{
    return (__bridge NSWindow *)quake.term->window->nswindow;
}

/*
 * Placement
 */

/* The area the terminal is placed in (macos-quake-area), and its screen */
static NSScreen *
placement_area(NSRect *area)
{
    const struct macos_quake *s = &quake.settings;

    if (s->area.width > 0. && s->area.height > 0.) {
        *area = NSMakeRect(s->area.x, s->area.y, s->area.width, s->area.height);

        const NSPoint center = NSMakePoint(NSMidX(*area), NSMidY(*area));
        for (NSScreen *screen in NSScreen.screens) {
            if (NSPointInRect(center, screen.frame))
                return screen;
        }

        /* E.g. on a display that's gone */
        LOG_WARN("macos-quake-area: not on a screen; using the main screen");
    }

    /* The screen with the keyboard focus */
    NSScreen *screen = NSScreen.mainScreen;
    if (screen == nil)
        screen = NSScreen.screens.firstObject;
    if (screen != nil)
        *area = screen.visibleFrame;
    return screen;
}

static NSRect
placement_frame(NSRect area)
{
    const struct macos_quake *s = &quake.settings;
    const CGFloat height = MAX(round(NSHeight(area) * s->height / 100.), 1.);

    /* Without a width, twice the height (as Omarchy's quake console), at most the area's */
    const CGFloat width = s->width > 0.
        ? MAX(round(NSWidth(area) * s->width / 100.), 1.)
        : MIN(2. * height, NSWidth(area));

    NSRect frame = NSMakeRect(
        NSMinX(area) + round((NSWidth(area) - width) / 2.),
        NSMinY(area) + round((NSHeight(area) - height) / 2.),
        width, height);

    switch ((enum quake_position)s->position) {
    case QUAKE_TOP:    frame.origin.y = NSMaxY(area) - height; break;
    case QUAKE_BOTTOM: frame.origin.y = NSMinY(area); break;
    case QUAKE_LEFT:   frame.origin.x = NSMinX(area); break;
    case QUAKE_RIGHT:  frame.origin.x = NSMaxX(area) - width; break;
    case QUAKE_CENTER: break;
    }

    return frame;
}

/* It slides in from its edge of the screen; at the center (or with Reduce Motion), it fades in */
static bool
slides(void)
{
    return quake.settings.position != QUAKE_CENTER &&
        !NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
}

/* Just beyond the screen's edge */
static NSRect
offscreen_frame(NSRect frame, NSScreen *screen)
{
    const NSRect bounds = screen.frame;

    switch ((enum quake_position)quake.settings.position) {
    case QUAKE_TOP:    frame.origin.y = NSMaxY(bounds); break;
    case QUAKE_BOTTOM: frame.origin.y = NSMinY(bounds) - NSHeight(frame); break;
    case QUAKE_LEFT:   frame.origin.x = NSMinX(bounds) - NSWidth(frame); break;
    case QUAKE_RIGHT:  frame.origin.x = NSMaxX(bounds); break;
    case QUAKE_CENTER: break;
    }

    return frame;
}

/*
 * Showing and hiding
 */

static void
animate(NSWindow *window, NSRect frame, CGFloat alpha, bool in, void (^done)(void))
{
    const double duration = in ? quake.settings.animation_in : quake.settings.animation_out;

    if (duration <= 0.) {
        [window setFrame:frame display:YES];
        window.alphaValue = alpha;
        done();
        return;
    }

    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration = duration;

        /* Omarchy's quake console's: easeOutQuint in, easeInOutCubic out */
        context.timingFunction = in
            ? [CAMediaTimingFunction functionWithControlPoints:.23 :1. :.32 :1.]
            : [CAMediaTimingFunction functionWithControlPoints:.65 :.05 :.36 :1.];

        [window.animator setFrame:frame display:YES];
        window.animator.alphaValue = alpha;
    } completionHandler:done];
}

static void
focus(NSWindow *window)
{
    [window makeKeyAndOrderFront:nil];
    if (!NSApp.isActive)
        [NSApp activateIgnoringOtherApps:YES];

    /* Shown on a screen without foot's other windows, it may take another run loop iteration */
    const uint64_t generation = quake.generation;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        if (quake.generation == generation && quake.visible && !window.isKeyWindow)
            [window makeKeyAndOrderFront:nil];
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

/*
 * Dimming (macos-quake-dim): while the terminal has the keyboard focus,
 * the rest of its screen is darkened, as Omarchy's quake console does.
 * A black window, under the terminal and over other windows; clicks go
 * through it.
 */

static NSWindow *
dim_window(void)
{
    if (quake.dim != nil)
        return quake.dim;

    NSPanel *panel = [[NSPanel alloc]
        initWithContentRect:NSZeroRect
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:YES];

    panel.backgroundColor = NSColor.blackColor;
    panel.opaque = NO;
    panel.hasShadow = NO;
    panel.ignoresMouseEvents = YES;
    panel.level = NSFloatingWindowLevel - 1;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorTransient |
                               NSWindowCollectionBehaviorFullScreenAuxiliary |
                               NSWindowCollectionBehaviorIgnoresCycle;
    panel.hidesOnDeactivate = NO;
    panel.canHide = NO;
    panel.releasedWhenClosed = NO;
    panel.excludedFromWindowsMenu = YES;
    panel.animationBehavior = NSWindowAnimationBehaviorNone;

    /* Not a window, to window managers (Accessibility) */
    [panel setAccessibilityElement:NO];

    quake.dim = panel;
    return panel;
}

/* Darkens the screen, or lightens it again; with the terminal's animation */
static void
dim(NSScreen *screen, bool on)
{
    const CGFloat alpha = on ? quake.settings.dim : 0.;
    if (alpha == 0. && quake.dim == nil)
        return;

    NSWindow *window = dim_window();
    if (screen != nil)
        [window setFrame:screen.frame display:NO];

    if (alpha == quake.dim_alpha)
        return;

    const uint64_t generation = ++quake.dim_generation;
    quake.dim_alpha = alpha;

    if (!window.isVisible) {
        window.alphaValue = 0.;
        [window orderFrontRegardless];
    }

    animate(window, window.frame, alpha, on, ^{
        if (quake.dim_generation == generation && alpha == 0.)
            [window orderOut:nil];
    });
}

static void
show(void)
{
    NSWindow *window = quake_window();

    NSRect area;
    NSScreen *screen = placement_area(&area);
    if (screen == nil)
        return;

    const NSRect frame = placement_frame(area);

    /* Re-activated when it's hidden */
    NSRunningApplication *front = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (!window.isKeyWindow && front != nil &&
        ![front isEqual:NSRunningApplication.currentApplication])
    {
        quake.previous = front;
    }

    /* Already shown: the (new) placement, and the keyboard focus */
    if (quake.visible && !quake.sliding_in) {
        [window setFrame:frame display:YES];
        dim(screen, true);
        focus(window);
        return;
    }

    const uint64_t generation = ++quake.generation;
    quake.visible = true;
    quake.sliding_in = true;

    /* From where it is, if it's still being hidden */
    NSRect from = slides() ? offscreen_frame(frame, screen) : frame;
    if (window.isVisible)
        from.origin = window.frame.origin;
    else
        window.alphaValue = slides() ? 1. : 0.;

    /* Above the menu bar, while it slides in (sizes the terminal, too) */
    window.level = NSPopUpMenuWindowLevel;
    [window setFrame:from display:NO];
    [window makeKeyAndOrderFront:nil];

    dim(screen, true);
    animate(window, frame, 1., true, ^{
        if (quake.generation != generation)
            return;

        quake.sliding_in = false;

        /* Below the menu bar (and e.g. input method candidates) */
        window.level = NSFloatingWindowLevel;
        focus(window);
    });
}

/* Back to the application that was active, unless the user went elsewhere already */
static void
restore_previous(NSWindow *window)
{
    NSRunningApplication *previous = quake.previous;
    quake.previous = nil;

    if (!window.isKeyWindow || !NSApp.isActive)
        return;

    if (previous != nil && !previous.terminated) {
        [previous activateWithOptions:0];
        return;
    }

    /* Hiding foot activates another application */
    for (NSWindow *other in NSApp.windows) {
        if (other != window && other.isVisible)
            return;
    }
    [NSApp hide:nil];
}

static void
hide(void)
{
    if (!quake.visible)
        return;

    NSWindow *window = quake_window();

    const uint64_t generation = ++quake.generation;
    quake.visible = false;
    quake.sliding_in = false;

    /* Before it's gone, or macOS activates another window of foot's */
    restore_previous(window);
    dim(nil, false);

    NSScreen *screen = window.screen;
    if (!window.isVisible || screen == nil) {
        [window orderOut:nil];
        return;
    }

    window.level = NSPopUpMenuWindowLevel;

    const bool slide = slides();
    animate(window, slide ? offscreen_frame(window.frame, screen) : window.frame,
            slide ? 1. : 0., false, ^{
        if (quake.generation != generation)
            return;

        [window orderOut:nil];
        window.alphaValue = 1.;
        window.level = NSFloatingWindowLevel;
    });
}

static bool
is_visible(void)
{
    return quake.visible || quake.show_when_configured;
}

bool
quake_is_focused(void)
{
    return quake.term != NULL && !quake.term->shutdown.in_progress &&
        quake.term->window != NULL && quake.term->window->is_configured &&
        quake.visible && quake_window().isKeyWindow;
}

/*
 * macos-quake-alpha: in place of the color themes' alpha
 */

static void
apply_alpha(void)
{
    struct config *conf = quake.conf;
    const float alpha = quake.settings.alpha;

    conf->colors_dark.alpha = alpha >= 0. ? alpha * 65535. : quake.alpha_dark;
    conf->colors_light.alpha = alpha >= 0. ? alpha * 65535. : quake.alpha_light;

    struct terminal *term = quake.term;
    if (term == NULL || term->window == NULL || !term->window->is_configured)
        return;

    const uint16_t theme_alpha = term->colors.active_theme == COLOR_THEME_DARK
        ? conf->colors_dark.alpha : conf->colors_light.alpha;

    if (term->colors.alpha == theme_alpha)
        return;

    term->colors.alpha = theme_alpha;
    wayl_win_alpha_changed(term->window);
    term_font_subpixel_changed(term);
    term_damage_view(term);
    term_damage_margins(term);
    render_refresh(term);
}

void
quake_theme_changed(void)
{
    if (quake.conf == NULL)
        return;

    quake.alpha_dark = quake.conf->colors_dark.alpha;
    quake.alpha_light = quake.conf->colors_light.alpha;
    apply_alpha();
}

/*
 * The window (window.m)
 */

void
quake_window_place(struct wl_window *win)
{
    NSRect area;
    if (placement_area(&area) != nil)
        [(__bridge NSWindow *)win->nswindow setFrame:placement_frame(area) display:NO];
}

void
quake_window_configured(struct wl_window *win)
{
    if (quake.show_when_configured) {
        quake.show_when_configured = false;
        show();
    }
}

void
quake_window_became_key(struct wl_window *win)
{
    /* While it slides in, it's placed (and dims) its screen already */
    if (win->term == quake.term && quake.visible && !quake.sliding_in)
        dim(((__bridge NSWindow *)win->nswindow).screen, true);
}

void
quake_window_resigned_key(struct wl_window *win)
{
    if (win->term != quake.term || !quake.visible || quake.sliding_in)
        return;

    /* E.g. an alert */
    if (((__bridge NSWindow *)win->nswindow).attachedSheet != nil)
        return;

    /* Without auto-hide, it stays, but not the dimming: the user works elsewhere */
    if (quake.settings.auto_hide)
        hide();
    else
        dim(nil, false);
}

static void
reset_state(void)
{
    quake.visible = false;
    quake.sliding_in = false;
    quake.show_when_configured = false;
    quake.previous = nil;
    quake.generation++;
}

void
quake_window_destroyed(struct wl_window *win)
{
    if (win->term != quake.term)
        return;

    if (quake.visible)
        restore_previous((__bridge NSWindow *)win->nswindow);

    dim(nil, false);
    reset_state();
}

/*
 * footclient --quake (server.c)
 */

static void
term_shutdown_handler(void *data, int exit_code)
{
    struct config *conf = data;

    if (conf == quake.conf) {
        quake.conf = NULL;
        quake.term = NULL;
    }

    config_free(conf);
    free(conf);
}

int
wayl_quake(struct wayland *wayl, struct fdm *fdm, struct reaper *reaper,
           const struct config *conf, int action, config_override_t *overrides,
           const char *cwd, int argc, char *const *argv, const char *const *envp)
{
    /* Its shell exited; it's going away (and frees its configuration) */
    if (quake.term != NULL && quake.term->shutdown.in_progress) {
        quake.term = NULL;
        quake.conf = NULL;
        reset_state();
    }

    if (action == CLIENT_QUAKE_STATUS)
        return is_visible() ? CLIENT_QUAKE_VISIBLE : CLIENT_QUAKE_HIDDEN;

    /* The server's configuration, and the footclient's overrides */
    struct config *new_conf = config_clone(conf);
    config_override_apply(new_conf, overrides, false);

    if (quake.term != NULL) {
        quake.settings = new_conf->macos.quake;
        config_free(new_conf);
        free(new_conf);

        apply_alpha();

        if (!quake.term->window->is_configured) {
            /* Not shown yet */
            quake.show_when_configured =
                action == CLIENT_QUAKE_SHOW ||
                (action == CLIENT_QUAKE_TOGGLE && !quake.show_when_configured);
        } else if (action == CLIENT_QUAKE_SHOW ||
                   (action == CLIENT_QUAKE_TOGGLE &&
                    !(quake.visible && quake_window().isKeyWindow)))
        {
            /* Toggled while shown, but not focused: focused */
            show();
        } else
            hide();

        return is_visible() ? CLIENT_QUAKE_VISIBLE : CLIENT_QUAKE_HIDDEN;
    }

    /* A new quake terminal; hidden, it's ready to be shown later */
    quake.conf = new_conf;
    quake.settings = new_conf->macos.quake;
    quake.alpha_dark = new_conf->colors_dark.alpha;
    quake.alpha_light = new_conf->colors_light.alpha;
    quake.show_when_configured = action != CLIENT_QUAKE_HIDE;
    apply_alpha();

    quake.term = term_init(
        new_conf, fdm, reaper, wayl, "foot", cwd, NULL, NULL, argc, argv,
        envp, &term_shutdown_handler, new_conf);

    if (quake.term == NULL) {
        LOG_ERR("failed to instantiate the quake terminal");
        config_free(new_conf);
        free(new_conf);
        quake.conf = NULL;
        quake.show_when_configured = false;
        return -1;
    }

    return is_visible() ? CLIENT_QUAKE_VISIBLE : CLIENT_QUAKE_HIDDEN;
}
