#import "app.h"
#import "view.h"

#import <QuartzCore/QuartzCore.h>

#include <math.h>

#include <tllist.h>

#define LOG_MODULE "window"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "debug.h"
#include "fdm.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "shm.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

/*
 * A terminal window: an NSWindow, with a FootView as its content view.
 *
 * Like a Wayland toplevel, the window is "configured" by the platform:
 * once at startup (choosing the initial size), and then whenever its
 * size, scale, focus or state (fullscreen etc) changes.
 */

@interface FootWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic) struct wl_window *win;
@end

static FootView *
view_of(const struct wl_window *win)
{
    return (__bridge FootView *)win->view;
}

static NSWindow *
nswindow_of(const struct wl_window *win)
{
    return (__bridge NSWindow *)win->nswindow;
}

static CALayer *
layer_of(const struct wayl_surface *surf)
{
    return (__bridge CALayer *)surf->layer;
}

/*
 * Surfaces
 */

void
wayl_surface_attach(struct wayl_surface *surf, struct buffer *buf)
{
    surf->buf = buf;
    surf->has_pending = true;
}

void
wayl_surface_damage(struct wayl_surface *surf, int x, int y, int width, int height)
{
    /* The window server composites whole layers */
}

void
wayl_surface_commit(struct wayl_surface *surf)
{
    if (!surf->has_pending)
        return;

    surf->has_pending = false;

    CALayer *layer = layer_of(surf);
    if (layer == nil)
        return;

    struct buffer *buf = surf->buf;
    struct buffer *old = surf->committed;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];

    if (buf != NULL) {
        const float scale = surf->scale > 0. ? surf->scale : 1.;

        /* Publishes our writes to the window server */
        shm_buffer_attached(buf);

        layer.contents = (__bridge id)buf->iosurface;
        layer.contentsScale = scale;
        layer.backgroundColor = NULL;
        layer.bounds = CGRectMake(0, 0, buf->width / scale, buf->height / scale);
        layer.hidden = NO;
    } else {
        layer.contents = nil;
        layer.backgroundColor = NULL;
        layer.hidden = YES;
    }

    [CATransaction commit];

    if (old != NULL && old != buf)
        shm_buffer_detached(old);
    surf->committed = buf;
}

void
wayl_surface_set_opaque(struct wayl_surface *surf, bool opaque)
{
    CALayer *layer = layer_of(surf);
    if (layer != nil && layer.opaque != opaque) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.opaque = opaque;
        [CATransaction commit];
    }
}

void
wayl_surface_scale(const struct wl_window *win, struct wayl_surface *surf,
                   const struct buffer *buf, float scale)
{
    surf->scale = scale;
}

void
wayl_surface_scale_explicit_width_height(
    const struct wl_window *win, struct wayl_surface *surf,
    int width, int height, float scale)
{
    surf->scale = scale;
}

void
wayl_win_scale(struct wl_window *win, const struct buffer *buf)
{
    wayl_surface_scale(win, &win->surface, buf, win->term->scale);
}

void
wayl_sub_surface_set_position(struct wayl_sub_surface *sub, int x, int y)
{
    CALayer *layer = layer_of(&sub->surface);
    if (layer == nil)
        return;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.position = CGPointMake(x, y);
    [CATransaction commit];
}

void
wayl_sub_surface_set_color(struct wayl_sub_surface *sub, uint32_t argb,
                           int width, int height)
{
    struct wayl_surface *surf = &sub->surface;
    CALayer *layer = layer_of(surf);
    if (layer == nil)
        return;

    CGColorRef color = CGColorCreateSRGB(
        ((argb >> 16) & 0xff) / 255.,
        ((argb >> 8) & 0xff) / 255.,
        (argb & 0xff) / 255.,
        ((argb >> 24) & 0xff) / 255.);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = nil;
    layer.backgroundColor = color;
    layer.position = CGPointZero;
    layer.bounds = CGRectMake(0, 0, width, height);
    layer.hidden = NO;
    [CATransaction commit];

    CGColorRelease(color);

    if (surf->committed != NULL)
        shm_buffer_detached(surf->committed);
    surf->committed = NULL;
    surf->buf = NULL;
    surf->has_pending = false;
}

static void
surface_init(struct wl_window *win, struct wayl_surface *surf, CALayer *layer)
{
    *surf = (struct wayl_surface){
        .layer = (void *)CFBridgingRetain(layer),
        .scale = win->scale > 0. ? win->scale : 1.,
    };
}

static void
surface_destroy(struct wayl_surface *surf)
{
    if (surf->layer == NULL)
        return;

    CALayer *layer = CFBridgingRelease(surf->layer);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [layer removeFromSuperlayer];
    layer.contents = nil;
    [CATransaction commit];

    if (surf->committed != NULL)
        shm_buffer_detached(surf->committed);

    *surf = (struct wayl_surface){0};
}

bool
wayl_win_subsurface_new_with_custom_parent(
    struct wl_window *win, struct wayl_surface *parent,
    struct wayl_sub_surface *sub, bool allow_pointer_input)
{
    CALayer *layer = view_create_layer();

    /* Stacked above the main surface, and previously created sub-surfaces */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [view_of(win).rootLayer addSublayer:layer];
    [CATransaction commit];

    surface_init(win, &sub->surface, layer);
    return true;
}

bool
wayl_win_subsurface_new(struct wl_window *win, struct wayl_sub_surface *sub,
                        bool allow_pointer_input)
{
    return wayl_win_subsurface_new_with_custom_parent(
        win, &win->surface, sub, allow_pointer_input);
}

void
wayl_win_subsurface_destroy(struct wayl_sub_surface *sub)
{
    if (sub == NULL)
        return;
    surface_destroy(&sub->surface);
}

/*
 * Window state
 */

static void
update_root_background(struct wl_window *win)
{
    /* Visible around the grid, e.g. while resizing */
    const struct terminal *term = win->term;
    const uint32_t bg = term->colors.bg;
    const bool transparent = term->colors.alpha < 0xffff && !win->is_fullscreen;

    CGColorRef color = CGColorCreateSRGB(
        ((bg >> 16) & 0xff) / 255., ((bg >> 8) & 0xff) / 255., (bg & 0xff) / 255.,
        transparent ? term->colors.alpha / 65535. : 1.);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view_of(win).rootLayer.backgroundColor = color;
    [CATransaction commit];

    CGColorRelease(color);
}

void
wayl_win_alpha_changed(struct wl_window *win)
{
    NSWindow *nswindow = nswindow_of(win);
    const bool transparent =
        win->term->colors.alpha < 0xffff && !win->is_fullscreen;

    nswindow.opaque = !transparent;
    nswindow.backgroundColor = transparent
        ? [NSColor clearColor] : [NSColor windowBackgroundColor];

    wayl_surface_set_opaque(&win->surface, !transparent);
    update_root_background(win);
}

void
window_update_outputs(struct wl_window *win)
{
    tll_free(win->on_outputs);

    const struct monitor *mon = app_monitor_for_screen(nswindow_of(win).screen);
    if (mon != NULL)
        tll_push_back(win->on_outputs, mon);
}

/* Scale or monitor changed; as update_term_for_output_change() upstream */
static void
output_changed(struct wl_window *win)
{
    struct terminal *term = win->term;

    win->scale = nswindow_of(win).backingScaleFactor;
    window_update_outputs(win);

    const float old_scale = term->scale;
    const float logical_width = term->width / old_scale;
    const float logical_height = term->height / old_scale;

    /* Note: order matters! term_update_scale() must come first */
    bool scale_updated = term_update_scale(term);
    bool fonts_updated = term_font_dpi_changed(term, old_scale);
    term_font_subpixel_changed(term);

    if (!win->is_configured)
        return;

    enum resize_options resize_opts = RESIZE_KEEP_GRID;

    if (fonts_updated)
        resize_opts |= RESIZE_FORCE;
    else if (!scale_updated)
        return;
    else if (term->conf->dpi_aware)
        resize_opts |= RESIZE_FORCE;

    render_resize(
        term, (int)roundf(logical_width), (int)roundf(logical_height), resize_opts);
}

void
window_backing_changed(struct wl_window *win)
{
    /* Before the initial configure (e.g. during wayl_win_init()), the
     * terminal isn't ready; the initial configure takes care of it */
    if (!win->is_configured)
        return;

    output_changed(win);
    app_run_hooks();
}

void
window_resized(struct wl_window *win)
{
    if (!win->is_configured)
        return;

    struct terminal *term = win->term;
    const NSSize size = view_of(win).bounds.size;

    NSWindow *nswindow = nswindow_of(win);

    /* A new tab of a full screen window is one, without entering it */
    const bool fullscreen = (nswindow.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (fullscreen != win->is_fullscreen) {
        win->is_fullscreen = fullscreen;
        wayl_win_alpha_changed(win);
    }

    win->is_maximized = nswindow.isZoomed && !win->is_fullscreen;

    render_resize(term, (int)size.width, (int)size.height, RESIZE_BY_CELLS);

    /* Render right away, to avoid stretched/stale contents while resizing */
    app_run_hooks();
}

/*
 * The cell size changed (font size, scale), and the terminal kept its
 * grid (resize-keep-grid): like a Wayland compositor, the window adopts
 * the terminal's new size. Within the screen; if it can't (or a window
 * manager resizes it back), the grid follows the window instead.
 */
static void
window_fit(struct wl_window *win)
{
    struct terminal *term = win->term;
    NSWindow *nswindow = nswindow_of(win);

    /* Tabs share their window's size */
    if (!win->is_fullscreen && !win->is_maximized && !win->is_tiled &&
        !win->is_resizing && nswindow.tabbedWindows.count <= 1)
    {
        const NSRect old = nswindow.frame;
        NSRect frame = [nswindow frameRectForContentRect:NSMakeRect(
            0, 0, term->width / term->scale, term->height / term->scale)];

        /* Keep the top left corner, but stay on the screen */
        const NSRect visible = nswindow.screen != nil
            ? nswindow.screen.visibleFrame : old;
        frame.size.width = MIN(NSWidth(frame), NSWidth(visible));
        frame.size.height = MIN(NSHeight(frame), NSHeight(visible));
        frame.origin.x = MAX(
            MIN(NSMinX(old), NSMaxX(visible) - NSWidth(frame)), NSMinX(visible));
        frame.origin.y = MIN(
            MAX(NSMaxY(old) - NSHeight(frame), NSMinY(visible)),
            NSMaxY(visible) - NSHeight(frame));

        if (!NSEqualRects(frame, old))
            [nswindow setFrame:frame display:YES];
    }

    /* Whatever size the window ended up with, the grid must fit it */
    window_resized(win);
}

/*
 * The initial configure; picks the window size, and shows the window.
 * As a tab of 'tab_parent', if any: its size, not the configuration's.
 */
static void
window_configure(struct wl_window *win, NSWindow *tab_parent)
{
    struct terminal *term = win->term;
    NSWindow *nswindow = nswindow_of(win);

    /* The quake terminal's size (and screen) is its placement's */
    if (win->quake)
        quake_window_place(win);

    output_changed(win);

    win->is_configured = true;

    if (win->quake) {
        const NSSize size = view_of(win).bounds.size;
        render_resize(term, (int)size.width, (int)size.height, RESIZE_BY_CELLS);
        app_apply_color_theme(term, false);
        quake_window_configured(win);

        term_window_configured(term);
        app_run_hooks();
        return;
    }

    /* Width/height 0: foot picks the size (from the configuration) */
    render_resize(term, 0, 0, RESIZE_BY_CELLS);

    /* The color theme, before the first frame */
    app_apply_color_theme(term, false);

    /* Before it's shown: as a tab of a full screen window, that resizes it */
    term_window_configured(term);

    const NSSize size = NSMakeSize(term->width / term->scale, term->height / term->scale);
    [nswindow setContentSize:size];

    /* Cascade from the key window, if any */
    NSWindow *key = NSApp.keyWindow;
    if (tab_parent != nil) {
        [nswindow setFrame:tab_parent.frame display:NO];
        [tab_parent addTabbedWindow:nswindow ordered:NSWindowAbove];
    } else if (key != nil && key != nswindow) {
        NSPoint top_left = NSMakePoint(NSMinX(key.frame), NSMaxY(key.frame));
        [nswindow cascadeTopLeftFromPoint:top_left];
        [nswindow cascadeTopLeftFromPoint:[nswindow cascadeTopLeftFromPoint:top_left]];
    } else
        [nswindow center];

    app_windows_changed();
    [nswindow makeKeyAndOrderFront:nil];

    app_run_hooks();
}

/*
 * Window API
 */

struct wl_window *
wayl_win_init(struct terminal *term, const char *token)
{
    const struct config *conf = term->conf;

    struct wl_window *win = xcalloc(1, sizeof(*win));
    win->term = term;
    win->csd_mode = CSD_NO;
    win->csd.move_timeout_fd = -1;
    win->resize_timeout_fd = -1;
    win->wm_capabilities.maximize = true;
    win->wm_capabilities.minimize = true;

    /* The real size is chosen in the initial configure */
    const NSRect rect = NSMakeRect(0, 0, 640, 400);

    /* The quake terminal's size is fixed, like a tiled window's */
    win->quake = quake_owns_config(conf);
    win->is_tiled = win->quake;

    NSWindow *nswindow;
    if (win->quake)
        nswindow = quake_window_create(rect);
    else {
        nswindow = [[NSWindow alloc]
            initWithContentRect:rect
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable |
                                 NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];

        nswindow.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    }

    nswindow.releasedWhenClosed = NO;
    nswindow.title = @"foot";
    win->scale = nswindow.backingScaleFactor;

    FootView *view = [[FootView alloc] initWithFrame:rect window:win];
    nswindow.contentView = view;
    [nswindow makeFirstResponder:view];

    FootWindowDelegate *delegate = [[FootWindowDelegate alloc] init];
    delegate.win = win;
    nswindow.delegate = delegate;

    win->nswindow = (void *)CFBridgingRetain(nswindow);
    win->delegate = (void *)CFBridgingRetain(delegate);
    win->view = (__bridge void *)view;

    surface_init(win, &win->surface, view.gridLayer);
    wayl_win_alpha_changed(win);

    if (!wayl_win_subsurface_new(win, &win->overlay, false)) {
        LOG_ERR("failed to create overlay surface");
        wayl_win_destroy(win);
        return NULL;
    }

    switch (conf->tweak.render_timer) {
    case RENDER_TIMER_OSD:
    case RENDER_TIMER_BOTH:
        if (!wayl_win_subsurface_new(win, &win->render_timer, false)) {
            LOG_ERR("failed to create render timer surface");
            wayl_win_destroy(win);
            return NULL;
        }
        break;

    case RENDER_TIMER_NONE:
    case RENDER_TIMER_LOG:
        break;
    }

    /*
     * Configure (size, and show) once the terminal is fully
     * initialized, i.e. after we've returned to the run loop. Like a
     * Wayland compositor's first configure event. In the run loop's
     * common modes, like FDM events (see wayl_init()).
     */
    __weak NSWindow *weak_window = nswindow;
    __weak NSWindow *weak_parent = app_new_tab_parent();
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        NSWindow *w = weak_window;
        FootWindowDelegate *d = w != nil ? w.delegate : nil;
        if (d != nil && d.win != NULL && !d.win->is_configured)
            window_configure(d.win, weak_parent);
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());

    return win;
}

void
wayl_win_destroy(struct wl_window *win)
{
    if (win == NULL)
        return;

    struct terminal *term = win->term;

    /*
     * Drop input focus (Wayland: the compositor's leave events). The
     * terminal is going away (its PTY may already be closed); reset
     * the seat's state, without notifying the client.
     */
    tll_foreach(term->wl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->kbd_focus == term) {
            ime_leave(seat);
            seat->kbd_focus = NULL;
            input_kbd_leave(seat);
        }
        if (seat->mouse_focus == term)
            input_pointer_leave(seat);
    }

    tll_foreach(win->urls, it) {
        wayl_win_subsurface_destroy(&it->item.surf);
        tll_remove(win->urls, it);
    }

    render_wait_for_preapply_damage(term);

    for (size_t i = 0; i < ALEN(win->csd.surface); i++)
        wayl_win_subsurface_destroy(&win->csd.surface[i]);
    wayl_win_subsurface_destroy(&win->search);
    wayl_win_subsurface_destroy(&win->scrollback_indicator);
    wayl_win_subsurface_destroy(&win->render_timer);
    wayl_win_subsurface_destroy(&win->overlay);
    surface_destroy(&win->surface);

    shm_purge(term->render.chains.search);
    shm_purge(term->render.chains.scrollback_indicator);
    shm_purge(term->render.chains.render_timer);
    shm_purge(term->render.chains.grid);
    shm_purge(term->render.chains.url);
    shm_purge(term->render.chains.csd);

    tll_free(win->on_outputs);

    if (win->view != NULL)
        [view_of(win) invalidate];

    if (win->delegate != NULL) {
        FootWindowDelegate *delegate = CFBridgingRelease(win->delegate);
        delegate.win = NULL;
    }

    if (win->quake)
        quake_window_destroyed(win);

    if (win->nswindow != NULL) {
        NSWindow *nswindow = CFBridgingRelease(win->nswindow);
        win->nswindow = NULL;
        nswindow.delegate = nil;

        /* E.g. the shell exited, while asking whether to close the window */
        if (nswindow.attachedSheet != nil)
            [nswindow endSheet:nswindow.attachedSheet];

        /* Ordering out a tab hides all of them */
        [nswindow.tabGroup removeWindow:nswindow];

        [nswindow orderOut:nil];
        [nswindow close];
    }

    if (!win->quake)
        app_windows_changed();

    if (win->resize_timeout_fd >= 0)
        fdm_del(term->wl->fdm, win->resize_timeout_fd);

    free(win);
}

void
wayl_win_request_frame(struct wl_window *win)
{
    win->frame_callback = true;
    [view_of(win) requestFrame];
}

void
wayl_win_set_title(struct wl_window *win, const char *title)
{
    NSString *str = [NSString stringWithUTF8String:title];
    nswindow_of(win).title = str != nil ? str : @"foot";
}

void
wayl_win_set_app_id(struct wl_window *win, const char *app_id)
{
    /* Windows are identified by the application bundle */
}

void
wayl_win_set_fullscreen(struct wl_window *win, bool fullscreen)
{
    /* initial-window-mode: a new tab is what its window is */
    if (win->quake || app_new_tab_parent() != nil)
        return;

    NSWindow *nswindow = nswindow_of(win);
    const bool is_fullscreen =
        (nswindow.styleMask & NSWindowStyleMaskFullScreen) != 0;

    if (fullscreen != is_fullscreen)
        [nswindow toggleFullScreen:nil];
}

void
wayl_win_set_maximized(struct wl_window *win, bool maximized)
{
    if (win->quake || app_new_tab_parent() != nil)
        return;

    NSWindow *nswindow = nswindow_of(win);
    if (maximized != nswindow.isZoomed)
        [nswindow zoom:nil];
}

void
wayl_win_minimize(struct wl_window *win)
{
    if (!win->quake)
        [nswindow_of(win) miniaturize:nil];
}

void
wayl_win_close(struct wl_window *win)
{
    term_shutdown(win->term);
}

void
wayl_win_set_content_size(struct wl_window *win, int width, int height)
{
    if (!win->quake)
        [nswindow_of(win) setContentSize:NSMakeSize(width, height)];
}

void
wayl_win_set_resize_increments(struct wl_window *win, int cell_width, int cell_height)
{
    NSWindow *nswindow = nswindow_of(win);
    const struct config *conf = win->term->conf;

    /* Not contentResizeIncrements: AppKit applies those to tiled
     * windows too, rounding up (overlapping the neighbor). See
     * windowWillResize:toSize: instead */
    nswindow.contentMinSize = NSMakeSize(
        cell_width + conf->pad_left + conf->pad_right,
        cell_height + conf->pad_top + conf->pad_bottom);

    /* A new cell size; not the initial one (see window_configure()) */
    const struct terminal *term = win->term;
    const bool changed = win->cell_width != 0 &&
        (term->cell_width != win->cell_width ||
         term->cell_height != win->cell_height);

    win->cell_width = term->cell_width;
    win->cell_height = term->cell_height;

    if (!changed)
        return;

    /* We're in render_resize(); fit once back in the run loop */
    __weak NSWindow *weak_window = nswindow;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        NSWindow *w = weak_window;
        FootWindowDelegate *d = w != nil ? w.delegate : nil;
        if (d != nil && d.win != NULL)
            window_fit(d.win);
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

bool
wayl_win_set_urgent(struct wl_window *win)
{
    if (!NSApp.isActive)
        [NSApp requestUserAttention:NSInformationalRequest];
    return true;
}

bool
wayl_win_ring_bell(const struct wl_window *win)
{
    NSBeep();
    return true;
}

bool
wayl_win_csd_titlebar_visible(const struct wl_window *win)
{
    return false;
}

bool
wayl_win_csd_borders_visible(const struct wl_window *win)
{
    return false;
}

void
wayl_win_ime_reset(struct wl_window *win)
{
    [view_of(win) resetInputMethod];
}

void
wayl_win_ime_cursor_rect_changed(struct wl_window *win)
{
    [view_of(win).inputContext invalidateCharacterCoordinates];
}

/*
 * Window events
 */

@implementation FootWindowDelegate

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    /* The terminal shuts down, and then destroys the window */
    if (_win != NULL && sender.attachedSheet == nil)
        window_close(_win);
    app_run_hooks();
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification *)notification
{
    struct wl_window *win = _win;
    struct seat *seat = app_seat();
    if (win == NULL || seat == NULL)
        return;

    struct terminal *term = win->term;

    win->is_activated = true;
    term_visual_focus_in(term);

    input_kbd_enter(seat, term, app_next_serial());
    ime_enter(seat, term);

    /* Modifiers may have changed while we were unfocused */
    uint32_t depressed, locked;
    keyboard_modifiers(seat, [NSEvent modifierFlags], app_option_as_alt(), &depressed, &locked);
    input_modifiers(seat, depressed, 0, locked, 0);

    if (win->quake)
        quake_window_became_key(win);

    app_run_hooks();
}

- (void)windowDidResignKey:(NSNotification *)notification
{
    struct wl_window *win = _win;
    struct seat *seat = app_seat();
    if (win == NULL || seat == NULL)
        return;

    struct terminal *term = win->term;

    if (seat->kbd_focus == term) {
        ime_leave(seat);
        input_kbd_leave(seat);
    }

    win->is_activated = false;
    term_visual_focus_out(term);

    if (win->quake)
        quake_window_resigned_key(win);

    app_run_hooks();
}

/*
 * resize-by-cells: the user resizes the window in whole cells. Only
 * while resizing it with the mouse; tiled, zoomed etc windows get the
 * exact size they're given. Never larger than proposed.
 */
- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)size
{
    struct wl_window *win = _win;
    if (win == NULL || !sender.inLiveResize)
        return size;

    const struct terminal *term = win->term;
    const struct config *conf = term->conf;
    if (!conf->resize_by_cells || term->cell_width == 0 || term->cell_height == 0)
        return size;

    /* In pixels, as render_resize() */
    const float scale = term->scale;
    const int pad_x = scale * conf->pad_left + scale * conf->pad_right;
    const int pad_y = scale * conf->pad_top + scale * conf->pad_bottom;

    NSRect content = [sender contentRectForFrameRect:
        NSMakeRect(0, 0, size.width, size.height)];

    const int width = roundf(NSWidth(content) * scale);
    const int height = roundf(NSHeight(content) * scale);
    const int cols = MAX((width - pad_x) / term->cell_width, 1);
    const int rows = MAX((height - pad_y) / term->cell_height, 1);

    /* Whole points: the grid is sized from the view's (integral) size */
    content.size = NSMakeSize(
        ceilf((cols * term->cell_width + pad_x) / scale),
        ceilf((rows * term->cell_height + pad_y) / scale));

    return [sender frameRectForContentRect:content].size;
}

- (void)windowWillStartLiveResize:(NSNotification *)notification
{
    if (_win != NULL)
        _win->is_resizing = true;
}

- (void)windowDidEndLiveResize:(NSNotification *)notification
{
    struct wl_window *win = _win;
    if (win == NULL)
        return;

    win->is_resizing = false;

    /* Send the final size to the client right away */
    const NSSize size = view_of(win).bounds.size;
    render_resize(win->term, (int)size.width, (int)size.height,
                  RESIZE_BY_CELLS | RESIZE_FORCE);
    app_run_hooks();
}

- (void)windowDidChangeScreen:(NSNotification *)notification
{
    if (_win != NULL && _win->is_configured) {
        output_changed(_win);
        app_run_hooks();
    }
}

- (void)windowWillEnterFullScreen:(NSNotification *)notification
{
    struct wl_window *win = _win;
    if (win == NULL)
        return;

    win->is_fullscreen = true;
    wayl_win_alpha_changed(win);
    render_refresh(win->term);
}

- (void)windowDidExitFullScreen:(NSNotification *)notification
{
    struct wl_window *win = _win;
    if (win == NULL)
        return;

    win->is_fullscreen = false;
    wayl_win_alpha_changed(win);
    window_resized(win);
    render_refresh(win->term);
    app_run_hooks();
}

- (void)windowDidMiniaturize:(NSNotification *)notification
{
    struct wl_window *win = _win;
    if (win == NULL)
        return;

    win->is_suspended = true;
    if (win->term->visibility_reports)
        term_send_visibility_report(win->term);
}

- (void)windowDidDeminiaturize:(NSNotification *)notification
{
    struct wl_window *win = _win;
    if (win == NULL)
        return;

    win->is_suspended = false;
    if (win->term->visibility_reports)
        term_send_visibility_report(win->term);
}

- (void)windowDidChangeOcclusionState:(NSNotification *)notification
{
    struct wl_window *win = _win;
    if (win == NULL)
        return;

    /* Hidden windows don't get frame callbacks (and don't render) */
    NSWindow *nswindow = notification.object;
    const BOOL visible = (nswindow.occlusionState & NSWindowOcclusionStateVisible) != 0;
    [view_of(win) setOccluded:!visible];
}

@end
