/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX system name structure (IEEE Std 1003.1-1990, s.4.4.1.2).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <limits.h>

struct utsname
{
    char sysname[_POSIX_NAME_MAX];
    char nodename[_POSIX_NAME_MAX];
    char release[_POSIX_NAME_MAX];
    char version[_POSIX_NAME_MAX];
    char machine[_POSIX_NAME_MAX];
};

int uname(struct utsname *Name);
