#pragma once

/*
 * Linux: footd only (macos/meson.build). The terminal core reads its
 * timers with timerfd_read(), as on macOS (macos/compat/sys/timerfd.h);
 * Linux' timerfd is read with read(2).
 *
 * <math.h>: the core uses it without including it (on macOS, a system
 * header brings it along).
 */

#include <math.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/timerfd.h>

static inline ssize_t
timerfd_read(int fd, uint64_t *expiration_count)
{
    return read(fd, expiration_count, sizeof(*expiration_count));
}
