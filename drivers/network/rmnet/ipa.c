/*
 * PROJECT:     ReactOS Cellular Data Adapter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client of the Qualcomm IP accelerator, the packet path to the modem
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rmnet.h"

#define NDEBUG
#include <debug.h>

/* {654C90FE-4665-3A98-9D43-8A8759B6C404}, published by qcipa850.sys */
static const GUID RmNetIpaInterfaceGuid =
    { 0x654C90FE, 0x4665, 0x3A98, { 0x9D, 0x43, 0x8A, 0x87, 0x59, 0xB6, 0xC4, 0x04 } };

static HANDLE RmNetIpaHandle;

/* The first packets each way are traced, enough to see where a stalled path stops */
#define RMNET_TRACE_PACKETS         8
static volatile LONG RmNetSendTraced;
static volatile LONG RmNetCompleteTraced;
static volatile LONG RmNetReceiveTraced;
static PETHREAD RmNetIpaThread;
static KEVENT RmNetIpaStop;

static
VOID
NTAPI
RmNetIpaReceive(
    _In_ PVOID Owner,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG Count,
    _In_ ULONG ReceiveFlags)
{
    PRMNET_ADAPTER Adapter = Owner;
    PNET_BUFFER_LIST List;
    PNET_BUFFER Buffer;
    UCHAR Storage[RMNET_MAX_MTU];
    PUCHAR Packet;
    ULONG Length;

    UNREFERENCED_PARAMETER(Count);
    UNREFERENCED_PARAMETER(ReceiveFlags);

    for (List = NetBufferLists; List; List = NET_BUFFER_LIST_NEXT_NBL(List))
    {
        for (Buffer = NET_BUFFER_LIST_FIRST_NB(List); Buffer; Buffer = NET_BUFFER_NEXT_NB(Buffer))
        {
            Length = NET_BUFFER_DATA_LENGTH(Buffer);
            if (!Adapter->Connected || !Length || Length > sizeof(Storage))
            {
                Adapter->DroppedPackets++;
                continue;
            }

            Packet = NdisGetDataBuffer(Buffer, Length, Storage, 1, 0);
            if (Packet && InterlockedIncrement(&RmNetReceiveTraced) <= RMNET_TRACE_PACKETS)
                DPRINT1("rmnet: IPA rx %lu bytes, version %u protocol %u\n", Length, Packet[0] >> 4, Packet[9]);
            if (Packet)
                RmNetIndicatePacket(Adapter, Packet, Length);
        }
    }

    /* The packets were copied out, IPA gets its buffers back at once */
    Adapter->Ipa.Return(NULL, NetBufferLists);
}

static
VOID
NTAPI
RmNetIpaSendComplete(
    _In_ PVOID Owner,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG SendCompleteFlags)
{
    PRMNET_ADAPTER Adapter = Owner;
    PNET_BUFFER_LIST List, Next;
    PMDL Mdl;
    PVOID Data;

    UNREFERENCED_PARAMETER(SendCompleteFlags);

    for (List = NetBufferLists; List; List = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(List);
        NET_BUFFER_LIST_NEXT_NBL(List) = NULL;

        if (InterlockedIncrement(&RmNetCompleteTraced) <= RMNET_TRACE_PACKETS)
            DPRINT1("rmnet: IPA send complete 0x%lx\n", NET_BUFFER_LIST_STATUS(List));

        if (NET_BUFFER_LIST_STATUS(List) != NDIS_STATUS_SUCCESS)
            Adapter->DroppedPackets++;

        Mdl = NET_BUFFER_FIRST_MDL(NET_BUFFER_LIST_FIRST_NB(List));
        Data = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        NdisFreeNetBufferList(List);
        NdisFreeMdl(Mdl);
        if (Data)
            ExFreePoolWithTag(Data, RMNET_TAG);
        InterlockedDecrement(&Adapter->IpaSendsPending);
    }
}

static
VOID
NTAPI
RmNetIpaState(
    _In_ PVOID Owner)
{
    UNREFERENCED_PARAMETER(Owner);
    DPRINT1("rmnet: IPA reports a data path state change\n");
}

BOOLEAN
NTAPI
RmNetIpaSend(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    PNET_BUFFER_LIST List;
    PUCHAR Copy;
    PMDL Mdl;

    if (!Adapter->IpaRegistered)
        return FALSE;

    Copy = ExAllocatePoolWithTag(NonPagedPool, Length, RMNET_TAG);
    if (!Copy)
        return FALSE;

    NdisMoveMemory(Copy, Packet, Length);
    Mdl = NdisAllocateMdl(Adapter->MiniportHandle, Copy, Length);
    if (!Mdl)
    {
        ExFreePoolWithTag(Copy, RMNET_TAG);
        return FALSE;
    }

    List = NdisAllocateNetBufferAndNetBufferList(Adapter->PacketPool, 0, 0, Mdl, 0, Length);
    if (!List)
    {
        NdisFreeMdl(Mdl);
        ExFreePoolWithTag(Copy, RMNET_TAG);
        return FALSE;
    }

    /* IPA reads the client id from the first slot and links its queue through the second */
    NET_BUFFER_LIST_MINIPORT_RESERVED(List)[0] = (PVOID)(ULONG_PTR)Adapter->ClientId;
    NET_BUFFER_LIST_MINIPORT_RESERVED(List)[1] = NULL;
    List->Scratch = NULL;
    InterlockedIncrement(&Adapter->IpaSendsPending);

    if (InterlockedIncrement(&RmNetSendTraced) <= RMNET_TRACE_PACKETS)
    {
        DPRINT1("rmnet: IPA tx %lu bytes protocol %u to client %lu at irql %u\n",
                Length, Packet[9], Adapter->ClientId, KeGetCurrentIrql());
        Adapter->Ipa.Send(List);
        DPRINT1("rmnet: IPA tx returned\n");
        return TRUE;
    }

    Adapter->Ipa.Send(List);
    return TRUE;
}

static
NTSTATUS
RmNetIpaCall(
    _In_ PRMNET_ADAPTER Adapter,
    _In_ PIRP Irp,
    _In_ PKEVENT Event,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    NTSTATUS Status;

    IoGetNextIrpStackLocation(Irp)->FileObject = Adapter->IpaFile;
    Status = IoCallDriver(Adapter->IpaDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus->Status;
    }
    return Status;
}

static
NTSTATUS
RmNetIpaQueryInterface(
    _In_ PRMNET_ADAPTER Adapter)
{
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, Adapter->IpaDevice, NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType = &RmNetIpaInterfaceGuid;
    Stack->Parameters.QueryInterface.Size = sizeof(Adapter->Ipa);
    Stack->Parameters.QueryInterface.Version = RMNET_IPA_INTERFACE_VERSION;
    Stack->Parameters.QueryInterface.Interface = (PINTERFACE)&Adapter->Ipa;
    Stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    return RmNetIpaCall(Adapter, Irp, &Event, &IoStatus);
}

static
NTSTATUS
RmNetIpaRegister(
    _In_ PRMNET_ADAPTER Adapter)
{
    RMNET_IPA_REGISTRATION Registration, Answer;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    NdisZeroMemory(&Registration, sizeof(Registration));
    NdisZeroMemory(&Answer, sizeof(Answer));
    Registration.NdisHandle = Adapter->MiniportHandle;
    Registration.Owner = Adapter;
    Registration.Receive = RmNetIpaReceive;
    Registration.SendComplete = RmNetIpaSendComplete;
    Registration.State = RmNetIpaState;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(RMNET_IPA_IOCTL_REGISTER,
                                        Adapter->IpaDevice,
                                        &Registration,
                                        sizeof(Registration),
                                        &Answer,
                                        sizeof(Answer),
                                        TRUE,
                                        &Event,
                                        &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = RmNetIpaCall(Adapter, Irp, &Event, &IoStatus);
    if (NT_SUCCESS(Status))
        Adapter->ClientId = Answer.ClientId;
    return Status;
}

/* Opens \Device\IPA and keeps the handle, IPA ties the client to that open */
static
NTSTATUS
RmNetIpaOpen(
    _In_ PRMNET_ADAPTER Adapter)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(RMNET_IPA_DEVICE);
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&RmNetIpaHandle,
                          GENERIC_READ | GENERIC_WRITE,
                          &Attributes,
                          &IoStatus,
                          NULL,
                          0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN,
                          0,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(RmNetIpaHandle, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&Adapter->IpaFile, NULL);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(RmNetIpaHandle);
        RmNetIpaHandle = NULL;
        return Status;
    }

    Adapter->IpaDevice = IoGetRelatedDeviceObject(Adapter->IpaFile);
    return STATUS_SUCCESS;
}

static
VOID
RmNetIpaClose(
    _In_ PRMNET_ADAPTER Adapter)
{
    if (Adapter->IpaFile)
    {
        ObDereferenceObject(Adapter->IpaFile);
        Adapter->IpaFile = NULL;
        Adapter->IpaDevice = NULL;
    }
    if (RmNetIpaHandle)
    {
        ZwClose(RmNetIpaHandle);
        RmNetIpaHandle = NULL;
    }
}

static
NTSTATUS
RmNetIpaAttach(
    _In_ PRMNET_ADAPTER Adapter)
{
    NTSTATUS Status;

    Status = RmNetIpaOpen(Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RmNetIpaQueryInterface(Adapter);
    if (!NT_SUCCESS(Status) || !Adapter->Ipa.Send || !Adapter->Ipa.Return)
    {
        RmNetIpaClose(Adapter);
        return NT_SUCCESS(Status) ? STATUS_NOT_SUPPORTED : Status;
    }

    Status = RmNetIpaRegister(Adapter);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("rmnet: IPA refused the client 0x%lx\n", Status);
        if (Adapter->Ipa.Header.InterfaceDereference)
            Adapter->Ipa.Header.InterfaceDereference(Adapter->Ipa.Header.Context);
        RtlZeroMemory(&Adapter->Ipa, sizeof(Adapter->Ipa));
        RmNetIpaClose(Adapter);
        return Status;
    }

    Adapter->IpaRegistered = TRUE;
    DPRINT1("rmnet: IPA client %lu registered, the modem binds the session to mux %lu\n",
            Adapter->ClientId, Adapter->ClientId);

    /* The receive path hands up one IP packet per frame, so IPA must not merge segments */
    RmNetIpaSetCoalescing(Adapter, FALSE);
    return STATUS_SUCCESS;
}

/* IPA starts after its PEP and firmware, the adapter may come first and waits for it */
static
VOID
NTAPI
RmNetIpaAttachThread(
    _In_ PVOID Context)
{
    PRMNET_ADAPTER Adapter = Context;
    LARGE_INTEGER Delay;
    NTSTATUS Status;
    NTSTATUS LastStatus = STATUS_SUCCESS;

    Delay.QuadPart = -(LONGLONG)RMNET_IPA_RETRY_MS * 10000;
    for (;;)
    {
        Status = RmNetIpaAttach(Adapter);
        if (NT_SUCCESS(Status))
            break;

        /* Missing device first, then no client interface until the modem wakes IPA */
        if (Status != LastStatus)
            DPRINT1("rmnet: IPA not ready (0x%lx), retrying every %u ms\n", Status, RMNET_IPA_RETRY_MS);
        LastStatus = Status;
        if (KeWaitForSingleObject(&RmNetIpaStop, Executive, KernelMode, FALSE, &Delay) == STATUS_SUCCESS)
            break;
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID
NTAPI
RmNetIpaConnect(
    _In_ PRMNET_ADAPTER Adapter)
{
    HANDLE Thread;
    NTSTATUS Status;

    KeInitializeEvent(&RmNetIpaStop, NotificationEvent, FALSE);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, RmNetIpaAttachThread, Adapter);
    if (!NT_SUCCESS(Status))
        return;

    ObReferenceObjectByHandle(Thread, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&RmNetIpaThread, NULL);
    ZwClose(Thread);
}

VOID
NTAPI
RmNetIpaSetCoalescing(
    _In_ PRMNET_ADAPTER Adapter,
    _In_ BOOLEAN Enable)
{
    if (!Adapter->IpaRegistered || !Adapter->Ipa.SetCoalescing)
        return;

    Adapter->Ipa.SetCoalescing(Adapter->ClientId, Enable, RMNET_IPA_FAMILY_IPV4);
    Adapter->Ipa.SetCoalescing(Adapter->ClientId, Enable, RMNET_IPA_FAMILY_IPV6);
}

VOID
NTAPI
RmNetIpaDisconnect(
    _In_ PRMNET_ADAPTER Adapter)
{
    LARGE_INTEGER Delay;
    ULONG Waited = 0;

    if (RmNetIpaThread)
    {
        KeSetEvent(&RmNetIpaStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(RmNetIpaThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(RmNetIpaThread);
        RmNetIpaThread = NULL;
    }

    if (!Adapter->IpaRegistered)
        return;

    /* Sends still with IPA complete into this adapter, wait a little for them */
    Delay.QuadPart = -10 * 10000;
    while (Adapter->IpaSendsPending > 0 && Waited++ < 200)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    if (Adapter->Ipa.Deregister)
        Adapter->Ipa.Deregister(Adapter->ClientId);
    if (Adapter->Ipa.Header.InterfaceDereference)
        Adapter->Ipa.Header.InterfaceDereference(Adapter->Ipa.Header.Context);

    Adapter->IpaRegistered = FALSE;
    RtlZeroMemory(&Adapter->Ipa, sizeof(Adapter->Ipa));
    RmNetIpaClose(Adapter);
}
