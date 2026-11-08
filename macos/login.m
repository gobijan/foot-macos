#import "app.h"

#import <Security/Security.h>

#include <tllist.h>

#define LOG_MODULE "login"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "session.h"
#include "terminal.h"

/*
 * Logging in to other machines (sessions there, over SSH): what ssh
 * asks, a password or a key's passphrase (or a yes or no), footd passes
 * on (footd-askpass.c), and it's asked here: a sheet, on a window there.
 * The answer may be kept in the Keychain (by machine and prompt): the
 * next time, it's the answer, without asking. One that turned out
 * wrong is forgotten, and asked for again. footd keeps answers too,
 * while it runs (connecting again doesn't ask).
 */

static NSString *const keychain_service = @"foot SSH";

/* Those asked, by ID (sheets) */
static NSMutableDictionary<NSNumber *, NSAlert *> *asking;

static NSMutableDictionary *
keychain_query(NSString *host, NSString *prompt)
{
    return [@{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: keychain_service,
        (__bridge id)kSecAttrAccount: host,
        (__bridge id)kSecAttrComment: prompt,
    } mutableCopy];
}

static NSString *
keychain_find(NSString *host, NSString *prompt)
{
    NSMutableDictionary *query = keychain_query(host, prompt);
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

    CFTypeRef data = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &data) != errSecSuccess)
        return nil;
    return [[NSString alloc] initWithData:(__bridge_transfer NSData *)data
                                 encoding:NSUTF8StringEncoding];
}

static void
keychain_forget(NSString *host, NSString *prompt)
{
    SecItemDelete((__bridge CFDictionaryRef)keychain_query(host, prompt));
}

static void
keychain_keep(NSString *host, NSString *prompt, NSString *answer)
{
    keychain_forget(host, prompt);

    NSMutableDictionary *item = keychain_query(host, prompt);
    item[(__bridge id)kSecAttrLabel] = [NSString stringWithFormat:@"foot: %@", host];
    item[(__bridge id)kSecValueData] = [answer dataUsingEncoding:NSUTF8StringEncoding];
    const OSStatus status = SecItemAdd((__bridge CFDictionaryRef)item, NULL);
    if (status != errSecSuccess)
        LOG_WARN("%s: failed to keep the password in the Keychain: %d", host.UTF8String, (int)status);
}

/* A window there: the key window, if it is; else the frontmost (nil: none) */
static NSWindow *
window_on(NSString *host)
{
    NSMutableSet<NSWindow *> *there = [NSMutableSet set];
    tll_foreach(app_wayland()->terms, it) {
        struct terminal *term = it->item;
        const char *on = session_link_host(term->session);
        if (on != NULL && term->window != NULL && term->window->nswindow != NULL &&
            [host isEqualToString:[NSString stringWithUTF8String:on]])
        {
            [there addObject:(__bridge NSWindow *)term->window->nswindow];
        }
    }

    if ([there containsObject:NSApp.keyWindow])
        return NSApp.keyWindow;
    for (NSWindow *window in NSApp.orderedWindows) {
        if ([there containsObject:window])
            return window;
    }
    return there.anyObject;
}

/* The question: a secret ('field', and whether to 'keep' it), or else yes or no */
static NSAlert *
login_alert(NSString *title, NSString *prompt, bool confirm, bool again,
            NSSecureTextField **field, NSButton **keep)
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.informativeText = prompt;
    *field = nil;
    *keep = nil;

    if (confirm) {
        alert.messageText = [NSString stringWithFormat:@"%@ asks", title];
        [alert addButtonWithTitle:@"Yes"];
        [alert addButtonWithTitle:@"No"];
        return alert;
    }

    alert.messageText = again
        ? [NSString stringWithFormat:@"That didn't work: log in to %@ again", title]
        : [NSString stringWithFormat:@"Log in to %@", title];
    [alert addButtonWithTitle:@"Log In"];
    [alert addButtonWithTitle:@"Cancel"];

    *field = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 22)];
    *keep = [NSButton checkboxWithTitle:@"Remember in Keychain" target:nil action:nil];
    (*keep).state = NSControlStateValueOn;

    NSStackView *stack = [NSStackView stackViewWithViews:@[*field, *keep]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    [(*field).widthAnchor constraintEqualToConstant:260].active = YES;
    stack.frame = (NSRect){.size = stack.fittingSize};
    alert.accessoryView = stack;
    alert.window.initialFirstResponder = *field;
    return alert;
}

static void
ask(uint64_t id, const char *host_, const char *prompt_, bool confirm, bool again, void *data)
{
    NSString *host = [NSString stringWithUTF8String:host_];
    NSString *prompt = [[NSString stringWithUTF8String:prompt_]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (host == nil || prompt == nil) {
        sessions_answer(id, false, NULL);
        return;
    }

    /* Kept (and right, as far as known) */
    if (!confirm) {
        if (again)
            keychain_forget(host, prompt);
        else {
            NSString *kept = keychain_find(host, prompt);
            if (kept != nil) {
                LOG_INFO("%s: answered from the Keychain", host_);
                sessions_answer(id, true, kept.UTF8String);
                return;
            }
        }
    }

    NSWindow *window = window_on(host);
    if (window == nil) {
        sessions_answer(id, false, NULL);
        return;
    }

    NSSecureTextField *field = nil;
    NSButton *keep = nil;
    NSAlert *alert = login_alert(connections_title(host), prompt, confirm, again, &field, &keep);

    if (asking == nil)
        asking = [NSMutableDictionary dictionary];
    asking[@(id)] = alert;

    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        /* (Not to be answered any more: asked()) */
        if (asking[@(id)] == nil)
            return;
        [asking removeObjectForKey:@(id)];

        const bool ok = response == NSAlertFirstButtonReturn;
        NSString *answer = field != nil ? field.stringValue : @"";
        sessions_answer(id, ok, answer.UTF8String);
        if (ok && keep.state == NSControlStateValueOn)
            keychain_keep(host, prompt, answer);
        field.stringValue = @"";
    }];

    /* (It's the user's turn, also while another application is active) */
    if (!NSApp.active)
        [NSApp requestUserAttention:NSInformationalRequest];
}

static void
asked(uint64_t id, void *data)
{
    NSAlert *alert = asking[@(id)];
    if (alert == nil)
        return;

    [asking removeObjectForKey:@(id)];
    [alert.window.sheetParent endSheet:alert.window returnCode:NSModalResponseAbort];
}

void
login_init(void)
{
    sessions_set_login(&ask, &asked, NULL);
}
