/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX system name structure (IEEE Std 1003.1-1990, s.4.4.1.2).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* 14 == _POSIX_NAME_MAX, spelled out so this header works in BOTH userland
 * header worlds (the psx limits.h defines the macro, newlib's does not --
 * and this file is the psx-SDK fallback on the newlib include path). The
 * field size is ABI: psxdll's uname() writes five 14-byte fields. */
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
