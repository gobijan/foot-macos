#pragma once

/*
 * The session protocol: between foot's windows and footd (macos/footd.c),
 * over a UNIX socket.
 *
 * A window's terminal doesn't have a PTY of its own: footd has it, and
 * the programs running in it. The window gets a socket instead, that
 * its terminal reads and writes as it would the PTY: what the programs
 * write, and the keyboard's input, as is. The socket is handed over (as
 * a file descriptor) in a message on the control connection, which also
 * carries everything else: creating, attaching to, resizing and ending
 * sessions, and the windows' layout.
 *
 * Messages are a header (type, payload size; 32 bits each, little
 * endian) and a payload of fields: integers (little endian), strings
 * (a 32-bit length, and the bytes) and lists of strings. Fields added
 * later come last, and are optional: whoever doesn't know them ignores
 * them, whoever does reads them if they're there (session_read_more()).
 *
 * Sessions on other machines: footd reaches them over SSH, where
 * `footd --bridge` passes them on to the footd there. That connection
 * is a stream (no file descriptors): what a window's socket would carry
 * goes in messages of its own instead, on a channel per socket.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tllist.h>

/* Different versions don't talk; each has a socket of its own */
#define SESSION_PROTOCOL_VERSION 1

/* Large enough for environments and layouts, but not unbounded */
#define SESSION_MAX_PAYLOAD (16 * 1024 * 1024)

enum session_msg_type {
    /* Window (foot) -> footd */
    SESSION_MSG_HELLO = 1,   /* u32 version, i32 pid */
    SESSION_MSG_CREATE,      /* u64 id, u64 token, size, str cwd, str config path,
                                str TERM, str shell, u8 login shell, u32 scrollback
                                lines, strv argv, strv envp, str host (optional:
                                where it runs, over SSH; empty: here); + the
                                window's socket */
    SESSION_MSG_ATTACH,      /* u64 id, u64 token, size; + the window's socket */
    SESSION_MSG_RESIZE,      /* u64 id, u64 token, size */
    SESSION_MSG_KILL,        /* u64 id, u64 token (0: not attached; and ID 0: all) */
    SESSION_MSG_LIST,        /* - */
    SESSION_MSG_LAYOUT,      /* blob: the windows' layout, kept for the next one */
    SESSION_MSG_UNINSTALL,   /* str host: footd is removed there (its sessions end);
                                UNINSTALLED answers */

    /* footd -> window */
    SESSION_MSG_WELCOME = 100,  /* u32 version, i32 pid, u32 features (optional) */
    SESSION_MSG_STARTED,        /* u64 id, u64 token, i32 shell PID (0: on another
                                   machine), str host (optional: that one) */
    SESSION_MSG_FAILED,         /* u64 id, u64 token, str reason */
    SESSION_MSG_DETACHED,       /* u64 id, u64 token: another window attached */
    SESSION_MSG_SESSIONS,       /* u32 count, {u64 id, u8 attached, i32 shell PID,
                                   str title, str cwd} * count, blob layout */
    SESSION_MSG_UNINSTALLED,    /* str host, str error (empty: removed) */

    /*
     * Over a stream (footd --bridge), both ways: the windows' sockets, as
     * channels. A CREATE or ATTACH has the channel announced before it,
     * instead of a socket.
     */
    SESSION_MSG_CHANNEL = 200,  /* u32 channel: the next CREATE or ATTACH's socket */
    SESSION_MSG_DATA,           /* u32 channel, blob: written to the socket */
    SESSION_MSG_EOF,            /* u32 channel: the socket is closed */
    SESSION_MSG_FLOW,           /* u32 channel, u8 paused: (not) to be read for now */
};

/* WELCOME's features */
#define SESSION_FEATURE_REMOTE (1u << 0)     /* CREATE's host */
#define SESSION_FEATURE_UNINSTALL (1u << 1)  /* UNINSTALL */

/* A terminal's size: what the PTY gets (TIOCSWINSZ) */
struct session_size {
    uint16_t cols;
    uint16_t rows;
    uint16_t cell_width;    /* Pixels */
    uint16_t cell_height;
};

/*
 * Building a message
 */

struct session_msg {
    uint8_t *data;
    size_t len;
    size_t size;
};

void session_msg_begin(struct session_msg *msg, enum session_msg_type type);
void session_msg_u8(struct session_msg *msg, uint8_t value);
void session_msg_u32(struct session_msg *msg, uint32_t value);
void session_msg_i32(struct session_msg *msg, int32_t value);
void session_msg_u64(struct session_msg *msg, uint64_t value);
void session_msg_size(struct session_msg *msg, const struct session_size *size);

/* NULL is sent as an empty string */
void session_msg_str(struct session_msg *msg, const char *s);

/* A NULL terminated list (NULL: an empty one) */
void session_msg_strv(struct session_msg *msg, const char *const *strv);
void session_msg_blob(struct session_msg *msg, const void *data, size_t len);

/* Bytes as they are: another message's payload, passed on */
void session_msg_raw(struct session_msg *msg, const void *data, size_t len);
void session_msg_free(struct session_msg *msg);

/*
 * Reading a message's payload. Reading past its end, or malformed
 * fields, make the reader fail: 'ok' is false then (and the values
 * read are zeroes, empty).
 */

struct session_reader {
    const uint8_t *p;
    const uint8_t *end;
    bool ok;
};

uint8_t session_read_u8(struct session_reader *r);
uint32_t session_read_u32(struct session_reader *r);
int32_t session_read_i32(struct session_reader *r);
uint64_t session_read_u64(struct session_reader *r);
struct session_size session_read_size(struct session_reader *r);

/* malloc:ed (and NUL terminated); NULL if it failed */
char *session_read_str(struct session_reader *r);

/* malloc:ed, NULL terminated (free with session_strv_free()) */
char **session_read_strv(struct session_reader *r);
void session_strv_free(char **strv);

/* Points into the message (not copied) */
const void *session_read_blob(struct session_reader *r, size_t *len);

/* Whether there's more: optional fields, added later (see above) */
bool session_read_more(const struct session_reader *r);

/*
 * A connection: buffered, non-blocking, passing file descriptors
 */

struct session_out_fd {
    size_t offset;  /* With the byte at this offset (of the output) */
    int fd;
};

struct session_conn {
    int fd;
    int out_fd;   /* The same, unless it's a stream */
    bool stream;  /* Pipes (e.g. SSH's): no file descriptors */

    struct {
        uint8_t *data;
        size_t len;
        size_t size;
        size_t consumed;  /* The last message returned, until the next call */
        tll(int) fds;     /* Received, in order */
    } in;

    struct {
        uint8_t *data;
        size_t len;
        size_t size;
        tll(struct session_out_fd) fds;
    } out;
};

void session_conn_init(struct session_conn *conn, int fd);

/* Over pipes, or any stream: read from 'in_fd', written to 'out_fd' */
void session_conn_init_stream(struct session_conn *conn, int in_fd, int out_fd);

/* Closes the connection, and any file descriptors not passed on */
void session_conn_close(struct session_conn *conn);

/*
 * Queues a message (with 'fd', unless -1; not on a stream), and sends
 * what can be sent right away. Returns false on errors.
 *
 * 'fd' remains the caller's: it must stay open until the other end has
 * answered. macOS' UNIX socket garbage collection may flush a socket in
 * flight (that's then at its end at once) if nobody else has it open.
 */
bool session_conn_send(struct session_conn *conn, struct session_msg *msg, int fd);

/* Sends what's queued. Returns false on errors */
bool session_conn_flush(struct session_conn *conn);
bool session_conn_has_output(const struct session_conn *conn);

/*
 * Reads what's there. Returns false on errors, and at the end ('*eof'
 * is set then).
 */
bool session_conn_read(struct session_conn *conn, bool *eof);

/*
 * The next complete message, if any: its type, and payload (valid
 * until the next call). Returns false if there's none (yet); '*error'
 * is set if the input is malformed.
 */
bool session_conn_next(struct session_conn *conn, uint32_t *type,
                       struct session_reader *payload, bool *error);

/* A file descriptor that came with a message (in order), or -1 */
int session_conn_take_fd(struct session_conn *conn);

/*
 * Where footd listens: $FOOT_SESSIONS_SOCKET, or else in the user's
 * Application Support folder. malloc:ed.
 */
char *session_socket_path(void);

/* The lock taken while connecting to footd, or starting it (and while it exits) */
char *session_lock_path(const char *socket_path);

/*
 * Connects to the footd listening on 'path' (creating its folder). With
 * 'spawn', starting one if there's none: 'spawn' gets a socket that
 * listens on 'path', for footd's --listen-fd, and returns whether it's
 * started. Under the lock above. Returns the connection (blocking), or
 * -1 (errno set).
 */
int session_footd_connect(const char *path,
                          bool (*spawn)(int listen_fd, const char *path, void *data),
                          void *data);

/*
 * The lock footd holds while it runs: a socket that refuses connections
 * is left behind by a footd that's gone, unless that's locked (its
 * backlog is full then)
 */
char *session_running_path(const char *socket_path);
