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
 * outside of foot.app, and when it fails. Being turned off isn't a
 * failure: then there are none, as the command would only send them
 * from Script Editor instead. macOS also turns them off when foot exits
 * while it asks for the permission, and doesn't ask again. This is
 * checked for each one, so turning them on in System Settings doesn't
 * take a restart.
 *
 * There are no helper processes. Those a program wants to hear about
 * are tracked, with an ID in place of the helper's PID, which notify.c
 * uses to find them. It's out of the range of real PIDs, as notify.c
 * signals those. macOS doesn't tell when a banner has timed out, so the
 * tracked ones are limited: they'd pile up otherwise.
 */

#define FIRST_ID (1 << 24)
#define MAX_TRACKED 64

/* Keys of the notification's userInfo */
static NSString *const key_term = @"term";
static NSString *const key_id = @"id";
static NSString *const key_focus = @"focus";

/* Custom actions (OSC 99 buttons) are numbered, starting at 1 */
static NSString *const action_prefix = @"button-";

static uint32_t next_id = FIRST_ID;

/* It failed, for another reason than being turned off */
static bool failed;

/* Notifications can be frequent; this is worth saying only once */
static bool warned_turned_off;

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

/* Completion handlers aren't called on the main thread */
static void
on_main_thread(void (^block)(void))
{
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, block);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

static struct terminal *
find_term(NSNumber *ptr)
{
    if (ptr == nil || app_wayland() == NULL)
        return NULL;

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
    const bool focus = [info[key_focus] boolValue];
    NSNumber *ptr = info[key_term];

    on_main_thread(^{
        struct terminal *term = find_term(ptr);
        if (term == NULL)
            return;

        if (activated && focus)
            wayl_activate(term->wl, term->window, NULL);

        notify_native_done(term, id, activated, button);
        app_run_hooks();
    });

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

/* It's gone, without anyone having seen it */
static void
forget(NSNumber *ptr, pid_t id)
{
    struct terminal *term = find_term(ptr);
    if (term != NULL) {
        notify_native_done(term, id, false, 0);
        app_run_hooks();
    }
}

/* A category per set of buttons; dismissing is reported to the program (OSC 99) */
static NSString *
category_of(NSArray<NSString *> *labels)
{
    static NSMutableSet<UNNotificationCategory *> *categories;
    static NSMutableSet<NSString *> *identifiers;

    if (categories == nil) {
        categories = [NSMutableSet set];
        identifiers = [NSMutableSet set];
    }

    NSString *identifier = [@[@"foot", labels].description copy];
    if ([identifiers containsObject:identifier])
        return identifier;

    NSMutableArray<UNNotificationAction *> *actions = [NSMutableArray array];
    for (NSString *label in labels) {
        NSString *action_id = [NSString stringWithFormat:@"%@%zu", action_prefix, actions.count + 1];
        [actions addObject:[UNNotificationAction actionWithIdentifier:action_id
                                                                title:label
                                                              options:UNNotificationActionOptionForeground]];
    }

    [identifiers addObject:identifier];
    [categories addObject:[UNNotificationCategory
        categoryWithIdentifier:identifier
                       actions:actions
             intentIdentifiers:@[]
                       options:UNNotificationCategoryOptionCustomDismissAction]];

    [[UNUserNotificationCenter currentNotificationCenter] setNotificationCategories:categories];
    return identifier;
}

bool
wayl_notify(struct terminal *term, struct notification *notif, const char *title,
            const char *body)
{
    if (NSBundle.mainBundle.bundleIdentifier == nil || failed ||
        title == NULL || !command_is_default(term))
    {
        return false;
    }

    /* Not all text is valid UTF-8 (OSC 99); that's for the command */
    NSString *title_str = @(title);
    NSString *body_str = body != NULL ? @(body) : @"";
    NSMutableArray<NSString *> *labels = [NSMutableArray array];
    tll_foreach(notif->actions, it) {
        NSString *label = @(it->item);
        if (label == nil)
            return false;
        [labels addObject:label];
    }

    if (title_str == nil || body_str == nil)
        return false;

    const uint32_t id = next_id++;
    NSNumber *ptr = @((uintptr_t)term);

    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = title_str;
    content.body = body_str;
    content.categoryIdentifier = category_of(labels);
    content.userInfo = @{key_term: ptr, key_id: @(id), key_focus: @(notif->focus)};
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

    UNNotificationRequest *request = [UNNotificationRequest
        requestWithIdentifier:identifier_of(term, notif->id, id)
                      content:content
                      trigger:nil];

    const bool track = notif->report_activated || notif->report_closed ||
                       notif->may_be_programatically_closed;

    /* The one it replaces is gone; nothing will report back on it */
    if (notif->id != NULL) {
        tll_foreach(term->active_notifications, it) {
            if (it->item.id != NULL && streq(it->item.id, notif->id)) {
                notify_free(term, &it->item);
                tll_remove(term->active_notifications, it);
            }
        }
    }

    if (track) {
        if (tll_length(term->active_notifications) >= MAX_TRACKED) {
            notify_free(term, &tll_front(term->active_notifications));
            tll_pop_front(term->active_notifications);
        }

        tll_push_back(term->active_notifications, ((struct notification){
            .id = notif->id != NULL ? xstrdup(notif->id) : NULL,
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
            [center addNotificationRequest:request withCompletionHandler:^(NSError *add_error) {
                if (add_error == nil)
                    return;

                LOG_WARN("failed to deliver a notification: %s",
                         add_error.localizedDescription.UTF8String);
                on_main_thread(^{ forget(ptr, id); });
            }];
            return;
        }

        on_main_thread(^{ forget(ptr, id); });

        /* Without an error, it's the user's decision */
        if (error == nil)
            return;

        /* It's also what a later request gets, the same as a failure would */
        [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
            const bool turned_off = settings.authorizationStatus == UNAuthorizationStatusDenied;

            on_main_thread(^{
                if (turned_off) {
                    if (!warned_turned_off) {
                        LOG_WARN("notifications are turned off: "
                                 "System Settings > Notifications > foot");
                        warned_turned_off = true;
                    }
                    return;
                }

                LOG_WARN("can't use Notification Center: %s",
                         error.localizedDescription.UTF8String);
                failed = true;

                /* This one, and all others, use the command */
                struct terminal *t = find_term(ptr);
                if (t == NULL)
                    return;

                struct notification fallback = {
                    .title = xstrdup(title_str.UTF8String),
                    .body = xstrdup(body_str.UTF8String),
                    .expire_time = -1,
                };
                notify_notify(t, &fallback);
                notify_free(t, &fallback);
                app_run_hooks();
            });
        }];
    }];

    /* As notify_notify() does; not all callers free them */
    free(notif->title);
    free(notif->body);
    notif->title = NULL;
    notif->body = NULL;

    return true;
}

bool
wayl_notify_close(struct terminal *term, struct notification *notif)
{
    if (notif->pid < FIRST_ID)
        return false;

    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    NSString *identifier = identifier_of(term, notif->id, notif->external_id);
    [center removePendingNotificationRequestsWithIdentifiers:@[identifier]];
    [center removeDeliveredNotificationsWithIdentifiers:@[identifier]];

    notify_native_done(term, notif->pid, false, 0);
    return true;
}
