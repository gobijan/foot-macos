/*
 * footd as ssh's SSH_ASKPASS (footd.c's spawn_ssh()): when ssh needs
 * something typed (a password, a key's passphrase, a yes or no), it
 * runs this with the prompt. It asks footd (its socket: $FOOTD_ASKPASS),
 * which answers from what it was told before, or else asks foot (a
 * dialog in the machine's window), and prints the answer for ssh.
 */

#include "footd-bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "session-protocol.h"

static int
fail(const char *what)
{
    fprintf(stderr, "footd --askpass: %s: %s\n", what, strerror(errno));
    return EXIT_FAILURE;
}

int
footd_askpass(const char *socket_path, const char *token, const char *prompt)
{
    /* ssh only tells something (e.g. to touch a security key): no answer */
    const char *kind = getenv("SSH_ASKPASS_PROMPT");
    if (kind != NULL && strcmp(kind, "none") == 0)
        return EXIT_SUCCESS;
    const bool confirm = kind != NULL && strcmp(kind, "confirm") == 0;

    /* (ssh's: its connection; footd notices it's gone only once nobody has it open) */
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        if (null != STDIN_FILENO)
            close(null);
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return fail(socket_path);
    }
    strcpy(addr.sun_path, socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return fail("socket");
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
    {
        close(fd);
        return fail(socket_path);
    }

    struct session_conn conn;
    session_conn_init(&conn, fd);

    struct session_msg msg = {0};
    session_msg_begin(&msg, SESSION_MSG_HELLO);
    session_msg_u32(&msg, SESSION_PROTOCOL_VERSION);
    session_msg_i32(&msg, getpid());
    bool ok = session_conn_send(&conn, &msg, -1);

    session_msg_begin(&msg, SESSION_MSG_ASKPASS);
    session_msg_u64(&msg, token != NULL ? strtoull(token, NULL, 16) : 0);
    session_msg_str(&msg, prompt);
    session_msg_u8(&msg, confirm);
    ok = ok && session_conn_send(&conn, &msg, -1);
    session_msg_free(&msg);

    /* Until footd answers (ssh ends this if it gives up meanwhile) */
    int status = EXIT_FAILURE;
    bool answered = false;
    while (ok && !answered) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN | (session_conn_has_output(&conn) ? POLLOUT : 0),
        };
        if (poll(&pfd, 1, -1) < 0) {
            ok = errno == EINTR;
            continue;
        }

        if ((pfd.revents & POLLOUT) && !session_conn_flush(&conn))
            break;
        if (!(pfd.revents & (POLLIN | POLLHUP)))
            continue;

        bool eof;
        ok = session_conn_read(&conn, &eof);

        uint32_t type;
        struct session_reader r;
        bool error;
        while (!answered && session_conn_next(&conn, &type, &r, &error)) {
            if (type != SESSION_MSG_ANSWERED)
                continue;

            answered = true;
            const bool yes = session_read_u8(&r);
            char *answer = session_read_str(&r);
            if (r.ok && yes) {
                if (!confirm)
                    printf("%s\n", answer != NULL ? answer : "");
                status = EXIT_SUCCESS;
            }
            session_erase(answer);
        }
        ok = ok && !error;
    }

    session_conn_close(&conn);
    fflush(stdout);
    return status;
}
