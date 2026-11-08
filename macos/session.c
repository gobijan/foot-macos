/*
 * Persistent sessions: the windows' side. See session.h, and footd.c.
 *
 * foot has one control connection to footd, and each terminal a socket
 * of its own (one end is the terminal's, the other footd's). footd is
 * started when there's none; the connection is kept while foot runs,
 * which keeps footd running too.
 */

#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * The system's spawn.h: foot has one of its own, found first (the
 * project's root is an include directory). sys/ is only in the SDK's.
 */
#include <sys/../spawn.h>

#include <mach-o/dyld.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <tllist.h>

#define LOG_MODULE "session"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "fdm.h"
#include "render.h"
#include "session-protocol.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

extern char **environ;

/* How long foot waits for footd's answer, when it has to */
#define LIST_TIMEOUT_MS 3000

struct session_link {
    struct terminal *term;
    uint64_t id;
    uint64_t token;         /* This attachment's, for footd to tell windows apart */

    char pty_path[32];      /* The terminal's end, to open: /dev/fd/N */
    int window_fd;          /* Until the terminal has opened it */
    int footd_fd;           /* footd's end: until footd has answered */

    bool attach;            /* To an existing session, not a new one */
    bool held;              /* Not attached before sessions_release() */
    bool sent;              /* CREATE or ATTACH */
    bool started;           /* footd answered */
    bool detached;          /* It ended, or another window took over */

    struct session_size size;
    pid_t pid;

    /* What runs in it (CREATE) */
    char *cwd;
    char **argv;
    char **envp;
    char *conf_path;
    char *term_env;
    char *shell;
    bool login_shell;
    uint32_t scrollback_lines;

    char *host;             /* On another machine (over SSH); NULL: here */
};

static struct {
    bool enabled;
    struct fdm *fdm;

    struct session_conn conn;   /* conn.fd < 0: not connected */
    bool writing;

    tll(struct session_link *) links;
    bool detaching;

    /* sessions_attach_next() */
    uint64_t attach_id;
    bool attach_hold;

    /* sessions_create_next_on() */
    char *next_host;

    uint32_t features;      /* footd's (WELCOME) */

    /* sessions_uninstall(): told once footd has answered */
    void (*uninstalled)(const char *host, const char *error, void *data);
    void *uninstalled_data;

    /* sessions_list(), waiting for footd's answer */
    struct session_list *list;
    bool list_done;
} client = {.conn = {.fd = -1}};

void
sessions_enable(bool enable)
{
    client.enabled = enable;
}

static struct session_link *
find_link(uint64_t id, uint64_t token)
{
    tll_foreach(client.links, it) {
        if (it->item->id == id && it->item->token == token)
            return it->item;
    }
    return NULL;
}

static char **
strv_dup(char *const *strv)
{
    size_t count = 0;
    while (strv != NULL && strv[count] != NULL)
        count++;

    char **copy = xcalloc(count + 1, sizeof(copy[0]));
    for (size_t i = 0; i < count; i++)
        copy[i] = xstrdup(strv[i]);
    return copy;
}

static void
close_footd_fd(struct session_link *link)
{
    if (link->footd_fd >= 0)
        close(link->footd_fd);
    link->footd_fd = -1;
}

/*
 * The connection
 */

static void disconnect(void);

static void
update_writing(void)
{
    const bool writing = client.conn.fd >= 0 && session_conn_has_output(&client.conn);
    if (writing == client.writing)
        return;

    client.writing = writing;
    if (writing)
        fdm_event_add(client.fdm, client.conn.fd, EPOLLOUT);
    else
        fdm_event_del(client.fdm, client.conn.fd, EPOLLOUT);
}

static bool
send_msg(struct session_msg *msg, int fd)
{
    if (client.conn.fd < 0)
        return false;

    if (!session_conn_send(&client.conn, msg, fd)) {
        disconnect();
        return false;
    }

    update_writing();
    return true;
}

static void
handle_message(uint32_t type, struct session_reader *r)
{
    switch (type) {
    case SESSION_MSG_WELCOME: {
        const uint32_t version = session_read_u32(r);
        const pid_t pid = session_read_i32(r);
        client.features = session_read_more(r) ? session_read_u32(r) : 0;
        LOG_INFO("footd (PID=%d, protocol %u, features %#x)", pid, version, client.features);
        break;
    }

    case SESSION_MSG_STARTED: {
        const uint64_t id = session_read_u64(r);
        const uint64_t token = session_read_u64(r);
        const pid_t pid = session_read_i32(r);
        char *host = session_read_more(r) ? session_read_str(r) : NULL;

        struct session_link *link = r->ok ? find_link(id, token) : NULL;
        if (link != NULL) {
            /* footd has its end of the socket now */
            link->started = true;
            link->pid = pid;
            close_footd_fd(link);

            /* Where it runs: e.g. one that's restored, its title says so */
            if (link->host == NULL && host != NULL && host[0] != '\0') {
                link->host = host;
                host = NULL;
                if (link->term != NULL)
                    render_refresh_title(link->term);
            }
        }
        free(host);
        break;
    }

    case SESSION_MSG_FAILED: {
        const uint64_t id = session_read_u64(r);
        const uint64_t token = session_read_u64(r);
        char *reason = session_read_str(r);

        struct session_link *link = r->ok ? find_link(id, token) : NULL;
        LOG_WARN("session %016llx: %s", (unsigned long long)id,
                 reason != NULL ? reason : "failed");
        free(reason);

        if (link != NULL) {
            /* The terminal's socket is at its end now: the window closes */
            link->detached = true;
            close_footd_fd(link);
        }
        break;
    }

    case SESSION_MSG_DETACHED: {
        const uint64_t id = session_read_u64(r);
        const uint64_t token = session_read_u64(r);

        /* Another window attached; footd closes this one's socket */
        struct session_link *link = r->ok ? find_link(id, token) : NULL;
        if (link != NULL)
            link->detached = true;
        break;
    }

    case SESSION_MSG_UNINSTALLED: {
        char *host = session_read_str(r);
        char *error = session_read_str(r);
        if (r->ok && client.uninstalled != NULL) {
            client.uninstalled(host, error != NULL && error[0] != '\0' ? error : NULL,
                               client.uninstalled_data);
        }
        free(host);
        free(error);
        break;
    }

    case SESSION_MSG_SESSIONS: {
        struct session_list *list = client.list;
        if (list == NULL)
            break;

        const uint32_t count = session_read_u32(r);
        if (!r->ok || count > SESSION_MAX_PAYLOAD / 8) {
            client.list_done = true;
            break;
        }

        list->sessions = xcalloc(count > 0 ? count : 1, sizeof(list->sessions[0]));
        for (uint32_t i = 0; i < count && r->ok; i++) {
            struct session_info *info = &list->sessions[list->count];
            info->id = session_read_u64(r);
            info->attached = session_read_u8(r);
            info->pid = session_read_i32(r);
            info->title = session_read_str(r);
            free(session_read_str(r));  /* The working directory */
            if (r->ok)
                list->count++;
            else
                free(info->title);
        }

        size_t len;
        const void *layout = session_read_blob(r, &len);
        if (r->ok && len > 0) {
            list->layout = xmemdup(layout, len);
            list->layout_len = len;
        }

        client.list_done = true;
        break;
    }

    default:
        LOG_WARN("unknown message from footd: %u", type);
        break;
    }
}

/* Reads, and handles, what footd sent. Returns false if it's gone */
static bool
receive(void)
{
    bool eof;
    const bool ok = session_conn_read(&client.conn, &eof);

    uint32_t type;
    struct session_reader r;
    bool error;
    while (session_conn_next(&client.conn, &type, &r, &error))
        handle_message(type, &r);

    if (!ok || error) {
        if (!eof)
            LOG_ERR("the connection to footd failed");
        else
            LOG_WARN("footd is gone");
        return false;
    }
    return true;
}

static bool
fdm_footd(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLOUT) {
        if (!session_conn_flush(&client.conn)) {
            disconnect();
            return true;
        }
        update_writing();
    }

    if ((events & (EPOLLIN | EPOLLHUP)) && !receive())
        disconnect();

    return true;
}

/* footd is gone: so are the sessions (their windows' sockets are at their end) */
static void
disconnect(void)
{
    if (client.conn.fd < 0)
        return;

    fdm_del_no_close(client.fdm, client.conn.fd);
    session_conn_close(&client.conn);
    client.writing = false;

    tll_foreach(client.links, it) {
        struct session_link *link = it->item;
        if (link->sent) {
            link->detached = true;
            close_footd_fd(link);
        }
    }
}

/* footd: next to foot */
static char *
footd_path(void)
{
    char exe[PATH_MAX];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return NULL;

    char real[PATH_MAX];
    if (realpath(exe, real) == NULL)
        return NULL;

    char *slash = strrchr(real, '/');
    if (slash == NULL)
        return NULL;
    *slash = '\0';

    return xasprintf("%s/footd", real);
}

/* footd gets foot's environment, but not what's injected into foot */
static char **
footd_environment(void)
{
    size_t count = 0;
    for (char **e = environ; *e != NULL; e++)
        count++;

    char **envp = xcalloc(count + 1, sizeof(envp[0]));
    size_t n = 0;
    for (char **e = environ; *e != NULL; e++) {
        if (strncmp(*e, "DYLD_", 5) == 0)
            continue;
        envp[n++] = *e;
    }
    return envp;
}

/* Starts footd, listening on 'listen_fd' (it's a daemon: foot doesn't wait) */
static bool
spawn_footd(int listen_fd, const char *socket_path, void *data)
{
    char *path = footd_path();
    if (path == NULL) {
        LOG_ERR("footd: not found");
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

    /* Its own session (no controlling terminal), and nothing else of ours */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(
        &attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT |
               POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    sigset_t mask;
    sigemptyset(&mask);
    posix_spawnattr_setsigmask(&attr, &mask);

    sigset_t defaults;
    sigfillset(&defaults);
    posix_spawnattr_setsigdefault(&attr, &defaults);

    char **envp = footd_environment();

    pid_t pid;
    const int err = posix_spawn(&pid, path, &actions, &attr, argv, envp);

    free(envp);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    free(socket_arg);

    if (err != 0) {
        errno = err;
        LOG_ERRNO("%s: failed to start", path);
        free(path);
        return false;
    }

    LOG_INFO("started footd (PID=%d)", pid);
    free(path);
    return true;
}

/*
 * Connects to footd; with 'start', starting it if there's none. Under a
 * lock, that footd also takes when it exits: either it's there (and
 * stays), or it's gone (and the socket with it).
 */
static bool
connect_footd(bool start)
{
    if (client.conn.fd >= 0)
        return true;

    char *path = session_socket_path();
    if (path == NULL)
        return false;

    int fd = session_footd_connect(path, start ? &spawn_footd : NULL, NULL);
    if (fd < 0) {
        if (start)
            LOG_ERRNO("%s: no sessions (footd)", path);
        free(path);
        return false;
    }
    free(path);

    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    {
        close(fd);
        return false;
    }

    session_conn_init(&client.conn, fd);
    if (!fdm_add(client.fdm, fd, EPOLLIN, &fdm_footd, NULL)) {
        session_conn_close(&client.conn);
        return false;
    }

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_HELLO);
    session_msg_u32(&msg, SESSION_PROTOCOL_VERSION);
    session_msg_i32(&msg, getpid());
    const bool ok = send_msg(&msg, -1);
    session_msg_free(&msg);
    return ok;
}

/*
 * Links
 */

struct session_link *
session_link_new(struct fdm *fdm, const struct config *conf, const char *cwd,
                 int argc, char *const *argv, const char *const *envp,
                 const char **pty_path)
{
    const uint64_t attach_id = client.attach_id;
    const bool attach_hold = client.attach_hold;
    client.attach_id = 0;
    client.attach_hold = false;

    char *host = client.next_host;
    client.next_host = NULL;

    /* (A session that's restored is attached to, also without macos-sessions) */
    if (!client.enabled || (!conf->macos.sessions && attach_id == 0 && host == NULL)) {
        free(host);
        return NULL;
    }

    client.fdm = fdm;
    if (!connect_footd(true)) {
        free(host);
        return NULL;
    }

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        LOG_ERRNO("failed to create a socket for a session");
        free(host);
        return NULL;
    }

    /* Large: footd's output arrives in large chunks */
    const int buffer_size = 512 * 1024;
    for (size_t i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
        setsockopt(fds[i], SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    }

    struct session_link *link = xcalloc(1, sizeof(*link));
    *link = (struct session_link){
        .window_fd = fds[0],
        .footd_fd = fds[1],
        .attach = attach_id != 0,
        .held = attach_id != 0 && attach_hold,
        .id = attach_id,
        .login_shell = conf->login_shell,
        .scrollback_lines = conf->scrollback.lines,
        .host = host,
    };

    while (link->id == 0)
        arc4random_buf(&link->id, sizeof(link->id));
    arc4random_buf(&link->token, sizeof(link->token));

    snprintf(link->pty_path, sizeof(link->pty_path), "/dev/fd/%d", fds[0]);
    *pty_path = link->pty_path;

    link->cwd = xstrdup(cwd != NULL ? cwd : "/");
    link->argv = strv_dup(argc > 0 ? argv : NULL);
    link->envp = strv_dup(envp != NULL ? (char *const *)envp : environ);
    link->conf_path = xstrdup(conf->conf_path != NULL ? conf->conf_path : "");
    link->term_env = xstrdup(conf->term != NULL ? conf->term : "");
    link->shell = xstrdup(conf->shell != NULL ? conf->shell : "");

    tll_push_back(client.links, link);
    return link;
}

void
session_link_set_term(struct session_link *link, struct terminal *term)
{
    if (link == NULL)
        return;

    link->term = term;

    /* The terminal has its own (it opened /dev/fd/N) */
    if (link->window_fd >= 0)
        close(link->window_fd);
    link->window_fd = -1;

    /*
     * Not for what foot starts (e.g. what opens URLs): footd would see
     * the session as attached while that runs, after a crash
     */
    if (term->ptmx >= 0 && fcntl(term->ptmx, F_SETFD, FD_CLOEXEC) < 0)
        LOG_ERRNO("failed to configure the session's socket");
}

bool
session_link_write_failed(struct terminal *term)
{
    if (term->session == NULL)
        return false;

    tll_foreach(term->ptmx_buffers, it) {
        free(it->item.data);
        tll_remove(term->ptmx_buffers, it);
    }
    tll_foreach(term->ptmx_paste_buffers, it) {
        free(it->item.data);
        tll_remove(term->ptmx_paste_buffers, it);
    }

    fdm_event_del(term->fdm, term->ptmx, EPOLLOUT);
    return true;
}

/* Creates the session, or attaches to it */
static void
open_session(struct session_link *link)
{
    if (link->footd_fd < 0 || client.conn.fd < 0) {
        link->detached = true;
        close_footd_fd(link);
        return;
    }

    struct session_msg msg = {0};

    if (link->attach) {
        session_msg_begin(&msg, SESSION_MSG_ATTACH);
        session_msg_u64(&msg, link->id);
        session_msg_u64(&msg, link->token);
        session_msg_size(&msg, &link->size);
    } else {
        session_msg_begin(&msg, SESSION_MSG_CREATE);
        session_msg_u64(&msg, link->id);
        session_msg_u64(&msg, link->token);
        session_msg_size(&msg, &link->size);
        session_msg_str(&msg, link->cwd);
        session_msg_str(&msg, link->conf_path);
        session_msg_str(&msg, link->term_env);
        session_msg_str(&msg, link->shell);
        session_msg_u8(&msg, link->login_shell);
        session_msg_u32(&msg, link->scrollback_lines);
        session_msg_strv(&msg, (const char *const *)link->argv);
        session_msg_strv(&msg, (const char *const *)link->envp);
        session_msg_str(&msg, link->host);
    }

    link->sent = true;

    /* footd's end stays open here, until footd has answered */
    if (!send_msg(&msg, link->footd_fd)) {
        link->detached = true;
        close_footd_fd(link);
    }
    session_msg_free(&msg);

    /* Not needed any more */
    session_strv_free(link->argv);
    session_strv_free(link->envp);
    link->argv = link->envp = NULL;
}

void
session_link_resize(struct session_link *link, int cols, int rows,
                    int cell_width, int cell_height)
{
    const struct session_size size = {
        .cols = min(max(cols, 1), UINT16_MAX),
        .rows = min(max(rows, 1), UINT16_MAX),
        .cell_width = min(max(cell_width, 1), UINT16_MAX),
        .cell_height = min(max(cell_height, 1), UINT16_MAX),
    };

    const bool changed = memcmp(&size, &link->size, sizeof(size)) != 0;
    link->size = size;

    if (link->detached)
        return;

    if (!link->sent) {
        if (!link->held)
            open_session(link);
        return;
    }

    if (!changed)
        return;

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_RESIZE);
    session_msg_u64(&msg, link->id);
    session_msg_u64(&msg, link->token);
    session_msg_size(&msg, &link->size);
    send_msg(&msg, -1);
    session_msg_free(&msg);
}

void
session_link_destroy(struct session_link *link)
{
    if (link == NULL)
        return;

    /* Closing the window ends its session; quitting foot doesn't */
    if (link->sent && !link->detached && !client.detaching) {
        struct session_msg msg = {0};
        session_msg_begin(&msg, SESSION_MSG_KILL);
        session_msg_u64(&msg, link->id);
        session_msg_u64(&msg, link->token);
        send_msg(&msg, -1);
        session_msg_free(&msg);
    }

    close_footd_fd(link);
    if (link->window_fd >= 0)
        close(link->window_fd);

    tll_foreach(client.links, it) {
        if (it->item == link) {
            tll_remove(client.links, it);
            break;
        }
    }

    free(link->cwd);
    session_strv_free(link->argv);
    session_strv_free(link->envp);
    free(link->conf_path);
    free(link->term_env);
    free(link->shell);
    free(link->host);
    free(link);
}

const char *
session_link_host(const struct session_link *link)
{
    return link != NULL ? link->host : NULL;
}

void
sessions_create_next_on(const char *host)
{
    free(client.next_host);
    client.next_host = host != NULL && host[0] != '\0' ? xstrdup(host) : NULL;
}

bool
sessions_uninstall(const char *host,
                   void (*done)(const char *host, const char *error, void *data),
                   void *data)
{
    if (client.conn.fd < 0 || !(client.features & SESSION_FEATURE_UNINSTALL))
        return false;

    client.uninstalled = done;
    client.uninstalled_data = data;

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_UNINSTALL);
    session_msg_str(&msg, host);
    const bool ok = send_msg(&msg, -1);
    session_msg_free(&msg);
    return ok;
}

bool
sessions_remote_supported(void)
{
    /* Not connected yet: the footd that's started will be ours */
    return client.enabled &&
        (client.conn.fd < 0 || (client.features & SESSION_FEATURE_REMOTE));
}

pid_t
session_link_pid(const struct session_link *link)
{
    return link != NULL ? link->pid : 0;
}

pid_t
session_link_foreground_pgrp(const struct session_link *link)
{
    return link != NULL ? session_foreground_pgrp(link->pid) : 0;
}

pid_t
session_foreground_pgrp(pid_t pid)
{
    if (pid <= 0)
        return 0;

    /* The process group its terminal's foreground is (tcgetpgrp(), from footd's PTY) */
    struct proc_bsdinfo info;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return 0;

    return info.e_tpgid > 0 ? (pid_t)info.e_tpgid : 0;
}

uint64_t
session_link_id(const struct session_link *link)
{
    return link != NULL && link->sent ? link->id : 0;
}

bool
session_link_has_command(const struct session_link *link)
{
    /* argv is freed once the session is created; it's not new then */
    return link != NULL && (link->sent || (link->argv != NULL && link->argv[0] != NULL));
}

pid_t
terminal_pid(const struct terminal *term)
{
    return term->session != NULL ? session_link_pid(term->session) : term->slave;
}

pid_t
terminal_foreground_pgrp(const struct terminal *term)
{
    if (term->session != NULL)
        return session_link_foreground_pgrp(term->session);
    if (term->ptmx < 0)
        return 0;

    const pid_t pgrp = tcgetpgrp(term->ptmx);
    return pgrp > 0 ? pgrp : 0;
}

void
sessions_detach_all(void)
{
    client.detaching = true;
}

void
sessions_end_detached(void)
{
    if (client.conn.fd < 0)
        return;

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_KILL);
    session_msg_u64(&msg, 0);
    session_msg_u64(&msg, 0);
    send_msg(&msg, -1);
    session_msg_free(&msg);
}

void
sessions_fini(void)
{
    client.detaching = true;
    if (client.conn.fd >= 0) {
        /* What's queued (e.g. the layout) */
        int flags = fcntl(client.conn.fd, F_GETFL);
        if (flags >= 0)
            fcntl(client.conn.fd, F_SETFL, flags & ~O_NONBLOCK);
        session_conn_flush(&client.conn);
    }
    disconnect();
}

/*
 * Restoring
 */

void
sessions_attach_next(uint64_t id, bool hold)
{
    client.attach_id = id;
    client.attach_hold = hold;
}

void
sessions_release(void)
{
    tll_foreach(client.links, it) {
        struct session_link *link = it->item;
        if (!link->held)
            continue;

        link->held = false;
        if (link->size.cols > 0 && !link->sent && !link->detached)
            open_session(link);
    }
}

static int64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool
sessions_list(struct fdm *fdm, struct session_list *list)
{
    *list = (struct session_list){0};

    if (!client.enabled)
        return false;

    /* Without footd, there's nothing to restore */
    client.fdm = fdm;
    if (!connect_footd(false))
        return true;

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_LIST);
    const bool sent = send_msg(&msg, -1);
    session_msg_free(&msg);
    if (!sent)
        return false;

    /* Waits for the answer (anything else that comes is handled) */
    client.list = list;
    client.list_done = false;

    const int64_t deadline = now_ms() + LIST_TIMEOUT_MS;
    while (!client.list_done && client.conn.fd >= 0) {
        if (session_conn_has_output(&client.conn) && !session_conn_flush(&client.conn))
            break;

        const int64_t left = deadline - now_ms();
        if (left <= 0) {
            LOG_WARN("footd didn't answer");
            break;
        }

        struct pollfd pfd = {
            .fd = client.conn.fd,
            .events = POLLIN | (session_conn_has_output(&client.conn) ? POLLOUT : 0),
        };
        if (poll(&pfd, 1, (int)left) < 0 && errno != EINTR)
            break;

        if ((pfd.revents & (POLLIN | POLLHUP)) && !receive()) {
            disconnect();
            break;
        }
    }

    update_writing();
    client.list = NULL;

    const bool done = client.list_done;
    if (!done)
        session_list_free(list);
    return done;
}

void
session_list_free(struct session_list *list)
{
    for (size_t i = 0; i < list->count; i++)
        free(list->sessions[i].title);
    free(list->sessions);
    free(list->layout);
    *list = (struct session_list){0};
}

void
sessions_set_layout(const void *layout, size_t len)
{
    if (client.conn.fd < 0)
        return;

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_LAYOUT);
    session_msg_blob(&msg, layout, len);
    send_msg(&msg, -1);
    session_msg_free(&msg);
}
