/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX file status (IEEE Std 1003.1-1990, s.5.6.1).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

struct stat
{
    mode_t  st_mode;        // 0x00
    ino_t   st_ino;         // 0x04
    dev_t   st_dev;         // 0x08
    nlink_t st_nlink;       // 0x0C
    uid_t   st_uid;         // 0x10
    gid_t   st_gid;         // 0x14
    off_t   st_size;        // 0x18
    time_t  st_atime;       // 0x1C
    time_t  st_mtime;       // 0x20
    time_t  st_ctime;       // 0x24  (total size 0x28 = 40 bytes)
};

//
// Type bits for st_mode (note the NT-specific S_IFMT mask 0770000, octal).
//
#define S_IFMT      0770000

#define S_IFIFO     0010000
#define S_IFCHR     0020000
#define S_IFDIR     0040000
#define S_IFBLK     0060000
#define S_IFREG     0100000

//
// Set-id bits for st_mode
//
#define S_ISUID     0004000
#define S_ISGID     0002000

//
// Protection bits for st_mode
//
#define _S_PROT     0000777

#define S_IRWXU     0000700
#define S_IRUSR     0000400
#define S_IWUSR     0000200
#define S_IXUSR     0000100

#define S_IRWXG     0000070
#define S_IRGRP     0000040
#define S_IWGRP     0000020
#define S_IXGRP     0000010

#define S_IRWXO     0000007
#define S_IROTH     0000004
#define S_IWOTH     0000002
#define S_IXOTH     0000001

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

mode_t umask(mode_t Mask);
int mkdir(const char *Path, mode_t Mode);
int mkfifo(const char *Path, mode_t Mode);
int stat(const char *Path, struct stat *Buffer);
int fstat(int FileDescriptor, struct stat *Buffer);
int chmod(const char *Path, mode_t Mode);
