#import "app.h"

#define LOG_MODULE "updates"
#define LOG_ENABLE_DBG 0
#include "log.h"

/*
 * foot > Check for Updates…, with Sparkle (https://sparkle-project.org).
 *
 * Only the released foot.app (macos/dist.sh) contains Sparkle.framework.
 * It's loaded at runtime, so that no other build depends on it; those
 * have no updater. The appcast and the key its updates are signed with
 * are in Info.plist (SUFeedURL, SUPublicEDKey).
 *
 * On the second launch, Sparkle asks whether to check for updates
 * automatically (once a day).
 */

/* The part of SPUStandardUpdaterController that's used */
@protocol FootUpdaterController
- (instancetype)initWithStartingUpdater:(BOOL)start_updater
                        updaterDelegate:(id)updater_delegate
                     userDriverDelegate:(id)user_driver_delegate;
- (void)checkForUpdates:(id)sender;
@end

NSMenuItem *
updates_menu_item(void)
{
    NSString *path = [NSBundle.mainBundle.privateFrameworksPath
        stringByAppendingPathComponent:@"Sparkle.framework"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:path])
        return nil;

    NSError *error = nil;
    NSBundle *sparkle = [NSBundle bundleWithPath:path];
    if (![sparkle loadAndReturnError:&error]) {
        LOG_WARN("failed to load Sparkle: %s", error.localizedDescription.UTF8String);
        return nil;
    }

    Class controller_class = NSClassFromString(@"SPUStandardUpdaterController");
    if (controller_class == nil) {
        LOG_WARN("Sparkle: no SPUStandardUpdaterController");
        return nil;
    }

    /* Lives as long as the application */
    static id<FootUpdaterController> controller;
    controller = [[controller_class alloc] initWithStartingUpdater:YES
                                                   updaterDelegate:nil
                                                userDriverDelegate:nil];

    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Check for Updates…"
                                                  action:@selector(checkForUpdates:)
                                           keyEquivalent:@""];
    item.target = controller;
    return item;
}
