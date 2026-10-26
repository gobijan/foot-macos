/*
 * Additional terminal windows in this process (Cmd+N, the Dock),
 * opening the configuration in an editor (Cmd+,), and the quake
 * terminal (Shell > Show Quake Terminal, the Dock, the hotkey).
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
#include "terminal.h"
#include "tokenize.h"
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
             int argc, char *const *argv)
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

    /* The terminal destroys itself when done; then we free its config */
    if (term_init(conf, ctx->fdm, ctx->reaper, ctx->wayl, "foot", cwd, NULL,
                  NULL, argc, argv, NULL, &window_config_free, conf) == NULL)
    {
        window_config_free(conf, 0);
        return false;
    }

    return true;
}

static bool
new_window(void *data, const char *cwd)
{
    return new_terminal(data, cwd, 0, NULL);
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

/* Opens the configuration in $EDITOR, in a new window */
static bool
open_settings(void *data)
{
    struct new_window_context *ctx = data;

    char *path = settings_path(ctx);
    if (path == NULL)
        return false;

    if (strchr(path, '\'') != NULL) {
        LOG_ERR("%s: unsupported configuration path", path);
        free(path);
        return false;
    }

    windows_create_settings(path);

    /* The editor runs in the user's login shell (e.g. for $PATH) */
    char **shell_argv = NULL;
    if (!tokenize_cmdline(ctx->conf->shell, &shell_argv) || shell_argv[0] == NULL) {
        free(shell_argv);
        free(path);
        return false;
    }

    char *editor = get_editor(shell_argv[0]);
    char *cmd = xasprintf("%s '%s'", editor, path);
    char *argv[] = {shell_argv[0], "-l", "-c", cmd, NULL};

    char *dir = xstrdup(path);
    *strrchr(dir, '/') = '\0';

    LOG_INFO("opening settings: %s", cmd);
    bool ret = new_terminal(ctx, dir, 4, argv);

    free(dir);
    free(cmd);
    free(editor);
    for (char **a = shell_argv; *a != NULL; a++)
        free(*a);
    free(shell_argv);
    free(path);
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
    wayl_set_settings_handler(wayl, &open_settings, &context);
    wayl_set_quake_handler(wayl, &toggle_quake, &context,
                           conf->macos.quake.hotkey.sym,
                           conf->macos.quake.hotkey.mods);
}
