#import "app.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#include <tllist.h>

#define LOG_MODULE "control"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "control.h"
#include "fdm.h"
#include "session.h"
#include "session-protocol.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"
#import "split-view.h"
#import "view.h"

/*
 * Remote control (control.h): the application's side. foot @ COMMAND
 * connects, sends its arguments, and gets the command's output.
 *
 * A terminal is a pane (struct wl_window), known by a number that it
 * keeps while it's open. Without --id, a command is for the focused
 * one: the key window's, or else the frontmost window's.
 *
 * commands and run are the menus' items, as the command palette has
 * them (palette.m), for that terminal: what can be chosen there can be
 * run from a script.
 */

struct client {
    struct session_conn conn;
};

static struct {
    struct fdm *fdm;
    struct wayland *wayl;
    int listen_fd;
    char *path;
    uint32_t last_id;
} control = {.listen_fd = -1};

struct reply {
    int status;
    NSData *output;
    NSString *error;
};

static struct reply
failed(NSString *error)
{
    return (struct reply){.status = EXIT_FAILURE, .error = error};
}

static struct reply
succeeded(NSData *output)
{
    return (struct reply){.status = EXIT_SUCCESS, .output = output};
}

/* Not all of a terminal's text is valid UTF-8 (e.g. a title) */
static NSString *
string(const char *s)
{
    NSString *str = s != NULL ? @(s) : nil;
    return str != nil ? str : @"";
}

/* In the order they were opened in */
static void
assign_ids(void)
{
    tll_foreach(control.wayl->terms, it) {
        struct wl_window *win = it->item->window;
        if (win != NULL && win->control_id == 0)
            win->control_id = ++control.last_id;
    }
}

static struct terminal *
terminal_with_id(uint32_t id)
{
    tll_foreach(control.wayl->terms, it) {
        const struct wl_window *win = it->item->window;
        if (win != NULL && win->control_id == id && !it->item->shutdown.in_progress)
            return it->item;
    }
    return NULL;
}

/* Its window's focused pane */
static struct terminal *
focused_in(NSWindow *window)
{
    if (window == nil)
        return NULL;

    tll_foreach(control.wayl->terms, it) {
        struct wl_window *win = it->item->window;
        if (win == NULL || win->nswindow != (__bridge void *)window ||
            it->item->shutdown.in_progress)
        {
            continue;
        }

        if (((__bridge FootSplitView *)win->splits).focused == win)
            return it->item;
    }
    return NULL;
}

static struct terminal *
focused_terminal(void)
{
    struct terminal *term = focused_in(NSApp.keyWindow);
    if (term == NULL)
        term = focused_in(NSApp.mainWindow);

    /* foot isn't in front: its frontmost window's */
    for (NSWindow *window in NSApp.orderedWindows) {
        if (term != NULL)
            break;
        term = focused_in(window);
    }
    return term;
}

/* --id=ID, or --id ID: removed from 'args'. Returns false if it's not a terminal's */
static bool
target(NSMutableArray<NSString *> *args, struct terminal **term, NSString **error)
{
    *term = NULL;

    for (NSUInteger i = 0; i < args.count; i++) {
        NSString *value = nil;
        if ([args[i] hasPrefix:@"--id="]) {
            value = [args[i] substringFromIndex:5];
            [args removeObjectAtIndex:i];
        } else if ([args[i] isEqualToString:@"--id"] && i + 1 < args.count) {
            value = args[i + 1];
            [args removeObjectsInRange:NSMakeRange(i, 2)];
        } else
            continue;

        *term = terminal_with_id((uint32_t)value.longLongValue);
        if (*term == NULL) {
            *error = [NSString stringWithFormat:@"%@: no terminal with this ID (see foot @ ls)", value];
            return false;
        }
        return true;
    }

    *term = focused_terminal();
    if (*term == NULL) {
        *error = @"there is no terminal";
        return false;
    }
    return true;
}

static struct reply
ls(void)
{
    const struct terminal *focused = focused_terminal();
    NSMutableArray *list = [NSMutableArray array];

    tll_foreach(control.wayl->terms, it) {
        const struct terminal *term = it->item;
        const struct wl_window *win = term->window;
        if (win == NULL || term->shutdown.in_progress)
            continue;

        NSMutableDictionary *entry = [NSMutableDictionary dictionary];
        entry[@"id"] = @(win->control_id);
        entry[@"window"] = @(((__bridge NSWindow *)win->nswindow).windowNumber);
        entry[@"focused"] = term == focused ? @YES : @NO;
        entry[@"title"] = string(win->fixed_title != NULL ? win->fixed_title : term->window_title);
        entry[@"cwd"] = string(term->cwd);
        entry[@"columns"] = @(term->cols);
        entry[@"rows"] = @(term->rows);
        if (win->quake)
            entry[@"quake"] = @YES;

        /* A session (footd's): where it runs, if on another machine */
        if (term->session != NULL) {
            entry[@"session"] = [NSString stringWithFormat:@"%016llx",
                                 (unsigned long long)session_link_id(term->session)];
            const char *host = session_link_host(term->session);
            if (host != NULL && host[0] != '\0')
                entry[@"host"] = string(host);
        }

        [list addObject:entry];
    }

    NSMutableData *json = [[NSJSONSerialization
        dataWithJSONObject:list
                   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys |
                           NSJSONWritingWithoutEscapingSlashes
                     error:NULL] mutableCopy];
    [json appendBytes:"\n" length:1];
    return succeeded(json);
}

static struct reply
send_text(NSMutableArray<NSString *> *args, NSData *data)
{
    struct terminal *term;
    NSString *error;
    if (!target(args, &term, &error))
        return failed(error);
    if (args.count > 0)
        return failed([NSString stringWithFormat:@"send-text: %@: unknown option", args[0]]);

    if (data.length > 0 && !term_to_slave(term, data.bytes, data.length))
        return failed(@"send-text: the terminal didn't take it");
    return succeeded(nil);
}

static struct reply
get_text(NSMutableArray<NSString *> *args)
{
    struct terminal *term;
    NSString *error;
    if (!target(args, &term, &error))
        return failed(error);

    bool (*extract)(const struct terminal *, char **, size_t *) = &term_view_to_text;
    if (args.count == 1 && [args[0] isEqualToString:@"--scrollback"])
        extract = &term_scrollback_to_text;
    else if (args.count == 1 && [args[0] isEqualToString:@"--command-output"])
        extract = &term_command_output_to_text;
    else if (args.count > 0)
        return failed([NSString stringWithFormat:@"get-text: %@: unknown option", args[0]]);

    char *text = NULL;
    size_t len = 0;
    if (!extract(term, &text, &len)) {
        return failed(extract == &term_command_output_to_text
            ? @"get-text: no command's output (it needs the shell's integration)"
            : @"get-text: failed");
    }

    return succeeded([NSData dataWithBytesNoCopy:text length:len freeWhenDone:YES]);
}

/* View > Change Title's; none: the programs' title again */
static struct reply
set_title(NSMutableArray<NSString *> *args)
{
    struct terminal *term;
    NSString *error;
    if (!target(args, &term, &error))
        return failed(error);

    window_set_fixed_title(term->window, [args componentsJoinedByString:@" "].UTF8String);
    return succeeded(nil);
}

static void
focus_terminal(struct terminal *term, bool activate)
{
    struct wl_window *win = term->window;
    NSWindow *window = (__bridge NSWindow *)win->nswindow;

    if (activate) {
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
    }
    [(__bridge FootSplitView *)win->splits focusPane:win];
}

static struct reply
focus(NSMutableArray<NSString *> *args)
{
    struct terminal *term;
    NSString *error;
    if (!target(args, &term, &error))
        return failed(error);
    if (args.count > 0)
        return failed([NSString stringWithFormat:@"focus: %@: unknown option", args[0]]);

    focus_terminal(term, true);
    return succeeded(nil);
}

static struct reply
commands(NSMutableArray<NSString *> *args, bool run)
{
    struct terminal *term = NULL;
    NSString *error;

    /* Without terminals, there's still the application's (e.g. New Window) */
    if (!target(args, &term, &error) && tll_length(control.wayl->terms) > 0)
        return failed(error);

    /* In a window of several: the menus' items are for its focused one */
    if (term != NULL)
        focus_terminal(term, false);

    NSArray<FootCommand *> *all = palette_commands(
        term != NULL ? (__bridge FootView *)term->window->view : nil);

    if (!run) {
        NSMutableString *out = [NSMutableString string];
        for (FootCommand *command in all) {
            [out appendString:command.title];
            if (command.shortcut.length > 0)
                [out appendFormat:@"\t%@", command.shortcut];
            [out appendString:@"\n"];
        }
        return succeeded([out dataUsingEncoding:NSUTF8StringEncoding]);
    }

    if (args.count == 0)
        return failed(@"run: which command? (see foot @ commands)");

    NSString *title = [args componentsJoinedByString:@" "];
    for (FootCommand *command in all) {
        if ([command.title caseInsensitiveCompare:title] == NSOrderedSame) {
            palette_run(command);
            return succeeded(nil);
        }
    }

    return failed([NSString stringWithFormat:
        @"run: %@: no such command here (see foot @ commands)", title]);
}

static struct reply
execute(NSMutableArray<NSString *> *args, NSData *data)
{
    if (args.count == 0)
        return failed(@"no command");

    NSString *command = args[0];
    [args removeObjectAtIndex:0];
    assign_ids();

    if ([command isEqualToString:@"ls"])
        return ls();
    if ([command isEqualToString:@"send-text"])
        return send_text(args, data);
    if ([command isEqualToString:@"get-text"])
        return get_text(args);
    if ([command isEqualToString:@"focus"])
        return focus(args);
    if ([command isEqualToString:@"set-title"])
        return set_title(args);
    if ([command isEqualToString:@"commands"])
        return commands(args, false);
    if ([command isEqualToString:@"run"])
        return commands(args, true);

    return failed([NSString stringWithFormat:@"%@: unknown command (see foot @ --help)", command]);
}

static void
client_close(struct client *c)
{
    fdm_del_no_close(control.fdm, c->conn.fd);
    session_conn_close(&c->conn);
    free(c);
}

static bool
fdm_client(struct fdm *fdm, int fd, int events, void *data)
{
    struct client *c = data;

    bool eof;
    const bool ok = session_conn_read(&c->conn, &eof);

    uint32_t type;
    struct session_reader r;
    bool error;
    if (!session_conn_next(&c->conn, &type, &r, &error)) {
        if (!ok || error)
            client_close(c);
        return true;
    }

    char **argv = session_read_strv(&r);
    size_t len;
    const void *bytes = session_read_blob(&r, &len);

    @autoreleasepool {
        struct reply reply = failed(@"not a request");
        if (type == CONTROL_MSG_REQUEST && r.ok && argv != NULL) {
            NSMutableArray<NSString *> *args = [NSMutableArray array];
            for (char **arg = argv; *arg != NULL; arg++)
                [args addObject:string(*arg)];
            reply = execute(args, [NSData dataWithBytes:bytes length:len]);
        }

        /*
         * The answer is written at once: the client reads it. Not
         * forever, if it doesn't (the scrollback may be more than the
         * socket's buffer takes)
         */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &(struct timeval){.tv_sec = 2}, sizeof(struct timeval));

        struct session_msg msg = {0};
        session_msg_begin(&msg, CONTROL_MSG_REPLY);
        session_msg_i32(&msg, reply.status);
        session_msg_blob(&msg, reply.output.bytes, reply.output.length);
        session_msg_str(&msg, reply.error.UTF8String);
        session_conn_send(&c->conn, &msg, -1);
        session_msg_free(&msg);
    }

    session_strv_free(argv);
    client_close(c);

    /* What the command changed is drawn */
    app_run_hooks();
    return true;
}

static bool
fdm_listen(struct fdm *fdm, int fd, int events, void *data)
{
    int client = accept(fd, NULL, NULL);
    if (client < 0)
        return true;

    /* Only the user's own */
    uid_t uid;
    gid_t gid;
    if (getpeereid(client, &uid, &gid) < 0 || uid != getuid() ||
        fcntl(client, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK) < 0)
    {
        close(client);
        return true;
    }

    struct client *c = xmalloc(sizeof(*c));
    session_conn_init(&c->conn, client);

    if (!fdm_add(fdm, client, EPOLLIN, &fdm_client, c)) {
        session_conn_close(&c->conn);
        free(c);
    }
    return true;
}

void
control_init(struct fdm *fdm, struct wayland *wayl, bool enabled)
{
    const char *env = getenv("FOOT_CONTROL_SOCKET");
    if (!enabled && (env == NULL || env[0] == '\0'))
        return;

    char *path = control_socket_path();
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (path == NULL || strlen(path) >= sizeof(addr.sun_path)) {
        free(path);
        return;
    }
    strcpy(addr.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERRNO("failed to create the control socket");
        free(path);
        return;
    }

    /* Another foot's, if it answers; else it's left behind */
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
        LOG_INFO("%s: another foot listens there", path);
        close(fd);
        free(path);
        return;
    }
    unlink(path);

    [[NSFileManager defaultManager]
              createDirectoryAtPath:@(path).stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:NULL];

    const mode_t mask = umask(0077);
    const bool ok =
        bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        listen(fd, 16) == 0 &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0;
    umask(mask);

    if (!ok || !fdm_add(fdm, fd, EPOLLIN, &fdm_listen, NULL)) {
        LOG_ERRNO("%s: failed to listen", path);
        close(fd);
        free(path);
        return;
    }

    control.fdm = fdm;
    control.wayl = wayl;
    control.listen_fd = fd;
    control.path = path;
}

void
control_fini(void)
{
    if (control.listen_fd < 0)
        return;

    fdm_del(control.fdm, control.listen_fd);
    unlink(control.path);
    free(control.path);
    control.path = NULL;
    control.listen_fd = -1;
}
