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
 * (a 32-bit length, and the bytes) and lists of strings.
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
                                lines, strv argv, strv envp; + the window's socket */
    SESSION_MSG_ATTACH,      /* u64 id, u64 token, size; + the window's socket */
    SESSION_MSG_RESIZE,      /* u64 id, u64 token, size */
    SESSION_MSG_KILL,        /* u64 id, u64 token (0: not attached; and ID 0: all) */
    SESSION_MSG_LIST,        /* - */
    SESSION_MSG_LAYOUT,      /* blob: the windows' layout, kept for the next one */

    /* footd -> window */
    SESSION_MSG_WELCOME = 100,  /* u32 version, i32 pid */
    SESSION_MSG_STARTED,        /* u64 id, u64 token, i32 shell PID */
    SESSION_MSG_FAILED,         /* u64 id, u64 token, str reason */
    SESSION_MSG_DETACHED,       /* u64 id, u64 token: another window attached */
    SESSION_MSG_SESSIONS,       /* u32 count, {u64 id, u8 attached, i32 shell PID,
                                   str title, str cwd} * count, blob layout */
};

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

/*
 * A connection: buffered, non-blocking, passing file descriptors
 */

struct session_out_fd {
    size_t offset;  /* With the byte at this offset (of the output) */
    int fd;
};

struct session_conn {
    int fd;

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

/* Closes the connection, and any file descriptors not passed on */
void session_conn_close(struct session_conn *conn);

/*
 * Queues a message (with 'fd', unless -1), and sends what can be sent
 * right away. Returns false on errors.
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
 * The lock footd holds while it runs: a socket that refuses connections
 * is left behind by a footd that's gone, unless that's locked (its
 * backlog is full then)
 */
char *session_running_path(const char *socket_path);
