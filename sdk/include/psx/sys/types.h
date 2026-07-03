/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX primitive system data types (IEEE Std 1003.1-1990, s.2.6).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

typedef unsigned long gid_t;
typedef unsigned long mode_t;
typedef unsigned long nlink_t;
typedef          long pid_t;
typedef unsigned long uid_t;

#ifndef _OFF_T_DEFINED
typedef long off_t;
#define _OFF_T_DEFINED
#endif

#ifndef _DEV_T_DEFINED
typedef unsigned long dev_t;
#define _DEV_T_DEFINED
#endif

#ifndef _INO_T_DEFINED
typedef unsigned long ino_t;
#define _INO_T_DEFINED
#endif

#ifndef _TIME_T_DEFINED
typedef long time_t;
#define _TIME_T_DEFINED
#endif

/* Separate guard: the ReactOS CRT defines time_t (setting _TIME_T_DEFINED)
   without defining clock_t, so clock_t must not hide behind time_t's guard. */
#ifndef _CLOCK_T_DEFINED
typedef long clock_t;
#define _CLOCK_T_DEFINED
#endif

#ifndef _SIZE_T_DEFINED
typedef unsigned int size_t;
#define _SIZE_T_DEFINED
#endif

#ifndef _SSIZE_T_DEFINED
typedef signed int ssize_t;
#define _SSIZE_T_DEFINED
#endif

//
// Additional (non-POSIX) BSD types. Strictly these belong under !_POSIX_SOURCE,
// but the reskit BSD userland (ls/find/grep, FTS) needs them even when built with
// -D_POSIX_SOURCE, so they are always visible here.
//
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned short ushort;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef unsigned char  unchar;
typedef char          *caddr_t;
typedef int            key_t;

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval  { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
#endif
