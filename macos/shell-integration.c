#include "shell-integration.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#define LOG_MODULE "shell-integration"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "util.h"
#include "xmalloc.h"

#include "shell-integration-bash.h"
#include "shell-integration-fish.h"
#include "shell-integration-zsh.h"

static const struct script {
    const char *path;
    const char *text;
    size_t len;
} scripts[] = {
    {"zsh/.zshenv", shell_integration_zsh, sizeof(shell_integration_zsh) - 1},
    {"bash/foot.bash", shell_integration_bash, sizeof(shell_integration_bash) - 1},
    {"fish/vendor_conf.d/foot-shell-integration.fish",
     shell_integration_fish, sizeof(shell_integration_fish) - 1},
};

/* The scripts' folder; NULL: no integration */
static char *dir;

/* FNV-1a, of the scripts and their paths */
static uint32_t
scripts_hash(void)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < ALEN(scripts); i++) {
        for (const char *p = scripts[i].path; *p != '\0'; p++)
            hash = (hash ^ (uint8_t)*p) * 16777619u;
        for (size_t j = 0; j < scripts[i].len; j++)
            hash = (hash ^ (uint8_t)scripts[i].text[j]) * 16777619u;
    }
    return hash;
}

static char *
scripts_dir(void)
{
    const char *home = getenv("HOME");

#if defined(__APPLE__)
    if (home == NULL || home[0] != '/')
        return NULL;
    return xasprintf("%s/Library/Application Support/foot/shell-integration/%08x",
                     home, scripts_hash());
#else
    /* Where footd's socket is (session_socket_path()) */
    const char *state = getenv("XDG_STATE_HOME");
    if (state != NULL && state[0] == '/')
        return xasprintf("%s/foot/shell-integration/%08x", state, scripts_hash());
    if (home == NULL || home[0] != '/')
        return NULL;
    return xasprintf("%s/.local/state/foot/shell-integration/%08x", home, scripts_hash());
#endif
}

/* The folders of a file */
static bool
make_parents(char *path)
{
    for (char *p = path + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;

        *p = '\0';
        const bool ok = mkdir(path, 0755) == 0 || errno == EEXIST;
        *p = '/';

        if (!ok) {
            LOG_ERRNO("%s: failed to create the folder of", path);
            return false;
        }
    }
    return true;
}

/* All of it, or nothing: another foot (or footd) may write it too */
static bool
write_script(char *path, const struct script *script)
{
    if (!make_parents(path))
        return false;

    char *tmp = xasprintf("%s.%ld.tmp", path, (long)getpid());
    bool ok = false;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t done = 0;
        while (done < script->len) {
            const ssize_t n = write(fd, script->text + done, script->len - done);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            done += n;
        }

        ok = done == script->len;
        ok = close(fd) == 0 && ok;
        ok = ok && rename(tmp, path) == 0;
    }

    if (!ok) {
        LOG_ERRNO("%s: failed to write", path);
        unlink(tmp);
    }

    free(tmp);
    return ok;
}

void
shell_integration_prepare(bool enabled)
{
    free(dir);
    dir = NULL;

    if (!enabled)
        return;

    char *base = scripts_dir();
    if (base == NULL)
        return;

    /* Each time: they may have been removed since */
    for (size_t i = 0; i < ALEN(scripts); i++) {
        char *path = xasprintf("%s/%s", base, scripts[i].path);
        const bool ok = access(path, R_OK) == 0 || write_script(path, &scripts[i]);
        free(path);

        if (!ok) {
            free(base);
            return;
        }
    }

    dir = base;
}

static const char *
env_get(char *const *env, const char *name)
{
    const size_t len = strlen(name);
    for (; env != NULL && *env != NULL; env++) {
        if (strncmp(*env, name, len) == 0 && (*env)[len] == '=')
            return *env + len + 1;
    }
    return NULL;
}

#if defined(__APPLE__)
/* macOS' own bash: it doesn't read $ENV in POSIX mode */
static bool
is_system_bash(const char *shell, char *const *env)
{
    char resolved[PATH_MAX];

    if (strchr(shell, '/') != NULL)
        return realpath(shell, resolved) == NULL || streq(resolved, "/bin/bash");

    /* The one execvp() finds */
    const char *search = env_get(env, "PATH");
    if (search == NULL)
        search = "/usr/bin:/bin";

    while (true) {
        const char *end = strchr(search, ':');
        if (end == NULL)
            end = search + strlen(search);
        char *path = end > search
            ? xasprintf("%.*s/%s", (int)(end - search), search, shell)
            : xstrdup(shell);
        const bool found = access(path, X_OK) == 0;
        const bool is_system = found &&
            (realpath(path, resolved) == NULL || streq(resolved, "/bin/bash"));
        free(path);

        if (found)
            return is_system;
        if (*end == '\0')
            return true;
        search = end + 1;
    }
}
#endif

size_t
shell_integration_apply(
    char ***argv, char *const *env,
    struct shell_integration_var vars[static SHELL_INTEGRATION_MAX_VARS])
{
    if (dir == NULL)
        return 0;

    char **args = *argv;
    const char *shell = args[0];
    const char *name = strrchr(shell, '/');
    name = name != NULL ? name + 1 : shell;

    size_t argc = 0;
    while (args[argc] != NULL)
        argc++;

    size_t n = 0;

    if (streq(name, "zsh")) {
        /* Without startup files, it wouldn't put ZDOTDIR back */
        for (size_t i = 1; i < argc; i++) {
            if (streq(args[i], "-f") || streq(args[i], "--no-rcs"))
                return 0;
        }

        const char *zdotdir = env_get(env, "ZDOTDIR");
        if (zdotdir != NULL)
            vars[n++] = (struct shell_integration_var){"FOOT_ZSH_ZDOTDIR", xstrdup(zdotdir)};
        vars[n++] = (struct shell_integration_var){"ZDOTDIR", xasprintf("%s/zsh", dir)};
    }

    else if (streq(name, "bash")) {
        /* An interactive one only: nothing to run, no other startup files */
        for (size_t i = 1; i < argc; i++) {
            if (!streq(args[i], "-l") && !streq(args[i], "--login") && !streq(args[i], "-i"))
                return 0;
        }

#if defined(__APPLE__)
        if (is_system_bash(shell, env))
            return 0;
#endif

        /* Its long options come first */
        char **posix = xmalloc((argc + 2) * sizeof(posix[0]));
        posix[0] = args[0];
        posix[1] = xstrdup("--posix");
        for (size_t i = 1; i <= argc; i++)
            posix[i + 1] = args[i];
        *argv = posix;

        const char *bash_env = env_get(env, "ENV");
        if (bash_env != NULL)
            vars[n++] = (struct shell_integration_var){"FOOT_BASH_ENV", xstrdup(bash_env)};
        vars[n++] = (struct shell_integration_var){"ENV", xasprintf("%s/bash/foot.bash", dir)};
        vars[n++] = (struct shell_integration_var){"FOOT_BASH_INJECT", xstrdup("1")};
    }

    else if (streq(name, "fish")) {
        const char *data_dirs = env_get(env, "XDG_DATA_DIRS");
        if (data_dirs != NULL && data_dirs[0] != '\0') {
            vars[n++] = (struct shell_integration_var){
                "XDG_DATA_DIRS", xasprintf("%s:%s", dir, data_dirs)};
        } else {
            vars[n++] = (struct shell_integration_var){"XDG_DATA_DIRS", xstrdup(dir)};
            vars[n++] = (struct shell_integration_var){"FOOT_FISH_XDG_UNSET", xstrdup("1")};
        }
        vars[n++] = (struct shell_integration_var){"FOOT_FISH_XDG_DATA_DIR", xstrdup(dir)};
    }

    return n;
}
