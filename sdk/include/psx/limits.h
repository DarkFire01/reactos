/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation-defined limits (IEEE Std 1003.1-1990, s.2.8).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

//
// C language limits (i386)
//
#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN    // char is signed on i386
#define CHAR_MAX    SCHAR_MAX
#define MB_LEN_MAX  2
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    0xFFFFFFFFU
#define LONG_MIN    (-2147483647L - 1)
#define LONG_MAX    2147483647L
#define ULONG_MAX   0xFFFFFFFFUL

//
// POSIX minimum values (IEEE Std 1003.1-1990, Table 2-3). These are the lower
// bounds the standard guarantees, and are the same on every conforming system.
//
#define _POSIX_ARG_MAX      4096
#define _POSIX_CHILD_MAX    6
#define _POSIX_LINK_MAX     8
#define _POSIX_MAX_CANON    255
#define _POSIX_MAX_INPUT    255
#define _POSIX_NAME_MAX     14
#define _POSIX_NGROUPS_MAX  0
#define _POSIX_OPEN_MAX     16
#define _POSIX_PATH_MAX     255
#define _POSIX_PIPE_BUF     512
#define _POSIX_SSIZE_MAX    32767
#define _POSIX_STREAM_MAX   8
#define _POSIX_TZNAME_MAX   3

//
// NAME_MAX recovered from the RE'd psxdll: opendir() allocates a 0x10C-byte DIR
// whose d_name array starts at offset 9, so struct dirent = d_name[NAME_MAX+1]
// with NAME_MAX = 255 -- which also matches the NTFS 255-char component limit.
//
#define NAME_MAX    255

// Runtime limits. PATH_MAX matches the NT MAX_PATH (260); OPEN_MAX is the psxdll
// fd-table size. (Provisional until locked 1:1 against an NT 4.0 source; the
// reskit userland needs them to size path/fd buffers at compile time.)
#define PATH_MAX    260
#define OPEN_MAX    64

//
// TODO: the remaining runtime limits (ARG_MAX, LINK_MAX, MAX_CANON, MAX_INPUT,
// PIPE_BUF, NGROUPS_MAX, ...) still need an authoritative NT 4.0 source to lock 1:1.
//

/* BSD aliases used by the reskit userland. */
#define MAXPATHLEN  PATH_MAX
#define MAXNAMLEN   NAME_MAX
