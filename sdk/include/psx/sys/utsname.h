/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX system name structure (IEEE Std 1003.1-1990, s.4.4.1.2).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Equal to _POSIX_NAME_MAX, spelled out because newlib's limits.h lacks it. Part of the psxdll ABI. */
#define _PSX_UTSNAME_LEN 14

struct utsname
{
    char sysname[_PSX_UTSNAME_LEN];
    char nodename[_PSX_UTSNAME_LEN];
    char release[_PSX_UTSNAME_LEN];
    char version[_PSX_UTSNAME_LEN];
    char machine[_PSX_UTSNAME_LEN];
};

int uname(struct utsname *Name);
