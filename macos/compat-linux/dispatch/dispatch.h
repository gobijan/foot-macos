#pragma once

/*
 * Linux: footd only (macos/meson.build). What the terminal core uses of
 * libdispatch on macOS, its semaphores (the render workers', which
 * footd doesn't start), on POSIX ones.
 */

#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct dispatch_semaphore_s { sem_t sem; } *dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;

#define DISPATCH_TIME_FOREVER (~(dispatch_time_t)0)

static inline dispatch_semaphore_t
dispatch_semaphore_create(long value)
{
    dispatch_semaphore_t sema = malloc(sizeof(*sema));
    if (sema != NULL && sem_init(&sema->sem, 0, (unsigned)value) < 0) {
        free(sema);
        return NULL;
    }
    return sema;
}

static inline long
dispatch_semaphore_signal(dispatch_semaphore_t sema)
{
    sem_post(&sema->sem);
    return 0;
}

/* Only DISPATCH_TIME_FOREVER is used */
static inline long
dispatch_semaphore_wait(dispatch_semaphore_t sema, dispatch_time_t timeout)
{
    (void)timeout;
    while (sem_wait(&sema->sem) < 0 && errno == EINTR)
        ;
    return 0;
}

static inline void
dispatch_release(dispatch_semaphore_t sema)
{
    sem_destroy(&sema->sem);
    free(sema);
}
