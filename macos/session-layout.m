/*
 * The windows' layout, for persistent sessions (macos/session.h).
 *
 * footd keeps it, for the next foot: which sessions are in which
 * windows, and where. The windows (with their tabs, and their splits'
 * trees, frames and focus) and the quake terminal. It's sent whenever
 * it changed (once per run loop iteration, at most), and when foot
 * quits.
 *
 * When foot starts, the sessions no window is attached to are restored
 * that way: their windows are opened, split and placed, then they're
 * attached (with the windows' final sizes: they're held until then).
 * Sessions that aren't in the layout get windows of their own.
 */

#import "app.h"
#import "session-layout.h"
#import "split-view.h"
#import "view.h"

#include <tllist.h>

#define LOG_MODULE "layout"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "fdm.h"
#include "session.h"
#include "terminal.h"

#define LAYOUT_VERSION 1

static struct {
    struct fdm *fdm;
    bool hooked;
    bool dirty;
    bool frozen;    /* Saved for the last time: the windows are going */
    NSData *sent;

    /* Restoring: the windows' frames, until they're configured */
    NSMapTable<NSWindow *, NSValue *> *frames;

    /* The quake terminal's session, until it's started */
    uint64_t quake_session;
} layout;

/*
 * Saving
 */

static uint64_t
pane_session(const struct wl_window *win)
{
    return win != NULL && win->term != NULL && !win->term->shutdown.in_progress
        ? session_link_id(win->term->session) : 0;
}

/* A split tree, without the panes that aren't sessions; nil if none is */
static NSDictionary *
save_node(const struct split *node)
{
    if (node->kind == SPLIT_LEAF) {
        const uint64_t id = pane_session(node->pane);
        return id != 0 ? @{@"session": @(id)} : nil;
    }

    NSDictionary *first = save_node(node->first);
    NSDictionary *second = save_node(node->second);
    if (first == nil || second == nil)
        return first != nil ? first : second;

    return @{
        @"split": node->kind == SPLIT_HORIZONTAL ? @"horizontal" : @"vertical",
        @"ratio": @(node->ratio),
        @"first": first,
        @"second": second,
    };
}

static FootSplitView *
splits_of(NSWindow *window)
{
    return [window.contentView isKindOfClass:[FootSplitView class]]
        ? window.contentView : nil;
}

static NSData *
serialize(void)
{
    NSMutableArray *windows = [NSMutableArray array];
    NSMutableArray<NSArray<NSWindow *> *> *groups = [NSMutableArray array];

    /* Front to back */
    NSInteger order = 0;
    for (NSWindow *window in NSApp.orderedWindows) {
        FootSplitView *splits = splits_of(window);
        if (splits == nil || splits.focused == NULL || splits.focused->quake)
            continue;

        NSDictionary *tree = save_node(splits.root);
        if (tree == nil)
            continue;

        /* Tabs are windows of their own, in a group */
        NSArray<NSWindow *> *tabs = window.tabbedWindows;
        NSInteger group = -1, tab = 0;
        if (tabs.count > 1) {
            for (NSUInteger i = 0; i < groups.count && group < 0; i++) {
                if ([groups[i] containsObject:window])
                    group = i;
            }
            if (group < 0) {
                [groups addObject:tabs];
                group = groups.count - 1;
            }
            tab = [tabs indexOfObject:window];
        }

        const uint64_t focused = pane_session(splits.focused);

        [windows addObject:@{
            @"order": @(order++),
            @"frame": NSStringFromRect(window.frame),
            @"fullscreen": @((window.styleMask & NSWindowStyleMaskFullScreen) != 0),
            @"key": @(window.isKeyWindow || window.isMainWindow),
            @"group": @(group),
            @"tab": @(tab),
            @"selected": @(tabs.count <= 1 || window.tabGroup.selectedWindow == window),
            @"focused": @(focused),
            @"tree": tree,
        }];
    }

    NSMutableDictionary *root = [NSMutableDictionary dictionary];
    root[@"version"] = @(LAYOUT_VERSION);
    root[@"windows"] = windows;

    struct terminal *quake = quake_terminal();
    const uint64_t quake_session = quake != NULL
        ? session_link_id(quake->session) : layout.quake_session;
    if (quake_session != 0)
        root[@"quake"] = @(quake_session);

    NSError *error = nil;
    NSData *data = [NSPropertyListSerialization
        dataWithPropertyList:root format:NSPropertyListBinaryFormat_v1_0
                     options:0 error:&error];
    if (data == nil)
        LOG_ERR("failed to serialize the layout: %s", error.description.UTF8String);
    return data;
}

void
session_layout_save(void)
{
    layout.dirty = false;
    if (layout.frozen)
        return;

    @autoreleasepool {
        NSData *data = serialize();
        if (data == nil || [data isEqualToData:layout.sent])
            return;

        sessions_set_layout(data.bytes, data.length);
        layout.sent = data;
    }
}

/* A window, or its splits, being resized with the mouse: it changes all the time */
static bool
resizing(void)
{
    tll_foreach(app_wayland()->terms, it) {
        if (it->item->window != NULL && it->item->window->is_resizing)
            return true;
    }
    return false;
}

static void
fdm_hook(struct fdm *fdm, void *data)
{
    /* Once it's done (the hooks run then, too) */
    if (layout.dirty && !resizing())
        session_layout_save();
}

void
session_layout_init(struct fdm *fdm)
{
    layout.fdm = fdm;
    layout.hooked = fdm_hook_add(fdm, &fdm_hook, NULL, FDM_HOOK_PRIORITY_LOW);
}

void
session_layout_fini(void)
{
    /* What's sent last is what the windows were before they're gone */
    layout.frozen = true;

    if (layout.hooked)
        fdm_hook_del(layout.fdm, &fdm_hook, FDM_HOOK_PRIORITY_LOW);
    layout.hooked = false;
    layout.sent = nil;
    layout.frames = nil;
}

void
session_layout_changed(void)
{
    layout.dirty = true;
}

/*
 * Restoring
 */

uint64_t
session_layout_take_quake_session(void)
{
    const uint64_t id = layout.quake_session;
    layout.quake_session = 0;
    return id;
}

bool
session_layout_restored_frame(NSWindow *window, NSRect *frame)
{
    NSValue *value = [layout.frames objectForKey:window];
    if (value == nil)
        return false;

    [layout.frames removeObjectForKey:window];
    *frame = value.rectValue;
    return true;
}

static NSNumber *
first_session(NSDictionary *node)
{
    return node[@"session"] != nil ? node[@"session"] : first_session(node[@"first"]);
}

/* The tree, without the sessions that are gone (or taken) */
static NSDictionary *
prune(NSDictionary *node, NSMutableSet<NSNumber *> *available)
{
    if (![node isKindOfClass:[NSDictionary class]])
        return nil;

    NSNumber *session = node[@"session"];
    if (session != nil) {
        if (![session isKindOfClass:[NSNumber class]] || ![available containsObject:session])
            return nil;
        [available removeObject:session];
        return node;
    }

    NSDictionary *first = prune(node[@"first"], available);
    NSDictionary *second = prune(node[@"second"], available);
    if (first == nil || second == nil)
        return first != nil ? first : second;

    NSMutableDictionary *pruned = [node mutableCopy];
    pruned[@"first"] = first;
    pruned[@"second"] = second;
    return pruned;
}

/* A terminal attached to 'session' (once released), in a new window, tab or split */
static struct terminal *
open_session(NSNumber *session, NSWindow *tab_parent,
             struct wl_window *split_parent, enum split_dir dir)
{
    sessions_attach_next(session.unsignedLongLongValue, true);
    struct terminal *term = app_new_terminal(NULL, tab_parent, split_parent, dir);
    sessions_attach_next(0, false);
    return term;
}

/*
 * 'leaf' is the pane that's 'node': each split splits it, the first
 * half being the pane, the second a new one
 */
static void
restore_node(NSDictionary *node, struct wl_window *leaf,
             NSMutableDictionary<NSNumber *, NSValue *> *panes)
{
    if (node[@"session"] != nil) {
        panes[node[@"session"]] = [NSValue valueWithPointer:leaf];
        return;
    }

    const bool horizontal = [node[@"split"] isEqual:@"horizontal"];
    struct terminal *second = open_session(
        first_session(node[@"second"]), nil, leaf,
        horizontal ? SPLIT_RIGHT : SPLIT_DOWN);

    if (second == NULL || second->window == NULL) {
        restore_node(node[@"first"], leaf, panes);
        return;
    }

    FootSplitView *splits = (__bridge FootSplitView *)leaf->splits;
    NSNumber *ratio = node[@"ratio"];
    if ([ratio isKindOfClass:[NSNumber class]])
        [splits setRatio:ratio.doubleValue ofSplitAbove:second->window];

    restore_node(node[@"first"], leaf, panes);
    restore_node(node[@"second"], second->window, panes);
}

bool
session_layout_restore(struct fdm *fdm)
{
    struct session_list list;
    if (!sessions_list(fdm, &list))
        return false;

    NSMutableSet<NSNumber *> *available = [NSMutableSet set];
    for (size_t i = 0; i < list.count; i++) {
        if (!list.sessions[i].attached)
            [available addObject:@(list.sessions[i].id)];
    }

    NSDictionary *saved = nil;
    if (list.layout != NULL) {
        NSData *data = [NSData dataWithBytes:list.layout length:list.layout_len];
        saved = [NSPropertyListSerialization propertyListWithData:data options:0
                                                           format:NULL error:nil];
        if (![saved isKindOfClass:[NSDictionary class]] ||
            ![saved[@"version"] isEqual:@(LAYOUT_VERSION)])
        {
            saved = nil;
        }
    }
    session_list_free(&list);

    if (available.count == 0)
        return false;

    LOG_INFO("restoring %zu sessions", (size_t)available.count);

    /* The quake terminal's, for when it's shown */
    NSNumber *quake = saved[@"quake"];
    if ([quake isKindOfClass:[NSNumber class]] && [available containsObject:quake]) {
        layout.quake_session = quake.unsignedLongLongValue;
        [available removeObject:quake];
    }

    layout.frames = [NSMapTable weakToStrongObjectsMapTable];

    /*
     * Windows, and tab groups, back to front (the front one is opened
     * last); a group's tabs in their order
     */
    NSArray *saved_windows = [saved[@"windows"] isKindOfClass:[NSArray class]]
        ? saved[@"windows"] : @[];

    NSMutableArray<NSMutableArray<NSDictionary *> *> *units = [NSMutableArray array];
    NSMutableDictionary<NSNumber *, NSMutableArray<NSDictionary *> *> *groups = [NSMutableDictionary dictionary];
    for (NSDictionary *window in saved_windows) {
        if (![window isKindOfClass:[NSDictionary class]])
            continue;

        NSNumber *group = [window[@"group"] isKindOfClass:[NSNumber class]] ? window[@"group"] : @(-1);
        if (group.integerValue >= 0 && groups[group] != nil) {
            [groups[group] addObject:window];
            continue;
        }

        NSMutableArray<NSDictionary *> *unit = [NSMutableArray arrayWithObject:window];
        [units addObject:unit];
        if (group.integerValue >= 0)
            groups[group] = unit;
    }

    for (NSMutableArray<NSDictionary *> *unit in units) {
        [unit sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            return [a[@"tab"] compare:b[@"tab"]];
        }];
    }

    /* The frontmost of a unit's windows is where it is */
    NSInteger (^front)(NSArray<NSDictionary *> *) = ^NSInteger(NSArray<NSDictionary *> *unit) {
        NSInteger order = NSIntegerMax;
        for (NSDictionary *window in unit)
            order = MIN(order, [window[@"order"] integerValue]);
        return order;
    };
    [units sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) {
        const NSInteger fa = front(a), fb = front(b);
        return fa > fb ? NSOrderedAscending : fa < fb ? NSOrderedDescending : NSOrderedSame;
    }];

    NSMutableArray<NSDictionary *> *restored = [NSMutableArray array];

    for (NSArray<NSDictionary *> *unit in units) {
        NSWindow *tab_parent = nil;

        for (NSDictionary *window in unit) {
            NSDictionary *tree = prune(window[@"tree"], available);
            if (tree == nil)
                continue;

            struct terminal *term = open_session(first_session(tree), tab_parent, NULL, 0);
            if (term == NULL || term->window == NULL)
                continue;

            NSWindow *nswindow = (__bridge NSWindow *)term->window->nswindow;

            /* Tabs share the window's frame */
            NSString *frame = window[@"frame"];
            if (tab_parent == nil && [frame isKindOfClass:[NSString class]]) {
                [layout.frames setObject:[NSValue valueWithRect:NSRectFromString(frame)]
                                  forKey:nswindow];
            }
            if (tab_parent == nil)
                tab_parent = nswindow;

            NSMutableDictionary<NSNumber *, NSValue *> *panes = [NSMutableDictionary dictionary];
            restore_node(tree, term->window, panes);

            [restored addObject:@{
                @"window": nswindow,
                @"panes": panes,
                @"focused": (window[@"focused"] != nil ? window[@"focused"] : @0),
                @"selected": (window[@"selected"] != nil ? window[@"selected"] : @YES),
                @"key": (window[@"key"] != nil ? window[@"key"] : @NO),
                @"fullscreen": (window[@"fullscreen"] != nil ? window[@"fullscreen"] : @NO),
            }];
        }
    }

    /* The rest (e.g. without a layout): a window each */
    for (NSNumber *session in [available.allObjects sortedArrayUsingSelector:@selector(compare:)])
        open_session(session, nil, NULL, 0);

    /*
     * Once the windows are configured (and placed): the tabs selected,
     * the panes focused; then the sessions are attached, in their
     * final sizes
     */
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        NSWindow *key = nil;
        for (NSDictionary *window in restored) {
            NSWindow *nswindow = window[@"window"];
            if (nswindow.contentView == nil)
                continue;

            NSValue *focused = window[@"panes"][window[@"focused"]];
            FootSplitView *splits = splits_of(nswindow);
            if (focused != nil && splits != nil)
                [splits focusPane:focused.pointerValue];

            if ([window[@"selected"] boolValue] && nswindow.tabGroup != nil)
                nswindow.tabGroup.selectedWindow = nswindow;

            if ([window[@"key"] boolValue])
                key = nswindow;
        }

        if (key != nil)
            [key makeKeyAndOrderFront:nil];

        for (NSDictionary *window in restored) {
            NSWindow *nswindow = window[@"window"];
            if ([window[@"fullscreen"] boolValue] && nswindow.contentView != nil &&
                !(nswindow.styleMask & NSWindowStyleMaskFullScreen))
            {
                [nswindow toggleFullScreen:nil];
            }
        }

        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            sessions_release();
            layout.frames = nil;
            session_layout_changed();
            app_run_hooks();
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        app_run_hooks();
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());

    return true;
}
