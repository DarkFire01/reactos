/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX process times (IEEE Std 1003.1-1990, s.4.5.2.2). 1:1 with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

#ifndef _CLOCK_T_DEFINED
#define _CLOCK_T_DEFINED
typedef long clock_t;
#endif

struct tms
{
    clock_t tms_utime;      // user CPU time
    clock_t tms_stime;      // system CPU time
    clock_t tms_cutime;     // user CPU time of terminated children
    clock_t tms_cstime;     // system CPU time of terminated children
};

clock_t times(struct tms *Buffer);
