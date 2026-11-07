#import "app.h"
#import "split-view.h"
#import "view.h"

#include <libproc.h>
#include <string.h>
#include <unistd.h>

#include <sys/param.h>
#include <sys/sysctl.h>

#include <tllist.h>

#define LOG_MODULE "close"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "session.h"
#include "terminal.h"
#include "util.h"

/*
 * Closing a window (or quitting) terminates what's running in it. With
 * something other than a shell waiting for a command, ask first
 * (macos-confirm-close). Asked from the processes, so that it works with
 * any shell (no shell integration needed). Quitting doesn't terminate
 * sessions (macos/session.h): they keep running.
 */

static bool
in_list(const char *name, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, list[i]) == 0)
            return true;
    }
    return false;
}

/* Unlike proc_name(), this works with other users' processes too (e.g. sudo) */
static bool
process_name(pid_t pid, char name[static MAXCOMLEN + 1])
{
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc info;
    size_t size = sizeof(info);

    if (sysctl(mib, ALEN(mib), &info, &size, NULL, 0) < 0 || size == 0)
        return false;

    strlcpy(name, info.kp_proc.p_comm, MAXCOMLEN + 1);
    return name[0] != '\0';
}

/*
 * The name of what runs in a terminal (its first process is 'pid', its
 * foreground process group 'pgrp'), or nil if there's nothing to lose
 */
static NSString *
running_process_of(pid_t pid, pid_t pgrp)
{
    static const char *const shells[] = {
        "bash", "csh", "dash", "elvish", "fish", "ksh", "nu", "pwsh", "sh",
        "tcsh", "xonsh", "zsh",
    };

    /* They keep running, detached */
    static const char *const multiplexers[] = {"screen", "tmux", "zellij"};

    if (pgrp <= 0)
        return nil;

    pid_t pids[16];
    const int count = proc_listpgrppids(pgrp, pids, sizeof(pids));

    /* The group's leader may be gone, e.g. the first process of a pipeline */
    char name[MAXCOMLEN + 1];
    bool named = process_name(pgrp, name);
    for (int i = 0; !named && i < MIN(count, (int)ALEN(pids)); i++)
        named = process_name(pids[i], name);

    if (!named)
        return count > 0 ? @"A process" : nil;

    if (in_list(name, multiplexers, ALEN(multiplexers)))
        return nil;

    /*
     * A shell waiting for a command is the terminal's own, and has its
     * process group for itself. Any other is e.g. running a script.
     */
    if (pgrp == pid && count == 1 && in_list(name, shells, ALEN(shells)))
        return nil;

    return @(name);
}

static NSString *
running_process(const struct terminal *term)
{
    if (!term->conf->macos.confirm_close || term->shutdown.in_progress || term->ptmx < 0)
        return nil;

    return running_process_of(terminal_pid(term), terminal_foreground_pgrp(term));
}

static NSAlert *
create_alert(NSString *message, NSString *processes, NSString *button)
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = message;
    alert.informativeText = [NSString stringWithFormat:
        @"%@ will be terminated.", processes];
    [alert addButtonWithTitle:button];
    [alert addButtonWithTitle:@"Cancel"];
    return alert;
}

/*
 * Shuts the panes' terminals down, now, or once confirmed (in a sheet
 * on 'nswindow'). The panes may be gone by then: their view's
 * terminal is NULL.
 */
static void
close_panes(NSArray<FootView *> *panes, NSWindow *nswindow, NSString *message,
            struct wl_window *sheet_pane)
{
    NSMutableOrderedSet<NSString *> *processes = [NSMutableOrderedSet orderedSet];
    for (FootView *view in panes) {
        NSString *process = view.term != NULL ? running_process(view.term) : nil;
        if (process != nil)
            [processes addObject:process];
    }

    if (processes.count == 0) {
        for (FootView *view in panes) {
            if (view.term != NULL)
                term_shutdown(view.term);
        }
        return;
    }

    NSString *list = [NSListFormatter localizedStringByJoiningStrings:processes.array];
    NSAlert *alert = create_alert(message, list, @"Close");

    /* A sheet on a tab that isn't selected wouldn't be seen */
    nswindow.tabGroup.selectedWindow = nswindow;

    FootSplitView *splits = [nswindow.contentView isKindOfClass:[FootSplitView class]]
        ? nswindow.contentView : nil;
    splits.sheetPane = sheet_pane;

    [alert beginSheetModalForWindow:nswindow completionHandler:^(NSModalResponse response) {
        if (splits.sheetPane == sheet_pane)
            splits.sheetPane = NULL;

        if (response != NSAlertFirstButtonReturn)
            return;

        for (FootView *view in panes) {
            if (view.term != NULL)
                term_shutdown(view.term);
        }
        app_run_hooks();
    }];
}

void
window_close(struct wl_window *win)
{
    FootSplitView *splits = (__bridge FootSplitView *)win->splits;
    NSString *message = splits.count > 1 ? @"Close this split?" : @"Close this window?";

    close_panes(@[(__bridge FootView *)win->view], (__bridge NSWindow *)win->nswindow,
                message, win);
}

void
window_close_all(NSWindow *window, NSArray<NSWindow *> *windows)
{
    NSMutableArray<FootView *> *panes = [NSMutableArray array];
    for (NSWindow *w in windows) {
        if ([w.contentView isKindOfClass:[FootSplitView class]])
            [panes addObjectsFromArray:((FootSplitView *)w.contentView).panes];
    }

    if (panes.count > 0)
        close_panes(panes, window, @"Close this window?", NULL);
}

bool
app_confirm_quit(bool end_sessions)
{
    NSMutableOrderedSet<NSString *> *processes = [NSMutableOrderedSet orderedSet];

    struct wayland *wayl = app_wayland();
    tll_foreach(wayl->terms, it) {
        /* Sessions keep running (macos/session.h) */
        if (it->item->session != NULL && !end_sessions)
            continue;

        NSString *process = running_process(it->item);
        if (process != nil)
            [processes addObject:process];
    }

    /* And the sessions no window shows (e.g. the quake terminal's, until it's shown) */
    struct session_list sessions;
    if (end_sessions && tll_front(wayl->terms)->conf->macos.confirm_close &&
        sessions_list(wayl->fdm, &sessions))
    {
        for (size_t i = 0; i < sessions.count; i++) {
            const struct session_info *session = &sessions.sessions[i];
            if (session->attached)
                continue;

            NSString *process = running_process_of(
                session->pid, session_foreground_pgrp(session->pid));
            if (process != nil)
                [processes addObject:process];
        }
        session_list_free(&sessions);
    }

    NSString *list = [NSListFormatter localizedStringByJoiningStrings:processes.array];

    /* Everything ends, the shells too: always asked (unless macos-confirm-close=no) */
    if (end_sessions) {
        if (!tll_front(wayl->terms)->conf->macos.confirm_close)
            return true;

        NSAlert *alert = create_alert(@"Quit foot and end all sessions?",
                                      processes.count > 0 ? list : @"Everything running in foot",
                                      @"End Sessions");
        return [alert runModal] == NSAlertFirstButtonReturn;
    }

    if (processes.count == 0)
        return true;

    return [create_alert(@"Quit foot?", list, @"Quit") runModal] == NSAlertFirstButtonReturn;
}
