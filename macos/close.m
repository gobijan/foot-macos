#import "app.h"

#include <libproc.h>
#include <string.h>
#include <unistd.h>

#include <sys/param.h>

#include <tllist.h>

#define LOG_MODULE "close"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "terminal.h"
#include "util.h"

/*
 * Closing a window (or quitting) terminates what's running in it. With
 * something other than a shell waiting for a command, ask first
 * (macos-confirm-close). Asked from the processes, so that it works with
 * any shell (no shell integration needed).
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

/* The name of what's running in the terminal, or nil if there's nothing to lose */
static NSString *
running_process(const struct terminal *term)
{
    static const char *const shells[] = {
        "bash", "csh", "dash", "elvish", "fish", "ksh", "nu", "pwsh", "sh",
        "tcsh", "xonsh", "zsh",
    };

    /* They keep running, detached */
    static const char *const multiplexers[] = {"screen", "tmux", "zellij"};

    if (!term->conf->macos.confirm_close || term->shutdown.in_progress || term->ptmx < 0)
        return nil;

    char name[2 * MAXCOMLEN + 1];
    const pid_t pgrp = tcgetpgrp(term->ptmx);
    if (pgrp <= 0 || proc_name(pgrp, name, sizeof(name)) <= 0)
        return nil;

    if (in_list(name, multiplexers, ALEN(multiplexers)))
        return nil;

    /*
     * A shell waiting for a command is the terminal's own, and has its
     * process group for itself. Any other is e.g. running a script.
     */
    pid_t pids[2];
    if (pgrp == term->slave && in_list(name, shells, ALEN(shells)) &&
        proc_listpgrppids(pgrp, pids, sizeof(pids)) == 1)
    {
        return nil;
    }

    return @(name);
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

void
window_close(struct wl_window *win)
{
    struct terminal *term = win->term;
    NSWindow *nswindow = (__bridge NSWindow *)win->nswindow;

    NSString *process = running_process(term);
    if (process == nil) {
        term_shutdown(term);
        return;
    }

    NSAlert *alert = create_alert(@"Close this window?", process, @"Close");

    [alert beginSheetModalForWindow:nswindow completionHandler:^(NSModalResponse response) {
        /* The delegate is gone with the terminal (see wayl_win_destroy()) */
        if (response == NSAlertFirstButtonReturn && nswindow.delegate != nil) {
            term_shutdown(term);
            app_run_hooks();
        }
    }];
}

bool
app_confirm_quit(void)
{
    NSMutableOrderedSet<NSString *> *processes = [NSMutableOrderedSet orderedSet];

    tll_foreach(app_wayland()->terms, it) {
        NSString *process = running_process(it->item);
        if (process != nil)
            [processes addObject:process];
    }

    if (processes.count == 0)
        return true;

    NSString *list = [NSListFormatter localizedStringByJoiningStrings:processes.array];
    return [create_alert(@"Quit foot?", list, @"Quit") runModal] == NSAlertFirstButtonReturn;
}
