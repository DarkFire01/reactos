/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX user database access (IEEE Std 1003.1-1990, s.9.2.2).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <sys/types.h>

struct passwd
{
    char  *pw_name;     // user's login name
    uid_t pw_uid;       // user id number
    gid_t pw_gid;       // group id number
    char  *pw_dir;      // home directory
    char  *pw_shell;    // shell
};

struct passwd *getpwuid(uid_t Uid);
struct passwd *getpwnam(const char *Name);
