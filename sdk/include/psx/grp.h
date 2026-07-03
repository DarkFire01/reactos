/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX group database access (IEEE Std 1003.1-1990, s.9.2.1).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

struct group
{
    char  *gr_name;     // the name of the group
    gid_t gr_gid;       // the numerical group ID
    char  **gr_mem;     // null-terminated vector of member-name pointers
};

struct group *getgrgid(gid_t Gid);
struct group *getgrnam(const char *Name);
