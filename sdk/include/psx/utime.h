/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX file-time update (IEEE Std 1003.1-1990, s.5.6.6.2).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

struct utimbuf
{
    time_t actime;      // access time
    time_t modtime;     // modification time
};

int utime(const char *Path, const struct utimbuf *Times);
