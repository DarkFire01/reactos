/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX directory access (IEEE Std 1003.1-1990, s.5.1)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>
#include <limits.h>

struct dirent
{
    char d_name[NAME_MAX + 1];      // NAME_MAX (255) -> 256 bytes
};

//
// Opaque to callers; the layout matches psxdll's DIR (sizeof == 0x10C):
//   0x00 Directory   - underlying file descriptor
//   0x04 Index       - scan position
//   0x08 RestartScan - BOOLEAN: a rewinddir() is pending
//   0x09 Dirent      - the entry returned by readdir()
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
