/*
 * The session protocol; see session-protocol.h.
 */

#include "session-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <limits.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#define LOG_MODULE "session-protocol"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

/* At most this many descriptors come with one recvmsg() */
#define MAX_FDS_PER_READ 16

/*
 * Building
 */

static void
put(struct session_msg *msg, const void *data, size_t len)
{
    if (msg->len + len > msg->size) {
        size_t size = msg->size > 0 ? msg->size : 256;
        while (size < msg->len + len)
            size *= 2;
        msg->data = xrealloc(msg->data, size);
        msg->size = size;
    }
    memcpy(&msg->data[msg->len], data, len);
    msg->len += len;
}

static void
put_le(struct session_msg *msg, uint64_t value, size_t bytes)
{
    uint8_t le[8];
    for (size_t i = 0; i < bytes; i++)
        le[i] = (value >> (8 * i)) & 0xff;
    put(msg, le, bytes);
}

void
session_msg_begin(struct session_msg *msg, enum session_msg_type type)
{
    msg->len = 0;
    put_le(msg, type, 4);
    put_le(msg, 0, 4);  /* The payload's size, once known */
}

void session_msg_u8(struct session_msg *msg, uint8_t value) { put_le(msg, value, 1); }
void session_msg_u32(struct session_msg *msg, uint32_t value) { put_le(msg, value, 4); }
void session_msg_i32(struct session_msg *msg, int32_t value) { put_le(msg, (uint32_t)value, 4); }
void session_msg_u64(struct session_msg *msg, uint64_t value) { put_le(msg, value, 8); }

void
session_msg_size(struct session_msg *msg, const struct session_size *size)
{
    put_le(msg, size->cols, 2);
    put_le(msg, size->rows, 2);
    put_le(msg, size->cell_width, 2);
    put_le(msg, size->cell_height, 2);
}

void
session_msg_blob(struct session_msg *msg, const void *data, size_t len)
{
    put_le(msg, len, 4);
    if (len > 0)
        put(msg, data, len);
}

void
session_msg_str(struct session_msg *msg, const char *s)
{
    session_msg_blob(msg, s, s != NULL ? strlen(s) : 0);
}

void
session_msg_strv(struct session_msg *msg, const char *const *strv)
{
    uint32_t count = 0;
    for (const char *const *s = strv; s != NULL && *s != NULL; s++)
        count++;

    session_msg_u32(msg, count);
    for (uint32_t i = 0; i < count; i++)
        session_msg_str(msg, strv[i]);
}

/* The payload's size, in the header */
static void
finish(struct session_msg *msg)
{
    xassert(msg->len >= 8);
    const uint32_t payload = msg->len - 8;
    for (size_t i = 0; i < 4; i++)
        msg->data[4 + i] = (payload >> (8 * i)) & 0xff;
}

void
session_msg_free(struct session_msg *msg)
{
    free(msg->data);
    *msg = (struct session_msg){0};
}

/*
 * Reading
 */

static uint64_t
get_le(struct session_reader *r, size_t bytes)
{
    if (!r->ok || (size_t)(r->end - r->p) < bytes) {
        r->ok = false;
        return 0;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < bytes; i++)
        value |= (uint64_t)r->p[i] << (8 * i);
    r->p += bytes;
    return value;
}

uint8_t session_read_u8(struct session_reader *r) { return get_le(r, 1); }
uint32_t session_read_u32(struct session_reader *r) { return get_le(r, 4); }
int32_t session_read_i32(struct session_reader *r) { return (int32_t)(uint32_t)get_le(r, 4); }
uint64_t session_read_u64(struct session_reader *r) { return get_le(r, 8); }

struct session_size
session_read_size(struct session_reader *r)
{
    struct session_size size = {
        .cols = get_le(r, 2),
        .rows = get_le(r, 2),
        .cell_width = get_le(r, 2),
        .cell_height = get_le(r, 2),
    };
    return size;
}

const void *
session_read_blob(struct session_reader *r, size_t *len)
{
    const uint32_t n = get_le(r, 4);
    if (!r->ok || (size_t)(r->end - r->p) < n) {
        r->ok = false;
        *len = 0;
        return NULL;
    }

    const void *data = r->p;
    r->p += n;
    *len = n;
    return data;
}

char *
session_read_str(struct session_reader *r)
{
    size_t len;
    const char *data = session_read_blob(r, &len);
    if (!r->ok)
        return NULL;

    /* Strings are for C: no NULs in them */
    if (len > 0 && memchr(data, '\0', len) != NULL) {
        r->ok = false;
        return NULL;
    }

    char *s = xmalloc(len + 1);
    if (len > 0)
        memcpy(s, data, len);
    s[len] = '\0';
    return s;
}

char **
session_read_strv(struct session_reader *r)
{
    const uint32_t count = get_le(r, 4);

    /* Each takes at least its length */
    if (!r->ok || count > (size_t)(r->end - r->p) / 4) {
        r->ok = false;
        return NULL;
    }

    char **strv = xcalloc(count + 1, sizeof(strv[0]));
    for (uint32_t i = 0; i < count; i++) {
        strv[i] = session_read_str(r);
        if (strv[i] == NULL) {
            session_strv_free(strv);
            return NULL;
        }
    }
    return strv;
}

void
session_strv_free(char **strv)
{
    if (strv == NULL)
        return;
    for (char **s = strv; *s != NULL; s++)
        free(*s);
    free(strv);
}

/*
 * Connections
 */

void
session_conn_init(struct session_conn *conn, int fd)
{
    *conn = (struct session_conn){
        .fd = fd,
        .in = {.fds = tll_init()},
        .out = {.fds = tll_init()},
    };
}

void
session_conn_close(struct session_conn *conn)
{
    if (conn->fd >= 0)
        close(conn->fd);
    conn->fd = -1;

    tll_foreach(conn->in.fds, it) {
        close(it->item);
        tll_remove(conn->in.fds, it);
    }
    tll_foreach(conn->out.fds, it) {
        close(it->item.fd);
        tll_remove(conn->out.fds, it);
    }

    free(conn->in.data);
    free(conn->out.data);
    conn->in.data = conn->out.data = NULL;
    conn->in.len = conn->in.size = conn->in.consumed = 0;
    conn->out.len = conn->out.size = 0;
}

bool
session_conn_has_output(const struct session_conn *conn)
{
    return conn->out.len > 0;
}

/* Drops 'count' sent bytes from the output; the descriptors' offsets follow */
static void
out_consume(struct session_conn *conn, size_t count)
{
    memmove(conn->out.data, &conn->out.data[count], conn->out.len - count);
    conn->out.len -= count;

    tll_foreach(conn->out.fds, it) {
        xassert(it->item.offset >= count);
        it->item.offset -= count;
    }
}

bool
session_conn_flush(struct session_conn *conn)
{
    while (conn->out.len > 0) {
        /*
         * A descriptor goes with the first byte of its message; up to
         * the next one's message, the bytes are sent with it, or on
         * their own (the descriptors are in the order of their offsets).
         */
        size_t len = conn->out.len;
        int fd = -1;

        tll_foreach(conn->out.fds, it) {
            if (it->item.offset == 0)
                fd = it->item.fd;
            else {
                len = it->item.offset;
                break;
            }
        }

        struct iovec iov = {.iov_base = conn->out.data, .iov_len = len};
        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } control;
        struct msghdr hdr = {.msg_iov = &iov, .msg_iovlen = 1};

        if (fd >= 0) {
            memset(&control, 0, sizeof(control));
            hdr.msg_control = control.buf;
            hdr.msg_controllen = sizeof(control.buf);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
        }

        ssize_t sent = sendmsg(conn->fd, &hdr, 0);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return true;
            LOG_ERRNO("failed to send");
            return false;
        }

        if (fd >= 0 && sent > 0) {
            /* It went with the first byte; ours is no longer needed */
            close(fd);
            tll_pop_front(conn->out.fds);
        }

        out_consume(conn, sent);
    }

    return true;
}

bool
session_conn_send(struct session_conn *conn, struct session_msg *msg, int fd)
{
    finish(msg);

    if (fd >= 0) {
        /* Ours is closed once it's sent; the caller's stays (see the header) */
        int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (copy < 0) {
            LOG_ERRNO("failed to duplicate the file descriptor to send");
            return false;
        }
        tll_push_back(conn->out.fds, ((struct session_out_fd){
            .offset = conn->out.len, .fd = copy}));
    }

    if (conn->out.len + msg->len > conn->out.size) {
        size_t size = conn->out.size > 0 ? conn->out.size : 4096;
        while (size < conn->out.len + msg->len)
            size *= 2;
        conn->out.data = xrealloc(conn->out.data, size);
        conn->out.size = size;
    }
    memcpy(&conn->out.data[conn->out.len], msg->data, msg->len);
    conn->out.len += msg->len;

    return session_conn_flush(conn);
}

bool
session_conn_read(struct session_conn *conn, bool *eof)
{
    *eof = false;

    /* The message returned last time is done with */
    if (conn->in.consumed > 0) {
        memmove(conn->in.data, &conn->in.data[conn->in.consumed],
                conn->in.len - conn->in.consumed);
        conn->in.len -= conn->in.consumed;
        conn->in.consumed = 0;
    }

    for (;;) {
        if (conn->in.size - conn->in.len < 4096) {
            conn->in.size = conn->in.size > 0 ? conn->in.size * 2 : 16 * 1024;
            conn->in.data = xrealloc(conn->in.data, conn->in.size);
        }

        struct iovec iov = {
            .iov_base = &conn->in.data[conn->in.len],
            .iov_len = conn->in.size - conn->in.len,
        };
        union {
            char buf[CMSG_SPACE(sizeof(int) * MAX_FDS_PER_READ)];
            struct cmsghdr align;
        } control;
        struct msghdr hdr = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = control.buf,
            .msg_controllen = sizeof(control.buf),
        };

        ssize_t count = recvmsg(conn->fd, &hdr, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return true;
            LOG_ERRNO("failed to receive");
            return false;
        }

        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr); cmsg != NULL;
             cmsg = CMSG_NXTHDR(&hdr, cmsg))
        {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                continue;

            const size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < n; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
                    LOG_ERRNO("failed to set FD_CLOEXEC on a received FD");
                tll_push_back(conn->in.fds, fd);
            }
        }

        if (hdr.msg_flags & MSG_CTRUNC) {
            LOG_ERR("received too many file descriptors at once");
            return false;
        }

        if (count == 0) {
            *eof = true;
            return false;
        }

        conn->in.len += count;

        /* Not too much at once (but more than one message, if they're small) */
        if (conn->in.len > 1024 * 1024)
            return true;
    }
}

bool
session_conn_next(struct session_conn *conn, uint32_t *type,
                  struct session_reader *payload, bool *error)
{
    *error = false;

    const uint8_t *data = &conn->in.data[conn->in.consumed];
    const size_t available = conn->in.len - conn->in.consumed;

    if (available < 8)
        return false;

    uint32_t size = 0;
    *type = 0;
    for (size_t i = 0; i < 4; i++) {
        *type |= (uint32_t)data[i] << (8 * i);
        size |= (uint32_t)data[4 + i] << (8 * i);
    }

    if (size > SESSION_MAX_PAYLOAD) {
        LOG_ERR("message too large: %u bytes", size);
        *error = true;
        return false;
    }

    if (available < 8 + (size_t)size)
        return false;

    *payload = (struct session_reader){
        .p = data + 8,
        .end = data + 8 + size,
        .ok = true,
    };
    conn->in.consumed += 8 + size;
    return true;
}

int
session_conn_take_fd(struct session_conn *conn)
{
    if (tll_length(conn->in.fds) == 0)
        return -1;
    return tll_pop_front(conn->in.fds);
}

/*
 * Where
 */

char *
session_socket_path(void)
{
    const char *env = getenv("FOOT_SESSIONS_SOCKET");
    if (env != NULL && env[0] != '\0')
        return xstrdup(env);

    /* A socket's path is short (sockaddr_un): else, the temporary folder */
    const char *home = getenv("HOME");
    if (home != NULL && home[0] == '/') {
        char *path = xasprintf("%s/Library/Application Support/foot/sessions-%d.sock",
                               home, SESSION_PROTOCOL_VERSION);
        if (strlen(path) < sizeof(((struct sockaddr_un *)NULL)->sun_path))
            return path;
        free(path);
    }

    char tmp[PATH_MAX];
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof(tmp)) == 0 || tmp[0] != '/')
        return NULL;

    char *path = xasprintf("%sfoot-sessions-%d.sock", tmp, SESSION_PROTOCOL_VERSION);
    if (strlen(path) >= sizeof(((struct sockaddr_un *)NULL)->sun_path)) {
        free(path);
        return NULL;
    }
    return path;
}

char *
session_lock_path(const char *socket_path)
{
    return xasprintf("%s.lock", socket_path);
}

char *
session_running_path(const char *socket_path)
{
    return xasprintf("%s.running", socket_path);
}
