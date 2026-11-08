/*
 * footd: foot's sessions.
 *
 * footd has the PTYs of foot's terminals, and the programs running in
 * them. A window attaches to a session (macos/session.c), and detaches
 * when it's gone: when foot quits, or crashes, the programs keep
 * running, and the next foot attaches again. Closing a window (or the
 * program exiting) ends its session.
 *
 * What the programs write goes to the attached window as is, and to a
 * terminal of footd's own (foot's terminal core, without a window: see
 * headless.c), that keeps the session's state: a window that attaches
 * gets it first (snapshot.c), then what follows. While no window is
 * attached, footd's terminal answers the programs' queries; else the
 * window does.
 *
 * foot starts footd when there's none (with the socket already
 * listening), and footd exits once there are neither sessions nor
 * windows left. Nothing wakes it up while idle.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#include <unistd.h>

/* The system's, not foot's spawn.h (the project's root is an include directory) */
#if defined(__APPLE__)
 #include <sys/../spawn.h>
#else
 #include <linux/../spawn.h>
#endif

#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#if defined(__APPLE__)
 /* The system's, not foot's notify.h (as spawn.h above) */
 #include <sys/../notify.h>
 #include <notify_keys.h>
#endif

#include <tllist.h>

#define LOG_MODULE "footd"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "fdm.h"
#include "headless.h"
#include "macros.h"
#include "rlimits.h"
#include "reaper.h"
#include "footd-bridge.h"
#include "session-mux.h"
#include "session-protocol.h"
#include "snapshot.h"
#include "terminal.h"
#include "user-notification.h"
#include "util.h"
#include "version.h"
#include "xmalloc.h"

/* Writing to a window pauses reading the PTY beyond this */
#define OUTPUT_HIGH_WATER (4 * 1024 * 1024)
#define OUTPUT_LOW_WATER (256 * 1024)

/* At most this much of a PTY is read at once (then others get a turn) */
#define READ_BUDGET (1024 * 1024)

/* Output for a window is sent once there's this much (or the PTY is empty) */
#define FLUSH_THRESHOLD (64 * 1024)

/* An incomplete control sequence is kept up to this size (see feed_output()) */
#define MAX_TAIL (1024 * 1024)

struct bytes {
    uint8_t *data;
    size_t len;
    size_t size;
};

static void
bytes_append(struct bytes *b, const void *data, size_t len)
{
    /* (Nothing to append: there may be no buffer yet, memcpy() takes none) */
    if (len == 0)
        return;

    if (b->len + len > b->size) {
        size_t size = b->size > 0 ? b->size : 16 * 1024;
        while (size < b->len + len)
            size *= 2;
        b->data = xrealloc(b->data, size);
        b->size = size;
    }
    memcpy(&b->data[b->len], data, len);
    b->len += len;
}

static void
bytes_consume(struct bytes *b, size_t len)
{
    memmove(b->data, &b->data[len], b->len - len);
    b->len -= len;
}

static void
bytes_free(struct bytes *b)
{
    free(b->data);
    *b = (struct bytes){0};
}

struct client;
struct session;

/* A window, attached to a session */
struct attachment {
    struct session *session;  /* NULL once it's ended (see drain()) */
    struct client *client;    /* NULL once its connection is gone */
    uint64_t token;
    int fd;                   /* The window's socket */
    struct bytes output;     /* Not yet written to it */
};

struct session {
    uint64_t id;
    struct config *conf;
    struct terminal *term;  /* footd's: NULL once it's gone */
    int pty;                /* The PTY master; -1 once closed */
    bool pty_registered;
    bool pty_paused;        /* Until the window has caught up */
    int replies;            /* The terminal's replies */
    pid_t pid;              /* What runs in it (the shell) */
    struct session_size size;

    /* Input (from the window, or footd's replies) not yet written to the PTY */
    struct bytes input;

    /* The end of the output, if it's an incomplete control sequence */
    struct bytes tail;
    bool tail_overflow;

    struct attachment *attachment;
    bool killed;

    /*
     * The window that was attached last: it may still end the session,
     * once detached (its socket is closed before it says so)
     */
    struct client *last_client;
    uint64_t last_token;
};

struct client {
    struct session_conn conn;
    pid_t pid;
    bool greeted;
    bool writing;
};

/* Another machine, over SSH (see "Sessions on other machines" below) */
struct remote {
    char *host;
    pid_t ssh;                  /* 0: none */
    int ssh_log;                /* Its stderr; -1: none */
    char *error;                /* Its last line (e.g. why it failed); NULL: none */
    struct session_mux *mux;    /* NULL: not connected */
    uint32_t next_channel;

    bool connected;             /* WELCOME: the other footd is there */
    int64_t connected_at;       /* (now_ms()) */
    int64_t heard_at;           /* When something came from there last */
    bool checking;              /* A LIST was sent: anything from there answers it */
    unsigned tries;             /* To connect again (since it was lost) */
    int64_t tried_at;           /* When it connected last */
    int timer;                  /* A check's timeout, or the next try; -1: none yet */

    /*
     * footd isn't there, or is too old (the remote command says so): it's
     * installed once a window says so, or right away to replace an old one
     */
    char *install;              /* "SYSTEM MACHINE" (uname); NULL: not needed (as far as known) */
    bool outdated;              /* There's one, too old */
    bool asking;                /* The windows ask whether to install it */
    pid_t installer;            /* ssh, installing it; 0: none */

    /* UNINSTALL: ssh removing it, and the window's client that asked (NULL: gone) */
    pid_t uninstaller;
    struct client *uninstalling;
};

struct remote_session {
    uint64_t id;
    struct remote *remote;
    struct client *client;      /* The window attached to it from here; NULL: none */
    uint64_t token;
    uint32_t channel;           /* Its window's socket's; 0: none */
    int fd;                     /* The window's socket, while disconnected; else -1 */
    struct session_size size;
    bool detached;              /* DETACHED: a window elsewhere has it now */
    bool started;               /* STARTED: it's there */
    struct session_msg create;  /* Its CREATE, until then (sent again if it couldn't connect) */
    bool told;                  /* The window was told it's disconnected (until attached) */

    /* The window that was attached last (as for sessions here) */
    struct client *last_client;
    uint64_t last_token;
};

static struct {
    struct fdm *fdm;
    struct reaper *reaper;

    int listen_fd;
    char *socket_path;
    char *lock_path;

    tll(struct session *) sessions;
    tll(struct client *) clients;

    /* Of sessions that have ended: their output's last bytes */
    tll(struct attachment *) draining;

    /* Sessions on other machines */
    tll(struct remote *) remotes;
    tll(struct remote_session *) remote_sessions;
    int network_fd;         /* Its changes (notify(3)); -1: not watched */
    int network_token;
    int network_timer;      /* Until they've settled; -1: none */

    /* The windows' layout, from the last window to tell */
    void *layout;
    size_t layout_len;

    bool listen_paused;
    int running_lock;   /* Held while footd runs (see session_running_path()) */

    volatile sig_atomic_t quit;
} footd = {.listen_fd = -1, .running_lock = -1, .network_fd = -1, .network_timer = -1};

extern char **environ;

static void session_free(struct session *s);
static void detach(struct attachment *att);
static void listen_pause(bool pause);

static bool
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static struct session *
session_find(uint64_t id)
{
    tll_foreach(footd.sessions, it) {
        if (it->item->id == id)
            return it->item;
    }
    return NULL;
}

/*
 * Clients (windows' control connections)
 */

static void client_free(struct client *c);

static void
client_send(struct client *c, struct session_msg *msg, int fd)
{
    if (c->conn.fd < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }

    if (!session_conn_send(&c->conn, msg, fd)) {
        LOG_WARN("client (PID=%d): failed to send", c->pid);
        return;
    }

    const bool writing = session_conn_has_output(&c->conn);
    if (writing != c->writing) {
        c->writing = writing;
        if (writing)
            fdm_event_add(footd.fdm, c->conn.fd, EPOLLOUT);
        else
            fdm_event_del(footd.fdm, c->conn.fd, EPOLLOUT);
    }
}

static void
send_failed(struct client *c, uint64_t id, uint64_t token, const char *reason)
{
    LOG_WARN("session %016llx: %s", (unsigned long long)id, reason);

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_FAILED);
    session_msg_u64(&msg, id);
    session_msg_u64(&msg, token);
    session_msg_str(&msg, reason);
    client_send(c, &msg, -1);
    session_msg_free(&msg);
}

static void
send_started(struct client *c, const struct session *s, uint64_t token)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_STARTED);
    session_msg_u64(&msg, s->id);
    session_msg_u64(&msg, token);
    session_msg_i32(&msg, s->pid);
    client_send(c, &msg, -1);
    session_msg_free(&msg);
}

/*
 * The PTY
 */

static void
pty_pause(struct session *s, bool pause)
{
    if (s->pty < 0 || !s->pty_registered || s->pty_paused == pause)
        return;

    s->pty_paused = pause;
    if (pause)
        fdm_event_del(footd.fdm, s->pty, EPOLLIN);
    else
        fdm_event_add(footd.fdm, s->pty, EPOLLIN);
}

/* Writes what it can of the input to the PTY; the rest when it's writable */
static void
flush_input(struct session *s)
{
    while (s->input.len > 0 && s->pty >= 0) {
        ssize_t n = write(s->pty, s->input.data, s->input.len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;

            /* E.g. EIO: what runs in it has exited */
            s->input.len = 0;
            break;
        }
        bytes_consume(&s->input, n);
    }

    if (s->pty < 0 || !s->pty_registered)
        return;

    const bool pending = s->input.len > 0;
    if (pending)
        fdm_event_add(footd.fdm, s->pty, EPOLLOUT);
    else
        fdm_event_del(footd.fdm, s->pty, EPOLLOUT);

    /* The window's input waits, while the program doesn't read it */
    struct attachment *att = s->attachment;
    if (att != NULL) {
        if (pending)
            fdm_event_del(footd.fdm, att->fd, EPOLLIN);
        else
            fdm_event_add(footd.fdm, att->fd, EPOLLIN);
    }
}

static void
write_input(struct session *s, const void *data, size_t len)
{
    bytes_append(&s->input, data, len);
    flush_input(s);
}

/*
 * The end of the output may be the start of a control sequence (its
 * rest comes later), or of a UTF-8 character. A window that attaches
 * gets it after the snapshot: its parser must be in the same state as
 * footd's terminal's.
 *
 * In foot's parser, an ESC always starts a sequence (it ends a string,
 * e.g. an OSC, that's been going on), and nothing else does but UTF-8
 * characters' first bytes. So the tail starts at the last ESC, or at
 * the last first byte after it, if the parser was in its ground state
 * right before that byte. Else, it's what's going on since an earlier
 * chunk. feed_output() feeds footd's terminal in two parts for this,
 * when there's such a byte.
 */
static void
set_tail(struct session *s, const uint8_t *data, size_t len, bool append)
{
    if (!append) {
        s->tail.len = 0;
        s->tail_overflow = false;
    }

    if (s->tail_overflow)
        return;

    if (s->tail.len + len > MAX_TAIL) {
        s->tail.len = 0;
        s->tail_overflow = true;
        return;
    }

    bytes_append(&s->tail, data, len);
}

static void
feed_output(struct session *s, const uint8_t *data, size_t len)
{
    ssize_t esc = -1;
    for (ssize_t i = len - 1; i >= 0; i--) {
        if (data[i] == '\033') {
            esc = i;
            break;
        }
    }

    ssize_t lead = -1;
    for (ssize_t i = len - 1; i > esc; i--) {
        if (data[i] >= 0xc0) {
            lead = i;
            break;
        }
    }

    bool lead_starts = false;
    if (lead >= 0) {
        headless_term_feed(s->term, data, lead);
        lead_starts = s->term->vt.state == 0;
        headless_term_feed(s->term, &data[lead], len - lead);
    } else
        headless_term_feed(s->term, data, len);

    if (s->term->vt.state == 0) {
        /* Ground state: nothing's going on */
        set_tail(s, NULL, 0, false);
    } else if (lead_starts)
        set_tail(s, &data[lead], len - lead, false);
    else if (esc >= 0)
        set_tail(s, &data[esc], len - esc, false);
    else
        set_tail(s, data, len, true);
}

static void flush_output(struct attachment *att);

/* What the programs wrote (sent to the window by read_pty()) */
static void
output(struct session *s, const uint8_t *data, size_t len)
{
    struct attachment *att = s->attachment;
    if (att != NULL)
        bytes_append(&att->output, data, len);

    if (s->term != NULL)
        feed_output(s, data, len);
}

/*
 * Reads what's there; returns false at the end.
 *
 * A PTY hands out (at most) 1 KiB per read(). Each is parsed right
 * away: while footd's terminal parses it, the programs write more (they
 * would wait for footd otherwise, and footd for them). It's sent to the
 * window in larger chunks: once the PTY is empty (right away, when
 * there's little), or there's enough.
 */
static bool
read_pty(struct session *s)
{
    uint8_t buf[64 * 1024];
    size_t total = 0;
    bool open = true;

    while (total < READ_BUDGET && !s->pty_paused) {
        ssize_t n = read(s->pty, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN) {
                /* EIO: the other end is closed */
                open = false;
            }
            break;
        }

        if (n == 0) {
            open = false;
            break;
        }

        output(s, buf, n);
        total += n;

        if (s->attachment != NULL && s->attachment->output.len >= FLUSH_THRESHOLD)
            flush_output(s->attachment);
    }

    if (s->attachment != NULL)
        flush_output(s->attachment);
    return open;
}

static void
close_pty(struct session *s)
{
    if (s->pty < 0)
        return;

    if (s->pty_registered)
        fdm_del(footd.fdm, s->pty);
    else
        close(s->pty);

    s->pty = -1;
    s->pty_registered = false;
}

static bool
fdm_pty(struct fdm *fdm, int fd, int events, void *data)
{
    struct session *s = data;

    if (events & EPOLLOUT)
        flush_input(s);

    bool open = true;
    if (events & EPOLLIN)
        open = read_pty(s);

    if (!open || ((events & EPOLLHUP) && !(events & EPOLLIN))) {
        /*
         * Nothing more will come (what runs in it has exited, or at
         * least closed it): the session ends when it has exited (see
         * session_terminated()). Until then, it stays as it is.
         */
        LOG_DBG("session %016llx: PTY closed", (unsigned long long)s->id);
        fdm_del_no_close(footd.fdm, s->pty);
        s->pty_registered = false;
    }

    return true;
}

/* footd's terminal's replies: only while no window answers instead */
static bool
fdm_replies(struct fdm *fdm, int fd, int events, void *data)
{
    struct session *s = data;

    if (events & EPOLLIN) {
        uint8_t buf[4096];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            if (s->attachment == NULL && s->pty >= 0)
                write_input(s, buf, n);
        }
    }

    if (events & EPOLLHUP) {
        fdm_del(fdm, fd);
        s->replies = -1;
    }
    return true;
}

/*
 * Attachments
 */

static void
flush_output(struct attachment *att)
{
    struct session *s = att->session;

    while (att->output.len > 0) {
        ssize_t n = write(att->fd, att->output.data, att->output.len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;

            /* The window is gone; its HUP detaches it */
            att->output.len = 0;
            break;
        }
        bytes_consume(&att->output, n);
    }

    if (att->output.len > 0)
        fdm_event_add(footd.fdm, att->fd, EPOLLOUT);
    else
        fdm_event_del(footd.fdm, att->fd, EPOLLOUT);

    if (s == NULL)
        return;

    /* Without blocking the programs for good, nor buffering without bounds */
    if (att->output.len > OUTPUT_HIGH_WATER)
        pty_pause(s, true);
    else if (att->output.len < OUTPUT_LOW_WATER)
        pty_pause(s, false);
}

static void
drain_done(struct attachment *att)
{
    tll_foreach(footd.draining, it) {
        if (it->item == att) {
            tll_remove(footd.draining, it);
            break;
        }
    }

    fdm_del(footd.fdm, att->fd);
    bytes_free(&att->output);
    free(att);
}

/*
 * The session has ended: once the window has its output's last bytes,
 * its socket is closed (and its terminal ends)
 */
static void
drain(struct attachment *att)
{
    att->session = NULL;
    fdm_event_del(footd.fdm, att->fd, EPOLLIN);
    flush_output(att);

    if (att->output.len == 0) {
        fdm_del(footd.fdm, att->fd);
        bytes_free(&att->output);
        free(att);
        return;
    }

    tll_push_back(footd.draining, att);
}

static bool
fdm_window(struct fdm *fdm, int fd, int events, void *data)
{
    struct attachment *att = data;
    struct session *s = att->session;

    if (s == NULL) {
        /* Draining */
        if (events & EPOLLOUT)
            flush_output(att);
        if (att->output.len == 0 || (events & EPOLLHUP))
            drain_done(att);
        return true;
    }

    if (events & EPOLLOUT)
        flush_output(att);

    bool open = true;
    if (events & EPOLLIN) {
        uint8_t buf[16 * 1024];
        ssize_t n;
        do {
            n = read(fd, buf, sizeof(buf));
        } while (n < 0 && errno == EINTR);

        if (n > 0) {
            if (s->pty >= 0)
                write_input(s, buf, n);
        } else if (n == 0 || errno != EAGAIN)
            open = false;
    }

    if (!open || ((events & EPOLLHUP) && !(events & EPOLLIN))) {
        LOG_DBG("session %016llx: detached", (unsigned long long)s->id);
        detach(att);
    }

    return true;
}

static void
detach(struct attachment *att)
{
    struct session *s = att->session;
    xassert(s->attachment == att);

    fdm_del(footd.fdm, att->fd);
    bytes_free(&att->output);
    free(att);

    s->attachment = NULL;

    /* Nobody's reading: the programs mustn't be blocked by that */
    pty_pause(s, false);

    /* The window's input may have been waiting */
    flush_input(s);
}

static void
resize(struct session *s, const struct session_size *size)
{
    if (size->cols == 0 || size->rows == 0)
        return;

    s->size = *size;

    if (s->term != NULL) {
        headless_term_resize(
            s->term, size->cols, size->rows,
            max(size->cell_width, 1), max(size->cell_height, 1));
    }

    if (s->pty >= 0 &&
        ioctl(s->pty, (unsigned int)TIOCSWINSZ,
              &(struct winsize){
                  .ws_row = size->rows,
                  .ws_col = size->cols,
                  .ws_xpixel = size->cols * size->cell_width,
                  .ws_ypixel = size->rows * size->cell_height}) < 0)
    {
        LOG_ERRNO("session %016llx: TIOCSWINSZ", (unsigned long long)s->id);
    }
}

/* Attaches the window's socket 'fd' (takes it) */
static bool
attach(struct session *s, struct client *c, uint64_t token, int fd,
       const struct session_size *size)
{
    if (!set_nonblocking(fd)) {
        close(fd);
        return false;
    }

    /* (Not too small: a window's output arrives in large chunks) */
    const int buffer_size = 512 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
#if defined(SO_NOSIGPIPE)  /* (Linux has none: SIGPIPE is ignored anyway) */
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    if (s->attachment != NULL) {
        /* Another window: it's no longer attached (its terminal ends) */
        struct attachment *old = s->attachment;
        if (old->client != NULL) {
            struct session_msg msg = {0};
            session_msg_begin(&msg, SESSION_MSG_DETACHED);
            session_msg_u64(&msg, s->id);
            session_msg_u64(&msg, old->token);
            client_send(old->client, &msg, -1);
            session_msg_free(&msg);
        }
        detach(old);
    }

    struct attachment *att = xmalloc(sizeof(*att));
    *att = (struct attachment){
        .session = s,
        .client = c,
        .token = token,
        .fd = fd,
    };

    if (!fdm_add(footd.fdm, fd, EPOLLIN, &fdm_window, att)) {
        close(fd);
        free(att);
        return false;
    }


    s->attachment = att;
    s->last_client = c;
    s->last_token = token;

    /* The window's size, before its state is taken */
    if (size->cols != s->size.cols || size->rows != s->size.rows ||
        size->cell_width != s->size.cell_width ||
        size->cell_height != s->size.cell_height)
    {
        resize(s, size);
    }

    /* The state, then an incomplete control sequence, if any */
    if (s->term != NULL) {
        size_t len;
        char *snapshot = snapshot_create(s->term, &len);
        bytes_append(&att->output, snapshot, len);
        free(snapshot);

        if (s->tail.len > 0)
            bytes_append(&att->output, s->tail.data, s->tail.len);
    }

    flush_output(att);
    flush_input(s);
    return true;
}

/*
 * Sessions
 */

/* footd's terminal is gone: what ran in it has exited (term_shutdown()) */
static void
session_terminated(void *data, int exit_code)
{
    struct session *s = data;

    LOG_INFO("session %016llx: ended (%d)", (unsigned long long)s->id, exit_code);
    s->term = NULL;

    /* What's left in the PTY goes to the window */
    if (s->pty >= 0 && !s->killed) {
        uint8_t buf[64 * 1024];
        for (size_t total = 0; total < READ_BUDGET;) {
            ssize_t n = read(s->pty, buf, sizeof(buf));
            if (n <= 0)
                break;
            if (s->attachment != NULL)
                bytes_append(&s->attachment->output, buf, n);
            total += n;
        }
    }

    tll_foreach(footd.sessions, it) {
        if (it->item == s) {
            tll_remove(footd.sessions, it);
            break;
        }
    }

    session_free(s);
}

static void
session_free(struct session *s)
{
    listen_pause(false);

    struct attachment *att = s->attachment;
    if (att != NULL) {
        /* The rest of its output, and then it's closed: its terminal ends */
        s->attachment = NULL;
        drain(att);
    }

    close_pty(s);

    if (s->replies >= 0)
        fdm_del(footd.fdm, s->replies);

    if (s->term != NULL) {
        /*
         * footd is exiting: not through term_shutdown(), and without
         * waiting for what runs in it (it gets a SIGHUP)
         */
        if (s->term->slave > 0) {
            kill(-s->term->slave, SIGHUP);
            reaper_del(footd.reaper, s->term->slave);
            s->term->slave = -1;
        }
        s->term->shutdown.cb = NULL;
        term_destroy(s->term);
    }

    if (s->conf != NULL) {
        config_free(s->conf);
        free(s->conf);
    }

    bytes_free(&s->input);
    bytes_free(&s->tail);
    free(s);
}

static struct session *
session_create(uint64_t id, const char *cwd, const char *conf_path,
               const char *term_env, const char *shell, bool login_shell,
               uint32_t scrollback_lines, char **argv, char **envp,
               const struct session_size *size, const char **error)
{
    struct config *conf = xmalloc(sizeof(*conf));

    /* As foot's windows load it; with what foot was told to use */
    user_notifications_t notifications = tll_init();
    config_override_t overrides = tll_init();
    if (!config_load(conf, conf_path[0] != '\0' ? conf_path : NULL,
                     &notifications, &overrides, false, false))
    {
        LOG_WARN("%s: configuration errors (see foot's)",
                 conf_path[0] != '\0' ? conf_path : "foot.ini");
    }
    user_notifications_free(&notifications);

    if (term_env[0] != '\0') {
        free(conf->term);
        conf->term = xstrdup(term_env);
    }
    if (shell[0] != '\0') {
        free(conf->shell);
        conf->shell = xstrdup(shell);
    }
    conf->login_shell = login_shell;
    if (scrollback_lines > 0)
        conf->scrollback.lines = scrollback_lines;

    /* Nothing is rendered; and holding is the window's business */
    conf->render_worker_count = 0;
    conf->hold_at_exit = false;

    /* What runs in it starts with the window's size (term_init()'s TIOCSWINSZ) */
    if (size->cols > 0 && size->rows > 0) {
        conf->size.type = CONF_SIZE_CELLS;
        conf->size.width = size->cols;
        conf->size.height = size->rows;
    }

    struct session *s = xmalloc(sizeof(*s));
    *s = (struct session){
        .id = id,
        .conf = conf,
        .pty = -1,
        .replies = -1,
    };

    int argc = 0;
    while (argv[argc] != NULL)
        argc++;

    s->term = headless_term_new(
        conf, cwd[0] != '\0' ? cwd : "/", argc, argv,
        (const char *const *)envp, true, &s->pty, &s->replies,
        &session_terminated, s);

    if (s->term == NULL) {
        *error = "failed to start";
        config_free(conf);
        free(conf);
        free(s);
        return NULL;
    }

    s->pid = s->term->slave;

    s->pty_registered = fdm_add(footd.fdm, s->pty, EPOLLIN, &fdm_pty, s);
    if (!s->pty_registered ||
        !fdm_add(footd.fdm, s->replies, EPOLLIN, &fdm_replies, s))
    {
        *error = "failed to watch its PTY";
        if (!s->pty_registered) {
            close(s->replies);
            s->replies = -1;
        }

        /* Ends it; then it's freed (session_terminated()) */
        s->killed = true;
        term_shutdown(s->term);
        return NULL;
    }

    resize(s, size);

    tll_push_back(footd.sessions, s);
    LOG_INFO("session %016llx: started (PID=%d)", (unsigned long long)id, s->pid);
    return s;
}

static void
session_kill(struct session *s)
{
    LOG_INFO("session %016llx: closed", (unsigned long long)s->id);

    if (s->attachment != NULL)
        detach(s->attachment);

    s->killed = true;

    /* SIGHUP; later SIGTERM, then SIGKILL (see term_shutdown()) */
    if (s->term != NULL)
        term_shutdown(s->term);
}

/*
 * Sessions on other machines (CREATE's host). They're reached over SSH,
 * where footd --bridge passes them on to the footd there
 * (footd-bridge.h): a connection per machine, the windows' sockets on
 * channels (session-mux.h). They're that footd's sessions: here, only
 * their ID, machine, and window are known.
 *
 * When a connection is lost, its windows stay, and are told so; they're
 * attached again (and get the state anew) once it's connected again:
 * right away, then after a while, a few times; after that, once one of
 * them gets input, or the network changes. A session that couldn't be
 * created (no connection) is, then.
 *
 * A connection that's gone unnoticed (e.g. the Mac slept, or changed
 * networks: SSH's TCP connection hangs) is found by checking it: when
 * the network changes, and when a window's input goes there after
 * nothing came for a while. Nothing is sent while it's idle.
 */

/*
 * Run by ssh on the other machine: footd --bridge, where a user installs
 * footd, unless it's older than this one's bridge (FOOTD_BRIDGE_LEVEL).
 * Else it says so (INSTALL_MARK, uname, and the old one's path or
 * "none"), and fails: footd is installed (remote_install()).
 */
#define INSTALL_MARK "footd-install: "

static char *
remote_command(void)
{
    return xasprintf(
        "sh -c 'for f in \"$HOME/.local/bin/footd\" "
        "/Applications/foot.app/Contents/MacOS/footd $(command -v footd); do "
        "[ -x \"$f\" ] || continue; "
        "[ \"$(\"$f\" --bridge-level 2>/dev/null)\" -ge %d ] 2>/dev/null && exec \"$f\" --bridge; "
        "old=$f; done; "
        "echo \"" INSTALL_MARK "$(uname -s) $(uname -m) ${old:-none}\" >&2; exit 3'",
        FOOTD_BRIDGE_LEVEL);
}

/* footd (stdin) to ~/.local/bin/footd, if it runs there */
static const char install_command[] =
    "sh -c 'umask 022; d=\"$HOME/.local/bin\"; mkdir -p \"$d\" && "
    "cat >\"$d/footd.new\" && chmod 755 \"$d/footd.new\" && "
    "\"$d/footd.new\" --bridge-level >/dev/null && mv -f \"$d/footd.new\" \"$d/footd\" || "
    "{ rm -f \"$d/footd.new\"; exit 1; }'";

/* ~/.local/bin/footd is removed, and its sessions end (footd's, and the bridges') */
static const char uninstall_command[] =
    "sh -c 'f=\"$HOME/.local/bin/footd\"; "
    "[ -e \"$f\" ] || { echo \"there is no ~/.local/bin/footd\" >&2; exit 1; }; "
    "pkill -TERM -u \"$(id -u)\" -f \"/[.]local/bin/footd( |$)\"; rm -f \"$f\"; "
    "s=\"${XDG_STATE_HOME:-$HOME/.local/state}/foot\"; rm -f \"$s\"/sessions-*; "
    "rmdir \"$s\" 2>/dev/null; exit 0'";

/* A check is answered within this (milliseconds), or the connection is lost */
#define CHECK_TIMEOUT 5000

/* Connecting takes at most this (milliseconds; ssh's ConnectTimeout is for the TCP connection only) */
#define CONNECT_TIMEOUT 30000

/*
 * Connecting again: after these (milliseconds); then on input, or a
 * network change. Not more often: servers limit how often one may
 * connect (e.g. ufw's limit: 6 in 30 seconds), and refuse for longer
 * then.
 */
static const int retry_after[] = {1000, 4000, 10000, 20000, 30000, 60000};

/* Tries to connect (again) are at least this far apart (milliseconds; at most 5 in 30 seconds) */
#define MIN_GAP 7000

/* A connection that lasted this long was fine: once lost, it's tried anew (milliseconds) */
#define LASTED 30000

/* The network's changes come in bursts: it's checked once they've settled (milliseconds) */
#define NETWORK_SETTLE 1500

static void remote_disconnected(struct remote *r);
static bool remote_connect(struct remote *r);
static void remote_install_needed(struct remote *r);

static int64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* $FOOTD_CHECK_TIMEOUT (milliseconds): e.g. tests' */
static int
check_timeout(void)
{
    const char *value = getenv("FOOTD_CHECK_TIMEOUT");
    const int ms = value != NULL ? atoi(value) : 0;
    return ms > 0 ? ms : CHECK_TIMEOUT;
}

/* Arms 'timer' (a timerfd) to expire in 'ms'; 0: disarms it */
static void
timer_set(int timer, int ms)
{
    if (timer < 0)
        return;
    struct itimerspec spec = {
        .it_value = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000},
    };
    if (timerfd_settime(timer, 0, &spec, NULL) < 0)
        LOG_ERRNO("failed to set a timer");
}

/* A timerfd, in the FDM; -1: failed */
static int
timer_new(fdm_fd_handler_t handler, void *data)
{
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer < 0) {
        LOG_ERRNO("failed to create a timer");
        return -1;
    }
    if (!fdm_add(footd.fdm, timer, EPOLLIN, handler, data)) {
        close(timer);
        return -1;
    }
    return timer;
}

/* Whether it expired (rather than being read in vain) */
static bool
timer_expired(int timer)
{
    uint64_t expirations;
    return timerfd_read(timer, &expirations) > 0 && expirations > 0;
}

static struct remote_session *
remote_session_find(uint64_t id)
{
    tll_foreach(footd.remote_sessions, it) {
        if (it->item->id == id)
            return it->item;
    }
    return NULL;
}

static struct remote_session *
remote_session_on(const struct remote *r, uint32_t channel)
{
    tll_foreach(footd.remote_sessions, it) {
        if (it->item->remote == r && it->item->channel == channel)
            return it->item;
    }
    return NULL;
}

/* Its window's socket is closed (its terminal ends) */
static void
remote_session_close_window(struct remote_session *rs)
{
    if (rs->channel != 0 && rs->remote->mux != NULL) {
        int fd = session_mux_take(rs->remote->mux, rs->channel);
        if (fd >= 0)
            close(fd);
    }
    rs->channel = 0;

    if (rs->fd >= 0)
        fdm_del(footd.fdm, rs->fd);
    rs->fd = -1;
    rs->client = NULL;
}

static void
remote_session_free(struct remote_session *rs)
{
    remote_session_close_window(rs);
    tll_foreach(footd.remote_sessions, it) {
        if (it->item == rs) {
            tll_remove(footd.remote_sessions, it);
            break;
        }
    }
    session_msg_free(&rs->create);
    free(rs);
}

/* Windows wait for it (disconnected): it's connected again for them */
static bool
remote_awaited(const struct remote *r)
{
    tll_foreach(footd.remote_sessions, it) {
        if (it->item->remote == r && it->item->fd >= 0)
            return true;
    }
    return false;
}

static bool
remote_send(struct remote *r, struct session_msg *msg)
{
    return r->mux != NULL && session_mux_send(r->mux, msg);
}

static bool fdm_disconnected_window(struct fdm *fdm, int fd, int events, void *data);
static void notify_disconnected(struct remote_session *rs);
static void ask_install(struct remote_session *rs);
static void remote_install(struct remote *r);

/* The window's socket 'fd' (taken) is kept, until it's connected again */
static void
keep_disconnected(struct remote_session *rs, int fd)
{
    rs->channel = 0;
    rs->fd = fd;
    notify_disconnected(rs);
    if (rs->remote->asking)
        ask_install(rs);
    if (!fdm_add(footd.fdm, fd, EPOLLIN, &fdm_disconnected_window, rs)) {
        close(fd);
        rs->fd = -1;
        rs->client = NULL;
    }
}

/* The window's socket 'fd' (taken) goes with 'msg' (a CREATE or ATTACH) */
static void
remote_send_with_window(struct remote_session *rs, struct session_msg *msg, int fd)
{
    struct remote *r = rs->remote;

    if (++r->next_channel == 0)
        r->next_channel = 1;
    const uint32_t channel = r->next_channel;

    if (r->mux == NULL || !session_mux_announce(r->mux, channel) || !remote_send(r, msg)) {
        /* Lost meanwhile: attached again, once connected */
        keep_disconnected(rs, fd);
        return;
    }

    /* (It's closed if that fails: the window's terminal ends) */
    if (session_mux_add(r->mux, channel, fd))
        rs->channel = channel;
}

static void
attach_msg(struct session_msg *msg, const struct remote_session *rs)
{
    session_msg_begin(msg, SESSION_MSG_ATTACH);
    session_msg_u64(msg, rs->id);
    session_msg_u64(msg, rs->token);
    session_msg_size(msg, &rs->size);
}

/* Connects again (for the windows that wait): now, unless it was tried just before; then soon */
static void
remote_reconnect(struct remote *r)
{
    if (r->mux != NULL)
        return;
    const int64_t wait = r->tried_at + MIN_GAP - now_ms();
    if (wait > 0)
        timer_set(r->timer, (int)wait);
    else
        remote_connect(r);
}

/* A window that's disconnected: its input connects again */
static bool
fdm_disconnected_window(struct fdm *fdm, int fd, int events, void *data)
{
    struct remote_session *rs = data;

    uint8_t buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR) ||
        ((events & EPOLLHUP) && !(events & EPOLLIN)))
    {
        /* The window is gone: the session runs on, there (if it was created) */
        if (!rs->started) {
            remote_session_free(rs);
            return true;
        }
        fdm_del(footd.fdm, rs->fd);
        rs->fd = -1;
        rs->client = NULL;
        return true;
    }

    /* Installing footd there: Enter, once asked; meanwhile, nothing */
    struct remote *r = rs->remote;
    if (r->asking) {
        if (memchr(buf, '\r', n) != NULL || memchr(buf, '\n', n) != NULL)
            remote_install(r);
        return true;
    }
    if (r->installer > 0 || r->uninstaller > 0)
        return true;

    r->tries = 0;
    remote_reconnect(r);
    return true;
}

/* Tells a window that's disconnected (a line of its own) */
static void PRINTF(2)
tell_window(struct remote_session *rs, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *what = xvasprintf(fmt, va);
    va_end(va);

    char *text = xasprintf("\r\n\033[0;7m foot: %s \033[m\r\n", what);
    if (write(rs->fd, text, strlen(text)) < 0)
        LOG_ERRNO("%s: failed to tell a window", rs->remote->host);
    free(text);
    free(what);
}

/* Tells the windows that wait for it */
static void PRINTF(2)
tell_windows(struct remote *r, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *what = xvasprintf(fmt, va);
    va_end(va);

    tll_foreach(footd.remote_sessions, it) {
        if (it->item->remote == r && it->item->fd >= 0)
            tell_window(it->item, "%s", what);
    }
    free(what);
}

/* Why it can't connect, as ssh said: ": ERROR", or "" (for "%s%s") */
#define WHY(r) (r)->error != NULL ? ": " : "", (r)->error != NULL ? (r)->error : ""

/* Once, until it's attached again (not while footd is to be installed there: that's asked) */
static void
notify_disconnected(struct remote_session *rs)
{
    const struct remote *r = rs->remote;
    if (rs->told || r->install != NULL || r->installer > 0)
        return;
    rs->told = true;

    if (rs->started)
        tell_window(rs, "the connection to %s is lost; connecting again…", r->host);
    else
        tell_window(rs, "can't connect to %s%s%s; trying again…", r->host, WHY(r));
}

/* From the other footd */
static bool
remote_message(struct session_mux *mux, uint32_t type, struct session_reader *r,
               void *data)
{
    struct remote *remote = data;

    if (type == SESSION_MSG_WELCOME) {
        LOG_INFO("%s: connected", remote->host);
        remote->connected = true;
        remote->connected_at = remote->heard_at = now_ms();
        free(remote->error);
        remote->error = NULL;
        return true;
    }

    if (type != SESSION_MSG_STARTED && type != SESSION_MSG_FAILED &&
        type != SESSION_MSG_DETACHED)
    {
        return true;
    }

    const uint64_t id = session_read_u64(r);
    const uint64_t token = session_read_u64(r);
    if (!r->ok)
        return false;

    struct remote_session *rs = remote_session_find(id);
    if (rs == NULL || rs->remote != remote || rs->token != token)
        return true;

    switch (type) {
    case SESSION_MSG_STARTED:
        rs->started = true;
        rs->told = false;
        session_msg_free(&rs->create);

        /* Its PID is the other machine's: none here */
        if (rs->client != NULL) {
            struct session_msg msg = {0};
            session_msg_begin(&msg, SESSION_MSG_STARTED);
            session_msg_u64(&msg, id);
            session_msg_u64(&msg, token);
            session_msg_i32(&msg, 0);
            session_msg_str(&msg, remote->host);
            client_send(rs->client, &msg, -1);
            session_msg_free(&msg);
        }
        break;

    case SESSION_MSG_FAILED: {
        /* It isn't there (any more) */
        char *reason = session_read_str(r);
        LOG_WARN("%s: session %016llx: %s", remote->host, (unsigned long long)id,
                 reason != NULL ? reason : "failed");
        if (rs->client != NULL)
            send_failed(rs->client, id, token, reason != NULL ? reason : "failed");
        free(reason);
        remote_session_free(rs);
        break;
    }

    case SESSION_MSG_DETACHED:
        /* A window elsewhere has it; its socket is closed next */
        rs->detached = true;
        if (rs->client != NULL) {
            struct session_msg msg = {0};
            session_msg_begin(&msg, SESSION_MSG_DETACHED);
            session_msg_u64(&msg, id);
            session_msg_u64(&msg, token);
            client_send(rs->client, &msg, -1);
            session_msg_free(&msg);
        }
        rs->client = NULL;
        break;
    }

    return true;
}

static void
remote_channel_closed(struct session_mux *mux, uint32_t channel, bool theirs, void *data)
{
    struct remote_session *rs = remote_session_on(data, channel);
    if (rs == NULL)
        return;

    rs->channel = 0;
    rs->client = NULL;

    /* There: the session has ended, unless a window elsewhere has it */
    if (theirs && !rs->detached)
        remote_session_free(rs);
}

static void
remote_closed(struct session_mux *mux, void *data)
{
    remote_disconnected(data);
}

/* Is it still there? Anything that comes answers (a LIST); nothing, in time: it's lost */
static void
remote_check(struct remote *r)
{
    if (r->mux == NULL || !r->connected || r->checking)
        return;

    LOG_INFO("%s: checking the connection", r->host);
    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_LIST);
    remote_send(r, &msg);
    session_msg_free(&msg);

    r->checking = true;
    timer_set(r->timer, check_timeout());
}

/* Something came from there */
static void
remote_received(struct session_mux *mux, void *data)
{
    struct remote *r = data;
    r->heard_at = now_ms();
    if (r->checking) {
        r->checking = false;
        timer_set(r->timer, 0);
    }
}

/* A window's input goes there: if nothing came for a while, is it still there? */
static void
remote_input(struct session_mux *mux, uint32_t channel, void *data)
{
    struct remote *r = data;
    if (r->connected && !r->checking && now_ms() - r->heard_at >= 2 * check_timeout())
        remote_check(r);
}

static const struct session_mux_callbacks remote_callbacks = {
    .message = &remote_message,
    .channel_closed = &remote_channel_closed,
    .closed = &remote_closed,
    .received = &remote_received,
    .input = &remote_input,
};

/* A check wasn't answered, or it's time to connect again */
static bool
fdm_remote_timer(struct fdm *fdm, int fd, int events, void *data)
{
    struct remote *r = data;
    if (!timer_expired(fd))
        return true;

    if (r->checking) {
        LOG_WARN("%s: not answering", r->host);
        r->checking = false;
        remote_disconnected(r);
    } else if (remote_awaited(r))
        remote_reconnect(r);
    return true;
}

static void ssh_log_read(struct remote *r);

/* Connecting again, for the windows: after a while; then they're told it's up to them */
static void
remote_retry(struct remote *r)
{
    timer_set(r->timer, 0);
    if (!remote_awaited(r))
        return;

    if (r->tries < sizeof(retry_after) / sizeof(retry_after[0])) {
        timer_set(r->timer, retry_after[r->tries++]);
        return;
    }

    tell_windows(r, "can't connect to %s%s%s; type to try again", r->host, WHY(r));
}

/* The connection is lost (or couldn't be made): its windows are told, and kept */
static void
remote_disconnected(struct remote *r)
{
    /* (Why, if ssh said so) */
    ssh_log_read(r);

    if (r->connected) {
        LOG_WARN("%s: disconnected", r->host);
        if (now_ms() - r->connected_at >= LASTED)
            r->tries = 0;
    } else
        LOG_WARN("%s: failed to connect", r->host);
    r->connected = r->checking = false;

    tll_foreach(footd.remote_sessions, it) {
        struct remote_session *rs = it->item;
        if (rs->remote != r)
            continue;

        if (rs->channel != 0) {
            const int fd = session_mux_take(r->mux, rs->channel);
            rs->channel = 0;
            if (fd >= 0)
                keep_disconnected(rs, fd);
        }

        /* Not created there, and its window is gone: nobody wants it */
        if (!rs->started && rs->fd < 0)
            remote_session_free(rs);
    }

    session_mux_destroy(r->mux);
    r->mux = NULL;

    if (r->ssh > 0)
        kill(r->ssh, SIGTERM);

    /* footd isn't there (or is too old): not tried again, but installed */
    if (r->install != NULL)
        remote_install_needed(r);
    else
        remote_retry(r);
}

static void
ssh_exited(struct reaper *reaper, pid_t pid, int status, void *data)
{
    struct remote *r = data;
    if (r->ssh == pid)
        r->ssh = 0;
}

/*
 * Installing footd there (the remote command said it isn't, or is too
 * old: 'install'). It's copied over SSH, to ~/.local/bin/footd: foot.app
 * has it for Linux (x86_64, aarch64; linked statically), and for macOS
 * (this footd, which is universal). Replacing an old one isn't asked;
 * else the windows ask first.
 */

/* footd for 'install' ("SYSTEM MACHINE", as uname says), or NULL ('*error') */
static char *
footd_for(const char *install, const char **error)
{
    char system[32], machine[32];
    if (sscanf(install, "%31s %31s", system, machine) != 2) {
        *error = "unknown system";
        return NULL;
    }

    const char *arch =
        strcmp(machine, "x86_64") == 0 || strcmp(machine, "amd64") == 0 ? "x86_64" :
        strcmp(machine, "aarch64") == 0 || strcmp(machine, "arm64") == 0 ? "aarch64" : NULL;
    if (arch == NULL || (strcmp(system, "Linux") != 0 && strcmp(system, "Darwin") != 0)) {
        *error = "foot has no footd for it";
        return NULL;
    }

    char *self = footd_own_path();
    if (self == NULL) {
        *error = "this footd's executable isn't found";
        return NULL;
    }

    /* This one, for a machine like this one (on macOS: universal) */
    struct utsname here;
    if (uname(&here) == 0 && strcmp(here.sysname, system) == 0 &&
        (strcmp(system, "Darwin") == 0 ||
         strcmp(here.machine, machine) == 0 ||
         (strcmp(arch, "aarch64") == 0 && strcmp(here.machine, "arm64") == 0)))
    {
        return self;
    }

    /* foot.app's: Contents/Resources (this one is in Contents/MacOS) */
    char *slash = strrchr(self, '/');
    if (slash != NULL)
        *slash = '\0';
    char *path = xasprintf("%s/../Resources/footd-%s-%s", self,
                           strcmp(system, "Linux") == 0 ? "linux" : "macos", arch);
    free(self);

    if (access(path, R_OK) < 0) {
        *error = "this foot has no footd for it (foot.app has, when built with Docker)";
        free(path);
        return NULL;
    }
    return path;
}

/* The windows ask (Enter installs it: fdm_disconnected_window()) */
static void
ask_install(struct remote_session *rs)
{
    const struct remote *r = rs->remote;
    tell_window(rs, "foot's footd isn't on %s (%s) yet. Press Enter to install it: "
                "one file, ~/.local/bin/footd, no sudo. Or close this window.",
                r->host, r->install);
}

static void
remote_install_needed(struct remote *r)
{
    timer_set(r->timer, 0);

    /* Nobody waits for it (any more) */
    if (!remote_awaited(r)) {
        free(r->install);
        r->install = NULL;
        return;
    }

    if (r->outdated) {
        remote_install(r);
        return;
    }

    r->asking = true;
    tll_foreach(footd.remote_sessions, it) {
        if (it->item->remote == r && it->item->fd >= 0)
            ask_install(it->item);
    }
}

static pid_t spawn_ssh(struct remote *r, int in, int out, const char *command,
                       reaper_cb exited);

/* ssh has installed it (or failed to): connected, for the windows */
static void
installer_exited(struct reaper *reaper, pid_t pid, int status, void *data)
{
    struct remote *r = data;
    if (r->installer != pid)
        return;

    r->installer = 0;
    ssh_log_read(r);
    free(r->install);
    r->install = NULL;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        LOG_INFO("%s: footd installed", r->host);
        if (remote_awaited(r))
            remote_connect(r);
        return;
    }

    LOG_WARN("%s: failed to install footd", r->host);
    tell_windows(r, "failed to install footd on %s%s%s; type to try again", r->host, WHY(r));
}

static void
remote_install(struct remote *r)
{
    r->asking = false;

    const char *error = NULL;
    char *path = footd_for(r->install, &error);
    const int fd = path != NULL ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) {
        if (error == NULL)
            error = strerror(errno);
        LOG_ERR("%s: can't install footd (%s): %s", r->host, r->install, error);
        tell_windows(r, "can't install footd on %s (%s): %s; type to try again",
                     r->host, r->install, error);
        free(path);
        free(r->install);
        r->install = NULL;
        return;
    }

    LOG_INFO("%s: installing %s", r->host, path);
    tell_windows(r, "%s footd on %s…", r->outdated ? "updating" : "installing", r->host);

    free(r->error);
    r->error = NULL;
    const int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    r->installer = spawn_ssh(r, fd, null, install_command, &installer_exited);
    close(fd);
    if (null >= 0)
        close(null);
    free(path);

    if (r->installer == 0) {
        free(r->install);
        r->install = NULL;
        tell_windows(r, "failed to install footd on %s; type to try again", r->host);
    }
}

/* UNINSTALL's answer ('error' NULL: it's removed) */
static void
send_uninstalled(struct client *c, const char *host, const char *error)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_UNINSTALLED);
    session_msg_str(&msg, host);
    session_msg_str(&msg, error);
    client_send(c, &msg, -1);
    session_msg_free(&msg);
}

static void
uninstaller_exited(struct reaper *reaper, pid_t pid, int status, void *data)
{
    struct remote *r = data;
    if (r->uninstaller != pid)
        return;

    r->uninstaller = 0;
    ssh_log_read(r);

    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok)
        LOG_INFO("%s: footd removed", r->host);
    else
        LOG_WARN("%s: failed to remove footd", r->host);

    if (r->uninstalling != NULL) {
        send_uninstalled(r->uninstalling, r->host,
                         ok ? NULL : r->error != NULL ? r->error : "failed");
    }
    r->uninstalling = NULL;

    /* Windows opened there meanwhile: connected (footd is installed again, once asked) */
    if (remote_awaited(r))
        remote_connect(r);
}

/* UNINSTALL: footd is removed there, its sessions end, and their windows here close */
static void
remote_uninstall(struct remote *r, struct client *c)
{
    if (r->uninstaller > 0) {
        send_uninstalled(c, r->host, "it's being removed already");
        return;
    }

    LOG_INFO("%s: removing footd", r->host);

    tll_foreach(footd.remote_sessions, it) {
        struct remote_session *rs = it->item;
        if (rs->remote != r)
            continue;
        if (rs->client != NULL)
            send_failed(rs->client, rs->id, rs->token, "footd was removed there");
        remote_session_free(rs);
    }

    /* Nothing goes on there any more */
    timer_set(r->timer, 0);
    session_mux_destroy(r->mux);
    r->mux = NULL;
    if (r->ssh > 0)
        kill(r->ssh, SIGTERM);
    if (r->installer > 0)
        kill(r->installer, SIGTERM);
    r->connected = r->checking = r->asking = false;
    free(r->install);
    r->install = NULL;
    free(r->error);
    r->error = NULL;

    const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    r->uninstaller = spawn_ssh(r, null, null, uninstall_command, &uninstaller_exited);
    if (null >= 0)
        close(null);

    if (r->uninstaller > 0)
        r->uninstalling = c;
    else
        send_uninstalled(c, r->host, "failed to run ssh");
}

/* The remote command says footd isn't there, or is too old: "SYSTEM MACHINE OLD" (or "none") */
static void
install_mark(struct remote *r, const char *what)
{
    LOG_INFO("%s: footd: %s", r->host, what);

    const char *old = strrchr(what, ' ');
    free(r->install);
    r->install = old != NULL ? xstrndup(what, old - what) : xstrdup(what);
    r->outdated = old != NULL && strcmp(old + 1, "none") != 0;

    /* (Said after the connection was gone) */
    if (r->mux == NULL && r->installer == 0 && !r->asking)
        remote_install_needed(r);
}

/* ssh's errors, in the log; the last one is kept (e.g. why it couldn't connect) */
static void
ssh_log_read(struct remote *r)
{
    if (r->ssh_log < 0)
        return;

    char buf[1024];
    ssize_t n;
    while ((n = read(r->ssh_log, buf, sizeof(buf) - 1)) > 0 || (n < 0 && errno == EINTR)) {
        if (n <= 0)
            continue;
        buf[n] = '\0';
        for (char *line = strtok(buf, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
            if (strncmp(line, INSTALL_MARK, strlen(INSTALL_MARK)) == 0) {
                install_mark(r, line + strlen(INSTALL_MARK));
                continue;
            }
            LOG_WARN("%s: %s", r->host, line);
            free(r->error);
            r->error = xstrdup(line);
        }
    }

    if (n == 0 || errno != EAGAIN) {
        fdm_del(footd.fdm, r->ssh_log);
        r->ssh_log = -1;
    }
}

static bool
fdm_ssh_log(struct fdm *fdm, int fd, int events, void *data)
{
    ssh_log_read(data);
    return true;
}

/*
 * ssh to the other machine, running 'command' with 'in' and 'out' (its
 * stderr is logged: ssh_log_read()). New hosts are trusted the first
 * time (their key is added to ~/.ssh/known_hosts), as ssh asks then;
 * not if their key has changed. 0: failed.
 */
static pid_t
spawn_ssh(struct remote *r, int in, int out, const char *command, reaper_cb exited)
{
    if (in < 0 || out < 0)
        return 0;

    int log[2];
    if (pipe(log) < 0) {
        LOG_ERRNO("%s: failed to create a pipe", r->host);
        return 0;
    }
    fcntl(log[0], F_SETFD, FD_CLOEXEC);
    fcntl(log[1], F_SETFD, FD_CLOEXEC);

    /* $FOOTD_SSH: instead of ssh (e.g. tests) */
    const char *ssh = getenv("FOOTD_SSH");
    if (ssh == NULL || ssh[0] == '\0')
        ssh = "ssh";

    char *const argv[] = {
        (char *)ssh, "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=accept-new", "--", r->host, (char *)command, NULL,
    };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, log[1], STDERR_FILENO);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);

    sigset_t mask;
    sigemptyset(&mask);
    posix_spawnattr_setsigmask(&attr, &mask);
    sigset_t defaults;
    sigfillset(&defaults);
    posix_spawnattr_setsigdefault(&attr, &defaults);

    pid_t pid;
    const int err = posix_spawnp(&pid, ssh, &actions, &attr, argv, environ);

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(log[1]);

    if (err != 0) {
        errno = err;
        LOG_ERRNO("%s: failed to run %s", r->host, ssh);
        close(log[0]);
        return 0;
    }

    reaper_add(footd.reaper, pid, exited, r);

    if (r->ssh_log >= 0)
        fdm_del(footd.fdm, r->ssh_log);
    r->ssh_log = log[0];
    if (!set_nonblocking(log[0]) || !fdm_add(footd.fdm, log[0], EPOLLIN, &fdm_ssh_log, r)) {
        close(log[0]);
        r->ssh_log = -1;
    }
    return pid;
}

/* Connects (if it isn't), and attaches the windows that were disconnected */
static bool
remote_connect(struct remote *r)
{
    if (r->mux != NULL)
        return true;

    /* footd is being installed there (or the windows ask), or removed: later */
    if (r->asking || r->installer > 0 || r->uninstaller > 0)
        return true;

    LOG_INFO("%s: connecting", r->host);
    r->tried_at = now_ms();
    free(r->error);
    r->error = NULL;
    free(r->install);
    r->install = NULL;
    if (r->timer < 0)
        r->timer = timer_new(&fdm_remote_timer, r);
    timer_set(r->timer, 0);

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        LOG_ERRNO("%s: failed to create a socket", r->host);
        remote_retry(r);
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    char *command = remote_command();
    r->ssh = spawn_ssh(r, fds[1], fds[1], command, &ssh_exited);
    free(command);
    close(fds[1]);
    if (r->ssh == 0) {
        close(fds[0]);
        remote_retry(r);
        return false;
    }

    r->mux = session_mux_new(footd.fdm, fds[0], fds[0], &remote_callbacks, r);
    if (r->mux == NULL) {
        remote_retry(r);
        return false;
    }

    /* (As a check: the other footd's WELCOME answers it) */
    r->checking = true;
    timer_set(r->timer, CONNECT_TIMEOUT);

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_HELLO);
    session_msg_u32(&msg, SESSION_PROTOCOL_VERSION);
    session_msg_i32(&msg, getpid());
    remote_send(r, &msg);
    session_msg_free(&msg);

    tll_foreach(footd.remote_sessions, it) {
        struct remote_session *rs = it->item;
        if (rs->remote != r || rs->fd < 0 || r->mux == NULL)
            continue;

        const int fd = rs->fd;
        rs->fd = -1;
        fdm_del_no_close(footd.fdm, fd);

        /* (Created, if it couldn't be) */
        if (!rs->started && rs->create.len > 0)
            remote_send_with_window(rs, &rs->create, fd);
        else {
            attach_msg(&msg, rs);
            remote_send_with_window(rs, &msg, fd);
            session_msg_free(&msg);
        }
    }

    return r->mux != NULL;
}

/*
 * The network changed (e.g. another Wi-Fi, or the Mac woke up): the
 * connections are checked, and those lost are tried again (macOS; a
 * notify(3) file descriptor). Watched once there are sessions on other
 * machines.
 */
#if defined(__APPLE__)
static bool
fdm_network_settled(struct fdm *fdm, int fd, int events, void *data)
{
    if (!timer_expired(fd))
        return true;

    LOG_INFO("the network changed");
    tll_foreach(footd.remotes, it) {
        struct remote *r = it->item;
        r->tries = 0;
        if (r->mux != NULL)
            remote_check(r);
        else if (remote_awaited(r))
            remote_reconnect(r);
    }
    return true;
}

static bool
fdm_network(struct fdm *fdm, int fd, int events, void *data)
{
    int token;
    while (read(fd, &token, sizeof(token)) == sizeof(token))
        ;
    timer_set(footd.network_timer, NETWORK_SETTLE);
    return true;
}
#endif

static void
network_watch(void)
{
#if defined(__APPLE__)
    if (footd.network_fd >= 0)
        return;

    int fd = -1;
    if (notify_register_file_descriptor(kNotifySCNetworkChange, &fd, 0,
                                        &footd.network_token) != NOTIFY_STATUS_OK)
    {
        LOG_WARN("failed to watch the network");
        return;
    }
    if (!set_nonblocking(fd) ||
        (footd.network_timer = timer_new(&fdm_network_settled, NULL)) < 0 ||
        !fdm_add(footd.fdm, fd, EPOLLIN, &fdm_network, NULL))
    {
        LOG_WARN("failed to watch the network");
        notify_cancel(footd.network_token);
        return;
    }
    footd.network_fd = fd;
#endif
}

static void
network_unwatch(void)
{
#if defined(__APPLE__)
    if (footd.network_fd >= 0) {
        fdm_del_no_close(footd.fdm, footd.network_fd);
        notify_cancel(footd.network_token);
        footd.network_fd = -1;
    }
#endif
    if (footd.network_timer >= 0)
        fdm_del(footd.fdm, footd.network_timer);
    footd.network_timer = -1;
}

static struct remote *
remote_get(const char *host)
{
    tll_foreach(footd.remotes, it) {
        if (strcmp(it->item->host, host) == 0)
            return it->item;
    }

    network_watch();

    struct remote *r = xmalloc(sizeof(*r));
    *r = (struct remote){.host = xstrdup(host), .ssh_log = -1, .timer = -1};
    tll_push_back(footd.remotes, r);
    return r;
}

static void
remote_free(struct remote *r)
{
    session_mux_destroy(r->mux);
    if (r->ssh > 0) {
        reaper_del(footd.reaper, r->ssh);
        kill(r->ssh, SIGTERM);
    }
    /* (Installing or removing footd there goes on) */
    if (r->installer > 0)
        reaper_del(footd.reaper, r->installer);
    if (r->uninstaller > 0)
        reaper_del(footd.reaper, r->uninstaller);
    if (r->ssh_log >= 0)
        fdm_del(footd.fdm, r->ssh_log);
    if (r->timer >= 0)
        fdm_del(footd.fdm, r->timer);
    free(r->install);
    free(r->error);
    free(r->host);
    free(r);
}

/*
 * A window wants 'rs': it's attached there, with its socket 'fd' (or
 * it's created there: its CREATE is kept, until it's started)
 */
static void
remote_attach_window(struct remote_session *rs, struct client *c, uint64_t token,
                     const struct session_size *size, int fd)
{
    /* Another window here had it: it's no longer attached (its terminal ends) */
    if (rs->client != NULL && (rs->client != c || rs->token != token)) {
        struct session_msg detached = {0};
        session_msg_begin(&detached, SESSION_MSG_DETACHED);
        session_msg_u64(&detached, rs->id);
        session_msg_u64(&detached, rs->token);
        client_send(rs->client, &detached, -1);
        session_msg_free(&detached);
    }
    remote_session_close_window(rs);

    rs->client = rs->last_client = c;
    rs->token = rs->last_token = token;
    rs->size = *size;
    rs->detached = false;

    if (!remote_connect(rs->remote)) {
        send_failed(c, rs->id, token, "failed to connect");
        close(fd);
        remote_session_free(rs);
        return;
    }

    if (!rs->started && rs->create.len > 0)
        remote_send_with_window(rs, &rs->create, fd);
    else {
        struct session_msg attach = {0};
        attach_msg(&attach, rs);
        remote_send_with_window(rs, &attach, fd);
        session_msg_free(&attach);
    }
}

/* Ends what's left of it, as it would end there: those no window here has (KILL 0) */
static void
remote_kill(struct remote_session *rs, uint64_t token)
{
    /* Never created there (it couldn't connect): nothing to end */
    if (!rs->started && rs->remote->mux == NULL)
        return;

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_KILL);
    session_msg_u64(&msg, rs->id);
    session_msg_u64(&msg, token);
    if (remote_connect(rs->remote))
        remote_send(rs->remote, &msg);
    session_msg_free(&msg);
}

/*
 * Messages
 */

static bool
handle_create(struct client *c, struct session_reader *r)
{
    uint64_t id = session_read_u64(r);
    uint64_t token = session_read_u64(r);
    struct session_size size = session_read_size(r);
    char *cwd = session_read_str(r);
    char *conf_path = session_read_str(r);
    char *term_env = session_read_str(r);
    char *shell = session_read_str(r);
    bool login_shell = session_read_u8(r);
    uint32_t scrollback = session_read_u32(r);
    char **argv = session_read_strv(r);
    char **envp = session_read_strv(r);
    char *host = session_read_more(r) ? session_read_str(r) : NULL;
    int fd = session_conn_take_fd(&c->conn);

    bool ok = r->ok && fd >= 0;
    if (!ok)
        LOG_ERR("client (PID=%d): malformed CREATE", c->pid);
    else if (session_find(id) != NULL || remote_session_find(id) != NULL)
        send_failed(c, id, token, "already exists");
    else if (host != NULL && host[0] != '\0') {
        /* On another machine: there, it's created as here (but for the host) */
        struct session_msg msg = {0};
        session_msg_begin(&msg, SESSION_MSG_CREATE);
        session_msg_u64(&msg, id);
        session_msg_u64(&msg, token);
        session_msg_size(&msg, &size);
        session_msg_str(&msg, cwd);
        session_msg_str(&msg, conf_path);
        session_msg_str(&msg, term_env);
        session_msg_str(&msg, shell);
        session_msg_u8(&msg, login_shell);
        session_msg_u32(&msg, scrollback);
        session_msg_strv(&msg, (const char *const *)argv);
        session_msg_strv(&msg, (const char *const *)envp);

        struct remote_session *rs = xmalloc(sizeof(*rs));
        *rs = (struct remote_session){
            .id = id, .remote = remote_get(host), .fd = -1, .create = msg};
        tll_push_back(footd.remote_sessions, rs);
        LOG_INFO("session %016llx: on %s", (unsigned long long)id, host);

        remote_attach_window(rs, c, token, &size, fd);
        fd = -1;
    } else {
        const char *error = NULL;
        struct session *s = session_create(
            id, cwd, conf_path, term_env, shell, login_shell, scrollback,
            argv, envp, &size, &error);

        if (s == NULL)
            send_failed(c, id, token, error);
        else if (attach(s, c, token, fd, &size)) {
            send_started(c, s, token);
            fd = -1;
        } else {
            /* Nobody would ever see it */
            fd = -1;
            send_failed(c, id, token, "failed to attach");
            session_kill(s);
        }
    }

    if (fd >= 0)
        close(fd);

    free(cwd);
    free(conf_path);
    free(term_env);
    free(shell);
    free(host);
    session_strv_free(argv);
    session_strv_free(envp);
    return ok;
}

static bool
handle_attach(struct client *c, struct session_reader *r)
{
    uint64_t id = session_read_u64(r);
    uint64_t token = session_read_u64(r);
    struct session_size size = session_read_size(r);
    int fd = session_conn_take_fd(&c->conn);

    if (!r->ok || fd < 0) {
        LOG_ERR("client (PID=%d): malformed ATTACH", c->pid);
        if (fd >= 0)
            close(fd);
        return false;
    }

    struct session *s = session_find(id);
    struct remote_session *rs = s == NULL ? remote_session_find(id) : NULL;
    if (rs != NULL) {
        remote_attach_window(rs, c, token, &size, fd);
        return true;
    }

    if (s == NULL || s->killed) {
        /* It ended meanwhile: closing the socket ends the window's terminal */
        send_failed(c, id, token, "no such session");
        close(fd);
        return true;
    }

    if (attach(s, c, token, fd, &size)) {
        LOG_INFO("session %016llx: attached", (unsigned long long)id);
        send_started(c, s, token);
    } else
        send_failed(c, id, token, "failed to attach");
    return true;
}

/* Only the window that's attached */
static struct session *
attached_session(struct client *c, uint64_t id, uint64_t token)
{
    struct session *s = session_find(id);
    if (s == NULL || s->attachment == NULL ||
        s->attachment->client != c || s->attachment->token != token)
    {
        return NULL;
    }
    return s;
}

static void
handle_list(struct client *c)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_SESSIONS);

    uint32_t count = tll_length(footd.remote_sessions);
    tll_foreach(footd.sessions, it) {
        if (!it->item->killed)
            count++;
    }
    session_msg_u32(&msg, count);

    tll_foreach(footd.sessions, it) {
        const struct session *s = it->item;
        if (s->killed)
            continue;

        session_msg_u64(&msg, s->id);
        session_msg_u8(&msg, s->attachment != NULL);
        session_msg_i32(&msg, s->pid);
        session_msg_str(&msg, s->term != NULL ? s->term->window_title : NULL);
        session_msg_str(&msg, s->term != NULL ? s->term->cwd : NULL);
    }

    /* On other machines: their title, directory and PID are theirs */
    tll_foreach(footd.remote_sessions, it) {
        const struct remote_session *rs = it->item;
        session_msg_u64(&msg, rs->id);
        session_msg_u8(&msg, rs->client != NULL);
        session_msg_i32(&msg, 0);
        session_msg_str(&msg, NULL);
        session_msg_str(&msg, NULL);
    }

    session_msg_blob(&msg, footd.layout, footd.layout_len);
    client_send(c, &msg, -1);
    session_msg_free(&msg);
}

/* Returns false if the client must go */
static bool
handle_message(struct client *c, uint32_t type, struct session_reader *r)
{
    if (!c->greeted && type != SESSION_MSG_HELLO) {
        LOG_ERR("client (PID=%d): no HELLO", c->pid);
        return false;
    }

    switch (type) {
    case SESSION_MSG_HELLO: {
        uint32_t version = session_read_u32(r);
        session_read_i32(r);
        if (!r->ok || version != SESSION_PROTOCOL_VERSION) {
            LOG_ERR("client (PID=%d): protocol version %u (expected %d)",
                    c->pid, version, SESSION_PROTOCOL_VERSION);
            return false;
        }

        c->greeted = true;

        struct session_msg msg = {0};
        session_msg_begin(&msg, SESSION_MSG_WELCOME);
        session_msg_u32(&msg, SESSION_PROTOCOL_VERSION);
        session_msg_i32(&msg, getpid());
        session_msg_u32(&msg, SESSION_FEATURE_REMOTE | SESSION_FEATURE_UNINSTALL);
        client_send(c, &msg, -1);
        session_msg_free(&msg);
        return true;
    }

    case SESSION_MSG_CREATE:
        return handle_create(c, r);

    case SESSION_MSG_ATTACH:
        return handle_attach(c, r);

    case SESSION_MSG_RESIZE: {
        uint64_t id = session_read_u64(r);
        uint64_t token = session_read_u64(r);
        struct session_size size = session_read_size(r);
        if (!r->ok)
            return false;

        struct session *s = attached_session(c, id, token);
        if (s != NULL)
            resize(s, &size);

        struct remote_session *rs = s == NULL ? remote_session_find(id) : NULL;
        if (rs != NULL && rs->client == c && rs->token == token) {
            rs->size = size;
            struct session_msg msg = {0};
            session_msg_begin(&msg, SESSION_MSG_RESIZE);
            session_msg_u64(&msg, id);
            session_msg_u64(&msg, token);
            session_msg_size(&msg, &size);
            remote_send(rs->remote, &msg);
            session_msg_free(&msg);
        }
        return true;
    }

    case SESSION_MSG_KILL: {
        uint64_t id = session_read_u64(r);
        uint64_t token = session_read_u64(r);
        if (!r->ok)
            return false;

        /*
         * Not if another window has attached to it meanwhile. Token 0:
         * one that no window is attached to; ID 0 too: all of them
         * (foot quits ending its sessions, and its windows end theirs)
         */
        if (id == 0 && token == 0) {
            tll_foreach(footd.sessions, it) {
                if (!it->item->killed && it->item->attachment == NULL)
                    session_kill(it->item);
            }

            /* On other machines, only those of windows here (not all there) */
            tll_foreach(footd.remote_sessions, it) {
                struct remote_session *rs = it->item;
                if (rs->client == NULL && !rs->detached) {
                    remote_kill(rs, 0);
                    remote_session_free(rs);
                }
            }
            return true;
        }

        struct remote_session *rs = remote_session_find(id);
        if (rs != NULL) {
            if (token == 0 ? rs->client == NULL
                           : rs->last_client == c && rs->last_token == token)
            {
                /* Once it's ended there, its window's socket is closed */
                remote_kill(rs, token);
                if (rs->channel == 0)
                    remote_session_free(rs);
            }
            return true;
        }

        struct session *s = session_find(id);
        if (s != NULL && !s->killed &&
            (token == 0 ? s->attachment == NULL
                        : s->last_client == c && s->last_token == token))
        {
            session_kill(s);
        }
        return true;
    }

    case SESSION_MSG_LIST:
        handle_list(c);
        return true;

    case SESSION_MSG_LAYOUT: {
        size_t len;
        const void *layout = session_read_blob(r, &len);
        if (!r->ok)
            return false;

        free(footd.layout);
        footd.layout = len > 0 ? xmemdup(layout, len) : NULL;
        footd.layout_len = len;
        return true;
    }

    case SESSION_MSG_UNINSTALL: {
        char *host = session_read_str(r);
        if (!r->ok || host == NULL || host[0] == '\0') {
            free(host);
            return false;
        }
        remote_uninstall(remote_get(host), c);
        free(host);
        return true;
    }
    }

    LOG_WARN("client (PID=%d): unknown message type %u", c->pid, type);
    return true;
}

static bool
fdm_client(struct fdm *fdm, int fd, int events, void *data)
{
    struct client *c = data;

    if (events & EPOLLOUT) {
        if (!session_conn_flush(&c->conn)) {
            client_free(c);
            return true;
        }
        if (!session_conn_has_output(&c->conn)) {
            c->writing = false;
            fdm_event_del(fdm, fd, EPOLLOUT);
        }
    }

    if (events & (EPOLLIN | EPOLLHUP)) {
        bool eof;
        bool ok = session_conn_read(&c->conn, &eof);

        uint32_t type;
        struct session_reader r;
        bool error;
        while (session_conn_next(&c->conn, &type, &r, &error)) {
            if (!handle_message(c, type, &r)) {
                client_free(c);
                return true;
            }
        }

        if (!ok || error) {
            if (!eof)
                LOG_WARN("client (PID=%d): connection error", c->pid);
            client_free(c);
            return true;
        }
    }

    return true;
}

static void
client_free(struct client *c)
{
    LOG_DBG("client (PID=%d): gone", c->pid);
    listen_pause(false);

    /* Its windows' sockets detach them, when they're closed */
    tll_foreach(footd.sessions, it) {
        if (it->item->attachment != NULL && it->item->attachment->client == c)
            it->item->attachment->client = NULL;
        if (it->item->last_client == c)
            it->item->last_client = NULL;
    }
    tll_foreach(footd.remote_sessions, it) {
        if (it->item->client == c)
            it->item->client = NULL;
        if (it->item->last_client == c)
            it->item->last_client = NULL;
    }
    tll_foreach(footd.remotes, it) {
        if (it->item->uninstalling == c)
            it->item->uninstalling = NULL;
    }

    tll_foreach(footd.clients, it) {
        if (it->item == c) {
            tll_remove(footd.clients, it);
            break;
        }
    }

    if (c->conn.fd >= 0) {
        fdm_del_no_close(footd.fdm, c->conn.fd);
        session_conn_close(&c->conn);
    }
    free(c);
}

/*
 * Without file descriptors left, nobody is accepted (the socket isn't
 * watched) until some are closed: when a session or client is gone.
 */
static void
listen_pause(bool pause)
{
    if (footd.listen_paused == pause || footd.listen_fd < 0)
        return;

    footd.listen_paused = pause;
    if (pause)
        fdm_event_del(footd.fdm, footd.listen_fd, EPOLLIN);
    else
        fdm_event_add(footd.fdm, footd.listen_fd, EPOLLIN);
}

/* Never fails: nobody else's sessions depend on one connection */
static bool
accept_client(void)
{
    int fd = accept(footd.listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
            LOG_ERRNO("failed to accept a connection; waiting for file descriptors");
            listen_pause(true);
        } else if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED)
            LOG_ERRNO("failed to accept a connection");
        return true;
    }

    /* Only the user's own */
    uid_t uid = (uid_t)-1;
    pid_t pid = -1;
#if defined(__APPLE__)
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) < 0)
        uid = (uid_t)-1;
    socklen_t len = sizeof(pid);
    getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len);
#else
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        uid = cred.uid;
        pid = cred.pid;
    }
#endif
    if (uid != getuid()) {
        LOG_WARN("refusing a connection from another user (UID=%d)", (int)uid);
        close(fd);
        return true;
    }

    if (!set_nonblocking(fd)) {
        close(fd);
        return true;
    }

    struct client *c = xmalloc(sizeof(*c));
    *c = (struct client){.pid = pid};
    session_conn_init(&c->conn, fd);

    if (!fdm_add(footd.fdm, fd, EPOLLIN, &fdm_client, c)) {
        close(fd);
        free(c);
        return true;
    }

    tll_push_back(footd.clients, c);
    LOG_DBG("client (PID=%d): connected", pid);
    return true;
}

static bool
fdm_listen(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP) {
        LOG_ERR("the socket is gone");
        return false;
    }
    return accept_client();
}

/*
 * Exiting
 */

static bool
fdm_signal_quit(struct fdm *fdm, int signo, void *data)
{
    footd.quit = true;
    return true;
}

/* Once footd doesn't accept connections any more */
static void
release_running_lock(void)
{
    if (footd.running_lock >= 0) {
        close(footd.running_lock);
        footd.running_lock = -1;
    }
}

/*
 * Without sessions, and windows: exit. Under the lock foot takes while
 * connecting, so that it either connects before this, or starts a new
 * footd after it.
 */
static bool
idle_exit(void)
{
    /* (Those on other machines too: their windows are laid out here) */
    if (tll_length(footd.sessions) > 0 || tll_length(footd.clients) > 0 ||
        tll_length(footd.remote_sessions) > 0)
    {
        return false;
    }

    int lock = footd.lock_path != NULL
        ? open(footd.lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600) : -1;
    if (lock >= 0 && flock(lock, LOCK_EX) < 0) {
        close(lock);
        lock = -1;
    }

    /* Someone connected meanwhile */
    accept_client();
    if (tll_length(footd.clients) > 0) {
        if (lock >= 0)
            close(lock);
        return false;
    }

    if (footd.socket_path != NULL)
        unlink(footd.socket_path);

    /* A footd foot starts now takes over (the lock is released when we're gone) */
    release_running_lock();
    return true;
}

/*
 * A UTF-8 one, as foot's (main.c): the terminals check titles, app IDs
 * etc with its character classes (widths are utf8proc's)
 */
static void
set_locale(void)
{
    static const char *const locales[] = {"", "C.UTF-8", "en_US.UTF-8"};

    for (size_t i = 0; i < ALEN(locales); i++) {
        char32_t c;
        if (setlocale(LC_CTYPE, locales[i]) != NULL &&
            mbrtoc32(&c, u8"ö", 2, &(mbstate_t){0}) == 2 && c == U'ö')
        {
            return;
        }
    }

    LOG_WARN("no UTF-8 locale: titles with other than ASCII characters are ignored");
}

static void
print_usage(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS...]\n"
        "\n"
        "foot's sessions (foot starts it when needed).\n"
        "\n"
        "Options:\n"
        "  -s,--socket=PATH         the socket to listen on (default: foot's)\n"
        "  -f,--listen-fd=FD        listen on this socket (already bound)\n"
        "  -d,--log-level={info|warning|error|none}  log level (warning)\n"
        "  -l,--log-to-stderr       log to stderr, not the system log\n"
        "  -b,--bridge              for footd on another machine, over SSH: its\n"
        "                           sessions on the footd here (stdin, stdout)\n"
        "     --bridge-level        what --bridge can do (a number), for that footd\n"
        "  -v,--version             show the version number and quit\n",
        prog);
}

int
main(int argc, char *const *argv)
{
    static const struct option longopts[] = {
        {"socket",        required_argument, NULL, 's'},
        {"listen-fd",     required_argument, NULL, 'f'},
        {"log-level",     required_argument, NULL, 'd'},
        {"log-to-stderr", no_argument,       NULL, 'l'},
        {"bridge",        no_argument,       NULL, 'b'},
        {"bridge-level",  no_argument,       NULL, 'L'},
        {"version",       no_argument,       NULL, 'v'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL,            no_argument,       NULL, 0},
    };

    const char *socket_path = NULL;
    int listen_fd = -1;
    enum log_class log_level = LOG_CLASS_WARNING;
    bool log_to_stderr = false;
    bool bridge = false;

    int c;
    while ((c = getopt_long(argc, argv, "s:f:d:lbvh", longopts, NULL)) != -1) {
        switch (c) {
        case 's':
            socket_path = optarg;
            break;

        case 'f':
            listen_fd = atoi(optarg);
            break;

        case 'd': {
            int lvl = log_level_from_string(optarg);
            if (lvl < 0) {
                fprintf(stderr, "-d,--log-level: %s: invalid log level\n", optarg);
                return EXIT_FAILURE;
            }
            log_level = lvl;
            break;
        }

        case 'l':
            log_to_stderr = true;
            break;

        case 'b':
            bridge = true;
            break;

        case 'L':
            printf("%d\n", FOOTD_BRIDGE_LEVEL);
            return EXIT_SUCCESS;

        case 'v':
            printf("footd version %s\n", FOOT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;

        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    log_init(LOG_COLORIZE_AUTO, !log_to_stderr, LOG_FACILITY_DAEMON, log_level);
    set_locale();

    /* Never from (or on to) a terminal */
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGHUP, &ignore, NULL);
    sigaction(SIGPIPE, &ignore, NULL);

    if (bridge)
        return footd_bridge(socket_path);

    char *path = socket_path != NULL ? xstrdup(socket_path) : session_socket_path();
    if (path == NULL) {
        LOG_ERR("no socket path (HOME not set?)");
        return EXIT_FAILURE;
    }

    footd.socket_path = path;
    footd.lock_path = session_lock_path(path);

    /*
     * As long as it runs: foot tells a footd that's busy from one
     * that's gone (and left its socket behind) by it
     */
    char *running_path = session_running_path(path);
    footd.running_lock = open(running_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (footd.running_lock < 0 || flock(footd.running_lock, LOCK_EX | LOCK_NB) < 0) {
        LOG_ERRNO("%s: another footd is running", running_path);
        free(running_path);
        return EXIT_FAILURE;
    }
    free(running_path);

    /*
     * Each session takes a dozen file descriptors (its PTY, sockets,
     * and its terminal's timers); applications start with a limit of
     * 256. What runs in the sessions gets the usual one
     * (slave_exec()).
     */
    rlimits_raise_open_files();

    if (listen_fd < 0) {
        /* Standalone (foot hands over a listening socket otherwise) */
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        if (strlen(path) >= sizeof(addr.sun_path)) {
            LOG_ERR("%s: socket path too long", path);
            return EXIT_FAILURE;
        }
        strcpy(addr.sun_path, path);

        listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        unlink(path);
        if (listen_fd < 0 ||
            bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            chmod(path, 0600) < 0 || listen(listen_fd, 16) < 0)
        {
            LOG_ERRNO("%s: failed to listen", path);
            return EXIT_FAILURE;
        }
    }

    if (!set_nonblocking(listen_fd)) {
        LOG_ERRNO("failed to configure the socket");
        return EXIT_FAILURE;
    }
    footd.listen_fd = listen_fd;

    int ret = EXIT_FAILURE;

    if ((footd.fdm = fdm_init()) == NULL ||
        (footd.reaper = reaper_init(footd.fdm)) == NULL ||
        !headless_init(footd.fdm, footd.reaper) ||
        !fdm_add(footd.fdm, listen_fd, EPOLLIN, &fdm_listen, NULL) ||
        !fdm_signal_add(footd.fdm, SIGTERM, &fdm_signal_quit, NULL) ||
        !fdm_signal_add(footd.fdm, SIGINT, &fdm_signal_quit, NULL))
    {
        goto out;
    }

    LOG_INFO("footd %s: listening on %s", FOOT_VERSION, path);

    for (;;) {
        if (!fdm_poll(footd.fdm)) {
            LOG_ERR("FDM failure");
            break;
        }

        if (footd.quit) {
            LOG_INFO("quitting");
            ret = EXIT_SUCCESS;
            break;
        }

        if (idle_exit()) {
            LOG_INFO("no sessions left");
            ret = EXIT_SUCCESS;
            break;
        }
    }

out:
    if (footd.fdm != NULL) {
        fdm_del(footd.fdm, listen_fd);
        fdm_signal_del(footd.fdm, SIGTERM);
        fdm_signal_del(footd.fdm, SIGINT);
    } else
        close(listen_fd);
    footd.listen_fd = -1;

    /* Connections are refused now: foot starts another footd (see connect_footd()) */
    release_running_lock();

    /* The sessions' programs get a SIGHUP, as the PTYs are closed */
    tll_foreach(footd.sessions, it) {
        session_free(it->item);
        tll_remove(footd.sessions, it);
    }
    tll_foreach(footd.clients, it)
        client_free(it->item);
    tll_foreach(footd.draining, it)
        drain_done(it->item);

    /* Those on other machines run on, there */
    tll_foreach(footd.remote_sessions, it)
        remote_session_free(it->item);
    tll_foreach(footd.remotes, it) {
        remote_free(it->item);
        tll_remove(footd.remotes, it);
    }
    network_unwatch();

    headless_fini();
    reaper_destroy(footd.reaper);
    fdm_destroy(footd.fdm);

    free(footd.layout);
    free(footd.lock_path);
    free(footd.socket_path);
    log_deinit();
    return ret;
}
