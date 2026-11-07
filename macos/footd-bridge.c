/*
 * footd --bridge; see footd-bridge.h.
 */

#include "footd-bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
/* The system's, not foot's spawn.h (the project's root is an include directory) */
#if defined(__APPLE__)
 #include <sys/../spawn.h>
#else
 #include <linux/../spawn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#if defined(__APPLE__)
 #include <mach-o/dyld.h>
#endif

#include <tllist.h>

#define LOG_MODULE "bridge"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "fdm.h"
#include "session-mux.h"
#include "session-protocol.h"
#include "xmalloc.h"

extern char **environ;

/* A window's socket handed to footd: kept until footd has answered */
struct pending {
    uint64_t id;
    uint64_t token;
    int fd;
};

static struct {
    struct fdm *fdm;
    struct session_mux *mux;     /* To the other footd (stdin, stdout) */
    struct session_conn footd;   /* To ours */
    bool footd_writing;

    int announced;               /* The announced channel's socket (footd's end) */
    tll(struct pending) pending;

    bool done;
} bridge = {.announced = -1};

/*
 * Starting footd
 */

static char *
own_path(void)
{
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return NULL;
    return realpath(buf, NULL);
#else
    return realpath("/proc/self/exe", NULL);
#endif
}

/* footd, as a daemon: it outlives the bridge (and the SSH connection) */
static bool
spawn_footd(int listen_fd, const char *socket_path, void *data)
{
    char *path = own_path();
    if (path == NULL) {
        LOG_ERRNO("failed to find footd's executable");
        return false;
    }

    char *socket_arg = xasprintf("--socket=%s", socket_path);
    char *const argv[] = {path, "--listen-fd=3", socket_arg, NULL};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawn_file_actions_adddup2(&actions, listen_fd, 3);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(
        &attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    sigset_t mask;
    sigemptyset(&mask);
    posix_spawnattr_setsigmask(&attr, &mask);
    sigset_t defaults;
    sigfillset(&defaults);
    posix_spawnattr_setsigdefault(&attr, &defaults);

    pid_t pid;
    const int err = posix_spawn(&pid, path, &actions, &attr, argv, environ);

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    free(socket_arg);
    free(path);

    if (err != 0) {
        errno = err;
        LOG_ERRNO("failed to start footd");
        return false;
    }

    LOG_INFO("started footd (PID=%d)", pid);
    return true;
}

/*
 * A session for another machine: it gets this one's environment, its
 * user's shell, configuration and home, not the window's (they're the
 * other machine's)
 */

/* Whether there's a terminfo entry for 'term' here */
static bool
have_terminfo(const char *term)
{
    if (term[0] == '\0' || strchr(term, '/') != NULL)
        return false;

    const char *home = getenv("HOME");
    const char *env = getenv("TERMINFO");
    const char *dirs[] = {
        env != NULL ? env : "",
        home != NULL ? home : "",
        "/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo", "/usr/lib/terminfo",
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (dirs[i][0] == '\0')
            continue;

        const char *sub = i == 1 ? "/.terminfo" : "";
        char path[PATH_MAX];

        /* By first letter, or (macOS) its hex code */
        snprintf(path, sizeof(path), "%s%s/%c/%s", dirs[i], sub, term[0], term);
        if (access(path, R_OK) == 0)
            return true;
        snprintf(path, sizeof(path), "%s%s/%02x/%s", dirs[i], sub, (unsigned char)term[0], term);
        if (access(path, R_OK) == 0)
            return true;
    }
    return false;
}

static void
add_create(struct session_msg *out, struct session_reader *r)
{
    const uint64_t id = session_read_u64(r);
    const uint64_t token = session_read_u64(r);
    const struct session_size size = session_read_size(r);
    char *cwd = session_read_str(r);
    char *conf_path = session_read_str(r);
    char *term = session_read_str(r);
    char *shell = session_read_str(r);
    const bool login_shell = session_read_u8(r);
    const uint32_t scrollback = session_read_u32(r);
    char **argv = session_read_strv(r);
    char **envp = session_read_strv(r);

    /* Without terminfo for foot here, programs would be lost */
    const char *term_here =
        term != NULL && have_terminfo(term) ? term : "xterm-256color";

    const char *home = getenv("HOME");
    if (home == NULL) {
        const struct passwd *pw = getpwuid(getuid());
        home = pw != NULL ? pw->pw_dir : "/";
    }

    session_msg_u64(out, id);
    session_msg_u64(out, token);
    session_msg_size(out, &size);
    session_msg_str(out, home);
    session_msg_str(out, NULL);          /* This user's foot.ini */
    session_msg_str(out, term_here);
    session_msg_str(out, NULL);          /* This user's shell */
    session_msg_u8(out, login_shell);
    session_msg_u32(out, scrollback);
    session_msg_strv(out, (const char *const *)argv);
    session_msg_strv(out, (const char *const *)environ);

    free(cwd);
    free(conf_path);
    free(term);
    free(shell);
    session_strv_free(argv);
    session_strv_free(envp);
}

/*
 * Messages
 */

static void
footd_update(void)
{
    const bool writing = session_conn_has_output(&bridge.footd);
    if (writing != bridge.footd_writing) {
        bridge.footd_writing = writing;
        if (writing)
            fdm_event_add(bridge.fdm, bridge.footd.fd, EPOLLOUT);
        else
            fdm_event_del(bridge.fdm, bridge.footd.fd, EPOLLOUT);
    }
}

static bool
to_footd(struct session_msg *msg, int fd)
{
    if (!session_conn_send(&bridge.footd, msg, fd))
        return false;
    footd_update();
    return true;
}

/* From the other footd: on to ours */
static bool
mux_message(struct session_mux *mux, uint32_t type, struct session_reader *r, void *data)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, type);

    if (type == SESSION_MSG_CREATE || type == SESSION_MSG_ATTACH) {
        struct session_reader peek = *r;
        const uint64_t id = session_read_u64(&peek);
        const uint64_t token = session_read_u64(&peek);

        const int fd = bridge.announced;
        bridge.announced = -1;
        if (!peek.ok || fd < 0) {
            LOG_ERR("CREATE/ATTACH without a channel");
            session_msg_free(&msg);
            if (fd >= 0)
                close(fd);
            return false;
        }

        if (type == SESSION_MSG_CREATE)
            add_create(&msg, r);
        else
            session_msg_raw(&msg, r->p, r->end - r->p);

        const bool ok = r->ok && to_footd(&msg, fd);
        session_msg_free(&msg);
        if (!ok) {
            close(fd);
            return false;
        }

        /* Until footd has answered (session_conn_send()) */
        tll_push_back(bridge.pending, ((struct pending){.id = id, .token = token, .fd = fd}));
        return true;
    }

    session_msg_raw(&msg, r->p, r->end - r->p);
    const bool ok = to_footd(&msg, -1);
    session_msg_free(&msg);
    return ok;
}

/* A channel for the next CREATE or ATTACH: a socket, its other end for footd */
static void
mux_announced(struct session_mux *mux, uint32_t channel, void *data)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        LOG_ERRNO("channel %u: failed to create a socket", channel);
        return;
    }

    const int buffer_size = 512 * 1024;
    for (size_t i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
        setsockopt(fds[i], SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    }

    if (bridge.announced >= 0)
        close(bridge.announced);
    bridge.announced = fds[1];

    session_mux_add(mux, channel, fds[0]);
}

static void
mux_closed(struct session_mux *mux, void *data)
{
    LOG_INFO("the other footd is gone");
    bridge.done = true;
}

static const struct session_mux_callbacks mux_callbacks = {
    .message = &mux_message,
    .announced = &mux_announced,
    .closed = &mux_closed,
};

/* footd has answered: the window's socket is its own now */
static void
answered(uint64_t id, uint64_t token)
{
    tll_foreach(bridge.pending, it) {
        if (it->item.id == id && it->item.token == token) {
            close(it->item.fd);
            tll_remove(bridge.pending, it);
            return;
        }
    }
}

/* From ours: on to the other footd */
static bool
fdm_footd(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLOUT) {
        if (!session_conn_flush(&bridge.footd)) {
            bridge.done = true;
            return true;
        }
        footd_update();
    }

    if (events & (EPOLLIN | EPOLLHUP)) {
        bool eof;
        bool ok = session_conn_read(&bridge.footd, &eof);

        uint32_t type;
        struct session_reader r;
        bool error;
        while (session_conn_next(&bridge.footd, &type, &r, &error)) {
            if (type == SESSION_MSG_STARTED || type == SESSION_MSG_FAILED) {
                struct session_reader peek = r;
                const uint64_t id = session_read_u64(&peek);
                const uint64_t token = session_read_u64(&peek);
                if (peek.ok)
                    answered(id, token);
            }

            struct session_msg msg = {0};
            session_msg_begin(&msg, type);
            session_msg_raw(&msg, r.p, r.end - r.p);
            session_mux_send(bridge.mux, &msg);
            session_msg_free(&msg);
        }

        if (!ok || error) {
            LOG_INFO("footd is gone");
            bridge.done = true;
        }
    }

    return true;
}

int
footd_bridge(const char *socket_path)
{
    int ret = EXIT_FAILURE;

    char *path = socket_path != NULL ? xstrdup(socket_path) : session_socket_path();
    if (path == NULL) {
        LOG_ERR("no socket for footd");
        return ret;
    }

    int fd = session_footd_connect(path, &spawn_footd, NULL);
    if (fd < 0) {
        LOG_ERRNO("%s: failed to connect to footd", path);
        free(path);
        return ret;
    }
    free(path);

    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    {
        LOG_ERRNO("failed to configure the connection to footd");
        close(fd);
        return ret;
    }
    session_conn_init(&bridge.footd, fd);

    if ((bridge.fdm = fdm_init()) == NULL)
        goto out;

    if (!fdm_add(bridge.fdm, fd, EPOLLIN, &fdm_footd, NULL))
        goto out;

    /* stdin, stdout: SSH's (dup:ed: what's logged mustn't end up in the stream) */
    const int in = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    const int out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (in < 0 || out < 0) {
        LOG_ERRNO("failed to take over stdin/stdout");
        goto out;
    }
    const int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        close(null);
    }

    bridge.mux = session_mux_new(bridge.fdm, in, out, &mux_callbacks, NULL);
    if (bridge.mux == NULL)
        goto out;

    while (!bridge.done) {
        if (!fdm_poll(bridge.fdm))
            break;
    }
    ret = EXIT_SUCCESS;

out:
    session_mux_destroy(bridge.mux);
    tll_foreach(bridge.pending, it) {
        close(it->item.fd);
        tll_remove(bridge.pending, it);
    }
    if (bridge.announced >= 0)
        close(bridge.announced);
    if (bridge.fdm != NULL) {
        fdm_del_no_close(bridge.fdm, bridge.footd.fd);
        fdm_destroy(bridge.fdm);
    }
    session_conn_close(&bridge.footd);
    return ret;
}
