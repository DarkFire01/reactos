/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX file control (open/fcntl) flags and structures
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

//
// Flags for open()
//
#define O_RDONLY    0x00000000
#define O_WRONLY    0x00000001
#define O_RDWR      0x00000002
#define O_ACCMODE   0x00000007  // mask for the access modes above
#define O_APPEND    0x00000008
#define O_CREAT     0x00000100
#define O_TRUNC     0x00000200
#define O_EXCL      0x00000400
#define O_NOCTTY    0x00000800
#define O_NONBLOCK  0x00001000

//
// Commands for fcntl()  (IEEE Std 1003.1-1988, s.6.5.2.2)
//
#define F_DUPFD     0
#define F_GETFD     1
#define F_GETLK     2
#define F_SETFD     3
#define F_GETFL     4
#define F_SETFL     5
#define F_SETLK     6
#define F_SETLKW    7

//
// File descriptor flags (argument to F_SETFD)
//
#define FD_CLOEXEC  0x1

#ifndef _FLOCK_DEFINED
#define _FLOCK_DEFINED
struct flock
{
    short l_type;       // F_RDLCK, F_WRLCK, or F_UNLCK
    short l_whence;     // flag for the starting offset
    off_t l_start;      // relative offset in bytes
    off_t l_len;        // size; if 0 then until EOF
    pid_t l_pid;        // pid of the process holding the lock
};
#endif

//
// Values for flock.l_type
//
#define F_RDLCK     1
#define F_UNLCK     2
#define F_WRLCK     3

int open(const char *Path, int OpenFlag, ...);
int creat(const char *Path, mode_t Mode);
int fcntl(int FileDescriptor, int Command, ...);
