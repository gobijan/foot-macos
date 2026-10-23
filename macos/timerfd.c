#include <sys/timerfd.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/event.h>

#define LOG_MODULE "timerfd"
#define LOG_ENABLE_DBG 0
#include "log.h"

/*
 * kqueue timers count mach absolute time, i.e. they do not advance
 * while the system is asleep. CLOCK_UPTIME_RAW is the same clock.
 */
#define TIMER_CLOCK CLOCK_UPTIME_RAW

/* Timers shorter than this are never coalesced by the kernel */
#define CRITICAL_THRESHOLD_NS (100 * 1000000LL)

/* The only knote in each timer kqueue */
#define TIMER_IDENT 1

/*
 * Book-keeping needed to implement timerfd_gettime(), and timers
 * whose initial expiration differs from their interval (kqueue
 * timers are either one-shot, or periodic with a fixed period).
 *
 * Indexed by FD. Timers are only ever used from the main thread.
 */
struct timer_state {
    bool valid;
    bool nonblocking;
    bool armed;
    bool switch_to_periodic;  /* Initial one-shot armed, interval pending */
    int64_t interval_ns;
    int64_t deadline_ns;      /* Next expiration, in TIMER_CLOCK */
};

static struct timer_state *timers = NULL;
static size_t timers_count = 0;

static int64_t
now_ns(void)
{
    return (int64_t)clock_gettime_nsec_np(TIMER_CLOCK);
}

static int64_t
timespec_to_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

static struct timespec
ns_to_timespec(int64_t ns)
{
    return (struct timespec){
        .tv_sec = ns / 1000000000LL,
        .tv_nsec = ns % 1000000000LL,
    };
}

static struct timer_state *
timer_state(int fd)
{
    if (fd < 0 || (size_t)fd >= timers_count || !timers[fd].valid) {
        errno = EBADF;
        return NULL;
    }
    return &timers[fd];
}

static bool
arm(int fd, int64_t delay_ns, bool periodic)
{
    if (delay_ns < 1)
        delay_ns = 1;

    unsigned fflags = NOTE_NSECONDS;
    if (delay_ns < CRITICAL_THRESHOLD_NS)
        fflags |= NOTE_CRITICAL;

    struct kevent ev;
    EV_SET(&ev, TIMER_IDENT, EVFILT_TIMER,
           EV_ADD | EV_ENABLE | (periodic ? 0 : EV_ONESHOT),
           fflags, delay_ns, NULL);

    return kevent(fd, &ev, 1, NULL, 0, NULL) == 0;
}

static void
disarm(int fd)
{
    /* Deleting the knote also discards any pending expirations */
    struct kevent ev;
    EV_SET(&ev, TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    kevent(fd, &ev, 1, NULL, 0, NULL);  /* ENOENT if not armed */
}

int
timerfd_create(int clockid, int flags)
{
    if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
        errno = EINVAL;
        return -1;
    }

    /* kqueues are never inherited across fork(2), so CLOEXEC is implied */
    int fd = kqueue();
    if (fd < 0)
        return -1;

    if ((size_t)fd >= timers_count) {
        size_t new_count = (size_t)fd + 16;
        struct timer_state *new_timers =
            realloc(timers, new_count * sizeof(new_timers[0]));

        if (new_timers == NULL) {
            close(fd);
            errno = ENOMEM;
            return -1;
        }

        memset(&new_timers[timers_count], 0,
               (new_count - timers_count) * sizeof(new_timers[0]));
        timers = new_timers;
        timers_count = new_count;
    }

    timers[fd] = (struct timer_state){
        .valid = true,
        .nonblocking = (flags & TFD_NONBLOCK) != 0,
    };
    return fd;
}

int
timerfd_gettime(int fd, struct itimerspec *curr_value)
{
    struct timer_state *t = timer_state(fd);
    if (t == NULL)
        return -1;

    int64_t remaining = 0;

    if (t->armed) {
        const int64_t now = now_ns();

        if (t->interval_ns > 0 && !t->switch_to_periodic) {
            /* Periodic: time until the next expiration */
            while (t->deadline_ns <= now)
                t->deadline_ns += t->interval_ns;
        }

        remaining = t->deadline_ns - now;
        if (remaining < 0)
            remaining = 0;
    }

    curr_value->it_value = ns_to_timespec(remaining);
    curr_value->it_interval = ns_to_timespec(t->interval_ns);
    return 0;
}

int
timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                struct itimerspec *old_value)
{
    struct timer_state *t = timer_state(fd);
    if (t == NULL)
        return -1;

    if (old_value != NULL && timerfd_gettime(fd, old_value) < 0)
        return -1;

    disarm(fd);
    t->armed = false;
    t->switch_to_periodic = false;
    t->interval_ns = timespec_to_ns(&new_value->it_interval);

    int64_t value_ns = timespec_to_ns(&new_value->it_value);
    if (value_ns == 0)
        return 0;

    const int64_t now = now_ns();

    if (flags & TFD_TIMER_ABSTIME) {
        /* Absolute CLOCK_MONOTONIC (continuous time) -> relative */
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        value_ns -= timespec_to_ns(&mono);
    }

    const bool periodic = t->interval_ns > 0 && t->interval_ns == value_ns;

    if (!arm(fd, value_ns, periodic))
        return -1;

    t->armed = true;
    t->switch_to_periodic = t->interval_ns > 0 && !periodic;
    t->deadline_ns = now + (value_ns > 0 ? value_ns : 1);
    return 0;
}

ssize_t
timerfd_read(int fd, uint64_t *expiration_count)
{
    struct timer_state *t = timer_state(fd);
    if (t == NULL)
        return -1;

    struct kevent ev;
    int r = kevent(
        fd, NULL, 0, &ev, 1, t->nonblocking ? &(struct timespec){0} : NULL);

    if (r < 0)
        return -1;

    if (r == 0) {
        errno = EAGAIN;
        return -1;
    }

    if (ev.flags & EV_ERROR) {
        errno = (int)ev.data;
        return -1;
    }

    const uint64_t count = ev.data > 0 ? (uint64_t)ev.data : 1;
    *expiration_count = count;

    if (t->switch_to_periodic) {
        /* Initial expiration done; continue with the interval */
        t->switch_to_periodic = false;
        if (!arm(fd, t->interval_ns, true)) {
            LOG_ERRNO("failed to re-arm periodic timer");
            t->armed = false;
            return -1;
        }
        t->deadline_ns = now_ns() + t->interval_ns;
    }

    else if (t->interval_ns > 0)
        t->deadline_ns += (int64_t)count * t->interval_ns;

    else
        t->armed = false;

    return sizeof(*expiration_count);
}
