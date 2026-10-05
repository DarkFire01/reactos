/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Request for the absolute ACPI namespace path of a device
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define IOCTL_UACPINT_QUERY_NAMESPACE_PATH \
    CTL_CODE(FILE_DEVICE_ACPI, 20, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define UACPINT_NAMESPACE_PATH_SIGNATURE    'BqNA'

/* Keep every name segment four characters long, padded with underscores */
#define UACPINT_NAMESPACE_PATH_PADDED       0x00000004

/** Input; the output is the path as a NUL terminated wide string starting with a backslash. */
typedef struct _UACPINT_NAMESPACE_PATH_REQUEST
{
    ULONG Signature;
    ULONG Flags;
} UACPINT_NAMESPACE_PATH_REQUEST, *PUACPINT_NAMESPACE_PATH_REQUEST;

C_ASSERT(sizeof(UACPINT_NAMESPACE_PATH_REQUEST) == 8);
