/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Restart decision, failure records, broadcasts and peer state
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "subsys.h"

#define NDEBUG
#include <debug.h>

/* Driver wide list so a broadcast reaches the other subsystem FDOs */
extern LIST_ENTRY SsDeviceList;
extern WDFWAITLOCK SsDeviceListLock;

#define SS_MINUTE_UNITS             (60LL * 10 * 1000 * 1000)
#define SS_SYSMON_ACK               "ssr:ack"
#define SS_SYSMON_MAX               50

static PCSTR SsReasonWatchdog = "Q6SW (Modem Software) watch dog bite";
static PCSTR SsReasonMonitor = "AMSS: Failure in the SMP2P monitor thread";
static PCSTR SsReasonState = "AMSS: Fatal error in the state machine";
static PCSTR SsReasonUnknown = "AMSS: Unknown failure reason";
static PCSTR SsInjectMarker = "err_inject_crash.c";

static
VOID
SsFillFailure(
    _Inout_ PSS_DEVICE Device,
    _In_ PCSTR Reason,
    _In_ ULONG Length)
{
    PSS_FAILURE_RECORD Record;

    if (Device->Failure)
    {
        ExFreePoolWithTag(Device->Failure, SS_TAG);
        Device->Failure = NULL;
    }

    Record = ExAllocatePoolZero(NonPagedPool, FIELD_OFFSET(SS_FAILURE_RECORD, Reason) + Length + 1, SS_TAG);
    if (!Record)
        return;

    Record->SubsystemType = 0;
    Record->Length = Length;
    RtlCopyMemory(Record->Reason, Reason, Length);
    Record->Reason[Length] = ANSI_NULL;
    Device->Failure = Record;
    DPRINT1("[subsys] failure record: '%s'\n", Record->Reason);
}

/* Reads the modem's own crash text out of shared memory */
static
PCSTR
SsCrashReason(
    _In_ PSS_DEVICE Device,
    _Out_ PULONG Length)
{
    PCSTR Text;
    ULONG Size;

    *Length = 0;
    if (!Device->Smem.GetAddr)
        return NULL;

    Text = Device->Smem.GetAddr(SS_SMEM_CRASH_REASON, &Size);
    if (!Text || Size == 0)
        return NULL;

    *Length = (ULONG)strnlen(Text, Size);
    return Text;
}

VOID
SsSetFailure(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG_PTR Reason)
{
    PCSTR Text;
    ULONG Length;

    switch (Reason)
    {
        case SS_FAULT_WATCHDOG:
            SsFillFailure(Device, SsReasonWatchdog, (ULONG)strlen(SsReasonWatchdog));
            return;

        case SS_FAULT_MONITOR:
            SsFillFailure(Device, SsReasonMonitor, (ULONG)strlen(SsReasonMonitor));
            return;

        case SS_FAULT_STATE_MACHINE:
            SsFillFailure(Device, SsReasonState, (ULONG)strlen(SsReasonState));
            return;

        default:
            Text = SsCrashReason(Device, &Length);
            if (Text)
                SsFillFailure(Device, Text, Length);
            else
                SsFillFailure(Device, SsReasonUnknown, (ULONG)strlen(SsReasonUnknown));
            return;
    }
}

ULONG
SsDecideRestart(
    _Inout_ PSS_DEVICE Device)
{
    LARGE_INTEGER Now;
    PCSTR Text;
    ULONG Length;

    if (!Device->RestartEnabled)
    {
        DPRINT1("[subsys] restart REFUSED: restart disabled\n");
        return SS_EV_RESTART_REFUSED;
    }

    /* Only injected test crashes are restarted when this mode is on */
    if (Device->CrashDetection)
    {
        Text = SsCrashReason(Device, &Length);
        if (Text && !strstr(Text, SsInjectMarker))
        {
            DPRINT1("[subsys] restart REFUSED: crash detection, not an injected crash\n");
            return SS_EV_RESTART_REFUSED;
        }
    }

    KeQuerySystemTime(&Now);
    if (!Device->WindowValid)
    {
        Device->WindowValid = TRUE;
        Device->WindowStart = Now;
        Device->WindowBase = Device->BootAttempts;
    }

    if (Now.QuadPart >= Device->WindowStart.QuadPart + (LONGLONG)Device->RestartTimerMinutes * SS_MINUTE_UNITS)
    {
        Device->WindowStart = Now;
        Device->WindowBase = Device->BootAttempts;
        DPRINT1("[subsys] restart OK: new %lu min window\n", Device->RestartTimerMinutes);
        return SS_EV_RESTART_OK;
    }

    if (Device->BootAttempts - Device->WindowBase >= Device->MaxRestartCount)
    {
        DPRINT1("[subsys] restart REFUSED: %lu attempts in window, limit %lu\n",
                Device->BootAttempts - Device->WindowBase, Device->MaxRestartCount);
        return SS_EV_RESTART_REFUSED;
    }

    DPRINT1("[subsys] restart OK: %lu of %lu in window\n",
            Device->BootAttempts - Device->WindowBase, Device->MaxRestartCount);
    return SS_EV_RESTART_OK;
}

DECLSPEC_NORETURN
VOID
SsFatal(
    _Inout_ PSS_DEVICE Device)
{
    DPRINT1("[subsys] FATAL: modem cannot be recovered, bugchecking 0x%x\n", SS_BUGCHECK_SOC_SUBSYSTEM);
    if (Device->Failure)
        DPRINT1("[subsys] FATAL reason: '%s'\n", Device->Failure->Reason);

    /* Unlock modem memory first so the crash dump can read it */
    SsHaltAxi(Device);
    SsAssertReset(Device);
    SsClocksOff(Device);
    SsBringUpMba(Device);

    KeBugCheckEx(SS_BUGCHECK_SOC_SUBSYSTEM,
                 (ULONG_PTR)Device->Failure,
                 0,
                 0,
                 (ULONG_PTR)Device->VersionPage);
}

NTSTATUS
SsBroadcast(
    _In_ PSS_DEVICE Device,
    _In_ PCSTR Message)
{
    PLIST_ENTRY Entry;
    PSS_DEVICE Other;

    DPRINT1("[subsys] broadcast '%s'\n", Message);
    WdfWaitLockAcquire(SsDeviceListLock, NULL);
    for (Entry = SsDeviceList.Flink; Entry != &SsDeviceList; Entry = Entry->Flink)
    {
        Other = CONTAINING_RECORD(Entry, SS_DEVICE, Link);
        if (Other != Device)
            SsPost(Other, SS_EV_BROADCAST, (ULONG_PTR)Message);
    }
    WdfWaitLockRelease(SsDeviceListLock);
    return STATUS_SUCCESS;
}

/* Peer coordination rides on the restart notification driver, not yet present */
NTSTATUS
SsPublishState(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG State)
{
    UNREFERENCED_PARAMETER(Device);

    DPRINT1("[subsys] peer state %lu (no restart notifier, not published)\n", State);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
SsSysmonNotify(
    _In_ QCOM_SMDL_HANDLE Port,
    _In_ ULONG Event,
    _In_opt_ PVOID Context)
{
    PSS_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(Port);

    DPRINT1("[subsys] sysmon channel event %lu\n", Event);
    if (Event == QCOM_SMDL_EVENT_OPEN)
        KeSetEvent(&Device->SysmonOpened, IO_NO_INCREMENT, FALSE);
    else if (Event == QCOM_SMDL_EVENT_READ)
        KeSetEvent(&Device->SysmonReadable, IO_NO_INCREMENT, FALSE);
}

NTSTATUS
SsSysmonOpen(
    _Inout_ PSS_DEVICE Device)
{
    if (!Device->Smdl.Open)
        return STATUS_DEVICE_NOT_CONNECTED;

    if (Device->Sysmon)
        return STATUS_SUCCESS;

    KeClearEvent(&Device->SysmonOpened);
    Device->Sysmon = Device->Smdl.Open("sys_mon",
                                       QCOM_SMD_EDGE_APPS_MODEM,
                                       0,
                                       1024,
                                       SsSysmonNotify,
                                       Device);
    DPRINT1("[subsys] sysmon channel open -> %p\n", Device->Sysmon);
    return Device->Sysmon ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

VOID
SsSysmonClose(
    _Inout_ PSS_DEVICE Device)
{
    if (Device->Sysmon)
    {
        Device->Smdl.Close(Device->Sysmon);
        Device->Sysmon = NULL;
    }
}

/* Forwards one SSR string to the modem and waits for its ack */
NTSTATUS
SsSysmonForward(
    _Inout_ PSS_DEVICE Device,
    _In_ PCSTR Message)
{
    LARGE_INTEGER Timeout;
    CHAR Reply[SS_SYSMON_MAX];
    LONG Length;

    WdfWaitLockAcquire(Device->SysmonLock, NULL);

    if (!Device->Sysmon)
    {
        WdfWaitLockRelease(Device->SysmonLock);
        return STATUS_DEVICE_NOT_READY;
    }

    KeClearEvent(&Device->SysmonReadable);
    Length = Device->Smdl.Write(Device->Sysmon, (ULONG)strlen(Message), Message, 0);
    DPRINT1("[subsys] sysmon send '%s' -> %ld\n", Message, Length);

    Timeout.QuadPart = -1LL * 10 * 1000 * 1000;
    KeWaitForSingleObject(&Device->SysmonReadable, Executive, KernelMode, FALSE, &Timeout);

    Length = Device->Smdl.Read(Device->Sysmon, sizeof(Reply) - 1, Reply);
    if (Length > 0)
    {
        Reply[Length] = ANSI_NULL;
        DPRINT1("[subsys] sysmon reply '%s'\n", Reply);
        if (strcmp(Reply, SS_SYSMON_ACK) != 0)
            DPRINT1("[subsys] sysmon reply is not an ack\n");
    }
    else
    {
        DPRINT1("[subsys] sysmon no reply (%ld)\n", Length);
    }

    WdfWaitLockRelease(Device->SysmonLock);
    return STATUS_SUCCESS;
}

/* Rail and clock voting belongs to the platform power engine, absent here */
BOOLEAN
SsPowerVote(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Vote)
{
    UNREFERENCED_PARAMETER(Device);

    DPRINT1("[subsys] power vote %lu (no power engine, treated as granted)\n", Vote);
    return TRUE;
}

VOID
SsCopyVersions(
    _Inout_ PSS_DEVICE Device)
{
    PVOID Source;
    ULONG Size;

    if (!Device->VersionPage || !Device->Smem.GetAddr)
        return;

    Source = Device->Smem.GetAddr(SS_SMEM_IMAGE_VERSIONS, &Size);
    if (Source)
        RtlCopyMemory(Device->VersionPage, Source, min(Size, PAGE_SIZE));
}
