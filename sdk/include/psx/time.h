/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Date and time (<time.h>) for the POSIX userland
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>      /* time_t, clock_t, size_t */

#define CLOCKS_PER_SEC  1000

#ifndef NULL
#define NULL ((void *)0)
#endif

/* _TM_DEFINED: shared guard with the ReactOS CRT's <wchar.h>/<time.h>, which
   define the (layout-identical) struct tm too -- avoids C2011 when both are
   visible in one translation unit. */
#ifndef _TM_DEFINED
#define _TM_DEFINED
struct tm
{
    int tm_sec;     /* seconds after the minute [0,60] */
    int tm_min;     /* minutes after the hour   [0,59] */
    int tm_hour;    /* hours since midnight      [0,23] */
    int tm_mday;    /* day of the month          [1,31] */
    int tm_mon;     /* months since January      [0,11] */
    int tm_year;    /* years since 1900 */
    int tm_wday;    /* days since Sunday         [0,6] */
    int tm_yday;    /* days since January 1      [0,365] */
    int tm_isdst;   /* daylight saving time flag */
};
#endif

clock_t clock(void);
time_t  time(time_t *Timer);
double  difftime(time_t Time1, time_t Time0);
time_t  mktime(struct tm *Tm);

struct tm *gmtime(const time_t *Timer);
struct tm *localtime(const time_t *Timer);
char      *asctime(const struct tm *Tm);
char      *ctime(const time_t *Timer);
size_t     strftime(char *String, size_t Max, const char *Format, const struct tm *Tm);
