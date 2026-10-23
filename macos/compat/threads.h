#pragma once

/*
 * Apple's libc does not ship C11 <threads.h>. This is the subset foot
 * uses, mapped 1:1 onto pthreads.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

enum {
    thrd_success = 0,
    thrd_busy = 1,
    thrd_error = 2,
    thrd_nomem = 3,
    thrd_timedout = 4,
};

enum {
    mtx_plain = 0,
    mtx_recursive = 1,
    mtx_timed = 2,
};

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef int (*thrd_start_t)(void *);

struct foot_thrd_trampoline {
    thrd_start_t func;
    void *arg;
};

static inline void *
foot_thrd_trampoline(void *_data)
{
    struct foot_thrd_trampoline data = *(struct foot_thrd_trampoline *)_data;
    free(_data);
    return (void *)(intptr_t)data.func(data.arg);
}

static inline int
thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    struct foot_thrd_trampoline *data = malloc(sizeof(*data));
    if (data == NULL)
        return thrd_nomem;

    data->func = func;
    data->arg = arg;

    int r = pthread_create(thr, NULL, &foot_thrd_trampoline, data);
    if (r != 0) {
        free(data);
        return r == EAGAIN ? thrd_nomem : thrd_error;
    }
    return thrd_success;
}

static inline int
thrd_join(thrd_t thr, int *res)
{
    void *ret;
    if (pthread_join(thr, &ret) != 0)
        return thrd_error;
    if (res != NULL)
        *res = (int)(intptr_t)ret;
    return thrd_success;
}

static inline int
mtx_init(mtx_t *mtx, int type)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(
        &attr, type & mtx_recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    int r = pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    return r == 0 ? thrd_success : thrd_error;
}

static inline void
mtx_destroy(mtx_t *mtx)
{
    pthread_mutex_destroy(mtx);
}

static inline int
mtx_lock(mtx_t *mtx)
{
    return pthread_mutex_lock(mtx) == 0 ? thrd_success : thrd_error;
}

static inline int
mtx_unlock(mtx_t *mtx)
{
    return pthread_mutex_unlock(mtx) == 0 ? thrd_success : thrd_error;
}

static inline int
cnd_init(cnd_t *cond)
{
    int r = pthread_cond_init(cond, NULL);
    return r == 0 ? thrd_success : r == ENOMEM ? thrd_nomem : thrd_error;
}

static inline void
cnd_destroy(cnd_t *cond)
{
    pthread_cond_destroy(cond);
}

static inline int
cnd_signal(cnd_t *cond)
{
    return pthread_cond_signal(cond) == 0 ? thrd_success : thrd_error;
}

static inline int
cnd_broadcast(cnd_t *cond)
{
    return pthread_cond_broadcast(cond) == 0 ? thrd_success : thrd_error;
}

static inline int
cnd_wait(cnd_t *cond, mtx_t *mtx)
{
    return pthread_cond_wait(cond, mtx) == 0 ? thrd_success : thrd_error;
}
