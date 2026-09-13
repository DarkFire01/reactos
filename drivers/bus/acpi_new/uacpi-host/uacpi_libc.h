/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     uACPI libc overrides, pointed at the kernel's own routines
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _UACPI_NT_UACPI_LIBC_H
#define _UACPI_NT_UACPI_LIBC_H

#include <sdkddkver.h>
#include <string.h>

#define uacpi_memcpy    memcpy
#define uacpi_memmove   memmove
#define uacpi_memset    memset
#define uacpi_memcmp    memcmp
#define uacpi_strlen    strlen
#define uacpi_strcmp    strcmp

// ntoskrnl does not export strnlen before Windows 7.
#if (NTDDI_VERSION >= NTDDI_WIN7)
#define uacpi_strnlen   strnlen
#endif

#endif /* _UACPI_NT_UACPI_LIBC_H */
