#import "app.h"
#import "view.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LOG_MODULE "ports"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "ports.h"
#include "session.h"
#include "terminal.h"
#include "url-mode.h"
#include "xmalloc.h"

/*
 * Another machine's ports; see ports.h.
 */

/* The machine itself, and its port, in a URL */
struct local_url {
    size_t host;        /* Where it begins, in the URL */
    size_t host_len;
    size_t end;         /* Of the host and port */
    uint16_t port;      /* 0: none given */
    bool any;           /* "All addresses" (0.0.0.0, [::]), which only a server says: here, it's localhost */
};

static bool
is_host(const char *host, size_t len, const char *name)
{
    return strlen(name) == len && strncasecmp(host, name, len) == 0;
}

/* SCHEME://[USER[:PASSWORD]@]HOST[:PORT][/...], with this machine for its host */
static bool
parse_local_url(const char *url, struct local_url *local)
{
    const char *scheme_end = strstr(url, "://");
    if (scheme_end == NULL || scheme_end == url)
        return false;
    for (const char *c = url; c < scheme_end; c++) {
        if (!isalnum((unsigned char)*c) && strchr("+.-", *c) == NULL)
            return false;
    }

    const char *authority = scheme_end + 3;
    const char *end = authority + strcspn(authority, "/?#");

    const char *host = authority;
    for (const char *c = authority; c < end; c++) {
        if (*c == '@')
            host = c + 1;
    }

    /* An IPv6 address is in brackets (it has colons) */
    const char *host_end = host;
    if (host < end && *host == '[') {
        while (host_end < end && *host_end != ']')
            host_end++;
        if (host_end == end)
            return false;
        host_end++;
    } else {
        while (host_end < end && *host_end != ':')
            host_end++;
    }

    long port = 0;
    if (host_end < end) {
        if (*host_end != ':' || host_end + 1 == end)
            return false;
        for (const char *c = host_end + 1; c < end; c++) {
            if (!isdigit((unsigned char)*c) || (port = port * 10 + (*c - '0')) > UINT16_MAX)
                return false;
        }
        if (port == 0)
            return false;
    }

    const size_t len = host_end - host;
    const bool any = is_host(host, len, "0.0.0.0") || is_host(host, len, "[::]");
    const bool loopback =
        is_host(host, len, "localhost") || is_host(host, len, "[::1]") ||
        (len > 10 && strncasecmp(host_end - 10, ".localhost", 10) == 0) ||
        (len >= 9 && strncmp(host, "127.", 4) == 0 && strspn(host, "0123456789.") >= len);
    if (!any && !loopback)
        return false;

    *local = (struct local_url){
        .host = host - url, .host_len = len, .end = end - url, .port = port, .any = any,
    };
    return true;
}

/* The port a scheme's servers listen on, unless the URL says; 0: unknown */
static uint16_t
default_port(const char *url)
{
    static const struct {
        const char *scheme;
        uint16_t port;
    } known[] = {
        {"http://", 80}, {"https://", 443}, {"ws://", 80}, {"wss://", 443},
        {"postgres://", 5432}, {"postgresql://", 5432}, {"mysql://", 3306},
        {"redis://", 6379}, {"mongodb://", 27017},
    };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strncasecmp(url, known[i].scheme, strlen(known[i].scheme)) == 0)
            return known[i].port;
    }
    return 0;
}

/* 'url', for the port it is here (malloc:ed) */
static char *
url_here(const char *url, const struct local_url *local, uint16_t port, uint16_t port_here)
{
    /* As it is, unless it wouldn't lead to it */
    if (port_here == port && !local->any)
        return xstrdup(url);

    return xasprintf("%.*s%.*s:%u%s", (int)local->host, url,
                     local->any ? (int)strlen("localhost") : (int)local->host_len,
                     local->any ? "localhost" : &url[local->host],
                     port_here, &url[local->end]);
}

static NSString *
machine_of(const struct terminal *term)
{
    const char *host = session_link_host(term->session);
    return host != NULL ? connections_title([NSString stringWithUTF8String:host]) : @"";
}

static NSWindow *
window_of(const struct terminal *term)
{
    return term->window != NULL ? (__bridge NSWindow *)term->window->nswindow : nil;
}

/* footd's reasons are a sentence's end: "nothing is listening on it" */
static NSString *
sentence(const char *reason)
{
    NSString *text = [NSString stringWithUTF8String:reason];
    if (text == nil || text.length == 0)
        return @"It failed.";
    return [NSString stringWithFormat:@"%@%@.", [text substringToIndex:1].uppercaseString,
            [text substringFromIndex:1]];
}

/* (After the callback has returned: it's footd's connection's) */
static void
show_failure(struct terminal *term, uint16_t port, const char *error)
{
    NSWindow *window = window_of(term);
    NSString *message = [NSString stringWithFormat:@"Port %u of %@ can’t be opened",
                         port, machine_of(term)];
    NSString *why = sentence(error);
    LOG_WARN("%s: %s", message.UTF8String, error);

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = message;
        alert.informativeText = why;
        if (window != nil && window.visible)
            [alert beginSheetModalForWindow:window completionHandler:nil];
        else
            [alert runModal];
    });
}

/*
 * A URL
 */

struct opening {
    char *url;
    struct local_url local;
    uint16_t port;
};

static void
url_port_opened(struct terminal *term, uint16_t port_here, const char *error, void *data)
{
    struct opening *opening = data;

    if (term != NULL && error != NULL)
        show_failure(term, opening->port, error);
    else if (term != NULL) {
        char *url = url_here(opening->url, &opening->local, opening->port, port_here);
        urls_launch(term, url);
        free(url);
    }

    free(opening->url);
    free(opening);
}

bool
ports_open_url(struct terminal *term, const char *url)
{
    /* (A [regex:name]'s matches are for its own launcher) */
    if (session_link_host(term->session) == NULL ||
        term->url.launch != &term->conf->url.launch || !sessions_ports_supported())
    {
        return false;
    }

    struct local_url local;
    if (!parse_local_url(url, &local))
        return false;

    const uint16_t port = local.port != 0 ? local.port : default_port(url);
    if (port == 0)
        return false;

    struct opening *opening = xmalloc(sizeof(*opening));
    *opening = (struct opening){.url = xstrdup(url), .local = local, .port = port};
    if (!sessions_open_port(term->session, port, &url_port_opened, opening)) {
        free(opening->url);
        free(opening);
        return false;
    }
    return true;
}

/*
 * Shell > Open Port of: the ports there are, there, to choose from
 */

@interface FootPortChooser : NSObject
@property (nonatomic, weak) FootView *view;
@end

static FootPortChooser *chooser;

/* It's open: where, and what to do with that */
static void
show_opened(FootView *view, NSString *machine, uint16_t port, uint16_t port_here)
{
    NSString *address = [NSString stringWithFormat:@"localhost:%u", port_here];

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:@"Port %u of %@ is open on this Mac",
                         port, machine];
    alert.informativeText = port_here == port
        ? [NSString stringWithFormat:@"Programs on this Mac reach it at %@, as long as a "
           "window on %@ is open.", address, machine]
        : [NSString stringWithFormat:@"Programs on this Mac reach it at %@, as long as a "
           "window on %@ is open. (Port %u is in use on this Mac.)", address, machine, port];
    [alert addButtonWithTitle:@"Copy Address"];
    [alert addButtonWithTitle:@"Open in Browser"];
    [alert addButtonWithTitle:@"Done"].keyEquivalent = @"\033";

    __weak FootView *weak = view;
    void (^done)(NSModalResponse) = ^(NSModalResponse response) {
        if (response == NSAlertFirstButtonReturn) {
            NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
            [pasteboard clearContents];
            [pasteboard setString:address forType:NSPasteboardTypeString];
        } else if (response == NSAlertSecondButtonReturn) {
            NSString *url = [@"http://" stringByAppendingString:address];
            struct terminal *term = weak.term;
            if (term == NULL || !urls_launch(term, url.UTF8String))
                [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:url]];
        }
    };

    if (view.window != nil && view.window.visible)
        [alert beginSheetModalForWindow:view.window completionHandler:done];
    else
        done([alert runModal]);
}

static void
chosen_port_opened(struct terminal *term, uint16_t port_here, const char *error, void *data)
{
    const uint16_t port = (uintptr_t)data;
    if (term == NULL)
        return;
    if (error != NULL) {
        show_failure(term, port, error);
        return;
    }

    FootView *view = term->window != NULL ? (__bridge FootView *)term->window->view : nil;
    NSString *machine = machine_of(term);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (view != nil)
            show_opened(view, machine, port, port_here);
    });
}

static void
open_port(FootView *view, uint16_t port)
{
    struct terminal *term = view.term;
    if (term == NULL ||
        !sessions_open_port(term->session, port, &chosen_port_opened, (void *)(uintptr_t)port))
    {
        NSBeep();
    }
}

@implementation FootPortChooser

- (void)choose:(NSMenuItem *)item
{
    FootView *view = self.view;
    if (view != nil)
        open_port(view, [item.representedObject unsignedShortValue]);
}

/* One that isn't listed (or none are: 'none') */
- (void)other:(id)sender
{
    [self askIn:self.view none:NO];
}

- (void)askIn:(FootView *)view none:(BOOL)none
{
    struct terminal *term = view.term;
    if (term == NULL || view.window == nil)
        return;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:@"Open Port of %@", machine_of(term)];
    alert.informativeText = none
        ? @"No program listening on a port was found there. Which port is to be "
          "opened on this Mac?"
        : @"Which port there is to be opened on this Mac?";
    [alert addButtonWithTitle:@"Open"];
    [alert addButtonWithTitle:@"Cancel"];

    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 120, 24)];
    field.placeholderString = @"3000";
    alert.accessoryView = field;
    alert.window.initialFirstResponder = field;

    __weak FootView *weak = view;
    [alert beginSheetModalForWindow:view.window completionHandler:^(NSModalResponse response) {
        FootView *pane = weak;
        if (response != NSAlertFirstButtonReturn || pane == nil)
            return;

        const NSInteger port = field.stringValue.integerValue;
        if (port < 1 || port > UINT16_MAX) {
            NSBeep();
            return;
        }

        /* (Once this sheet is gone: the next one is the window's too) */
        dispatch_async(dispatch_get_main_queue(), ^{
            open_port(pane, port);
        });
    }];
}

@end

static void
ports_listed(struct terminal *term, const struct session_port *ports, size_t count,
             const char *error, void *data)
{
    if (term == NULL || term->window == NULL)
        return;
    /* (Then, one can still be named) */
    const bool failed = error != NULL;
    if (failed)
        LOG_WARN("the ports there aren't known: %s", error);

    if (chooser == nil)
        chooser = [[FootPortChooser alloc] init];
    FootView *view = (__bridge FootView *)term->window->view;
    chooser.view = view;

    /* The user's own programs' first; then the system's (e.g. a database that's a service) */
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Ports"];
    menu.autoenablesItems = NO;
    bool own = true;
    for (size_t i = 0; i < count; i++) {
        if (own && !ports[i].own && menu.numberOfItems > 0)
            [menu addItem:[NSMenuItem separatorItem]];
        own = ports[i].own;

        NSString *title = ports[i].program != NULL
            ? [NSString stringWithFormat:@"%s — %u", ports[i].program, ports[i].port]
            : [NSString stringWithFormat:@"Port %u", ports[i].port];
        NSMenuItem *item = [menu addItemWithTitle:title action:@selector(choose:)
                                    keyEquivalent:@""];
        item.target = chooser;
        item.representedObject = @(ports[i].port);
    }

    const bool none = menu.numberOfItems == 0;
    if (!none) {
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItemWithTitle:@"Other Port…" action:@selector(other:) keyEquivalent:@""]
            .target = chooser;
    }

    /* (After the callback has returned: a menu, or a sheet, runs the event loop) */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (view.window == nil)
            return;
        if (none) {
            [chooser askIn:view none:!failed];
            return;
        }

        const NSRect bounds = view.bounds;
        const NSPoint at = NSMakePoint(
            NSMinX(bounds) + 12, view.flipped ? NSMinY(bounds) + 12 : NSMaxY(bounds) - 12);
        [menu popUpMenuPositioningItem:nil atLocation:at inView:view];
    });
}

@interface FootView (Ports)
- (void)openRemotePort:(id)sender;
@end

@implementation FootView (Ports)

/* Shell > Open Port of (the focused terminal's machine) */
- (void)openRemotePort:(id)sender
{
    struct terminal *term = self.term;
    if (term == NULL || !sessions_list_ports(term->session, &ports_listed, NULL))
        NSBeep();
}

@end
