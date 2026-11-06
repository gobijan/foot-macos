/*
 * The smoke test's driver (macos/smoke-test.py): loaded into foot with
 * DYLD_INSERT_LIBRARIES, it runs the menu actions the test asks for,
 * on the first terminal window. No keystrokes, and no window needs to
 * be key: what the test does doesn't depend on what the user does
 * meanwhile.
 *
 * The test writes an action's selector (e.g. "splitRight:") to
 * $FOOT_SMOKE_DIR/action; it's removed once performed. Actions go to
 * the first terminal window that's shown.
 */

#import <AppKit/AppKit.h>

#include <stdlib.h>

static NSString *action_path;

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
