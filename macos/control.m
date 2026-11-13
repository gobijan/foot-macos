#import "app.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
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
 * keeps while it's open. Without --id, or --match, a command is for the
 * focused one: the key window's, or else the frontmost window's.
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

/* What runs in it: its foreground process' name (not on other machines) */
static NSString *
program_of(const struct terminal *term)
{
    const pid_t pids[] = {terminal_foreground_pgrp(term), terminal_pid(term)};
    for (size_t i = 0; i < ALEN(pids); i++) {
        char name[2 * MAXCOMLEN + 1];
        if (pids[i] > 0 && proc_name(pids[i], name, sizeof(name)) > 0)
            return string(name);
    }
    return @"";
}

/* --match's FIELD:REGEX; the regular expression is searched for in the field */
static bool
matches(const struct terminal *term, NSString *field, NSRegularExpression *regex)
{
    const struct wl_window *win = term->window;
    NSString *value;

    if ([field isEqualToString:@"id"])
        value = [NSString stringWithFormat:@"%u", win->control_id];
    else if ([field isEqualToString:@"title"])
        value = string(win->fixed_title != NULL ? win->fixed_title : term->window_title);
    else if ([field isEqualToString:@"cwd"])
        value = string(term->cwd);
    else if ([field isEqualToString:@"program"])
        value = program_of(term);
    else {
        const char *host = term->session != NULL ? session_link_host(term->session) : NULL;
        value = string(host);
    }

    /* An ID is that one, not one that has it */
    if ([field isEqualToString:@"id"])
        return [value isEqualToString:regex.pattern];

    return [regex firstMatchInString:value options:0
                               range:NSMakeRange(0, value.length)] != nil;
}

/*
 * --id=ID and --match=FIELD:REGEX (or with the value as the next
 * argument), removed from 'args': the terminals that are meant, in the
 * order they were opened in; without these options, the focused one.
 * Returns nil, with an error, if there's none.
 */
static NSArray<NSValue *> *
targets(NSMutableArray<NSString *> *args, NSString **error)
{
    NSMutableArray<NSValue *> *terms = [NSMutableArray array];

    for (NSUInteger i = 0; i < args.count; i++) {
        NSString *option = nil, *value = nil;
        for (NSString *name in @[@"--id", @"--match"]) {
            if ([args[i] hasPrefix:[name stringByAppendingString:@"="]]) {
                option = name;
                value = [args[i] substringFromIndex:name.length + 1];
                [args removeObjectAtIndex:i];
            } else if ([args[i] isEqualToString:name] && i + 1 < args.count) {
                option = name;
                value = args[i + 1];
                [args removeObjectsInRange:NSMakeRange(i, 2)];
            }
            if (option != nil)
                break;
        }
        if (option == nil)
            continue;

        if ([option isEqualToString:@"--id"]) {
            struct terminal *term = terminal_with_id((uint32_t)value.longLongValue);
            if (term == NULL) {
                *error = [NSString stringWithFormat:
                    @"%@: no terminal with this ID (see foot @ ls)", value];
                return nil;
            }
            return @[[NSValue valueWithPointer:term]];
        }

        const NSRange colon = [value rangeOfString:@":"];
        NSString *field = colon.location != NSNotFound ? [value substringToIndex:colon.location] : @"";
        if (![@[@"id", @"title", @"cwd", @"program", @"host"] containsObject:field]) {
            *error = [NSString stringWithFormat:
                @"--match %@: it's FIELD:REGEX, with id, title, cwd, program or host", value];
            return nil;
        }

        NSRegularExpression *regex = [NSRegularExpression
            regularExpressionWithPattern:[value substringFromIndex:colon.location + 1]
                                 options:0 error:NULL];
        if (regex == nil) {
            *error = [NSString stringWithFormat:@"--match %@: not a regular expression", value];
            return nil;
        }

        tll_foreach(control.wayl->terms, it) {
            if (it->item->window != NULL && !it->item->shutdown.in_progress &&
                matches(it->item, field, regex))
            {
                [terms addObject:[NSValue valueWithPointer:it->item]];
            }
        }

        if (terms.count == 0) {
            *error = [NSString stringWithFormat:@"--match %@: no such terminal (see foot @ ls)", value];
            return nil;
        }
        return terms;
    }

    struct terminal *term = focused_terminal();
    if (term == NULL) {
        *error = @"there is no terminal";
        return nil;
    }
    return @[[NSValue valueWithPointer:term]];
}

/* For what one terminal does: the first of them */
static bool
target(NSMutableArray<NSString *> *args, struct terminal **term, NSString **error)
{
    NSArray<NSValue *> *terms = targets(args, error);
    *term = terms != nil ? terms[0].pointerValue : NULL;
    return terms != nil;
}

/* All of them; with --id, or --match, those */
static struct reply
ls(NSMutableArray<NSString *> *args)
{
    NSArray<NSValue *> *only = nil;
    if (args.count > 0) {
        NSString *error;
        only = targets(args, &error);
        if (only == nil)
            return failed(error);
        if (args.count > 0)
            return failed([NSString stringWithFormat:@"ls: %@: unknown option", args[0]]);
    }

    const struct terminal *focused = focused_terminal();
    NSMutableArray *list = [NSMutableArray array];

    tll_foreach(control.wayl->terms, it) {
        const struct terminal *term = it->item;
        const struct wl_window *win = term->window;
        if (win == NULL || term->shutdown.in_progress)
            continue;
        if (only != nil && ![only containsObject:[NSValue valueWithPointer:term]])
            continue;

        NSMutableDictionary *entry = [NSMutableDictionary dictionary];
        entry[@"id"] = @(win->control_id);
        entry[@"window"] = @(((__bridge NSWindow *)win->nswindow).windowNumber);
        entry[@"focused"] = term == focused ? @YES : @NO;
        entry[@"title"] = string(win->fixed_title != NULL ? win->fixed_title : term->window_title);
        entry[@"cwd"] = string(term->cwd);
        entry[@"program"] = program_of(term);
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
    /* All that match */
    NSString *error;
    NSArray<NSValue *> *terms = targets(args, &error);
    if (terms == nil)
        return failed(error);
    if (args.count > 0)
        return failed([NSString stringWithFormat:@"send-text: %@: unknown option", args[0]]);

    bool ok = true;
    for (NSValue *term in terms)
        ok = (data.length == 0 || term_to_slave(term.pointerValue, data.bytes, data.length)) && ok;
    return ok ? succeeded(nil) : failed(@"send-text: the terminal didn't take it");
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
    NSString *error;
    NSArray<NSValue *> *terms = targets(args, &error);
    if (terms == nil)
        return failed(error);

    for (NSValue *term in terms) {
        window_set_fixed_title(((struct terminal *)term.pointerValue)->window,
                               [args componentsJoinedByString:@" "].UTF8String);
    }
    return succeeded(nil);
}

/*
 * launch [OPTIONS] [COMMAND...]: a new terminal, in a window, a tab or
 * a split, next to the terminal that's meant. 'data' is the client's
 * environment (each variable ends with a NUL): the command's, as with
 * footclient. Prints the new terminal's ID.
 */
static struct reply
launch(NSMutableArray<NSString *> *args, NSData *data)
{
    /* The options end where the command begins */
    NSMutableArray<NSString *> *options = [NSMutableArray array];
    while (args.count > 0 && [args[0] hasPrefix:@"--"]) {
        NSString *option = args[0];
        [args removeObjectAtIndex:0];
        if ([option isEqualToString:@"--"])
            break;
        [options addObject:option];

        /* --id ID */
        if (([option isEqualToString:@"--id"] || [option isEqualToString:@"--match"]) &&
            args.count > 0)
        {
            [options addObject:args[0]];
            [args removeObjectAtIndex:0];
        }
    }

    /* Without terminals, there's still a new window */
    struct terminal *near = NULL;
    NSString *error;
    const bool chosen = [options indexOfObjectPassingTest:^BOOL(NSString *o, NSUInteger i, BOOL *s) {
        return [o hasPrefix:@"--id"] || [o hasPrefix:@"--match"];
    }] != NSNotFound;
    if (!target(options, &near, &error) && (chosen || tll_length(control.wayl->terms) > 0))
        return failed(error);

    enum launch_type type = LAUNCH_WINDOW;
    enum split_dir dir = SPLIT_RIGHT;
    NSString *cwd = nil, *client_cwd = nil, *title = nil;
    bool hold = false;

    for (NSString *option in options) {
        NSString *value = [option substringFromIndex:
            MIN(option.length, [option rangeOfString:@"="].location + 1)];

        if ([option isEqualToString:@"--type=window"])
            type = LAUNCH_WINDOW;
        else if ([option isEqualToString:@"--type=tab"])
            type = LAUNCH_TAB;
        else if ([option isEqualToString:@"--type=split"])
            type = LAUNCH_SPLIT;
        else if ([option isEqualToString:@"--direction=right"])
            dir = SPLIT_RIGHT;
        else if ([option isEqualToString:@"--direction=down"])
            dir = SPLIT_DOWN;
        else if ([option isEqualToString:@"--direction=left"])
            dir = SPLIT_LEFT;
        else if ([option isEqualToString:@"--direction=up"])
            dir = SPLIT_UP;
        else if ([option hasPrefix:@"--cwd="])
            cwd = value;
        else if ([option hasPrefix:@"--client-cwd="])
            client_cwd = value;
        else if ([option hasPrefix:@"--title="])
            title = value;
        else if ([option isEqualToString:@"--hold"])
            hold = true;
        else
            return failed([NSString stringWithFormat:@"launch: %@: unknown option", option]);
    }

    if (near == NULL && type != LAUNCH_WINDOW)
        return failed(@"launch: there is no terminal for a tab, or a split");
    if (cwd == nil)
        cwd = client_cwd;

    /* The command, and the client's environment */
    const size_t argc = args.count;
    char **argv = xcalloc(argc + 1, sizeof(argv[0]));
    for (size_t i = 0; i < argc; i++)
        argv[i] = xstrdup(args[i].UTF8String);

    size_t env_count = 0;
    const char *bytes = data.bytes;
    const bool have_env = data.length > 0 && bytes[data.length - 1] == '\0';
    for (size_t i = 0; have_env && i < data.length; i++)
        env_count += bytes[i] == '\0';

    const char **envp = xcalloc(env_count + 1, sizeof(envp[0]));
    for (size_t i = 0, n = 0; have_env && n < env_count; i += strlen(bytes + i) + 1)
        envp[n++] = bytes + i;

    struct terminal *term = app_launch(
        near, type, dir, cwd.length > 0 ? cwd.fileSystemRepresentation : NULL,
        (int)argc, argv, have_env ? envp : NULL, hold);

    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    free(envp);

    if (term == NULL || term->window == NULL) {
        return failed(type == LAUNCH_TAB
            ? @"launch: failed (this window has no tabs?)" : @"launch: failed");
    }

    if (title.length > 0)
        window_set_fixed_title(term->window, title.UTF8String);

    assign_ids();
    return succeeded([[NSString stringWithFormat:@"%u\n", term->window->control_id]
                      dataUsingEncoding:NSUTF8StringEncoding]);
}

/*
 * As the menus' Close, for each of them: asks first, if something runs
 * there; not with --force
 */
static struct reply
close_terminals(NSMutableArray<NSString *> *args)
{
    NSString *error;
    NSArray<NSValue *> *terms = targets(args, &error);
    if (terms == nil)
        return failed(error);
    const bool force = args.count == 1 && [args[0] isEqualToString:@"--force"];
    if (args.count > 0 && !force)
        return failed([NSString stringWithFormat:@"close: %@: unknown option", args[0]]);

    for (NSValue *value in terms) {
        struct terminal *term = value.pointerValue;
        if (force)
            term_shutdown(term);
        else
            window_close(term->window);
    }
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
            if (command.shortcut.length > 0 || command.checked)
                [out appendFormat:@"\t%@", command.shortcut];
            if (command.checked)
                [out appendString:@"\t✓"];  /* As in the menus: it's on, or the one chosen */
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
        return ls(args);
    if ([command isEqualToString:@"send-text"])
        return send_text(args, data);
    if ([command isEqualToString:@"get-text"])
        return get_text(args);
    if ([command isEqualToString:@"focus"])
        return focus(args);
    if ([command isEqualToString:@"set-title"])
        return set_title(args);
    if ([command isEqualToString:@"launch"])
        return launch(args, data);
    if ([command isEqualToString:@"close"])
        return close_terminals(args);
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
