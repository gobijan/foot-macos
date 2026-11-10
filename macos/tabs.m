#import "app.h"
#import "split-view.h"

#define LOG_MODULE "tabs"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "terminal.h"

/*
 * Terminal windows (FootWindow), and their tabs:
 *
 * - Each tab shows the shortcut that selects it (Cmd+1 to Cmd+8, and
 *   Cmd+9 the last one; view.m), as Ghostty's.
 *
 * - With macos-titlebar-style=tabs, the tab bar is in the titlebar, next
 *   to the window's buttons, and the titlebar has the terminal's
 *   background color, as Ghostty's titlebar tabs. There's no API for
 *   that: like Ghostty (TitlebarTabsTahoeTerminalWindow.swift), this
 *   moves AppKit's own (private) views of the titlebar, found by their
 *   class names. macOS 26 and later; before, the tab bar stays below the
 *   titlebar. Should a macOS update change those views, the tab bar may
 *   end up below the titlebar again; nothing else depends on it.
 */

@interface FootWindow : NSWindow <NSToolbarDelegate>
- (instancetype)initWithContentRect:(NSRect)rect titlebarTabs:(BOOL)titlebar_tabs;
- (void)relabel;
- (void)setTitlebarColor:(NSColor *)color;
@end

static NSToolbarItemIdentifier const title_item = @"io.github.gobijan.foot.title";
static NSUserInterfaceItemIdentifier const tab_bar_identifier = @"io.github.gobijan.foot.tab-bar";

/* The window buttons' width, left of the tabs */
#define WINDOW_BUTTONS_WIDTH 70

static void
perform_soon(void (^block)(void))
{
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, block);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

/*
 * Shortcuts
 */

static void
relabel(void)
{
    for (NSWindow *window in NSApp.windows) {
        if ([window isKindOfClass:[FootWindow class]])
            [(FootWindow *)window relabel];
    }
}

/* Once back in the run loop, once (e.g. while the tab bar is laid out) */
static void
relabel_soon(void)
{
    static bool pending;
    if (pending)
        return;

    pending = true;
    perform_soon(^{
        pending = false;
        relabel();
    });
}

/*
 * AppKit's views: the first one of class 'name' in 'view' (itself
 * included), or around it
 */

static NSView *
find_view(NSView *view, NSString *name)
{
    if ([NSStringFromClass([view class]) isEqualToString:name])
        return view;

    for (NSView *subview in view.subviews) {
        NSView *found = find_view(subview, name);
        if (found != nil)
            return found;
    }
    return nil;
}

static NSView *
find_superview(NSView *view, NSString *name)
{
    for (NSView *it = view.superview; it != nil; it = it.superview) {
        if ([NSStringFromClass([it class]) isEqualToString:name])
            return it;
    }
    return nil;
}

/* The window's title without tabs (in the toolbar): it takes no clicks */
@interface FootTitleLabel : NSTextField
@end

@implementation FootTitleLabel

- (NSView *)hitTest:(NSPoint)point
{
    return nil;
}

@end

@implementation FootWindow {
    NSTextField *_shortcut;
    id _shortcutObserver;

    /* macos-titlebar-style=tabs */
    BOOL _titlebarTabs;
    FootTitleLabel *_titleLabel;
    NSColor *_titlebarColor;
    id _tabBarObserver;
    BOOL _hasTabBar;

    /* AppKit's views of the tab bar, as moved (they're put back once it's gone) */
    __weak NSView *_tabBarClip;
    __weak NSView *_tabBarAccessory;
    NSArray<NSLayoutConstraint *> *_tabBarConstraints;
    NSArray<id> *_fullScreenObservers;
}

- (instancetype)initWithContentRect:(NSRect)rect titlebarTabs:(BOOL)titlebar_tabs
{
    self = [super initWithContentRect:rect
                            styleMask:(NSWindowStyleMaskTitled |
                                       NSWindowStyleMaskClosable |
                                       NSWindowStyleMaskMiniaturizable |
                                       NSWindowStyleMaskResizable)
                              backing:NSBackingStoreBuffered
                                defer:NO];
    if (self == nil)
        return nil;

    _shortcut = [NSTextField labelWithString:@""];
    _shortcut.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    [_shortcut setContentCompressionResistancePriority:NSLayoutPriorityWindowSizeStayPut
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    self.tab.accessoryView = _shortcut;

    /*
     * Tabs moved with the mouse: nothing tells, but the tab bar lays its
     * tabs out again (as in Ghostty)
     */
    _shortcut.postsFrameChangedNotifications = YES;
    _shortcutObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_shortcut
                     queue:nil
                usingBlock:^(NSNotification *notification) {
                    relabel_soon();
                }];

    if (@available(macOS 26, *)) {
        if (titlebar_tabs)
            [self setUpTitlebarTabs];
    }

    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:_shortcutObserver];
    if (_tabBarObserver != nil)
        [NSNotificationCenter.defaultCenter removeObserver:_tabBarObserver];
    for (id observer in _fullScreenObservers)
        [NSNotificationCenter.defaultCenter removeObserver:observer];
}

/* Its shortcut, if it's a tab: Cmd+1 to Cmd+8, and Cmd+9 for the last one */
- (void)relabel
{
    NSArray<NSWindow *> *tabs = self.tabbedWindows;
    const NSUInteger index = tabs.count > 1 ? [tabs indexOfObjectIdenticalTo:self] : NSNotFound;

    NSString *text = @"";
    if (index != NSNotFound) {
        if (index < 8)
            text = [NSString stringWithFormat:@"⌘%lu", (unsigned long)index + 1];
        else if (index == tabs.count - 1)
            text = @"⌘9";
    }

    if (![_shortcut.stringValue isEqualToString:text])
        _shortcut.stringValue = text;

    NSColor *color = self.isKeyWindow ? NSColor.labelColor : NSColor.secondaryLabelColor;
    if (_shortcut.textColor != color)
        _shortcut.textColor = color;
}

- (void)becomeKeyWindow
{
    [super becomeKeyWindow];
    relabel_soon();
}

- (void)resignKeyWindow
{
    [super resignKeyWindow];
    relabel_soon();
}

- (void)close
{
    [super close];
    relabel_soon();
}

- (void)mergeAllWindows:(id)sender
{
    [super mergeAllWindows:sender];
    relabel_soon();
}

/*
 * macos-titlebar-style=tabs
 */

- (void)setUpTitlebarTabs API_AVAILABLE(macos(26))
{
    _titlebarTabs = YES;

    /*
     * The tabs have the titles. Without tabs, the toolbar has it: in the
     * middle, as usual (a window's own title would be on the left)
     */
    self.titleVisibility = NSWindowTitleHidden;

    _titleLabel = [FootTitleLabel labelWithString:@"foot"];
    _titleLabel.font = [NSFont titleBarFontOfSize:0];
    _titleLabel.alignment = NSTextAlignmentCenter;
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_titleLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];

    /*
     * Its own: AppKit keeps toolbars with the same identifier alike (e.g.
     * shown or not), and this one isn't shown in full screen without tabs
     */
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:
        [@"io.github.gobijan.foot.toolbar." stringByAppendingString:NSUUID.UUID.UUIDString]];
    toolbar.delegate = self;
    toolbar.centeredItemIdentifiers = [NSSet setWithObject:title_item];
    toolbar.allowsUserCustomization = NO;
    self.toolbar = toolbar;
    self.toolbarStyle = NSWindowToolbarStyleUnifiedCompact;

    /* The titlebar and the terminal are one (same color) */
    self.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;

    /* Before the window changes, so that it doesn't change twice */
    __weak FootWindow *weak_self = self;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    _fullScreenObservers = @[
        [center addObserverForName:NSWindowWillEnterFullScreenNotification
                            object:self queue:nil usingBlock:^(NSNotification *n) {
                                [weak_self updateToolbarInFullScreen:YES];
                            }],
        [center addObserverForName:NSWindowWillExitFullScreenNotification
                            object:self queue:nil usingBlock:^(NSNotification *n) {
                                [weak_self updateToolbarInFullScreen:NO];
                            }],
    ];
}

/*
 * In full screen, the toolbar is there for the tabs only: without them,
 * it'd be a bar with nothing in it. The titlebar is then as usual in full
 * screen (shown with the menu bar), with the window's own title.
 */
- (void)updateToolbar
{
    [self updateToolbarInFullScreen:(self.styleMask & NSWindowStyleMaskFullScreen) != 0];
}

- (void)updateToolbarInFullScreen:(BOOL)full_screen
{
    if (!_titlebarTabs)
        return;

    const BOOL visible = !full_screen || _hasTabBar;

    if (self.toolbar.visible != visible)
        self.toolbar.visible = visible;

    const NSWindowTitleVisibility title = visible ? NSWindowTitleHidden : NSWindowTitleVisible;
    if (self.titleVisibility != title)
        self.titleVisibility = title;
}

- (void)setTitle:(NSString *)title
{
    [super setTitle:title];

    /* Not empty: NSToolbar removes items without a size */
    _titleLabel.stringValue = title.length > 0 ? title : @" ";
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    return @[title_item, NSToolbarFlexibleSpaceItemIdentifier];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    return @[NSToolbarFlexibleSpaceItemIdentifier, title_item, NSToolbarFlexibleSpaceItemIdentifier];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
 willBeInsertedIntoToolbar:(BOOL)flag
{
    if (![identifier isEqualToString:title_item])
        return nil;

    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
    item.view = _titleLabel;
    item.visibilityPriority = NSToolbarItemVisibilityPriorityUser;
    item.enabled = NO;
    item.bordered = NO;     /* No glass around it */
    return item;
}

- (void)becomeMainWindow
{
    [super becomeMainWindow];
    _titleLabel.textColor = NSColor.labelColor;

    /* Only the main window of the tabs has the tab bar */
    if (_titlebarTabs) {
        [self updateToolbar];
        [self setUpTabBar];
        [self updateTitlebar];
    }
}

/* Its titlebar is there once it's shown */
- (void)orderWindow:(NSWindowOrderingMode)place relativeTo:(NSInteger)other
{
    [super orderWindow:place relativeTo:other];

    if (_titlebarTabs && place != NSWindowOut) {
        __weak FootWindow *weak_self = self;
        perform_soon(^{
            [weak_self updateTitlebar];
        });
    }
}

- (void)resignMainWindow
{
    [super resignMainWindow];
    _titleLabel.textColor = NSColor.secondaryLabelColor;
}

/* The view in the titlebar (out of the window, in full screen) */
- (NSView *)titlebarView
{
    NSView *frame = self.contentView;
    while (frame.superview != nil)
        frame = frame.superview;

    if (frame == nil || ![frame respondsToSelector:NSSelectorFromString(@"titlebarView")])
        return nil;

    id view = [frame valueForKey:@"titlebarView"];
    return [view isKindOfClass:[NSView class]] ? view : nil;
}

- (BOOL)isTabBar:(NSTitlebarAccessoryViewController *)controller
{
    if (controller.identifier != nil)
        return [controller.identifier isEqualToString:tab_bar_identifier];

    if (find_view(controller.view, @"NSTabBar") != nil)
        return YES;

    /* A new tab's: empty at first, the tab bar comes later */
    return controller.layoutAttribute == NSLayoutAttributeBottom &&
        [NSStringFromClass([controller.view class]) isEqualToString:@"NSView"] &&
        controller.view.subviews.count == 0;
}

/* AppKit adds the tab bar as an accessory, below the titlebar */
- (void)addTitlebarAccessoryViewController:(NSTitlebarAccessoryViewController *)controller
{
    if (!_titlebarTabs || ![self isTabBar:controller]) {
        [super addTitlebarAccessoryViewController:controller];
        return;
    }

    controller.identifier = tab_bar_identifier;
    [self forgetTabBar];

    /* In full screen, the toolbar is back for it */
    _hasTabBar = YES;
    [self updateToolbar];

    /* Before it's added (an exception otherwise) */
    controller.layoutAttribute = NSLayoutAttributeRight;
    [super addTitlebarAccessoryViewController:controller];

    /* Once it's laid out */
    __weak FootWindow *weak_self = self;
    perform_soon(^{
        [weak_self setUpTabBar];
    });
}

- (void)removeTitlebarAccessoryViewControllerAtIndex:(NSInteger)index
{
    NSArray<NSTitlebarAccessoryViewController *> *controllers = self.titlebarAccessoryViewControllers;
    const BOOL tab_bar = _titlebarTabs && index >= 0 && (NSUInteger)index < controllers.count &&
        [self isTabBar:controllers[index]];

    /* AppKit's views as they were (they may stay, e.g. in full screen) */
    if (tab_bar)
        [self restoreTabBarViews];

    [super removeTitlebarAccessoryViewControllerAtIndex:index];

    if (tab_bar) {
        [self forgetTabBar];
        _hasTabBar = NO;
        _titleLabel.alphaValue = 1.;
        [self updateToolbar];
        [self updateTitlebar];
    }
}

- (void)restoreTabBarViews
{
    [NSLayoutConstraint deactivateConstraints:_tabBarConstraints];
    _tabBarConstraints = nil;

    _tabBarClip.translatesAutoresizingMaskIntoConstraints = YES;
    _tabBarAccessory.translatesAutoresizingMaskIntoConstraints = YES;
    _tabBarClip = nil;
    _tabBarAccessory = nil;
}

- (void)forgetTabBar
{
    if (_tabBarObserver != nil)
        [NSNotificationCenter.defaultCenter removeObserver:_tabBarObserver];
    _tabBarObserver = nil;
}

/*
 * The tab bar, moved into the titlebar: its container fills the
 * toolbar, right of the window buttons. Again whenever AppKit lays it
 * out anew (e.g. the appearance changed), which may undo it.
 */
- (void)setUpTabBar
{
    if (!_titlebarTabs || _tabBarObserver != nil)
        return;

    NSView *titlebar = [self titlebarView];
    NSView *tab_bar = titlebar != nil ? find_view(titlebar, @"NSTabBar") : nil;
    if (tab_bar == nil)
        return;

    _titleLabel.alphaValue = 0.;

    /* macOS 26: a clip view; 27: a container view */
    NSView *clip = find_superview(tab_bar, @"NSTitlebarAccessoryClipView");
    if (clip == nil)
        clip = find_superview(tab_bar, @"NSTitlebarAccessoryContainerView");
    NSView *accessory = clip.subviews.firstObject;
    NSView *toolbar = find_view(titlebar, @"NSToolbarView");
    if (clip == nil || accessory == nil || toolbar == nil) {
        LOG_WARN("titlebar tabs: AppKit's titlebar isn't as expected");
        return;
    }

    /* As high as its new tab button (else it's stretched) */
    NSView *new_tab = find_view(titlebar, @"NSTabBarNewTabButton");
    if (new_tab != nil)
        [tab_bar setFrameSize:NSMakeSize(NSWidth(tab_bar.frame), NSWidth(new_tab.frame))];

    [self restoreTabBarViews];

    clip.translatesAutoresizingMaskIntoConstraints = NO;
    accessory.translatesAutoresizingMaskIntoConstraints = NO;
    _tabBarClip = clip;
    _tabBarAccessory = accessory;

    _tabBarConstraints = @[
        [clip.leftAnchor constraintEqualToAnchor:toolbar.leftAnchor constant:WINDOW_BUTTONS_WIDTH],
        [clip.rightAnchor constraintEqualToAnchor:toolbar.rightAnchor],
        [clip.topAnchor constraintEqualToAnchor:toolbar.topAnchor constant:2],
        [clip.heightAnchor constraintEqualToAnchor:toolbar.heightAnchor],
        [accessory.leftAnchor constraintEqualToAnchor:clip.leftAnchor],
        [accessory.rightAnchor constraintEqualToAnchor:clip.rightAnchor],
        [accessory.topAnchor constraintEqualToAnchor:clip.topAnchor],
        [accessory.heightAnchor constraintEqualToAnchor:clip.heightAnchor],
    ];
    [NSLayoutConstraint activateConstraints:_tabBarConstraints];

    clip.needsLayout = YES;
    accessory.needsLayout = YES;

    [self updateTitlebar];

    __weak FootWindow *weak_self = self;
    tab_bar.postsFrameChangedNotifications = YES;
    _tabBarObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:NSViewFrameDidChangeNotification
                    object:tab_bar
                     queue:nil
                usingBlock:^(NSNotification *notification) {
                    [weak_self forgetTabBar];
                    perform_soon(^{
                        [weak_self setUpTabBar];
                    });
                }];
}

/* The titlebar has the terminal's background color, and appearance */
- (void)setTitlebarColor:(NSColor *)color
{
    if (!_titlebarTabs || [color isEqual:_titlebarColor])
        return;

    _titlebarColor = color;

    CGFloat r, g, b, a;
    [[color colorUsingColorSpace:NSColorSpace.sRGBColorSpace] getRed:&r green:&g blue:&b alpha:&a];
    const BOOL light = .299 * r + .587 * g + .114 * b > .5;
    self.appearance = [NSAppearance appearanceNamed:
        light ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua];

    [self updateTitlebar];
}

- (void)updateTitlebar
{
    if (!_titlebarTabs || _titlebarColor == nil)
        return;

    NSView *titlebar = [self titlebarView];
    if (titlebar == nil)
        return;

    titlebar.wantsLayer = YES;
    titlebar.layer.backgroundColor = _titlebarColor.CGColor;

    NSView *container = find_superview(titlebar, @"NSTitlebarContainerView");
    if (container == nil)
        container = titlebar;

    /* It has a color of its own */
    find_view(container, @"NSTitlebarBackgroundView").hidden = YES;

    /* macOS 27: the tab bar's glass is less opaque (AppKit resets it, e.g. with a new tab) */
    if (@available(macOS 27, *)) {
        __weak NSView *weak_container = container;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            NSView *glass = find_view(weak_container, @"NSSubduedGlassEffectView");
            glass.subviews.firstObject.alphaValue = .5;
        });
    }
}

@end

/*
 * Interface (app.h)
 */

NSWindow *
tabs_window_create(NSRect content_rect, bool titlebar_tabs)
{
    return [[FootWindow alloc] initWithContentRect:content_rect titlebarTabs:titlebar_tabs];
}

void
tabs_window_colors_changed(struct wl_window *win)
{
    NSWindow *window = (__bridge NSWindow *)win->nswindow;
    FootSplitView *splits = (__bridge FootSplitView *)win->splits;
    if (![window isKindOfClass:[FootWindow class]] || splits == nil || splits.focused != win)
        return;

    const struct terminal *term = win->term;
    const uint32_t bg = term->colors.bg;
    const bool transparent = term->colors.alpha < 0xffff && !win->is_fullscreen;

    [(FootWindow *)window setTitlebarColor:[NSColor
        colorWithSRGBRed:((bg >> 16) & 0xff) / 255.
                   green:((bg >> 8) & 0xff) / 255.
                    blue:(bg & 0xff) / 255.
                   alpha:transparent ? term->colors.alpha / 65535. : 1.]];
}
