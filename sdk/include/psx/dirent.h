/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX directory access (IEEE Std 1003.1-1990, s.5.1)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>
#include <limits.h>

//
// d_name stays at offset 0 so binaries built against the name-only layout keep working.
// d_ino is non-zero for valid entries; d_type is DT_UNKNOWN when the type is not known.
//
struct dirent
{
    char          d_name[NAME_MAX + 1]; // 0x000  NUL-terminated name (256 bytes)
    unsigned long d_ino;                // 0x100  inode (non-zero for valid entries)
    unsigned char d_type;               // 0x104  DT_* file type
};

// d_type values (BSD/glibc convention)
#define DT_UNKNOWN   0
#define DT_FIFO      1
#define DT_CHR       2
#define DT_DIR       4
#define DT_BLK       6
#define DT_REG       8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

// Convert between stat st_mode and d_type
#define IFTODT(mode)    (((mode) & 0170000) >> 12)
#define DTTOIF(dirtype) ((dirtype) << 12)

//
// Opaque to callers. Layout is shared with psxdll:
//   0x00 Directory   underlying file descriptor
//   0x04 Index       scan position
//   0x08 RestartScan nonzero when a rewinddir() is pending
//   Dirent           entry filled by readdir()
//
typedef struct _DIR
{
    int           Directory;
    unsigned long Index;
    char          RestartScan;
    struct dirent Dirent;
} DIR;

DIR *opendir(const char *Path);
struct dirent *readdir(DIR *Directory);
void rewinddir(DIR *Directory);
int closedir(DIR *Directory);
