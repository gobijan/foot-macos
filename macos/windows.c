/*
 * Additional terminal windows in this process (Cmd+N, the Dock), also
 * running a script (Open With), opening the configuration in an editor
 * (Cmd+,), and the quake terminal (Shell > Show Quake Terminal, the
 * Dock, the hotkey).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <tllist.h>

#define LOG_MODULE "windows"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "client-protocol.h"
#include "config.h"
#include "debug.h"
#include "terminal.h"
#include "tokenize.h"
#include "util.h"
#include "wayland.h"
#include "windows.h"
#include "xmalloc.h"

#include "default-config.h"

struct new_window_context {
    const struct config *conf;
    const char *conf_path;          /* -c,--config, or NULL */

    struct fdm *fdm;
    struct reaper *reaper;
    struct wayland *wayl;
};

static struct new_window_context context;

/* The command line's (-o etc.): they're the first window's only */
static config_override_t initial_overrides = tll_init();

void
wayl_keep_config_overrides(config_override_t *overrides)
{
    tll_foreach(*overrides, it)
        tll_push_back(initial_overrides, xstrdup(it->item));
}

struct config *
windows_initial_config(void)
{
    return (struct config *)context.conf;
}

void
windows_apply_overrides(struct config *conf)
{
    /* Applying them takes them apart (key and value): copies */
    config_override_t overrides = tll_init();
    tll_foreach(initial_overrides, it)
        tll_push_back(overrides, xstrdup(it->item));

    config_override_apply(conf, &overrides, false);
    tll_free_and_free(overrides, free);
}

/* The configuration, as new windows load it */
bool
windows_load_config(struct config *conf)
{
    user_notifications_t notifications = tll_init();
    config_override_t overrides = tll_init();

    bool ret = config_load(conf, context.conf_path, &notifications, &overrides,
                           false, false);
    user_notifications_free(&notifications);
    return ret;
}

static void
window_config_free(void *data, int exit_code)
{
    struct config *conf = data;
    config_free(conf);
    free(conf);
}

/*
 * Additional windows (application menu, dock), in this process. The
 * configuration is re-loaded for each window, so that changes apply
 * without restarting foot (command line overrides only apply to the
 * first window).
 */
static bool
new_terminal(struct new_window_context *ctx, const char *cwd,
             int argc, char *const *argv, const char *const *envp, bool hold)
{
    if (cwd == NULL)
        cwd = getenv("HOME");
    if (cwd == NULL)
        cwd = "/";

    struct config *conf = xmalloc(sizeof(*conf));

    if (!windows_load_config(conf)) {
        config_free(conf);
        free(conf);
        conf = config_clone(ctx->conf);
    }

    conf->hold_at_exit = conf->hold_at_exit || hold;

    /* The terminal destroys itself when done; then we free its config */
    if (term_init(conf, ctx->fdm, ctx->reaper, ctx->wayl, "foot", cwd, NULL,
                  NULL, argc, argv, envp, &window_config_free, conf) == NULL)
    {
        window_config_free(conf, 0);
        return false;
    }

    return true;
}

static bool
new_window(void *data, const char *cwd)
{
    return new_terminal(data, cwd, 0, NULL, NULL, false);
}

/* foot @ launch: a command (none: the shell), with an environment (NULL: ours) */
static bool
launch(void *data, const char *cwd, int argc, char *const *argv,
       const char *const *envp, bool hold)
{
    return new_terminal(data, cwd, argc, argv, envp, hold);
}

/*
 * Toggles the quake terminal, in the home directory. Its macos-quake-*
 * options are re-loaded each time, so that changes apply right away.
 */
static void
toggle_quake(void *data)
{
    struct new_window_context *ctx = data;
    struct config *conf = xmalloc(sizeof(*conf));

    if (!windows_load_config(conf)) {
        config_free(conf);
        free(conf);
        conf = config_clone(ctx->conf);
    }

    const char *home = getenv("HOME");
    config_override_t overrides = tll_init();

    if (wayl_quake(ctx->wayl, ctx->fdm, ctx->reaper, conf, CLIENT_QUAKE_TOGGLE,
                   &overrides, home != NULL ? home : "/", 0, NULL, NULL) < 0)
    {
        LOG_ERR("failed to open the quake terminal");
    }

    config_free(conf);
    free(conf);
}

static char *
settings_path(const struct new_window_context *ctx)
{
    if (ctx->conf_path != NULL)
        return xstrdup(ctx->conf_path);

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home != NULL && xdg_config_home[0] != '\0')
        return xstrjoin(xdg_config_home, "/foot/foot.ini");

    const char *home = getenv("HOME");
    return home != NULL ? xstrjoin(home, "/.config/foot/foot.ini") : NULL;
}

char *
windows_settings_path(void)
{
    return settings_path(&context);
}

/* Creates the parent directories of 'path' */
static bool
create_parent_dirs(const char *path)
{
    char *copy = xstrdup(path);

    for (char *p = strchr(copy + 1, '/'); p != NULL; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(copy, 0755) < 0 && errno != EEXIST) {
            LOG_ERRNO("%s: failed to create directory", copy);
            free(copy);
            return false;
        }
        *p = '/';
    }

    free(copy);
    return true;
}

/* Creates the configuration, from the documented defaults, if it doesn't exist */
bool
windows_create_settings(const char *path)
{
    if (access(path, F_OK) == 0)
        return true;

    if (!create_parent_dirs(path))
        return false;

    FILE *f = fopen(path, "wx");
    if (f == NULL) {
        LOG_ERRNO("%s: failed to create", path);
        return false;
    }

    fputs(default_config, f);
    fclose(f);
    return true;
}

/*
 * $VISUAL, or $EDITOR. When started from Finder/Dock, these aren't in
 * our environment; ask the user's login shell (works with POSIX
 * shells, and fish).
 */
static char *
get_editor(const char *shell)
{
    const char *editor = getenv("VISUAL");
    if (editor == NULL || editor[0] == '\0')
        editor = getenv("EDITOR");
    if (editor != NULL && editor[0] != '\0')
        return xstrdup(editor);

    char *cmd = xasprintf(
        "'%s' -l -c 'printf \"%%s\\n%%s\" \"$VISUAL\" \"$EDITOR\"' 2>/dev/null </dev/null",
        shell);
    FILE *f = popen(cmd, "r");
    free(cmd);

    char visual[1024] = {0};
    char editor_buf[1024] = {0};

    if (f != NULL) {
        if (fgets(visual, sizeof(visual), f) != NULL)
            (void)!fgets(editor_buf, sizeof(editor_buf), f);
        pclose(f);
    }

    visual[strcspn(visual, "\n")] = '\0';
    editor_buf[strcspn(editor_buf, "\n")] = '\0';

    return xstrdup(visual[0] != '\0' ? visual :
                   editor_buf[0] != '\0' ? editor_buf : "vi");
}

/*
 * In the user's login shell (e.g. for $PATH), as Terminal.app; without
 * it, if it wouldn't take the quoted path for a command to run.
 */
static bool
run_script(void *data, const char *path)
{
    struct new_window_context *ctx = data;

    char **shell_argv = NULL;
    if (!tokenize_cmdline(ctx->conf->shell, &shell_argv) || shell_argv[0] == NULL) {
        free(shell_argv);
        return false;
    }

    const char *shell = strrchr(shell_argv[0], '/');
    shell = shell != NULL ? shell + 1 : shell_argv[0];

    const bool use_shell =
        strpbrk(path, "'\\") == NULL &&
        strcmp(shell, "nu") != 0 && strcmp(shell, "pwsh") != 0 &&
        strcmp(shell, "elvish") != 0 && strcmp(shell, "xonsh") != 0;

    char *cmd = xasprintf("'%s'", path);
    char *shell_cmd[] = {shell_argv[0], "-l", "-c", cmd, NULL};
    char *direct_cmd[] = {(char *)path, NULL};

    char *dir = xstrdup(path);
    *strrchr(dir, '/') = '\0';

    bool ret = new_terminal(
        ctx, dir[0] != '\0' ? dir : "/", use_shell ? 4 : 1,
        use_shell ? shell_cmd : direct_cmd, NULL, true);

    free(dir);
    free(cmd);
    for (char **a = shell_argv; *a != NULL; a++)
        free(*a);
    free(shell_argv);
    return ret;
}

/*
 * 'path' in the pager, in a new window, in the user's login shell (e.g.
 * for $PATH, $LESS); the file is removed when the pager exits. The
 * quoting is one that fish reads as POSIX shells do.
 */
static bool
open_pager(void *data, const char *command, const char *path)
{
    struct new_window_context *ctx = data;

    if (strchr(path, '\'') != NULL || strchr(command, '\'') != NULL) {
        LOG_ERR("macos-pager: a command, or a file, with a single quote: %s, %s", command, path);
        return false;
    }

    char **shell_argv = NULL;
    if (!tokenize_cmdline(ctx->conf->shell, &shell_argv) || shell_argv[0] == NULL) {
        free(shell_argv);
        return false;
    }

    char *cmd = xasprintf("/bin/sh -c '%s \"$1\"; rm -f \"$1\"' sh '%s'", command, path);
    char *argv[] = {shell_argv[0], "-l", "-c", cmd, NULL};

    bool ret = new_terminal(ctx, NULL, 4, argv, NULL, false);

    free(cmd);
    for (char **a = shell_argv; *a != NULL; a++)
        free(*a);
    free(shell_argv);
    return ret;
}

/*
 * How an editor is told where in a file to begin: by its command's
 * name. The last one is for all others: the line as vi takes it, and
 * most do.
 */
static const struct editor {
    const char *names;   /* Each between spaces */
    const char *option;  /* What's before the line */
    const char *column;  /* What's between the line and the column; NULL: the line only */
    bool after_path;     /* 'a.c:12:5', not: +12:5 'a.c' */
} editors[] = {
    {" code code-insiders codium cursor windsurf ", "-g ", ":", true},
    {" hx subl zed ", "", ":", true},
    {" emacs emacsclient kak micro ", "+", ":", false},
    {" nano ", "+", ",", false},
    {" mate xed ", "-l ", NULL, false},
    {"", "+", NULL, false},
};

/*
 * The shell's command for 'editor' ($EDITOR: a command, maybe with
 * options) with a file, at 'line' and 'column' (0: none)
 */
static char *
editor_command(const char *editor, const char *path, int line, int column)
{
    if (line <= 0)
        return xasprintf("%s '%s'", editor, path);

    const char *end = editor + strcspn(editor, " ");
    const char *name = end;
    while (name > editor && name[-1] != '/')
        name--;

    char *key = xasprintf(" %.*s ", (int)(end - name), name);
    const struct editor *e = editors;
    while (e->names[0] != '\0' && strstr(e->names, key) == NULL)
        e++;
    free(key);

    char *at = column > 0 && e->column != NULL
        ? xasprintf("%d%s%d", line, e->column, column)
        : xasprintf("%d", line);
    char *cmd = e->after_path
        ? xasprintf("%s %s'%s:%s'", editor, e->option, path, at)
        : xasprintf("%s %s%s '%s'", editor, e->option, at, path);
    free(at);
    return cmd;
}

UNITTEST
{
    static const char *const expected[][2] = {
        {"vi", "vi +12 'a b.c'"},
        {"/opt/homebrew/bin/nvim -u NONE", "/opt/homebrew/bin/nvim -u NONE +12 'a b.c'"},
        {"hx", "hx 'a b.c:12:5'"},
        {"code -w", "code -w -g 'a b.c:12:5'"},
        {"/usr/local/bin/subl -w", "/usr/local/bin/subl -w 'a b.c:12:5'"},
        {"emacsclient -t", "emacsclient -t +12:5 'a b.c'"},
        {"nano", "nano +12,5 'a b.c'"},
        {"mate -w", "mate -w -l 12 'a b.c'"},
    };

    for (size_t i = 0; i < ALEN(expected); i++) {
        char *cmd = editor_command(expected[i][0], "a b.c", 12, 5);
        xassert(streq(cmd, expected[i][1]));
        free(cmd);
    }

    char *cmd = editor_command("hx", "a.c", 12, 0);
    xassert(streq(cmd, "hx 'a.c:12'"));
    free(cmd);

    cmd = editor_command("hx", "a.c", 0, 0);
    xassert(streq(cmd, "hx 'a.c'"));
    free(cmd);
}

/*
 * Opens a file (its absolute path) in $EDITOR, in a new window, in the
 * file's folder; at 'line' and 'column', if they aren't 0
 */
static bool
open_editor(void *data, const char *path, int line, int column)
{
    struct new_window_context *ctx = data;

    if (path[0] != '/' || strchr(path, '\'') != NULL) {
        LOG_ERR("%s: not a path the editor is started with", path);
        return false;
    }

    /* The editor runs in the user's login shell (e.g. for $PATH) */
    char **shell_argv = NULL;
    if (!tokenize_cmdline(ctx->conf->shell, &shell_argv) || shell_argv[0] == NULL) {
        free(shell_argv);
        return false;
    }

    char *editor = get_editor(shell_argv[0]);
    char *cmd = editor_command(editor, path, line, column);
    char *argv[] = {shell_argv[0], "-l", "-c", cmd, NULL};

    char *dir = xstrdup(path);
    *strrchr(dir, '/') = '\0';

    LOG_INFO("opening in the editor: %s", cmd);
    bool ret = new_terminal(ctx, dir[0] != '\0' ? dir : "/", 4, argv, NULL, false);

    free(dir);
    free(cmd);
    free(editor);
    for (char **a = shell_argv; *a != NULL; a++)
        free(*a);
    free(shell_argv);
    return ret;
}

void
wayl_enable_new_windows(struct wayland *wayl, const struct config *conf,
                        const char *conf_path, struct fdm *fdm,
                        struct reaper *reaper)
{
    context = (struct new_window_context){
        .conf = conf,
        .conf_path = conf_path,
        .fdm = fdm,
        .reaper = reaper,
        .wayl = wayl,
    };

    wayl_set_new_window_handler(wayl, &new_window, &context);
    wayl_set_run_script_handler(wayl, &run_script, &context);
    wayl_set_editor_handler(wayl, &open_editor, &context);
    wayl_set_pager_handler(wayl, &open_pager, &context);
    wayl_set_launch_handler(wayl, &launch, &context);
    wayl_set_quake_handler(wayl, &toggle_quake, &context,
                           conf->macos.quake.hotkey.sym,
                           conf->macos.quake.hotkey.mods);
}
