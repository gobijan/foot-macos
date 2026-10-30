#import "app.h"

#import <UserNotifications/UserNotifications.h>

#include <string.h>

#include <tllist.h>

#define LOG_MODULE "notifications"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "notify.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

/*
 * Desktop notifications (OSC 9, 99, 777, the bell) in Notification
 * Center, as foot's own: with its icon, and a click leads back to the
 * terminal, also the program in it, if it asked for that (OSC 99).
 *
 * Only with desktop-notifications.command at its default, which is the
 * fallback for what can't use Notification Center: the foot binary
 * outside of foot.app, and builds macOS doesn't accept (not signed).
 *
 * There are no helper processes. Notifications that are tracked have an
 * ID instead, in place of the helper's PID, which notify.c uses to find
 * them. It's out of the range of real PIDs, as notify.c signals those.
 */

#define FIRST_ID (1 << 24)

/* Keys of the notification's userInfo */
static NSString *const key_term = @"term";
static NSString *const key_id = @"id";

/* Custom actions (OSC 99 buttons) are numbered, starting at 1 */
static NSString *const action_prefix = @"button-";

static uint32_t next_id = FIRST_ID;

/* macOS doesn't allow foot to use it at all (e.g. no real signature) */
static bool not_allowed;

/* The default of desktop-notifications.command, in config.c */
static bool
command_is_default(const struct terminal *term)
{
    static const char *const expected[] = {
        "osascript", "-e", "on run argv",
        "-e", "display notification (item 2 of argv) with title (item 1 of argv)",
        "-e", "end run", "${title}", "${body}", NULL,
    };

    char *const *argv = term->conf->desktop_notifications.command.argv.args;
    if (argv == NULL)
        return false;

    for (size_t i = 0; i < ALEN(expected); i++) {
        if (expected[i] == NULL || argv[i] == NULL)
            return expected[i] == argv[i];
        if (!streq(expected[i], argv[i]))
            return false;
    }
    return false;
}

static struct terminal *
find_term(NSNumber *ptr)
{
    tll_foreach(app_wayland()->terms, it) {
        if ((uintptr_t)it->item == ptr.unsignedLongLongValue)
            return it->item;
    }
    return NULL;
}

@interface FootNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation FootNotificationDelegate

/* foot decides when to notify (desktop-notifications.inhibit-when-focused) */
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))handler
{
    handler(UNNotificationPresentationOptionBanner |
            UNNotificationPresentationOptionList |
            UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))handler
{
    NSDictionary *info = response.notification.request.content.userInfo;
    NSString *action = response.actionIdentifier;

    const bool activated = ![action isEqualToString:UNNotificationDismissActionIdentifier];
    const uint32_t button = [action hasPrefix:action_prefix]
        ? (uint32_t)[action substringFromIndex:action_prefix.length].integerValue : 0;
    const pid_t id = [info[key_id] intValue];
    NSNumber *ptr = info[key_term];

    /* Not on the main thread's run loop, necessarily */
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        struct terminal *term = ptr != nil && app_wayland() != NULL ? find_term(ptr) : NULL;
        if (term != NULL) {
            notify_native_done(term, id, activated, button);
            app_run_hooks();
        }
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());

    handler();
}

@end

void
notifications_init(void)
{
    /* UNUserNotificationCenter throws an exception outside of a bundle */
    if (NSBundle.mainBundle.bundleIdentifier == nil)
        return;

    static FootNotificationDelegate *delegate;
    delegate = [[FootNotificationDelegate alloc] init];
    [UNUserNotificationCenter currentNotificationCenter].delegate = delegate;
}

static NSString *
identifier_of(const struct terminal *term, const char *id, uint32_t unique)
{
    /* One with the same ID replaces it */
    return id != NULL
        ? [NSString stringWithFormat:@"%p-%s", (const void *)term, id]
        : [NSString stringWithFormat:@"%p-#%u", (const void *)term, unique];
}

static void
deliver(UNNotificationRequest *request, UNNotificationCategory *category)
{
    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];

    [center getNotificationCategoriesWithCompletionHandler:^(NSSet<UNNotificationCategory *> *categories) {
        [center setNotificationCategories:[categories setByAddingObject:category]];
        [center addNotificationRequest:request withCompletionHandler:^(NSError *error) {
            if (error != nil)
                LOG_WARN("failed to deliver a notification: %s",
                         error.localizedDescription.UTF8String);
        }];
    }];
}

bool
wayl_notify(struct terminal *term, struct notification *notif, const char *title,
            const char *body, bool track)
{
    if (NSBundle.mainBundle.bundleIdentifier == nil || not_allowed ||
        !command_is_default(term))
    {
        return false;
    }

    const uint32_t id = next_id++;

    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = @(title);
    content.body = body != NULL ? @(body) : @"";
    content.userInfo = @{key_term: @((uintptr_t)term), key_id: @(id)};
    if (!notif->muted)
        content.sound = [UNNotificationSound defaultSound];

    switch (notif->urgency) {
    case NOTIFY_URGENCY_LOW:
        content.interruptionLevel = UNNotificationInterruptionLevelPassive;
        break;
    case NOTIFY_URGENCY_NORMAL:
        content.interruptionLevel = UNNotificationInterruptionLevelActive;
        break;
    case NOTIFY_URGENCY_CRITICAL:
        content.interruptionLevel = UNNotificationInterruptionLevelTimeSensitive;
        break;
    }

    /* A category per set of buttons */
    NSMutableArray<UNNotificationAction *> *actions = [NSMutableArray array];
    NSMutableString *category_id = [NSMutableString stringWithString:@"foot"];
    tll_foreach(notif->actions, it) {
        NSString *label = @(it->item);
        NSString *action_id = [NSString stringWithFormat:@"%@%zu", action_prefix, actions.count + 1];
        [actions addObject:[UNNotificationAction actionWithIdentifier:action_id
                                                                title:label
                                                              options:UNNotificationActionOptionForeground]];
        [category_id appendFormat:@"\n%@", label];
    }

    /* Dismissing it is reported to the program (OSC 99) */
    UNNotificationCategory *category = [UNNotificationCategory
        categoryWithIdentifier:category_id
                       actions:actions
             intentIdentifiers:@[]
                       options:UNNotificationCategoryOptionCustomDismissAction];
    content.categoryIdentifier = category_id;

    UNNotificationRequest *request = [UNNotificationRequest
        requestWithIdentifier:identifier_of(term, notif->id, id)
                      content:content
                      trigger:nil];

    if (track) {
        /* The one it replaces is gone; nothing will report back on it */
        if (notif->id != NULL) {
            tll_foreach(term->active_notifications, it) {
                if (it->item.id != NULL && streq(it->item.id, notif->id)) {
                    notify_free(term, &it->item);
                    tll_remove(term->active_notifications, it);
                }
            }
        }

        tll_push_back(term->active_notifications, ((struct notification){
            .id = notif->id != NULL ? xstrdup(notif->id) : NULL,
            .focus = notif->focus,
            .may_be_programatically_closed = notif->may_be_programatically_closed,
            .report_activated = notif->report_activated,
            .report_closed = notif->report_closed,
            .external_id = id,
            .pid = id,
            .stdout_fd = -1,
            .icon_fd = -1,
        }));
    }

    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                          completionHandler:^(BOOL granted, NSError *error) {
        if (granted) {
            deliver(request, category);
            return;
        }

        /* Without an error, it's the user's decision */
        if (error == nil)
            return;

        LOG_WARN("notifications aren't allowed: %s", error.localizedDescription.UTF8String);
        not_allowed = true;

        /* This one, and all others, use the command */
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            struct terminal *t = app_wayland() != NULL ? find_term(content.userInfo[key_term]) : NULL;
            if (t == NULL)
                return;

            notify_native_done(t, id, false, 0);
            notify_notify(t, &(struct notification){
                .title = xstrdup(content.title.UTF8String),
                .body = xstrdup(content.body.UTF8String),
                .expire_time = -1,
            });
            app_run_hooks();
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }];

    return true;
}

bool
wayl_notify_close(struct terminal *term, struct notification *notif)
{
    if (notif->external_id < FIRST_ID)
        return false;

    NSString *identifier = identifier_of(term, notif->id, notif->external_id);
    [[UNUserNotificationCenter currentNotificationCenter]
        removeDeliveredNotificationsWithIdentifiers:@[identifier]];

    notify_native_done(term, notif->pid, false, 0);
    return true;
}
