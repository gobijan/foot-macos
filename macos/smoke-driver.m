/*
 * The smoke test's driver (macos/smoke-test.py): loaded into foot with
 * DYLD_INSERT_LIBRARIES, it runs the menu actions the test asks for,
 * on the first terminal window. No keystrokes, and no window needs to
 * be key: what the test does doesn't depend on what the user does
 * meanwhile.
 *
 * The test writes an action's selector (e.g. "splitRight:") to
 * $FOOT_SMOKE_DIR/action; it's removed once performed. Actions go to
 * the first terminal window that's shown. "windows" (not a selector)
 * writes the terminal windows that are shown to $FOOT_SMOKE_DIR/windows
 * instead, as JSON: their tabs, and how many panes each has.
 * "dragSelection" drags a selection in the first pane, beyond its top
 * and bottom edges and back, again and again (the events are the
 * view's only, not the system's), and creates $FOOT_SMOKE_DIR/dragged
 * when it's done. "palette TEXT" opens the command palette (View >
 * Command Palette), types TEXT there, and presses Return. "optionClick
 * PERCENT" clicks in the first pane's first row, that far from the left,
 * with Option.
 */

#import <AppKit/AppKit.h>

#include <stdio.h>
#include <stdlib.h>

static NSString *action_path;

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
option_click(NSView *view, int percent)
{
    const NSRect b = view.bounds;
    const NSPoint p = NSMakePoint(
        NSMinX(b) + b.size.width * percent / 100, view.isFlipped ? NSMinY(b) + 16 : NSMaxY(b) - 16);

    [view mouseDown:mouse_event_with(view, NSEventTypeLeftMouseDown, p, NSEventModifierFlagOption)];
    [view mouseUp:mouse_event_with(view, NSEventTypeLeftMouseUp, p, NSEventModifierFlagOption)];
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
            splits = window.contentView;
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

            [windows addObject:@{
                @"tabs": @(MAX(window.tabbedWindows.count, 1)),
                @"panes": @(panes),
            }];
        }

        NSData *json = [NSJSONSerialization dataWithJSONObject:windows options:0 error:nil];
        NSString *dir = action_path.stringByDeletingLastPathComponent;
        [json writeToFile:[dir stringByAppendingPathComponent:@"windows.tmp"] atomically:NO];
        rename([dir stringByAppendingPathComponent:@"windows.tmp"].fileSystemRepresentation,
               [dir stringByAppendingPathComponent:@"windows"].fileSystemRepresentation);
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

    if ([action hasPrefix:@"optionClick "]) {
        for (NSView *view in splits.subviews) {
            if ([NSStringFromClass([view class]) isEqualToString:@"FootView"]) {
                option_click(view, [action substringFromIndex:12].intValue);
                break;
            }
        }
        return;
    }

    if ([action hasPrefix:@"palette "]) {
        palette([action substringFromIndex:8]);
        return;
    }

    /* The window's, or the application's (e.g. terminate:, Cmd+Q) */
    const SEL selector = NSSelectorFromString(action);
    if (![splits tryToPerform:selector with:nil] && ![NSApp tryToPerform:selector with:nil])
        fprintf(stderr, "smoke-driver: %s: not performed\n", action.UTF8String);
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

    dispatch_async(dispatch_get_main_queue(), ^{
        NSTimer *timer = [NSTimer timerWithTimeInterval:.05 repeats:YES block:^(NSTimer *t) {
            poll_action();
        }];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    });
}
