/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMD channel transports, consumed through the SMD Lite interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define NDEBUG
#include <debug.h>

/* Carries the owning transport on the per channel receive work item */
typedef struct _IPC_RXWORK
{
    PIPC_XPORT Xport;
} IPC_RXWORK, *PIPC_RXWORK;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IPC_RXWORK, IpcGetRxWork);

NTSTATUS
IpcSmdlAcquire(
    _Inout_ PIPC_DEVICE Device)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    DECLARE_CONST_UNICODE_STRING(SmdName, L"\\Device\\SMD");
    NTSTATUS Status;

    Status = WdfIoTargetCreate(Device->WdfDevice, WDF_NO_OBJECT_ATTRIBUTES, &Device->SmdTarget);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams,
                                                &SmdName,
                                                GENERIC_READ | GENERIC_WRITE);

    Status = WdfIoTargetOpen(Device->SmdTarget, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Cannot open \\Device\\SMD 0x%lx\n", Status);
        return Status;
    }

    Status = WdfIoTargetQueryForInterface(Device->SmdTarget,
                                          &GUID_QCOM_SMDL_INTERFACE,
                                          (PINTERFACE)&Device->Smdl,
                                          sizeof(Device->Smdl),
                                          0,
                                          NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SMD Lite interface unavailable 0x%lx\n", Status);
        return Status;
    }

    Device->SmdlReady = TRUE;
    return STATUS_SUCCESS;
}

VOID
IpcSmdlRelease(
    _Inout_ PIPC_DEVICE Device)
{
    Device->SmdlReady = FALSE;

    if (Device->SmdTarget)
    {
        WdfIoTargetClose(Device->SmdTarget);
        WdfObjectDelete(Device->SmdTarget);
        Device->SmdTarget = NULL;
    }
}

static
VOID
IpcXportDrain(
    _In_ PIPC_XPORT Xport)
{
    PQCOM_SMDL_INTERFACE Smdl = &Xport->Device->Smdl;
    PIPC_MBUF Mbuf, Chain;
    LONG Available, Read;

    for (;;)
    {
        Available = Smdl->RxPeek(Xport->Channel);
        if (Available <= 0)
            break;

        Mbuf = IpcMbufAllocate(0, (ULONG)Available);
        if (!Mbuf)
            break;

        Read = Smdl->Read(Xport->Channel, (ULONG)Available, Mbuf->Data);
        if (Read != Available)
        {
            IpcMbufFree(Mbuf);
            break;
        }

        Mbuf->Length = (ULONG)Available;
        Chain = Mbuf;
        IpcReceive(Xport, &Chain);
    }
}

static
VOID
IpcXportSendHello(
    _In_ PIPC_XPORT Xport)
{
    UCHAR Body[IPC_CONTROL_SIZE];

    RtlZeroMemory(Body, sizeof(Body));
    *(PULONG)Body = IPC_TYPE_HELLO;

    if (IpcControlSend(IPC_TYPE_HELLO, Xport, NULL, Body) == IPC_OK)
        Xport->HelloSent = TRUE;
}

static
EVT_WDF_WORKITEM IpcXportRxWork;

static
VOID
IpcXportRxWork(
    _In_ WDFWORKITEM WorkItem)
{
    PIPC_XPORT Xport = IpcGetRxWork(WorkItem)->Xport;

    if (Xport->ChannelOpen && !Xport->HelloSent)
        IpcXportSendHello(Xport);

    IpcXportDrain(Xport);
}

static
VOID
NTAPI
IpcSmdlNotify(
    _In_ QCOM_SMDL_HANDLE Channel,
    _In_ ULONG Event,
    _In_opt_ PVOID Context)
{
    PIPC_XPORT Xport = Context;

    UNREFERENCED_PARAMETER(Channel);

    if (!Xport)
        return;

    switch (Event)
    {
        case QCOM_SMDL_EVENT_OPEN:
            Xport->ChannelOpen = TRUE;
            WdfWorkItemEnqueue(Xport->RxWork);
            break;

        case QCOM_SMDL_EVENT_READ:
        case QCOM_SMDL_EVENT_READ_PARTIAL:
            WdfWorkItemEnqueue(Xport->RxWork);
            break;

        case QCOM_SMDL_EVENT_REMOTE_CLOSE:
        case QCOM_SMDL_EVENT_CLOSE_COMPLETE:
            Xport->ChannelOpen = FALSE;
            Xport->Up = FALSE;
            Xport->HelloSent = FALSE;
            break;

        default:
            break;
    }
}

ULONG
IpcXportSend(
    _In_ PIPC_XPORT Xport,
    _Inout_ PIPC_MBUF *Chain)
{
    PQCOM_SMDL_INTERFACE Smdl = &Xport->Device->Smdl;
    PIPC_MBUF Mbuf;
    PUCHAR Buffer, Cursor;
    ULONG Total;
    LONG Written;
    ULONG Status = IPC_ENODEV;

    Total = IpcMbufChainLength(*Chain);
    if (Total == 0 || !Xport->ChannelOpen)
    {
        IpcMbufFreeChain(Chain);
        return IPC_ENODEV;
    }

    Buffer = ExAllocatePoolWithTag(IPC_POOL, Total, IPC_TAG);
    if (!Buffer)
    {
        IpcMbufFreeChain(Chain);
        return IPC_ENOMEM;
    }

    Cursor = Buffer;
    for (Mbuf = *Chain; Mbuf; Mbuf = Mbuf->Next)
    {
        RtlCopyMemory(Cursor, Mbuf->Data, Mbuf->Length);
        Cursor += Mbuf->Length;
    }

    Written = Smdl->Write(Xport->Channel, Total, Buffer, 0);
    if (Written == (LONG)Total)
        Status = IPC_OK;

    ExFreePoolWithTag(Buffer, IPC_TAG);
    IpcMbufFreeChain(Chain);
    return Status;
}

NTSTATUS
IpcXportStartAll(
    _Inout_ PIPC_DEVICE Device)
{
    WDF_WORKITEM_CONFIG WorkConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PIPC_XPORT Xport;
    NTSTATUS Status;
    ULONG i;

    for (i = 0; i < Device->XportCount; i++)
    {
        Xport = &Device->Xports[i];
        Xport->Device = Device;
        Xport->Remote.Node = IPC_NODE_BROADCAST;
        Xport->Remote.Port = IPC_PORT_CONTROL;
        Xport->Up = FALSE;
        Xport->HelloSent = FALSE;
        Xport->ChannelOpen = FALSE;

        WDF_WORKITEM_CONFIG_INIT(&WorkConfig, IpcXportRxWork);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, IPC_RXWORK);
        Attributes.ParentObject = Device->WdfDevice;

        Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &Xport->RxWork);
        if (!NT_SUCCESS(Status))
            return Status;

        IpcGetRxWork(Xport->RxWork)->Xport = Xport;

        Xport->Channel = Device->Smdl.Open(Xport->Name,
                                           Xport->Edge,
                                           Xport->Flags,
                                           Xport->FifoSize,
                                           IpcSmdlNotify,
                                           Xport);
        if (!Xport->Channel)
            DPRINT1("Transport %s on edge %lu did not open\n", Xport->Name, Xport->Edge);
    }

    return STATUS_SUCCESS;
}

VOID
IpcXportStopAll(
    _Inout_ PIPC_DEVICE Device)
{
    PIPC_XPORT Xport;
    ULONG i;

    for (i = 0; i < Device->XportCount; i++)
    {
        Xport = &Device->Xports[i];

        if (Xport->Channel && Device->SmdlReady)
        {
            Device->Smdl.Close(Xport->Channel);
            Xport->Channel = NULL;
        }

        if (Xport->RxWork)
        {
            WdfWorkItemFlush(Xport->RxWork);
            WdfObjectDelete(Xport->RxWork);
            Xport->RxWork = NULL;
        }

        IpcPortRemoveByXport(Xport);
    }
}
