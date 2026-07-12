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
// POSIX directory entry. d_ino is a non-zero identifier for real entries
// (glob/fts key on d_ino != 0); d_type is a DT_* file-type hint (DT_UNKNOWN
// when unavailable, in which case callers fall back to stat()).
//
// NOTE: d_name is kept FIRST (offset 0) on purpose -- it is ABI-compatible with
// the original name-only struct, so binaries built before d_ino/d_type existed
// keep reading the name correctly via the shared psxdll. POSIX does not require
// a particular field order, only that the members exist.
//
struct dirent
{
    char          d_name[NAME_MAX + 1]; // 0x000  NUL-terminated name (255 -> 256)
    unsigned long d_ino;                // 0x100  inode (non-zero for real entries)
    unsigned char d_type;               // 0x104  DT_* file type
};

// d_type values (BSD/glibc convention).
#define DT_UNKNOWN   0
#define DT_FIFO      1
#define DT_CHR       2
#define DT_DIR       4
#define DT_BLK       6
#define DT_REG       8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

// IFTODT/DTTOIF map between stat st_mode and d_type.
#define IFTODT(mode)    (((mode) & 0170000) >> 12)
#define DTTOIF(dirtype) ((dirtype) << 12)

//
// Opaque to callers; the layout is 1:1 with psxdll's DIR:
//   0x00 Directory   - underlying file descriptor
//   0x04 Index       - scan position
//   0x08 RestartScan - BOOLEAN: a rewinddir() is pending
//   Dirent           - the entry readdir() fills (server writes d_ino/d_type/d_name)
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
