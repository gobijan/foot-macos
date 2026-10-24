#include "fdm.h"

#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>

#include <sys/epoll.h>
#include <sys/event.h>

#include <tllist.h>

#define LOG_MODULE "fdm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

/*
 * kqueue implementation of the FDM.
 *
 * Descriptors are registered with EVFILT_READ and/or EVFILT_WRITE
 * knotes. kqueue reports each filter separately; events for the same
 * descriptor are merged before dispatching, so handlers see the same
 * combined event mask as with epoll.
 *
 * Signals are received with EVFILT_SIGNAL knotes, on the same queue,
 * so no signal handler (or signal mask juggling) is needed.
 *
 * Besides the classic blocking fdm_poll(), the FDM can be driven by a
 * native run loop: fdm_fd() is readable whenever events are pending,
 * fdm_dispatch() handles them without blocking, and fdm_run_hooks()
 * should be called after that, and before the run loop goes to sleep.
 */

struct fd_handler {
    int fd;
    int events;
    fdm_fd_handler_t callback;
    void *callback_data;
    bool deleted;

    /* Events collected for the current dispatch round */
    int revents;
};

struct sig_handler {
    fdm_signal_handler_t callback;
    void *callback_data;
    struct sigaction original;
};

struct hook {
    fdm_hook_t callback;
    void *callback_data;
};

typedef tll(struct hook) hooks_t;

struct fdm {
    int kq;
    bool is_polling;
    tll(struct fd_handler *) fds;
    tll(struct fd_handler *) deferred_delete;

    struct sig_handler signal_handlers[NSIG];

    hooks_t hooks_low;
    hooks_t hooks_normal;
    hooks_t hooks_high;
};

static const char *
signame(int signo)
{
    return signo > 0 && signo < NSIG ? sys_signame[signo] : "?";
}

struct fdm *
fdm_init(void)
{
    int kq = kqueue();
    if (kq == -1) {
        LOG_ERRNO("failed to create kqueue");
        return NULL;
    }

    struct fdm *fdm = calloc(1, sizeof(*fdm));
    if (unlikely(fdm == NULL)) {
        LOG_ERRNO("malloc() failed");
        close(kq);
        return NULL;
    }

    fdm->kq = kq;
    fdm->is_polling = false;
    return fdm;
}

void
fdm_destroy(struct fdm *fdm)
{
    if (fdm == NULL)
        return;

    if (tll_length(fdm->fds) > 0)
        LOG_WARN("FD list not empty");

    for (int i = 0; i < NSIG; i++) {
        if (fdm->signal_handlers[i].callback != NULL)
            LOG_WARN("handler for signal %d (SIG%s) not removed",
                     i, signame(i));
    }

    if (tll_length(fdm->hooks_low) > 0 ||
        tll_length(fdm->hooks_normal) > 0 ||
        tll_length(fdm->hooks_high) > 0)
    {
        LOG_WARN("hook list not empty");
    }

    xassert(tll_length(fdm->fds) == 0);
    xassert(tll_length(fdm->deferred_delete) == 0);
    xassert(tll_length(fdm->hooks_low) == 0);
    xassert(tll_length(fdm->hooks_normal) == 0);
    xassert(tll_length(fdm->hooks_high) == 0);

    tll_free(fdm->fds);
    tll_free(fdm->deferred_delete);
    tll_free(fdm->hooks_low);
    tll_free(fdm->hooks_normal);
    tll_free(fdm->hooks_high);
    close(fdm->kq);
    free(fdm);
}

/* Adds/removes the read and write knotes, to go from old_events to new_events */
static bool
update_knotes(struct fdm *fdm, struct fd_handler *handler,
              int old_events, int new_events)
{
    struct kevent changes[2];
    int count = 0;

    if ((old_events ^ new_events) & EPOLLIN) {
        EV_SET(&changes[count++], handler->fd, EVFILT_READ,
               new_events & EPOLLIN ? EV_ADD | EV_ENABLE : EV_DELETE,
               0, 0, handler);
    }

    if ((old_events ^ new_events) & EPOLLOUT) {
        EV_SET(&changes[count++], handler->fd, EVFILT_WRITE,
               new_events & EPOLLOUT ? EV_ADD | EV_ENABLE : EV_DELETE,
               0, 0, handler);
    }

    if (count == 0)
        return true;

    return kevent(fdm->kq, changes, count, NULL, 0, NULL) == 0;
}

bool
fdm_add(struct fdm *fdm, int fd, int events, fdm_fd_handler_t cb, void *data)
{
#if defined(_DEBUG)
    tll_foreach(fdm->fds, it) {
        if (it->item->fd == fd) {
            BUG("FD=%d already registered", fd);
        }
    }
#endif

    struct fd_handler *handler = malloc(sizeof(*handler));
    if (unlikely(handler == NULL)) {
        LOG_ERRNO("malloc() failed");
        return false;
    }

    *handler = (struct fd_handler) {
        .fd = fd,
        .events = events,
        .callback = cb,
        .callback_data = data,
        .deleted = false,
    };

    /*
     * Note: unlike epoll, kqueue only reports hangups through a
     * filter. That's fine, since all users request at least one of
     * EPOLLIN or EPOLLOUT; both filters report EOF.
     */
    if (!update_knotes(fdm, handler, 0, events)) {
        LOG_ERRNO("failed to register FD=%d with kqueue", fd);
        free(handler);
        return false;
    }

    tll_push_back(fdm->fds, handler);
    return true;
}

static bool
fdm_del_internal(struct fdm *fdm, int fd, bool close_fd)
{
    if (fd == -1)
        return true;

    tll_foreach(fdm->fds, it) {
        if (it->item->fd != fd)
            continue;

        if (close_fd) {
            /* Closing the descriptor removes its knotes */
            close(it->item->fd);
        } else {
            if (!update_knotes(fdm, it->item, it->item->events, 0))
                LOG_ERRNO("failed to unregister FD=%d from kqueue", fd);
        }

        it->item->deleted = true;
        if (fdm->is_polling)
            tll_push_back(fdm->deferred_delete, it->item);
        else
            free(it->item);

        tll_remove(fdm->fds, it);
        return true;
    }

    LOG_ERR("no such FD: %d", fd);
    close(fd);
    return false;
}

bool
fdm_del(struct fdm *fdm, int fd)
{
    return fdm_del_internal(fdm, fd, true);
}

bool
fdm_del_no_close(struct fdm *fdm, int fd)
{
    return fdm_del_internal(fdm, fd, false);
}

static bool
event_modify(struct fdm *fdm, struct fd_handler *fd, int new_events)
{
    if (new_events == fd->events)
        return true;

    if (!update_knotes(fdm, fd, fd->events, new_events)) {
        LOG_ERRNO("failed to modify FD=%d with kqueue (events 0x%08x -> 0x%08x)",
                  fd->fd, fd->events, new_events);
        return false;
    }

    fd->events = new_events;
    return true;
}

bool
fdm_event_add(struct fdm *fdm, int fd, int events)
{
    tll_foreach(fdm->fds, it) {
        if (it->item->fd != fd)
            continue;

        return event_modify(fdm, it->item, it->item->events | events);
    }

    LOG_ERR("FD=%d not registered with the FDM", fd);
    return false;
}

bool
fdm_event_del(struct fdm *fdm, int fd, int events)
{
    tll_foreach(fdm->fds, it) {
        if (it->item->fd != fd)
            continue;

        return event_modify(fdm, it->item, it->item->events & ~events);
    }

    LOG_ERR("FD=%d not registered with the FDM", fd);
    return false;
}

static hooks_t *
hook_priority_to_list(struct fdm *fdm, enum fdm_hook_priority priority)
{
    switch (priority) {
    case FDM_HOOK_PRIORITY_LOW:    return &fdm->hooks_low;
    case FDM_HOOK_PRIORITY_NORMAL: return &fdm->hooks_normal;
    case FDM_HOOK_PRIORITY_HIGH:   return &fdm->hooks_high;
    }

    BUG("unhandled priority type");
    return NULL;
}

bool
fdm_hook_add(struct fdm *fdm, fdm_hook_t hook, void *data,
             enum fdm_hook_priority priority)
{
    hooks_t *hooks = hook_priority_to_list(fdm, priority);

#if defined(_DEBUG)
    tll_foreach(*hooks, it) {
        if (it->item.callback == hook) {
            LOG_ERR("hook=0x%" PRIxPTR " already registered", (uintptr_t)hook);
            return false;
        }
    }
#endif

    tll_push_back(*hooks, ((struct hook){hook, data}));
    return true;
}

bool
fdm_hook_del(struct fdm *fdm, fdm_hook_t hook, enum fdm_hook_priority priority)
{
    hooks_t *hooks = hook_priority_to_list(fdm, priority);

    tll_foreach(*hooks, it) {
        if (it->item.callback != hook)
            continue;

        tll_remove(*hooks, it);
        return true;
    }

    LOG_WARN("hook=0x%" PRIxPTR " not registered", (uintptr_t)hook);
    return false;
}

/* Signals whose default action is to discard them */
static bool
default_action_is_ignore(int signo)
{
    return signo == SIGCHLD || signo == SIGWINCH || signo == SIGURG ||
        signo == SIGCONT || signo == SIGINFO;
}

bool
fdm_signal_add(struct fdm *fdm, int signo, fdm_signal_handler_t handler, void *data)
{
    if (signo <= 0 || signo >= NSIG) {
        LOG_ERR("invalid signal: %d", signo);
        return false;
    }

    if (fdm->signal_handlers[signo].callback != NULL) {
        LOG_ERR("signal %d (SIG%s) already has a handler",
                signo, signame(signo));
        return false;
    }

    /*
     * EVFILT_SIGNAL records every attempt to deliver the signal,
     * even if it is ignored. Make sure the default action doesn't
     * terminate us. Signals that are discarded by default are left
     * alone; SIG_IGN for SIGCHLD would e.g. enable auto-reaping.
     */
    struct sigaction original;
    if (sigaction(signo, NULL, &original) < 0) {
        LOG_ERRNO("failed to get signal handler for signal %d (SIG%s)",
                  signo, signame(signo));
        return false;
    }

    if (!default_action_is_ignore(signo)) {
        struct sigaction action = {.sa_handler = SIG_IGN};
        sigemptyset(&action.sa_mask);
        if (sigaction(signo, &action, NULL) < 0) {
            LOG_ERRNO("failed to ignore signal %d (SIG%s)",
                      signo, signame(signo));
            return false;
        }
    }

    struct kevent ev;
    EV_SET(&ev, signo, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(fdm->kq, &ev, 1, NULL, 0, NULL) < 0) {
        LOG_ERRNO("failed to register signal %d (SIG%s) with kqueue",
                  signo, signame(signo));
        sigaction(signo, &original, NULL);
        return false;
    }

    /* Make sure it isn't blocked, or it will never be delivered */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    fdm->signal_handlers[signo].callback = handler;
    fdm->signal_handlers[signo].callback_data = data;
    fdm->signal_handlers[signo].original = original;
    return true;
}

bool
fdm_signal_del(struct fdm *fdm, int signo)
{
    if (signo <= 0 || signo >= NSIG ||
        fdm->signal_handlers[signo].callback == NULL)
    {
        return false;
    }

    struct kevent ev;
    EV_SET(&ev, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    if (kevent(fdm->kq, &ev, 1, NULL, 0, NULL) < 0)
        LOG_ERRNO("failed to unregister signal %d (SIG%s) from kqueue",
                  signo, signame(signo));

    if (sigaction(signo, &fdm->signal_handlers[signo].original, NULL) < 0) {
        LOG_ERRNO("failed to restore signal handler for signal %d (SIG%s)",
                  signo, signame(signo));
        return false;
    }

    fdm->signal_handlers[signo].callback = NULL;
    fdm->signal_handlers[signo].callback_data = NULL;
    return true;
}

int
fdm_fd(const struct fdm *fdm)
{
    return fdm->kq;
}

void
fdm_run_hooks(struct fdm *fdm)
{
    tll_foreach(fdm->hooks_high, it) {
        LOG_DBG(
            "executing high priority hook 0x%" PRIxPTR" (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
    tll_foreach(fdm->hooks_normal, it) {
        LOG_DBG(
            "executing normal priority hook 0x%" PRIxPTR " (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
    tll_foreach(fdm->hooks_low, it) {
        LOG_DBG(
            "executing low priority hook 0x%" PRIxPTR " (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
}

static int
read_filter_events(const struct kevent *ev)
{
    int events = 0;
    if (ev->data > 0 || !(ev->flags & EV_EOF))
        events |= EPOLLIN;
    if (ev->flags & EV_EOF)
        events |= EPOLLHUP | (ev->fflags != 0 ? EPOLLERR : 0);
    return events;
}

static int
write_filter_events(const struct kevent *ev)
{
    int events = EPOLLOUT;
    if (ev->flags & EV_EOF)
        events |= EPOLLHUP | (ev->fflags != 0 ? EPOLLERR : 0);
    return events;
}

/* Handles 'count' events retrieved from the kqueue */
static bool
dispatch(struct fdm *fdm, const struct kevent *events, int count)
{
    /* Signals first, then descriptors (merged per descriptor) */
    struct fd_handler *ready[count];
    int ready_count = 0;

    /* Handlers deleted from here on are freed after we're done */
    fdm->is_polling = true;

    bool ret = true;

    for (int i = 0; i < count; i++) {
        const struct kevent *ev = &events[i];

        if (ev->flags & EV_ERROR) {
            LOG_ERRNO_P((int)ev->data, "kqueue error on ident=%" PRIuPTR,
                        (uintptr_t)ev->ident);
            continue;
        }

        if (ev->filter == EVFILT_SIGNAL) {
            const int signo = (int)ev->ident;
            struct sig_handler *handler = &fdm->signal_handlers[signo];

            if (handler->callback != NULL &&
                !handler->callback(fdm, signo, handler->callback_data))
            {
                ret = false;
                break;
            }
            continue;
        }

        struct fd_handler *fd = ev->udata;
        if (fd == NULL || fd->deleted)
            continue;

        if (fd->revents == 0)
            ready[ready_count++] = fd;

        fd->revents |= ev->filter == EVFILT_READ
            ? read_filter_events(ev)
            : write_filter_events(ev);
    }

    for (int i = 0; ret && i < ready_count; i++) {
        struct fd_handler *fd = ready[i];

        const int revents = fd->revents;
        fd->revents = 0;

        if (fd->deleted)
            continue;

        if (!fd->callback(fdm, fd->fd, revents, fd->callback_data))
            ret = false;
    }

    /* Reset state of handlers we didn't get to */
    for (int i = 0; i < ready_count; i++)
        ready[i]->revents = 0;

    fdm->is_polling = false;

    tll_foreach(fdm->deferred_delete, it) {
        free(it->item);
        tll_remove(fdm->deferred_delete, it);
    }

    return ret;
}

/* Returns the number of events handled, or -1 on failure */
static int
fdm_wait_and_dispatch(struct fdm *fdm, const struct timespec *timeout)
{
    xassert(!fdm->is_polling && "nested calls to fdm_poll() not allowed");
    if (fdm->is_polling) {
        LOG_ERR("nested calls to fdm_poll() not allowed");
        return -1;
    }

    /* Read and write filters per FD, plus signals */
    const size_t max_events = 2 * tll_length(fdm->fds) + 8;
    struct kevent events[max_events];

    int r = kevent(fdm->kq, NULL, 0, events, (int)max_events, timeout);
    if (unlikely(r < 0)) {
        if (errno == EINTR)
            return 0;

        LOG_ERRNO("failed to wait for kqueue events");
        return -1;
    }

    return dispatch(fdm, events, r) ? r : -1;
}

int
fdm_dispatch(struct fdm *fdm)
{
    return fdm_wait_and_dispatch(fdm, &(struct timespec){0});
}

bool
fdm_poll(struct fdm *fdm)
{
    fdm_run_hooks(fdm);
    return fdm_wait_and_dispatch(fdm, NULL) >= 0;
}
