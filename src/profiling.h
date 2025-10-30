/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PROFILING_H
#define PROFILING_H

#include <stdio.h>
#include <time.h>

/* Simple profiling macros for measuring render pipeline performance */

#ifdef ENABLE_PROFILING

static struct timespec prof_start, prof_end;

#define PROFILE_START() clock_gettime(CLOCK_MONOTONIC, &prof_start)

#define PROFILE_END(name)                                                 \
    do {                                                                  \
        clock_gettime(CLOCK_MONOTONIC, &prof_end);                        \
        long ns = (prof_end.tv_sec - prof_start.tv_sec) * 1000000000L +   \
                  (prof_end.tv_nsec - prof_start.tv_nsec);                \
        fprintf(stderr, "%s: %ld ns (%.2f us)\n", name, ns, ns / 1000.0); \
    } while (0)

#else

/* No-op when profiling is disabled */
#define PROFILE_START() ((void) 0)
#define PROFILE_END(name) ((void) 0)

#endif

#endif /* PROFILING_H */
