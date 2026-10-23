#pragma once

/*
 * Force-included in every translation unit (see meson.build).
 *
 * Fills the few gaps between the POSIX/Linux interfaces foot's
 * terminal core is written against, and macOS' libc.
 */

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

/* macOS has no real-time signals */
#if !defined(SIGRTMAX)
 #define SIGRTMAX NSIG
#endif

/*
 * pipe2() was only added in macOS 27. The non-atomic variant is fine
 * for foot: new processes are only ever forked from the main thread.
 */
static inline int
foot_pipe2(int fds[2], int flags)
{
    if (pipe(fds) < 0)
        return -1;

    for (int i = 0; i < 2; i++) {
        if (((flags & O_CLOEXEC) &&
             fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) ||
            ((flags & O_NONBLOCK) &&
             fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) < 0))
        {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
    }

    return 0;
}
#define pipe2(fds, flags) foot_pipe2(fds, flags)
