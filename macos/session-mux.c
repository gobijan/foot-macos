/*
 * Sockets over a stream; see session-mux.h.
 */

#include "session-mux.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>

#include <tllist.h>

#define LOG_MODULE "session-mux"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

/* Read from a socket at once (a DATA message) */
#define CHUNK (64 * 1024)

/* A channel's socket that isn't read beyond this pauses the other side's */
#define CHANNEL_HIGH_WATER (1024 * 1024)
#define CHANNEL_LOW_WATER (256 * 1024)

/* The stream: no socket is read beyond this, until it's drained below the other */
#define STREAM_HIGH_WATER (4 * 1024 * 1024)
#define STREAM_LOW_WATER (1024 * 1024)

/*
 * A TCP connection's socket isn't read beyond this already: what the
 * windows' sockets carry (e.g. a key, and its echo) isn't to wait for
 * more of e.g. a download than that
 */
#define TCP_HIGH_WATER (256 * 1024)
#define TCP_LOW_WATER (64 * 1024)

/*
 * And not beyond what the other side has received (ACK) by more than
 * a window: the stream is one connection (ssh's), and what's sent is
 * behind all that's on its way, in ssh's buffers and the network's.
 * How long that takes is measured, from sending some of it to its ACK.
 * The window grows while that's (nearly) as short as it was with
 * nothing on the way, and shrinks once it's longer by TCP_DELAY: a
 * download fills the line, but not its queues, and a key's echo isn't
 * later than that. (As LEDBAT, RFC 6817.)
 */
#define TCP_CHUNK (16 * 1024)
#define TCP_WINDOW (64 * 1024)
#define TCP_WINDOW_MIN TCP_CHUNK
#define TCP_WINDOW_MAX (16 * 1024 * 1024)
#define TCP_DELAY (60 * 1000 * 1000)            /* ns */
#define TCP_ADJUST_EVERY (20 * 1000 * 1000)     /* ns, at least (else: the round trip's time) */
#define TCP_MARKS 32

struct channel {
    struct session_mux *mux;
    uint32_t id;
    int fd;

    /* Not yet written to the socket */
    uint8_t *out;
    size_t out_len;
    size_t out_size;

    bool paused;        /* The other side isn't reading ours: not read */
    bool pausing;       /* The other side was told to stop */
    bool ended;         /* EOF from the other side: closed once written */
    bool reading;       /* EPOLLIN */

    /* A TCP connection (session_mux_add_tcp()): each direction ends on its own */
    bool tcp;
    bool shut_in;       /* The socket's end was read: the other side was told (SHUT) */
    bool shut_out;      /* SHUT from the other side: no more is written, once written */
    bool shut_done;     /* shutdown(SHUT_WR) */
};

struct session_mux {
    struct fdm *fdm;
    struct session_conn conn;
    const struct session_mux_callbacks *cb;
    void *data;

    tll(struct channel *) channels;
    bool full;          /* The stream: STREAM_HIGH_WATER */
    bool busy;          /* TCP_HIGH_WATER */

    /* TCP connections' data, on all channels (TCP_WINDOW) */
    uint64_t tcp_sent;
    uint64_t tcp_acked;         /* By the other side */
    uint64_t tcp_received;
    uint64_t tcp_ack_sent;      /* Its ACK is sent */
    size_t tcp_window;
    bool tcp_waiting;           /* For ACKs: the window is used up */
    bool tcp_limited;           /* It was, since the window was adjusted */
    int64_t tcp_adjusted;       /* When (ns) */
    int64_t tcp_fastest;        /* The shortest time from sending to its ACK (ns); 0: unknown */

    /* When what was sent: its ACK tells how long it took */
    struct {
        uint64_t sent;
        int64_t at;
    } tcp_marks[TCP_MARKS];
    unsigned tcp_mark_first;
    unsigned tcp_mark_count;
    bool writing;       /* EPOLLOUT on the stream */
    bool dead;          /* Failed: the callback is told (once) */
    bool reported;
};

static void channel_free(struct channel *ch, bool close_fd);
static bool fdm_channel(struct fdm *fdm, int fd, int events, void *data);

static struct channel *
channel_find(struct session_mux *mux, uint32_t id)
{
    tll_foreach(mux->channels, it) {
        if (it->item->id == id)
            return it->item;
    }
    return NULL;
}

/* Reads a channel's socket only if neither side is full */
static void
channel_update_reading(struct channel *ch)
{
    const bool full = ch->tcp ? ch->mux->busy || ch->mux->tcp_waiting : ch->mux->full;
    const bool read = !ch->paused && !full && !ch->ended && !ch->shut_in;
    if (read == ch->reading)
        return;

    ch->reading = read;
    if (read)
        fdm_event_add(ch->mux->fdm, ch->fd, EPOLLIN);
    else
        fdm_event_del(ch->mux->fdm, ch->fd, EPOLLIN);
}

static void
stream_update(struct session_mux *mux)
{
    const size_t len = mux->conn.out.len;

    const bool writing = len > 0;
    if (writing != mux->writing) {
        mux->writing = writing;
        if (writing)
            fdm_event_add(mux->fdm, mux->conn.out_fd, EPOLLOUT);
        else
            fdm_event_del(mux->fdm, mux->conn.out_fd, EPOLLOUT);
    }

    const bool full = mux->full ? len > STREAM_LOW_WATER : len > STREAM_HIGH_WATER;
    const bool busy = mux->busy ? len > TCP_LOW_WATER : len > TCP_HIGH_WATER;
    if (full != mux->full || busy != mux->busy) {
        mux->full = full;
        mux->busy = busy;
        tll_foreach(mux->channels, it)
            channel_update_reading(it->item);
    }
}

bool
session_mux_send(struct session_mux *mux, struct session_msg *msg)
{
    if (mux->dead)
        return false;
    if (!session_conn_send(&mux->conn, msg, -1)) {
        mux->dead = true;
        return false;
    }
    stream_update(mux);
    return true;
}

static void
send_channel_msg(struct session_mux *mux, enum session_msg_type type, uint32_t id)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, type);
    session_msg_u32(&msg, id);
    session_mux_send(mux, &msg);
    session_msg_free(&msg);
}

static void
send_flow(struct channel *ch, bool paused)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_FLOW);
    session_msg_u32(&msg, ch->id);
    session_msg_u8(&msg, paused);
    session_mux_send(ch->mux, &msg);
    session_msg_free(&msg);
    ch->pausing = paused;
}

/*
 * Writes what's there for the socket; the other side may go on, once
 * it's below the mark. False: the socket can't be written to any more.
 */
static bool
channel_flush(struct channel *ch)
{
    bool gone = false;
    size_t done = 0;
    while (done < ch->out_len) {
        ssize_t n = write(ch->fd, &ch->out[done], ch->out_len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN) {
                /* It's gone: its HUP closes the channel (a TCP connection's: the caller) */
                done = ch->out_len;
                gone = true;
            }
            break;
        }
        done += n;
    }

    memmove(ch->out, &ch->out[done], ch->out_len - done);
    ch->out_len -= done;

    if (ch->out_len > 0)
        fdm_event_add(ch->mux->fdm, ch->fd, EPOLLOUT);
    else
        fdm_event_del(ch->mux->fdm, ch->fd, EPOLLOUT);

    if (ch->pausing && ch->out_len < CHANNEL_LOW_WATER)
        send_flow(ch, false);
    else if (!ch->pausing && ch->out_len > CHANNEL_HIGH_WATER)
        send_flow(ch, true);
    return !gone;
}

static void
channel_append(struct channel *ch, const void *data, size_t len)
{
    if (len == 0)
        return;

    if (ch->out_len + len > ch->out_size) {
        size_t size = ch->out_size > 0 ? ch->out_size : CHUNK;
        while (size < ch->out_len + len)
            size *= 2;
        ch->out = xrealloc(ch->out, size);
        ch->out_size = size;
    }
    memcpy(&ch->out[ch->out_len], data, len);
    ch->out_len += len;
}

/* The channel is done with; the callback is told */
static void
channel_close(struct channel *ch)
{
    struct session_mux *mux = ch->mux;
    const uint32_t id = ch->id;
    const bool theirs = ch->ended;
    channel_free(ch, true);
    if (mux->cb->channel_closed != NULL)
        mux->cb->channel_closed(mux, id, theirs, mux->data);
}

/* The same, and the other side is told, unless it was the one to end it */
static void
channel_end(struct channel *ch)
{
    if (!ch->ended)
        send_channel_msg(ch->mux, SESSION_MSG_EOF, ch->id);
    channel_close(ch);
}

/*
 * After channel_flush() ('ok': what it returned): a channel that only
 * had that left to do is done with (true: it's closed), and a TCP
 * connection that gets no more is told so
 */
static bool
channel_written(struct channel *ch, bool ok)
{
    if (ch->tcp && !ok) {
        channel_end(ch);
        return true;
    }
    if (ch->out_len > 0)
        return false;

    if (ch->ended) {
        channel_close(ch);
        return true;
    }

    if (ch->shut_out && !ch->shut_done) {
        shutdown(ch->fd, SHUT_WR);
        ch->shut_done = true;
    }
    if (ch->shut_in && ch->shut_done) {
        channel_end(ch);
        return true;
    }
    return false;
}

static void
channel_free(struct channel *ch, bool close_fd)
{
    struct session_mux *mux = ch->mux;
    tll_foreach(mux->channels, it) {
        if (it->item == ch) {
            tll_remove(mux->channels, it);
            break;
        }
    }

    if (close_fd)
        fdm_del(mux->fdm, ch->fd);
    else
        fdm_del_no_close(mux->fdm, ch->fd);

    free(ch->out);
    free(ch);
}

static void
send_data(struct channel *ch, const uint8_t *buf, size_t len)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, ch->tcp ? SESSION_MSG_TCP : SESSION_MSG_DATA);
    session_msg_u32(&msg, ch->id);
    session_msg_blob(&msg, buf, len);
    session_mux_send(ch->mux, &msg);
    session_msg_free(&msg);
}

static int64_t
now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/* The TCP connections' sockets are read while the window isn't used up */
static void
tcp_update(struct session_mux *mux)
{
    const bool waiting = mux->tcp_sent - mux->tcp_acked >= mux->tcp_window;
    if (waiting)
        mux->tcp_limited = true;
    if (waiting == mux->tcp_waiting)
        return;

    mux->tcp_waiting = waiting;
    tll_foreach(mux->channels, it) {
        if (it->item->tcp)
            channel_update_reading(it->item);
    }
}

/* A TCP connection's data is sent */
static void
tcp_send(struct channel *ch, const uint8_t *buf, size_t len)
{
    struct session_mux *mux = ch->mux;
    send_data(ch, buf, len);
    mux->tcp_sent += len;

    if (mux->tcp_mark_count < TCP_MARKS) {
        const unsigned i = (mux->tcp_mark_first + mux->tcp_mark_count++) % TCP_MARKS;
        mux->tcp_marks[i].sent = mux->tcp_sent;
        mux->tcp_marks[i].at = now_ns();
    }
    tcp_update(mux);
}

/* ACK: the other side has received that much; false: malformed */
static bool
tcp_acked(struct session_mux *mux, struct session_reader *r)
{
    const uint64_t total = session_read_u64(r);
    if (!r->ok || total > mux->tcp_sent || total < mux->tcp_acked)
        return false;
    mux->tcp_acked = total;

    /* How long the last of it took */
    const int64_t now = now_ns();
    int64_t took = -1;
    while (mux->tcp_mark_count > 0 && mux->tcp_marks[mux->tcp_mark_first].sent <= total) {
        took = now - mux->tcp_marks[mux->tcp_mark_first].at;
        mux->tcp_mark_first = (mux->tcp_mark_first + 1) % TCP_MARKS;
        mux->tcp_mark_count--;
    }

    if (took >= 0) {
        if (mux->tcp_fastest == 0 || took < mux->tcp_fastest)
            mux->tcp_fastest = took > 0 ? took : 1;

        /* Longer than that: it waited, behind what was sent before it */
        const int64_t delay = took - mux->tcp_fastest;
        const int64_t every =
            mux->tcp_fastest > TCP_ADJUST_EVERY ? mux->tcp_fastest : TCP_ADJUST_EVERY;

        if (now - mux->tcp_adjusted >= every) {
            if (delay > TCP_DELAY) {
                mux->tcp_window -= mux->tcp_window / 4;
                if (mux->tcp_window < TCP_WINDOW_MIN)
                    mux->tcp_window = TCP_WINDOW_MIN;
            } else if (delay < TCP_DELAY / 2 && mux->tcp_limited) {
                mux->tcp_window += mux->tcp_window / 4;
                if (mux->tcp_window > TCP_WINDOW_MAX)
                    mux->tcp_window = TCP_WINDOW_MAX;
            }
            mux->tcp_limited = false;
            mux->tcp_adjusted = now;
        }
    }

    tcp_update(mux);
    return true;
}

/*
 * A TCP connection's socket. At its end, only that direction is done
 * with (SHUT): the other side's answer may still be on its way. It's
 * closed once both are, or it's broken.
 */
static void
tcp_channel_read(struct channel *ch, int events)
{
    struct session_mux *mux = ch->mux;
    const bool was_shut = ch->shut_in;

    /* Its end (kqueue: HUP), or all of it is: what's left goes all the same (not read meanwhile) */
    const bool all = events & (EPOLLHUP | EPOLLERR);

    while (!ch->shut_in) {
        uint8_t buf[TCP_CHUNK];
        const ssize_t n = read(ch->fd, buf, sizeof(buf));

        if (n > 0) {
            tcp_send(ch, buf, n);
            if (mux->cb->input != NULL)
                mux->cb->input(mux, ch->id, mux->data);
            if (!all)
                return;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            break;
        if (n < 0) {
            channel_end(ch);
            return;
        }

        ch->shut_in = true;
        send_channel_msg(mux, SESSION_MSG_SHUT, ch->id);
        channel_update_reading(ch);
    }

    if (ch->shut_in && ch->shut_done)
        channel_end(ch);
    else if (all && ((events & EPOLLERR) || was_shut || !ch->shut_in))
        channel_end(ch);
}

static bool
fdm_channel(struct fdm *fdm, int fd, int events, void *data)
{
    struct channel *ch = data;
    struct session_mux *mux = ch->mux;

    if (events & EPOLLOUT) {
        if (channel_written(ch, channel_flush(ch)))
            return true;
    }

    if (ch->tcp) {
        if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            tcp_channel_read(ch, events);
        return true;
    }

    if ((events & EPOLLIN) && !(events & (EPOLLHUP | EPOLLERR))) {
        uint8_t buf[CHUNK];
        ssize_t n;
        do {
            n = read(fd, buf, sizeof(buf));
        } while (n < 0 && errno == EINTR);

        if (n > 0) {
            send_data(ch, buf, n);
            if (mux->cb->input != NULL)
                mux->cb->input(mux, ch->id, mux->data);
            return true;
        }

        if (n == 0 || errno != EAGAIN) {
            send_channel_msg(mux, SESSION_MSG_EOF, ch->id);
            channel_close(ch);
            return true;
        }
    }

    if (events & (EPOLLHUP | EPOLLERR)) {
        /* Not read meanwhile (paused): what's left goes all the same, then its end */
        uint8_t buf[CHUNK];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR)) {
            if (n > 0)
                send_data(ch, buf, n);
        }
        send_channel_msg(mux, SESSION_MSG_EOF, ch->id);
        channel_close(ch);
    }

    return true;
}

static bool
channel_add(struct session_mux *mux, uint32_t channel, int fd, bool tcp)
{
    xassert(channel_find(mux, channel) == NULL);

    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERRNO("channel %u: failed to configure its socket", channel);
        close(fd);
        return false;
    }

    struct channel *ch = xmalloc(sizeof(*ch));
    *ch = (struct channel){.mux = mux, .id = channel, .fd = fd, .tcp = tcp};

    if (!fdm_add(mux->fdm, fd, 0, &fdm_channel, ch)) {
        close(fd);
        free(ch);
        return false;
    }

    tll_push_back(mux->channels, ch);
    channel_update_reading(ch);
    return true;
}

bool
session_mux_add(struct session_mux *mux, uint32_t channel, int fd)
{
    return channel_add(mux, channel, fd, false);
}

bool
session_mux_add_tcp(struct session_mux *mux, uint32_t channel, int fd)
{
    return channel_add(mux, channel, fd, true);
}

bool
session_mux_announce(struct session_mux *mux, uint32_t channel)
{
    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_CHANNEL);
    session_msg_u32(&msg, channel);
    const bool ok = session_mux_send(mux, &msg);
    session_msg_free(&msg);
    return ok;
}

int
session_mux_take(struct session_mux *mux, uint32_t channel)
{
    struct channel *ch = channel_find(mux, channel);
    if (ch == NULL)
        return -1;

    const int fd = ch->fd;
    send_channel_msg(mux, SESSION_MSG_EOF, channel);
    channel_free(ch, false);
    return fd;
}

/* The channels' own messages; false: malformed */
static bool
channel_message(struct session_mux *mux, uint32_t type, struct session_reader *r)
{
    const uint32_t id = session_read_u32(r);

    switch (type) {
    case SESSION_MSG_CHANNEL:
        if (!r->ok)
            return false;
        if (mux->cb->announced != NULL)
            mux->cb->announced(mux, id, mux->data);
        return true;

    case SESSION_MSG_TCP:
    case SESSION_MSG_DATA: {
        size_t len;
        const void *data = session_read_blob(r, &len);
        if (!r->ok)
            return false;

        /* (Whatever becomes of it: the other side counts it too) */
        if (type == SESSION_MSG_TCP)
            mux->tcp_received += len;

        /* Already closed here: the other side will know (our EOF) */
        struct channel *ch = channel_find(mux, id);
        if (ch == NULL || ch->ended)
            return true;

        channel_append(ch, data, len);
        channel_written(ch, channel_flush(ch));
        return true;
    }

    case SESSION_MSG_EOF: {
        if (!r->ok)
            return false;
        struct channel *ch = channel_find(mux, id);
        if (ch == NULL)
            return true;

        /* What's left is written first */
        ch->ended = true;
        channel_update_reading(ch);
        if (ch->out_len == 0)
            channel_close(ch);
        return true;
    }

    case SESSION_MSG_SHUT: {
        if (!r->ok)
            return false;
        struct channel *ch = channel_find(mux, id);
        if (ch == NULL || ch->ended)
            return true;

        /* (Not a TCP connection: there are no halves) */
        if (!ch->tcp) {
            ch->ended = true;
            channel_update_reading(ch);
        } else
            ch->shut_out = true;
        channel_written(ch, true);
        return true;
    }

    case SESSION_MSG_FLOW: {
        const bool paused = session_read_u8(r);
        if (!r->ok)
            return false;
        struct channel *ch = channel_find(mux, id);
        if (ch != NULL) {
            ch->paused = paused;
            channel_update_reading(ch);
        }
        return true;
    }
    }

    return false;
}

/* The callback destroys it: nothing may touch it after this */
static void
stream_closed(struct session_mux *mux)
{
    mux->dead = true;
    if (!mux->reported) {
        mux->reported = true;
        mux->cb->closed(mux, mux->data);
    }
}

static bool
fdm_stream(struct fdm *fdm, int fd, int events, void *data)
{
    struct session_mux *mux = data;

    /* A send failed meanwhile */
    if (mux->dead) {
        stream_closed(mux);
        return true;
    }

    if ((events & EPOLLOUT) && fd == mux->conn.out_fd) {
        if (!session_conn_flush(&mux->conn)) {
            stream_closed(mux);
            return true;
        }
        stream_update(mux);
    }

    if ((events & (EPOLLIN | EPOLLHUP)) && fd == mux->conn.fd) {
        const size_t before = mux->conn.in.len - mux->conn.in.consumed;
        bool eof;
        bool ok = session_conn_read(&mux->conn, &eof);
        if (mux->conn.in.len > before && mux->cb->received != NULL)
            mux->cb->received(mux, mux->data);

        uint32_t type;
        struct session_reader r;
        bool error;
        while (!mux->dead && session_conn_next(&mux->conn, &type, &r, &error)) {
            bool handled =
                type == SESSION_MSG_ACK ? tcp_acked(mux, &r) :
                (type >= SESSION_MSG_CHANNEL && type <= SESSION_MSG_SHUT) || type == SESSION_MSG_TCP
                    ? channel_message(mux, type, &r)
                    : mux->cb->message(mux, type, &r, mux->data);
            if (!handled) {
                LOG_ERR("malformed message (type %u)", type);
                stream_closed(mux);
                return true;
            }
        }

        /* What was received of TCP connections, with this: the other side may send as much more */
        if (!mux->dead && mux->tcp_received != mux->tcp_ack_sent) {
            struct session_msg msg = {0};
            session_msg_begin(&msg, SESSION_MSG_ACK);
            session_msg_u64(&msg, mux->tcp_received);
            session_mux_send(mux, &msg);
            session_msg_free(&msg);
            mux->tcp_ack_sent = mux->tcp_received;
        }

        if (mux->dead || !ok || error) {
            stream_closed(mux);
            return true;
        }
    } else if ((events & (EPOLLHUP | EPOLLERR)) && fd == mux->conn.out_fd) {
        /* Nobody reads what we write */
        stream_closed(mux);
    }

    return true;
}

struct session_mux *
session_mux_new(struct fdm *fdm, int in_fd, int out_fd,
                const struct session_mux_callbacks *callbacks, void *data)
{
    struct session_mux *mux = xmalloc(sizeof(*mux));
    *mux = (struct session_mux){
        .fdm = fdm, .cb = callbacks, .data = data, .tcp_window = TCP_WINDOW,
    };
    session_conn_init_stream(&mux->conn, in_fd, out_fd);

    for (int i = 0; i < (in_fd == out_fd ? 1 : 2); i++) {
        int fd = i == 0 ? in_fd : out_fd;
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        {
            LOG_ERRNO("failed to configure the stream");
            session_conn_close(&mux->conn);
            free(mux);
            return NULL;
        }
    }

    if (!fdm_add(fdm, in_fd, EPOLLIN, &fdm_stream, mux)) {
        session_conn_close(&mux->conn);
        free(mux);
        return NULL;
    }
    if (out_fd != in_fd && !fdm_add(fdm, out_fd, 0, &fdm_stream, mux)) {
        fdm_del_no_close(fdm, in_fd);
        session_conn_close(&mux->conn);
        free(mux);
        return NULL;
    }

    return mux;
}

void
session_mux_destroy(struct session_mux *mux)
{
    if (mux == NULL)
        return;

    while (tll_length(mux->channels) > 0)
        channel_free(tll_front(mux->channels), true);

    if (mux->conn.out_fd != mux->conn.fd && mux->conn.out_fd >= 0)
        fdm_del_no_close(mux->fdm, mux->conn.out_fd);
    if (mux->conn.fd >= 0)
        fdm_del_no_close(mux->fdm, mux->conn.fd);
    session_conn_close(&mux->conn);
    free(mux);
}
