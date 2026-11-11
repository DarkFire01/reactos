/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMP2P monitor thread watching the modem status bits
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "subsys.h"

#define QCDBG_COMPONENT "qcomsubsys"
#include <qcom/qcomdbg.h>

/* Waits for either the stop request or a change on the entry, TRUE means stop */
static
BOOLEAN
SsWaitEntry(
    _In_ PSS_DEVICE Device,
    _In_ PKEVENT EntryEvent)
{
    PVOID Objects[2];
    NTSTATUS Status;

    Objects[0] = &Device->MonitorStop;
    Objects[1] = EntryEvent;

    Status = KeWaitForMultipleObjects(2,
                                      Objects,
                                      WaitAny,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      NULL,
                                      NULL);
    return Status == STATUS_WAIT_0;
}

/* Rising edges only, in the order the platform driver checks them */
static
VOID
SsHandleBits(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Rising)
{
    QC_LOG("SMP2P rising bits 0x%04lx%s%s%s%s%s", Rising,
           (Rising & SS_SMP2P_ERR_FATAL) ? " ERR_FATAL" : "",
           (Rising & SS_SMP2P_READY) ? " READY" : "",
           (Rising & SS_SMP2P_HANDOVER) ? " HANDOVER" : "",
           (Rising & SS_SMP2P_STOP_ACK) ? " STOP_ACK" : "",
           (Rising & SS_SMP2P_NO_DUMP) ? " NO_DUMP" : "");

    if (Rising & SS_SMP2P_NO_DUMP)
        Device->NoDump = TRUE;

    if (Rising & SS_SMP2P_ERR_FATAL)
        SsPost(Device, SS_EV_FATAL, SS_FAULT_ERR_FATAL);

    if (Rising & SS_SMP2P_READY)
        SsPost(Device, SS_EV_MODEM_READY, 0);

    if (Rising & SS_SMP2P_HANDOVER)
        SsPost(Device, SS_EV_MODEM_HANDOVER, 0);

    if (Rising & SS_SMP2P_STOP_ACK)
        SsPost(Device, SS_EV_STOP_ACK, 0);
}

static
KSTART_ROUTINE SsMonitorThread;

static
VOID
NTAPI
SsMonitorThread(
    _In_ PVOID Context)
{
    PSS_DEVICE Device = Context;
    PKEVENT Event;
    ULONG Value, Previous, Rising;
    LONG Result;

    QC_LOG("SMP2P monitor thread start");

    /* Outbound entry may not be claimable until the modem is up */
    Result = Device->Smp2p.Create(&Device->Outbound, SS_SMP2P_HOST_MODEM, SS_SMP2P_OUT_NAME);
    QC_LOG("SMP2P create '%s' -> %ld", SS_SMP2P_OUT_NAME, Result);
    while (Result == SS_SMP2P_NOT_READY && Device->Outbound)
    {
        if (SsWaitEntry(Device, Device->Smp2p.GetEvent(Device->Outbound)))
            goto Stop;
        Result = Device->Smp2p.Create(&Device->Outbound, SS_SMP2P_HOST_MODEM, SS_SMP2P_OUT_NAME);
        QC_LOG("SMP2P create retry -> %ld", Result);
    }
    if (Result < 0)
        goto Fail;

    Result = Device->Smp2p.Register(&Device->Inbound, SS_SMP2P_HOST_MODEM, SS_SMP2P_IN_NAME);
    QC_LOG("SMP2P register '%s' -> %ld", SS_SMP2P_IN_NAME, Result);
    while (Result == SS_SMP2P_NOT_CREATED && Device->Inbound)
    {
        if (SsWaitEntry(Device, Device->Smp2p.GetEvent(Device->Inbound)))
            goto Stop;
        Result = Device->Smp2p.Register(&Device->Inbound, SS_SMP2P_HOST_MODEM, SS_SMP2P_IN_NAME);
        QC_LOG("SMP2P register retry -> %ld", Result);
    }
    if (Result < 0 || !Device->Inbound)
        goto Fail;

    Event = Device->Smp2p.GetEvent(Device->Inbound);
    Previous = 0;

    for (;;)
    {
        if (SsWaitEntry(Device, Event))
            goto Stop;

        if (Device->Smp2p.Read(Device->Inbound, &Value) < 0)
            goto Fail;

        QC_LOG("SMP2P inbound value 0x%08lx (prev 0x%04lx)", Value, Previous);
        Value &= 0xFFFF;
        Rising = Value & ~Previous;
        Previous = Value;
        SsHandleBits(Device, Rising);
    }

Fail:
    QC_LOG("SMP2P monitor thread FAILED (last result %ld)", Result);
    SsPost(Device, SS_EV_FATAL, SS_FAULT_MONITOR);
    KeWaitForSingleObject(&Device->MonitorStop, Executive, KernelMode, FALSE, NULL);

Stop:
    QC_LOG("SMP2P monitor thread exit");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
SsStartMonitor(
    _Inout_ PSS_DEVICE Device)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE Handle;

    if (Device->Monitor)
        return STATUS_SUCCESS;

    KeClearEvent(&Device->MonitorStop);

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Handle,
                                  THREAD_ALL_ACCESS,
                                  &ObjectAttributes,
                                  NULL,
                                  NULL,
                                  SsMonitorThread,
                                  Device);
    if (!NT_SUCCESS(Status))
        return Status;

    ObReferenceObjectByHandle(Handle,
                              THREAD_ALL_ACCESS,
                              *PsThreadType,
                              KernelMode,
                              (PVOID *)&Device->Monitor,
                              NULL);
    ZwClose(Handle);
    return STATUS_SUCCESS;
}

VOID
SsStopMonitor(
    _Inout_ PSS_DEVICE Device)
{
    if (!Device->Monitor)
        return;

    KeSetEvent(&Device->MonitorStop, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(Device->Monitor, Executive, KernelMode, FALSE, NULL);
    KeClearEvent(&Device->MonitorStop);
    ObDereferenceObject(Device->Monitor);
    Device->Monitor = NULL;

    if (Device->Outbound)
        Device->Smp2p.Write(Device->Outbound, SS_SMP2P_OUT_CLEAR);
}

LONG
SsWriteOutbound(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Value)
{
    LONG Result;

    if (!Device->Outbound)
    {
        QC_LOG("SMP2P outbound write %lu: no entry", Value);
        return -1;
    }

    Result = Device->Smp2p.Write(Device->Outbound, Value);
    QC_LOG("SMP2P outbound write %lu -> %ld", Value, Result);
    return Result;
}
