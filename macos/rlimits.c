/*
 * The open files limit; see rlimits.h.
 */

#include "rlimits.h"

#include <stdbool.h>
#include <sys/resource.h>
#if defined(__APPLE__)
 #include <sys/syslimits.h>
#endif

#define LOG_MODULE "rlimits"
#define LOG_ENABLE_DBG 0
#include "log.h"

static struct rlimit original;
static bool raised;

void
rlimits_raise_open_files(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) {
        LOG_ERRNO("failed to get the open files limit");
        return;
    }

#if defined(__APPLE__)
    /* OPEN_MAX: setrlimit() doesn't take more (nor RLIM_INFINITY) */
    const rlim_t wanted = limit.rlim_max < OPEN_MAX ? limit.rlim_max : OPEN_MAX;
#else
    const rlim_t wanted = limit.rlim_max;
#endif
    if (limit.rlim_cur >= wanted)
        return;

    original = limit;
    limit.rlim_cur = wanted;
    if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
        LOG_ERRNO("failed to raise the open files limit");
        return;
    }

    raised = true;
    LOG_INFO("open files limit: %llu (was %llu)",
             (unsigned long long)limit.rlim_cur, (unsigned long long)original.rlim_cur);
}

void
rlimits_restore_open_files(void)
{
    /* Only lowers it: what's open stays open (until exec(), with FD_CLOEXEC) */
    if (raised)
        setrlimit(RLIMIT_NOFILE, &original);
}
