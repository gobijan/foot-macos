/*
 * The smoke test's driver (macos/smoke-test.py): loaded into foot with
 * DYLD_INSERT_LIBRARIES, it runs the menu actions the test asks for,
 * on the first terminal window. No keystrokes, and no window needs to
 * be key: what the test does doesn't depend on what the user does
 * meanwhile.
 *
 * The test writes an action's selector (e.g. "splitRight:") to
 * $FOOT_SMOKE_DIR/action; it's removed once performed. Actions go to
 * the first terminal window that's shown (its selected tab, or its first
 * pane). "windows" (not a selector) writes the terminal windows that are
 * shown to $FOOT_SMOKE_DIR/windows instead, as JSON, each tab a window:
 * their tabs, and how many panes each has; the tabs' numbers, left to
 * right ("order"), and the selected one's ("selected").
 * "dragSelection" drags a selection in the first pane, beyond its top
 * and bottom edges and back, again and again (the events are the
 * view's only, not the system's), and creates $FOOT_SMOKE_DIR/dragged
 * when it's done. "palette TEXT" opens the command palette (View >
 * Command Palette), types TEXT there, and presses Return. "optionClick
 * PERCENT" clicks in the first pane's first row, that far from the left,
 * with Option; "commandClick PERCENT" with Command.
 *
 * Menus that pop up (e.g. Shell > Open Port of's) would wait for the
 * mouse: they aren't shown here. Their items' titles are written to
 * $FOOT_SMOKE_DIR/menu instead (JSON), and "menu TITLE" chooses one of
 * the last one's. "sheet" writes the window's sheet to
 * $FOOT_SMOKE_DIR/sheet: its "texts" and its "buttons" titles (none:
 * an empty object); "sheetText TEXT" fills in its text field, and
 * "sheetButton TITLE" presses a button.
 */

#import <AppKit/AppKit.h>

#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *action_path;

/* The menu that would have popped up last */
static NSMenu *shown_menu;

/* (At once: the test may read the file at any time) */
static void
write_json(NSString *name, id object)
{
    NSData *json = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    NSString *dir = action_path.stringByDeletingLastPathComponent;
    NSString *tmp = [dir stringByAppendingPathComponent:[name stringByAppendingString:@".tmp"]];
    [json writeToFile:tmp atomically:NO];
    rename(tmp.fileSystemRepresentation,
           [dir stringByAppendingPathComponent:name].fileSystemRepresentation);
}

/* What a sheet has: its labels' texts, its buttons, and its text field (to type in) */
static void
sheet_parts(NSView *view, NSMutableArray<NSString *> *texts, NSMutableArray<NSButton *> *buttons,
            NSTextField **field)
{
    if ([view isKindOfClass:[NSButton class]]) {
        [buttons addObject:(NSButton *)view];
        return;
    }
    if ([view isKindOfClass:[NSTextField class]]) {
        NSTextField *text = (NSTextField *)view;
        if (text.editable)
            *field = text;
        else if (text.stringValue.length > 0)
            [texts addObject:text.stringValue];
        return;
    }

    for (NSView *subview in view.subviews)
        sheet_parts(subview, texts, buttons, field);
}

static void
sheet(NSWindow *window, NSString *action)
{
    NSMutableArray<NSString *> *texts = [NSMutableArray array];
    NSMutableArray<NSButton *> *buttons = [NSMutableArray array];
    NSTextField *field = nil;

    NSWindow *sheet = window.attachedSheet;
    if (sheet != nil)
        sheet_parts(sheet.contentView, texts, buttons, &field);

    if ([action isEqualToString:@"sheet"]) {
        NSMutableArray<NSString *> *titles = [NSMutableArray array];
        for (NSButton *button in buttons)
            [titles addObject:button.title];
        write_json(@"sheet", sheet != nil ? @{@"texts": texts, @"buttons": titles} : @{});
        return;
    }

    if ([action hasPrefix:@"sheetText "] && field != nil) {
        field.stringValue = [action substringFromIndex:10];
        return;
    }

    if ([action hasPrefix:@"sheetButton "]) {
        for (NSButton *button in buttons) {
            if ([button.title isEqualToString:[action substringFromIndex:12]]) {
                [button performClick:nil];
                return;
            }
        }
    }

    fprintf(stderr, "smoke-driver: %s: not with this sheet\n", action.UTF8String);
}

static NSEvent *
mouse_event_with(NSView *view, NSEventType type, NSPoint p, NSEventModifierFlags flags)
{
    return [NSEvent mouseEventWithType:type
                              location:[view convertPoint:p toView:nil]
                         modifierFlags:flags
                             timestamp:NSProcessInfo.processInfo.systemUptime
                          windowNumber:view.window.windowNumber
                               context:nil
                           eventNumber:1
                            clickCount:1
                              pressure:1];
}

static NSEvent *
mouse_event(NSView *view, NSEventType type, NSPoint p)
{
    return mouse_event_with(view, type, p, 0);
}

/* In the first row (below the padding), 'percent' of the width from the left */
static void
click(NSView *view, int percent, NSEventModifierFlags flags)
{
    const NSRect b = view.bounds;
    const NSPoint p = NSMakePoint(
        NSMinX(b) + b.size.width * percent / 100, view.isFlipped ? NSMinY(b) + 16 : NSMaxY(b) - 16);

    [view mouseDown:mouse_event_with(view, NSEventTypeLeftMouseDown, p, flags)];
    [view mouseUp:mouse_event_with(view, NSEventTypeLeftMouseUp, p, flags)];
}

/*
 * Beyond an edge, the selection scrolls, on a timer; each move sets it
 * again, and back inside stops it. The run loop runs in between.
 */
static void
drag_selection(NSView *view)
{
    const int steps = 150;
    __block int step = 0;

    const NSRect b = view.bounds;
    const NSPoint center = NSMakePoint(NSMidX(b), NSMidY(b));
    [view mouseDown:mouse_event(view, NSEventTypeLeftMouseDown, center)];

    NSTimer *timer = [NSTimer timerWithTimeInterval:.03 repeats:YES block:^(NSTimer *t) {
        if (view.window == nil || step == steps) {
            [t invalidate];
            if (view.window != nil)
                [view mouseUp:mouse_event(view, NSEventTypeLeftMouseUp, center)];

            NSString *done = [action_path.stringByDeletingLastPathComponent
                stringByAppendingPathComponent:@"dragged"];
            [[NSFileManager defaultManager] createFileAtPath:done contents:nil attributes:nil];
            return;
        }

        CGFloat y;
        switch (step++ % 4) {
        case 0:  y = NSMinY(b) - 5 - arc4random_uniform(200); break;
        case 2:  y = NSMaxY(b) + 5 + arc4random_uniform(200); break;
        default: y = NSMinY(b) + arc4random_uniform((uint32_t)NSHeight(b)); break;
        }

        const int moves = 1 + (int)arc4random_uniform(4);
        for (int i = 0; i < moves; i++) {
            const NSPoint p = NSMakePoint(NSMinX(b) + arc4random_uniform((uint32_t)NSWidth(b)), y + i);
            [view mouseDragged:mouse_event(view, NSEventTypeLeftMouseDragged, p)];
        }
    }];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
}

static void
palette(NSString *text)
{
    [NSApp tryToPerform:NSSelectorFromString(@"showCommandPalette:") with:nil];

    for (NSWindow *window in NSApp.windows) {
        if (![NSStringFromClass([window class]) isEqualToString:@"FootPalettePanel"])
            continue;

        for (NSView *view in window.contentView.subviews) {
            if (![view isKindOfClass:[NSTextField class]])
                continue;

            NSTextField *field = (NSTextField *)view;
            id<NSTextFieldDelegate> delegate = field.delegate;
            field.stringValue = text;
            [delegate controlTextDidChange:
                [NSNotification notificationWithName:NSControlTextDidChangeNotification object:field]];
            [delegate control:field textView:(NSTextView *)field.currentEditor
                doCommandBySelector:@selector(insertNewline:)];
            return;
        }
    }

    fprintf(stderr, "smoke-driver: no command palette\n");
}

static void
poll_action(void)
{
    NSString *action = [NSString stringWithContentsOfFile:action_path
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
    if (action == nil)
        return;

    action = [action stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];

    /* (Not a window that's being closed, or isn't shown yet) */
    NSView *splits = nil;
    for (NSWindow *window in NSApp.windows) {
        if (window.isVisible &&
            [NSStringFromClass([window.contentView class]) isEqualToString:@"FootSplitView"])
        {
            splits = (window.tabGroup.selectedWindow ?: window).contentView;
            break;
        }
    }

    /* Not yet */
    if (splits == nil)
        return;

    [[NSFileManager defaultManager] removeItemAtPath:action_path error:nil];

    if ([action isEqualToString:@"windows"]) {
        NSMutableArray *windows = [NSMutableArray array];
        for (NSWindow *window in NSApp.orderedWindows) {
            if (!window.isVisible ||
                ![NSStringFromClass([window.contentView class]) isEqualToString:@"FootSplitView"])
            {
                continue;
            }

            /* Panes are FootViews (the dividers are views too) */
            NSUInteger panes = 0;
            for (NSView *view in window.contentView.subviews)
                panes += [NSStringFromClass([view class]) isEqualToString:@"FootView"];

            NSMutableArray *order = [NSMutableArray array];
            for (NSWindow *tab in window.tabbedWindows ?: @[window])
                [order addObject:@(tab.windowNumber)];

            [windows addObject:@{
                @"tabs": @(MAX(window.tabbedWindows.count, 1)),
                @"panes": @(panes),
                @"order": order,
                @"selected": @((window.tabGroup.selectedWindow ?: window).windowNumber),
            }];
        }

        write_json(@"windows", windows);
        return;
    }

    if ([action hasPrefix:@"sheet"]) {
        sheet(splits.window, action);
        return;
    }

    if ([action hasPrefix:@"menu "]) {
        for (NSMenuItem *item in shown_menu.itemArray) {
            if ([item.title isEqualToString:[action substringFromIndex:5]]) {
                [NSApp sendAction:item.action to:item.target from:item];
                return;
            }
        }
        fprintf(stderr, "smoke-driver: %s: no such item\n", action.UTF8String);
        return;
    }

    if ([action isEqualToString:@"dragSelection"]) {
        for (NSView *view in splits.subviews) {
            if ([NSStringFromClass([view class]) isEqualToString:@"FootView"]) {
                drag_selection(view);
                break;
            }
        }
        return;
    }

    const BOOL command = [action hasPrefix:@"commandClick "];
    if (command || [action hasPrefix:@"optionClick "]) {
        const int percent = [action componentsSeparatedByString:@" "].lastObject.intValue;
        for (NSView *view in splits.subviews) {
            if ([NSStringFromClass([view class]) isEqualToString:@"FootView"]) {
                click(view, percent,
                      command ? NSEventModifierFlagCommand : NSEventModifierFlagOption);
                break;
            }
        }
        return;
    }

    if ([action hasPrefix:@"palette "]) {
        palette([action substringFromIndex:8]);
        return;
    }

    /* The window's, its first pane's, or the application's (e.g. terminate:, Cmd+Q) */
    const SEL selector = NSSelectorFromString(action);
    NSView *pane = nil;
    for (NSView *view in splits.subviews) {
        if ([NSStringFromClass([view class]) isEqualToString:@"FootView"]) {
            pane = view;
            break;
        }
    }

    if (![splits tryToPerform:selector with:nil] && ![pane tryToPerform:selector with:nil] &&
        ![NSApp tryToPerform:selector with:nil])
    {
        fprintf(stderr, "smoke-driver: %s: not performed\n", action.UTF8String);
    }
}

__attribute__((constructor))
static void
init(void)
{
    /* Not for foot's children */
    unsetenv("DYLD_INSERT_LIBRARIES");

    const char *dir = getenv("FOOT_SMOKE_DIR");
    if (dir == NULL)
        return;

    action_path = [@(dir) stringByAppendingPathComponent:@"action"];

    /* A menu that pops up: its items, for the test to choose from */
    Method pop_up = class_getInstanceMethod(
        [NSMenu class], @selector(popUpMenuPositioningItem:atLocation:inView:));
    method_setImplementation(pop_up, imp_implementationWithBlock(
        ^BOOL(NSMenu *menu, NSMenuItem *item, NSPoint at, NSView *view) {
            NSMutableArray<NSString *> *titles = [NSMutableArray array];
            for (NSMenuItem *each in menu.itemArray) {
                if (!each.separatorItem && !each.hidden)
                    [titles addObject:each.title];
            }
            shown_menu = menu;
            write_json(@"menu", titles);
            return NO;
        }));

    dispatch_async(dispatch_get_main_queue(), ^{
        NSTimer *timer = [NSTimer timerWithTimeInterval:.05 repeats:YES block:^(NSTimer *t) {
            poll_action();
        }];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    });
}
