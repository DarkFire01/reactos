/*
 * PROJECT:     ReactOS Windows Error Reporting kernel API set
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stubs of the WerLiveKernel entry points drivers import
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * These satisfy the ext-ms-win-ntos-werkernel imports of drivers that would
 * file a live kernel error report. Reporting is not supported, so each one
 * fails, which a driver treats as "no report was made" and carries on.
 */

#include <ntddk.h>

NTSTATUS
NTAPI
WerLiveKernelCreateReport(
    _In_opt_ PVOID Parameter1,
    _In_ ULONG Parameter2,
    _Out_opt_ PVOID *Parameter3)
{
    UNREFERENCED_PARAMETER(Parameter1);
    UNREFERENCED_PARAMETER(Parameter2);

    if (Parameter3 != NULL)
        *Parameter3 = NULL;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WerLiveKernelOpenDumpFile(
    _In_opt_ PVOID Parameter1,
    _Out_opt_ PVOID *Parameter2)
{
    UNREFERENCED_PARAMETER(Parameter1);

    if (Parameter2 != NULL)
        *Parameter2 = NULL;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WerLiveKernelSubmitReport(
    _In_opt_ PVOID Parameter1,
    _In_ ULONG Parameter2,
    _In_opt_ PVOID Parameter3)
{
    UNREFERENCED_PARAMETER(Parameter1);
    UNREFERENCED_PARAMETER(Parameter2);
    UNREFERENCED_PARAMETER(Parameter3);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WerLiveKernelCancelReport(
    _In_opt_ PVOID Parameter1)
{
    UNREFERENCED_PARAMETER(Parameter1);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WerLiveKernelCloseHandle(
    _In_opt_ PVOID Parameter1)
{
    UNREFERENCED_PARAMETER(Parameter1);

    return STATUS_NOT_IMPLEMENTED;
}

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
