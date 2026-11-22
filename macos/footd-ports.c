/*
 * Another machine's TCP ports; see footd-ports.h.
 */

#include "footd-ports.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#if defined(__APPLE__)
 #include <libproc.h>
 #include <sys/proc_info.h>
#endif

#include <tllist.h>

#define LOG_MODULE "ports"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "xmalloc.h"

/* Whether something listens on a port here is known at once; a machine that's busy gets this long (milliseconds) */
#define PROBE_TIMEOUT 200

/* Connections waiting to be accepted */
#define BACKLOG 64

union address {
    struct sockaddr sa;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
};

/* This machine itself: 127.0.0.1, or ::1 */
static socklen_t
loopback(int family, uint16_t port, union address *address)
{
    memset(address, 0, sizeof(*address));
    if (family == AF_INET) {
        address->in.sin_family = AF_INET;
        address->in.sin_port = htons(port);
        address->in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return sizeof(address->in);
    }

    address->in6.sin6_family = AF_INET6;
    address->in6.sin6_port = htons(port);
    address->in6.sin6_addr = in6addr_loopback;
    return sizeof(address->in6);
}

static bool
configure(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

/* Non-blocking; -1: errno */
static int
tcp_socket(int family)
{
    const int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    if (!configure(fd)) {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

/* What's typed (e.g. a database's prompt) isn't to wait for more */
static void
no_delay(int fd)
{
    const int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/*
 * Listening here
 */

/* Whether a program listens on 'port' here (connecting to it tells) */
static bool
listening_here(int family, uint16_t port)
{
    const int fd = tcp_socket(family);
    if (fd < 0)
        return false;

    union address address;
    const socklen_t len = loopback(family, port, &address);

    int err = 0;
    if (connect(fd, &address.sa, len) < 0) {
        err = errno;
        if (err == EINPROGRESS || err == EINTR) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            socklen_t err_len = sizeof(err);
            if (poll(&pfd, 1, PROBE_TIMEOUT) != 1)
                err = ETIMEDOUT;
            else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0)
                err = errno;
        }
    }
    close(fd);

    /* Refused, or there's no such address: nobody; no answer: somebody who's busy */
    return err == 0 || err == ETIMEDOUT;
}

/* -1: errno */
static int
listen_on(int family, uint16_t port, uint16_t *bound)
{
    const int fd = tcp_socket(family);
    if (fd < 0)
        return -1;

    /* (Connections of before, on their way out, don't take the port) */
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));

    union address address;
    socklen_t len = loopback(family, port, &address);

    if (bind(fd, &address.sa, len) < 0 || listen(fd, BACKLOG) < 0 ||
        getsockname(fd, &address.sa, &len) < 0)
    {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }

    *bound = ntohs(family == AF_INET ? address.in.sin_port : address.in6.sin6_port);
    return fd;
}

/* The same port on both, or (without IPv6) on 127.0.0.1 only; 0: not this one */
static uint16_t
listen_both(uint16_t port, int fds[static 2])
{
    uint16_t bound = 0;
    fds[1] = -1;
    if ((fds[0] = listen_on(AF_INET, port, &bound)) < 0)
        return 0;

    uint16_t same;
    fds[1] = listen_on(AF_INET6, bound, &same);
    if (fds[1] < 0 && (errno == EADDRINUSE || errno == EACCES)) {
        close(fds[0]);
        fds[0] = -1;
        errno = EADDRINUSE;
        return 0;
    }
    return bound;
}

uint16_t
ports_listen(uint16_t port, int fds[static 2])
{
    fds[0] = fds[1] = -1;

    if (port != 0) {
        /*
         * A program that listens on all addresses, or on the other one,
         * leaves this one to be bound: localhost would then be us for
         * some, and it for others
         */
        if (listening_here(AF_INET, port) || listening_here(AF_INET6, port))
            return 0;

        const uint16_t bound = listen_both(port, fds);
        if (bound == 0 && errno != EADDRINUSE && errno != EACCES)
            LOG_ERRNO("failed to listen on port %u", port);
        return bound;
    }

    /* A free one: on 127.0.0.1, which ::1 may not have (then another) */
    for (int i = 0; i < 8; i++) {
        const uint16_t bound = listen_both(0, fds);
        if (bound != 0)
            return bound;
        if (errno != EADDRINUSE)
            break;
    }
    LOG_ERRNO("failed to listen on a port");
    return 0;
}

int
ports_accept(int listen_fd)
{
    int fd;
    do {
        fd = accept(listen_fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;

    if (!configure(fd)) {
        close(fd);
        return -1;
    }
    no_delay(fd);
    return fd;
}

/*
 * Connecting
 */

struct connecting {
    struct fdm *fdm;
    int fd;             /* -1: none (yet) */
    uint16_t port;
    bool localhost;     /* ::1 is tried next */
    int refused;        /* Why 127.0.0.1 couldn't be (errno); 0: it wasn't tried */
    ports_connected_t done;
    void *data;
};

static tll(struct connecting *) connects;

static void connect_to(struct connecting *c, const union address *address, socklen_t len);

static void
connect_done(struct connecting *c, int fd, const char *error)
{
    tll_foreach(connects, it) {
        if (it->item == c) {
            tll_remove(connects, it);
            break;
        }
    }

    const ports_connected_t done = c->done;
    void *data = c->data;
    free(c);
    done(fd, error, data);
}

static void
connect_failed(struct connecting *c, int err)
{
    if (c->localhost) {
        c->localhost = false;
        c->refused = err;

        union address address;
        const socklen_t len = loopback(AF_INET6, c->port, &address);
        connect_to(c, &address, len);
        return;
    }

    /* (Without IPv6, ::1 says nothing: what 127.0.0.1 said) */
    if (c->refused != 0 && err != ECONNREFUSED)
        err = c->refused;
    connect_done(c, -1, err == ECONNREFUSED ? "nothing is listening on it" : strerror(err));
}

static bool
fdm_connecting(struct fdm *fdm, int fd, int events, void *data)
{
    struct connecting *c = data;

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        err = errno;

    if (err == 0) {
        union address peer;
        socklen_t peer_len = sizeof(peer);
        if (getpeername(fd, &peer.sa, &peer_len) < 0)
            err = errno == ENOTCONN ? ECONNREFUSED : errno;
    }

    c->fd = -1;
    if (err != 0) {
        fdm_del(fdm, fd);
        connect_failed(c, err);
        return true;
    }

    fdm_del_no_close(fdm, fd);
    no_delay(fd);
    connect_done(c, fd, NULL);
    return true;
}

static void
connect_to(struct connecting *c, const union address *address, socklen_t len)
{
    const int fd = tcp_socket(address->sa.sa_family);
    if (fd < 0) {
        connect_failed(c, errno);
        return;
    }

    if (connect(fd, &address->sa, len) == 0) {
        no_delay(fd);
        connect_done(c, fd, NULL);
        return;
    }

    if (errno != EINPROGRESS && errno != EINTR) {
        const int err = errno;
        close(fd);
        connect_failed(c, err);
        return;
    }

    if (!fdm_add(c->fdm, fd, EPOLLOUT, &fdm_connecting, c)) {
        close(fd);
        c->localhost = false;
        connect_failed(c, ENOMEM);
        return;
    }
    c->fd = fd;
}

void
ports_connect(struct fdm *fdm, const char *host, uint16_t port,
              ports_connected_t done, void *data)
{
    union address address;
    socklen_t len;
    bool localhost = false;

    memset(&address, 0, sizeof(address));
    if (strcmp(host, "localhost") == 0) {
        len = loopback(AF_INET, port, &address);
        localhost = true;
    } else if (inet_pton(AF_INET, host, &address.in.sin_addr) == 1) {
        address.in.sin_family = AF_INET;
        address.in.sin_port = htons(port);
        len = sizeof(address.in);
    } else if (inet_pton(AF_INET6, host, &address.in6.sin6_addr) == 1) {
        address.in6.sin6_family = AF_INET6;
        address.in6.sin6_port = htons(port);
        len = sizeof(address.in6);
    } else {
        done(-1, "its machine isn't an address (names aren't looked up)", data);
        return;
    }

    struct connecting *c = xmalloc(sizeof(*c));
    *c = (struct connecting){
        .fdm = fdm, .fd = -1, .port = port, .localhost = localhost, .done = done, .data = data,
    };
    tll_push_back(connects, c);
    connect_to(c, &address, len);
}

void
ports_connect_cancel(const void *data)
{
    tll_foreach(connects, it) {
        struct connecting *c = it->item;
        if (data != NULL && c->data != data)
            continue;

        if (c->fd >= 0)
            fdm_del(c->fdm, c->fd);
        tll_remove(connects, it);
        free(c);
    }
}

/*
 * Which ports there are
 */

struct found {
    struct listening_port *ports;
    size_t count;
};

/* Each port once: with a name, if one of those listening on it has one */
static void
found_add(struct found *found, uint16_t port, const char *program, bool own)
{
    for (size_t i = 0; i < found->count; i++) {
        struct listening_port *p = &found->ports[i];
        if (p->port != port)
            continue;

        if (p->program == NULL && program != NULL)
            p->program = xstrdup(program);
        p->own = p->own || own;
        return;
    }

    found->ports = xreallocarray(found->ports, found->count + 1, sizeof(found->ports[0]));
    found->ports[found->count++] = (struct listening_port){
        .port = port, .program = program != NULL ? xstrdup(program) : NULL, .own = own,
    };
}

static int
by_owner_and_port(const void *_a, const void *_b)
{
    const struct listening_port *a = _a;
    const struct listening_port *b = _b;
    if (a->own != b->own)
        return a->own ? -1 : 1;
    return (int)a->port - (int)b->port;
}

#if defined(__APPLE__)

/* The user's own programs' sockets (others' aren't shown to us) */
static void
find_ports(struct found *found)
{
    int size = proc_listallpids(NULL, 0);
    if (size <= 0)
        return;

    /* (Some may start meanwhile) */
    size += 64 * sizeof(pid_t);
    pid_t *pids = xmalloc(size);
    const int count = proc_listallpids(pids, size);

    for (int i = 0; i < count; i++) {
        const pid_t pid = pids[i];

        const int fds_size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
        if (fds_size <= 0)
            continue;

        struct proc_fdinfo *fds = xmalloc(fds_size);
        const int fds_len = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, fds_size);

        char name[2 * MAXCOMLEN + 1] = "";
        for (size_t j = 0; fds_len > 0 && j < fds_len / sizeof(fds[0]); j++) {
            if (fds[j].proc_fdtype != PROX_FDTYPE_SOCKET)
                continue;

            struct socket_fdinfo info;
            if (proc_pidfdinfo(pid, fds[j].proc_fd, PROC_PIDFDSOCKETINFO, &info,
                               sizeof(info)) != sizeof(info) ||
                info.psi.soi_kind != SOCKINFO_TCP ||
                info.psi.soi_proto.pri_tcp.tcpsi_state != TSI_S_LISTEN)
            {
                continue;
            }

            if (name[0] == '\0')
                proc_name(pid, name, sizeof(name));

            const uint16_t port = ntohs(info.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport);
            if (port != 0)
                found_add(found, port, name[0] != '\0' ? name : NULL, true);
        }
        free(fds);
    }
    free(pids);
}

#else

/* A listening socket: the kernel's list has them all, whose they are, but not the program's */
struct listener {
    uint16_t port;
    unsigned long inode;
    bool own;
    char *program;
};

static void
read_listeners(const char *path, struct listener **listeners, size_t *count)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        /* sl local:port remote:port st tx:rx tr:when retrnsmt uid timeout inode */
        unsigned port, state, uid;
        unsigned long inode;
        if (sscanf(line, " %*d: %*[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%*x %x %*x:%*x %*x:%*x %*x %u %*d %lu",
                   &port, &state, &uid, &inode) != 4)
        {
            continue;
        }

        /* TCP_LISTEN */
        if (state != 0x0a || port == 0 || port > UINT16_MAX)
            continue;

        *listeners = xreallocarray(*listeners, *count + 1, sizeof((*listeners)[0]));
        (*listeners)[(*count)++] = (struct listener){
            .port = port, .inode = inode, .own = uid == (unsigned)getuid(),
        };
    }
    fclose(f);
}

/* The programs that have those sockets: our own (others' can't be looked into) */
static void
name_listeners(struct listener *listeners, size_t count)
{
    DIR *proc = opendir("/proc");
    if (proc == NULL)
        return;

    const struct dirent *process;
    while ((process = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)process->d_name[0]))
            continue;

        char path[300];
        snprintf(path, sizeof(path), "/proc/%s/fd", process->d_name);
        DIR *fds = opendir(path);
        if (fds == NULL)
            continue;

        char name[64] = "";
        const struct dirent *fd;
        while ((fd = readdir(fds)) != NULL) {
            char target[64];
            const ssize_t len = readlinkat(dirfd(fds), fd->d_name, target, sizeof(target) - 1);
            if (len <= 0)
                continue;
            target[len] = '\0';

            unsigned long inode;
            if (sscanf(target, "socket:[%lu]", &inode) != 1)
                continue;

            for (size_t i = 0; i < count; i++) {
                if (listeners[i].inode != inode || listeners[i].program != NULL)
                    continue;

                if (name[0] == '\0') {
                    snprintf(path, sizeof(path), "/proc/%s/comm", process->d_name);
                    FILE *comm = fopen(path, "r");
                    if (comm != NULL) {
                        if (fgets(name, sizeof(name), comm) != NULL)
                            name[strcspn(name, "\n")] = '\0';
                        fclose(comm);
                    }
                }
                if (name[0] != '\0')
                    listeners[i].program = xstrdup(name);
            }
        }
        closedir(fds);
    }
    closedir(proc);
}

static void
find_ports(struct found *found)
{
    struct listener *listeners = NULL;
    size_t count = 0;
    read_listeners("/proc/net/tcp", &listeners, &count);
    read_listeners("/proc/net/tcp6", &listeners, &count);
    name_listeners(listeners, count);

    for (size_t i = 0; i < count; i++) {
        found_add(found, listeners[i].port, listeners[i].program, listeners[i].own);
        free(listeners[i].program);
    }
    free(listeners);
}

#endif

struct listening_port *
ports_listening(size_t *count)
{
    struct found found = {0};
    find_ports(&found);

    if (found.count > 0)
        qsort(found.ports, found.count, sizeof(found.ports[0]), &by_owner_and_port);
    *count = found.count;
    return found.ports;
}

void
ports_listening_free(struct listening_port *ports, size_t count)
{
    for (size_t i = 0; i < count; i++)
        free(ports[i].program);
    free(ports);
}
