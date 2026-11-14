#import "app.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* The system's, not foot's spawn.h (the project's root is an include directory) */
#include <sys/../spawn.h>

extern char **environ;

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
 *
 * If foot.app was replaced while foot runs (Homebrew's cask does that),
 * there's nothing to download: foot offers to restart instead.
 */

/* The part of SPUStandardUpdaterController that's used */
@protocol FootUpdaterController
- (instancetype)initWithStartingUpdater:(BOOL)start_updater
                        updaterDelegate:(id)updater_delegate
                     userDriverDelegate:(id)user_driver_delegate;
- (void)checkForUpdates:(id)sender;
@end

/*
 * foot.app as it's on disk now, if it isn't what's running any more:
 * replaced while foot was running (e.g. by `brew upgrade`). Sparkle
 * would offer that version as an update, and fail to install it: its
 * installer is in the bundle that's gone. All that's left to do is to
 * start it. nil: it's what's running.
 */
static NSString *
replaced_version(void)
{
    NSBundle *bundle = NSBundle.mainBundle;

    /* (The bundle's own dictionary is the one read at launch) */
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:
        [bundle.bundleURL URLByAppendingPathComponent:@"Contents/Info.plist"]];
    NSString *build = info[@"CFBundleVersion"];
    NSString *running = [bundle objectForInfoDictionaryKey:@"CFBundleVersion"];

    if (![build isKindOfClass:NSString.class] || running == nil || [build isEqualToString:running])
        return nil;

    NSString *version = info[@"CFBundleShortVersionString"];
    return [version isKindOfClass:NSString.class] ? version : build;
}

/*
 * The foot on disk is started once this one is gone: by a shell that
 * waits for the end of a pipe only this foot has the other end of.
 * Returns that end (and the shell); -1: failed.
 */
static int
relaunch_when_gone(pid_t *shell)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    /* (-n: this foot may not be quite gone yet for Launch Services) */
    char *const argv[] = {
        "/bin/sh", "-c", "read -r _; exec /usr/bin/open -n \"$0\"",
        (char *)NSBundle.mainBundle.bundlePath.fileSystemRepresentation, NULL,
    };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);

    /* Its own session: it outlives foot; and nothing else of ours */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(
        &attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT |
               POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    sigset_t mask;
    sigemptyset(&mask);
    posix_spawnattr_setsigmask(&attr, &mask);

    sigset_t defaults;
    sigfillset(&defaults);
    posix_spawnattr_setsigdefault(&attr, &defaults);

    const int err = posix_spawn(shell, argv[0], &actions, &attr, argv, environ);

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[0]);

    if (err != 0) {
        errno = err;
        LOG_ERRNO("failed to start the shell that starts foot again");
        close(fds[1]);
        return -1;
    }
    return fds[1];
}

@interface FootUpdates : NSObject <NSMenuItemValidation>
@end

@implementation FootUpdates {
    id<FootUpdaterController> _controller;
}

- (void)start
{
    Class controller_class = NSClassFromString(@"SPUStandardUpdaterController");
    _controller = [[controller_class alloc] initWithStartingUpdater:YES
                                                    updaterDelegate:self
                                                 userDriverDelegate:nil];
}

- (void)checkForUpdates:(id)sender
{
    if (replaced_version() == nil) {
        [_controller checkForUpdates:sender];
        return;
    }

    /* (Not in foot @ run's callback: the alert has an event loop of its own) */
    dispatch_async(dispatch_get_main_queue(), ^{
        [self restart:sender];
    });
}

- (void)restart:(id)sender
{
    NSString *version = replaced_version();
    if (version == nil)
        return;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:@"foot %@ is installed already", version];
    alert.informativeText = [NSString stringWithFormat:
        @"foot.app was updated while this foot (%@) was running. Restart foot to use it; "
        "what runs in its sessions keeps running.",
        [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]];
    [alert addButtonWithTitle:@"Restart foot"];
    [alert addButtonWithTitle:@"Later"].keyEquivalent = @"\033";
    if ([alert runModal] != NSAlertFirstButtonReturn)
        return;

    pid_t shell;
    const int fd = relaunch_when_gone(&shell);
    if (fd < 0)
        return;

    [NSApp terminate:sender];

    /* Still here, and not on its way out: quitting was cancelled */
    if (!app_terminating()) {
        kill(shell, SIGTERM);
        close(fd);
        waitpid(shell, NULL, 0);
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (replaced_version() != nil)
        return YES;

    /* (Not while Sparkle is checking already) */
    id controller = _controller;
    return ![controller respondsToSelector:@selector(validateMenuItem:)] ||
           [controller validateMenuItem:item];
}

/*
 * SPUUpdaterDelegate: Sparkle's own checks (once a day) don't offer
 * what's installed already either
 */
- (BOOL)updater:(id)updater mayPerformUpdateCheck:(NSInteger)check error:(NSError **)error
{
    NSString *version = replaced_version();
    if (version == nil)
        return YES;

    if (error != NULL) {
        *error = [NSError errorWithDomain:@"foot" code:0 userInfo:@{
            NSLocalizedDescriptionKey: [NSString stringWithFormat:
                @"foot %@ is installed already. Restart foot to use it.", version],
        }];
    }
    return NO;
}
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

    if (NSClassFromString(@"SPUStandardUpdaterController") == nil) {
        LOG_WARN("Sparkle: no SPUStandardUpdaterController");
        return nil;
    }

    /* Lives as long as the application */
    static FootUpdates *updates;
    updates = [[FootUpdates alloc] init];
    [updates start];

    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Check for Updates…"
                                                  action:@selector(checkForUpdates:)
                                           keyEquivalent:@""];
    item.target = updates;
    return item;
}
