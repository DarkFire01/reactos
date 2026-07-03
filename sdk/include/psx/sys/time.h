/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Time of day and interval timers (<sys/time.h>)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>      /* struct timeval, struct timezone */

int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);   /* psxdll extension 122 */
int utimes(const char *path, const struct timeval *times);

/* Interval timers are declared for source compatibility only; setitimer/getitimer are no-ops. */
struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

#define ITIMER_REAL     0
#define ITIMER_VIRTUAL  1
#define ITIMER_PROF     2

int setitimer(int which, const struct itimerval *value, struct itimerval *ovalue);
int getitimer(int which, struct itimerval *value);

/* BSD timeval helper macros */
#ifndef timerclear
#define timerclear(tvp)   ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timerisset(tvp)   ((tvp)->tv_sec || (tvp)->tv_usec)
#define timercmp(a, b, CMP) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec) \
                                  : ((a)->tv_sec CMP (b)->tv_sec))
#endif
