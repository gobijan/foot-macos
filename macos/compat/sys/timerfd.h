#pragma once

/*
 * Timer file descriptors, implemented natively on kqueue (see
 * macos/timerfd.c).
 *
 * Each timer is its own kqueue holding a single EVFILT_TIMER. A kqueue
 * descriptor is itself pollable, so these can be registered with the
 * FDM like any other descriptor, and closed with close(2).
 *
 * The only difference to Linux' timerfd is that the expiration count
 * is retrieved with timerfd_read(), instead of read(2).
 */

#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define TFD_TIMER_ABSTIME 0x1

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

/* Returns sizeof(uint64_t) on success. Fails with EAGAIN if the timer
 * has not expired */
ssize_t timerfd_read(int fd, uint64_t *expiration_count);
