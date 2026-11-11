/*
 * foot @ COMMAND: the client of foot's remote control (control.h). It
 * sends the command to the running foot, and prints what that answers.
 */

#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "session-protocol.h"
#include "xmalloc.h"

static const char usage[] =
    "Usage: foot @ COMMAND [OPTIONS...]\n"
    "\n"
    "Controls the running foot (the application).\n"
    "\n"
    "Commands:\n"
    "  ls                      the terminals, as JSON: their ID, title, directory, size\n"
    "  send-text [TEXT...]     types TEXT (\\r is Return; also \\n, \\t, \\e, \\\\, \\xHH),\n"
    "                          or with --stdin, what's read from there (as it is)\n"
    "  get-text                prints what the terminal shows; with --scrollback, all\n"
    "                          of it; with --command-output, the last command's output\n"
    "  focus                   brings the terminal to the front\n"
    "  commands                what the menus (and the command palette) have\n"
    "  run COMMAND             one of them, by its title: e.g. foot @ run New Tab\n"
    "\n"
    "Options:\n"
    "  --id=ID                 the terminal (see ls); the focused one otherwise\n";

char *
control_socket_path(void)
{
    const char *env = getenv("FOOT_CONTROL_SOCKET");
    if (env != NULL && env[0] != '\0')
        return xstrdup(env);

    const char *home = getenv("HOME");
    if (home == NULL || home[0] != '/')
        return NULL;

    char *path = xasprintf("%s/Library/Application Support/foot/control.sock", home);
    if (strlen(path) >= sizeof(((struct sockaddr_un *)NULL)->sun_path)) {
        free(path);
        return NULL;
    }
    return path;
}

static int
hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* In place; returns the length (the text may have NULs then) */
static size_t
unescape(char *text)
{
    char *out = text;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p != '\\' || p[1] == '\0') {
            *out++ = *p;
            continue;
        }

        switch (*++p) {
        case 'n': *out++ = '\n'; break;
        case 'r': *out++ = '\r'; break;
        case 't': *out++ = '\t'; break;
        case 'e': *out++ = '\033'; break;
        case '\\': *out++ = '\\'; break;

        case 'x':
            if (hex(p[1]) >= 0 && hex(p[2]) >= 0) {
                *out++ = (char)(hex(p[1]) << 4 | hex(p[2]));
                p += 2;
                break;
            }
            /* FALLTHROUGH */

        default:
            *out++ = '\\';
            *out++ = *p;
            break;
        }
    }
    return out - text;
}

static char *
read_all(int fd, size_t *len)
{
    char *data = NULL;
    size_t size = 0;
    *len = 0;

    while (true) {
        if (*len == size) {
            size = size > 0 ? size * 2 : 65536;
            data = xrealloc(data, size);
        }

        const ssize_t n = read(fd, data + *len, size - *len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return data;
        *len += n;
    }
}

int
control_client(int argc, char *const *argv)
{
    if (argc == 0 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0 ||
        strcmp(argv[0], "help") == 0)
    {
        fputs(usage, argc == 0 ? stderr : stdout);
        return argc == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    /*
     * send-text's text is the data: what's typed, as bytes. The rest
     * is the application's to make sense of
     */
    const bool send_text = strcmp(argv[0], "send-text") == 0;
    const char **args = xcalloc(argc + 1, sizeof(args[0]));
    size_t arg_count = 0;
    char *data = NULL;
    size_t data_len = 0;
    bool from_stdin = false;

    for (int i = 0; i < argc; i++) {
        const bool option = i == 0 || (strncmp(argv[i], "--", 2) == 0 && argv[i][2] != '\0');
        if (send_text && strcmp(argv[i], "--stdin") == 0)
            from_stdin = true;
        else if (!send_text || option) {
            args[arg_count++] = argv[i];

            /* --id ID */
            if (strcmp(argv[i], "--id") == 0 && i + 1 < argc)
                args[arg_count++] = argv[++i];
        } else {
            const size_t len = strlen(argv[i]);
            data = xrealloc(data, data_len + len + 2);
            if (data_len > 0)
                data[data_len++] = ' ';
            memcpy(data + data_len, argv[i], len + 1);
            data_len += len;
        }
    }

    if (from_stdin) {
        free(data);
        data = read_all(STDIN_FILENO, &data_len);
    } else if (data != NULL)
        data_len = unescape(data);

    char *path = control_socket_path();
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (path == NULL || strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "foot @: no socket to reach foot on\n");
        return EXIT_FAILURE;
    }
    strcpy(addr.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
    {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            fprintf(stderr, "foot @: foot isn't running "
                    "(the application; or macos-remote-control=no)\n");
        } else
            fprintf(stderr, "foot @: %s: %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }

    struct session_conn conn;
    session_conn_init(&conn, fd);

    struct session_msg msg = {0};
    session_msg_begin(&msg, CONTROL_MSG_REQUEST);
    session_msg_strv(&msg, args);
    session_msg_blob(&msg, data, data_len);
    bool ok = session_conn_send(&conn, &msg, -1);
    session_msg_free(&msg);

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
            if (type != CONTROL_MSG_REPLY)
                continue;

            answered = true;
            const int code = session_read_i32(&r);
            size_t len;
            const void *output = session_read_blob(&r, &len);
            char *message = session_read_str(&r);
            if (!r.ok) {
                free(message);
                break;
            }

            fwrite(output, 1, len, stdout);
            if (message != NULL && message[0] != '\0')
                fprintf(stderr, "foot @: %s\n", message);
            free(message);
            status = code;
        }
    }

    if (!answered)
        fprintf(stderr, "foot @: foot didn't answer\n");

    session_conn_close(&conn);
    free(args);
    free(data);
    free(path);
    return fflush(stdout) == 0 ? status : EXIT_FAILURE;
}
