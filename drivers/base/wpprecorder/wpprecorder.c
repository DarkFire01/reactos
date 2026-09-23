/*
 * PROJECT:     ReactOS WPP trace recorder
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stubs of the WPP recorder entry points drivers import
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A WPP instrumented driver keeps a circular trace buffer here so that a crash dump
 * carries the driver's own log. Nothing collects those traces, so every log is
 * reported as unavailable and the recording calls do nothing, which leaves the
 * driver tracing to no one rather than failing to load.
 */

#include <ntddk.h>

NTSTATUS
NTAPI
DllInitialize(
    _In_opt_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WppAutoLogStart(
    _In_opt_ PVOID DriverObject,
    _In_opt_ PVOID RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}

VOID
NTAPI
WppAutoLogStop(
    _In_opt_ PVOID DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

VOID
NTAPI
WppAutoLogTrace(
    _In_opt_ PVOID Log,
    _In_ ULONG Level,
    _In_opt_ PVOID Message)
{
    UNREFERENCED_PARAMETER(Log);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Message);
}

VOID
NTAPI
imp_WppRecorderConfigure(
    _In_opt_ PVOID Params)
{
    UNREFERENCED_PARAMETER(Params);
}

VOID
NTAPI
imp_WppRecorderDumpLiveDriverData(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    UNREFERENCED_PARAMETER(Param1);
    UNREFERENCED_PARAMETER(Param2);
    UNREFERENCED_PARAMETER(Param3);
}

PVOID
NTAPI
imp_WppRecorderGetCounterHandle(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    UNREFERENCED_PARAMETER(Param1);
    UNREFERENCED_PARAMETER(Param2);

    return NULL;
}

PVOID
NTAPI
imp_WppRecorderGetTriageInfo(VOID)
{
    return NULL;
}

BOOLEAN
NTAPI
imp_WppRecorderIsDefaultLogAvailable(VOID)
{
    return FALSE;
}

VOID
NTAPI
imp_WppRecorderLinkCounters(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    UNREFERENCED_PARAMETER(Param1);
    UNREFERENCED_PARAMETER(Param2);
}

NTSTATUS
NTAPI
imp_WppRecorderLogCreate(
    _In_opt_ PVOID CreateParams,
    _Out_opt_ PVOID *Log)
{
    UNREFERENCED_PARAMETER(CreateParams);

    if (Log != NULL)
        *Log = NULL;

    return STATUS_NOT_SUPPORTED;
}

VOID
NTAPI
imp_WppRecorderLogDelete(
    _In_opt_ PVOID Log)
{
    UNREFERENCED_PARAMETER(Log);
}

VOID
NTAPI
imp_WppRecorderLogDumpLiveData(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    UNREFERENCED_PARAMETER(Param1);
    UNREFERENCED_PARAMETER(Param2);
    UNREFERENCED_PARAMETER(Param3);
}

PVOID
NTAPI
imp_WppRecorderLogGetDefault(VOID)
{
    return NULL;
}

VOID
NTAPI
imp_WppRecorderLogSetIdentifier(
    _In_opt_ PVOID Log,
    _In_opt_ PCSTR Identifier)
{
    UNREFERENCED_PARAMETER(Log);
    UNREFERENCED_PARAMETER(Identifier);
}

VOID
NTAPI
imp_WppRecorderReplay(
    _In_opt_ PVOID Log,
    _In_ ULONG Param2,
    _In_ ULONG Param3)
{
    UNREFERENCED_PARAMETER(Log);
    UNREFERENCED_PARAMETER(Param2);
    UNREFERENCED_PARAMETER(Param3);
}

/* EOF */
