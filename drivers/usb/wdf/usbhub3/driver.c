/*
 * PROJECT:     ReactOS Modern USB Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USBHUB3 Driver Entry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <pch.h>
//define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}
