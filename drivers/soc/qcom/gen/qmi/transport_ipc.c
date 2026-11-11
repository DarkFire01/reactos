/*
 * PROJECT:     ReactOS Qualcomm QMI framework
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI transport binding over the IPC router \Device\IPC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <qcom/qmiipc.h>
#include <qcom/ipcclient.h>

#define QMI_TAG                     'imQc'
#define QMI_IPC_MAX_MESSAGE         8192

#define NDEBUG
#include <debug.h>

typedef struct _QMI_IPC
{
    QMI_TRANSPORT Base;
    HANDLE Handle;
    IPC_ADDR Self;
    PKTHREAD RxThread;
    volatile LONG Stopping;
    PUCHAR RxBuffer;
} QMI_IPC, *PQMI_IPC;

static
PQMI_IPC
QmiIpcFromTransport(
    _In_ PQMI_TRANSPORT Transport)
{
    return CONTAINING_RECORD(Transport, QMI_IPC, Base);
}

/* The handle is asynchronous so a blocked read never holds off a write, so
 * every call waits on its own event */
static
NTSTATUS
QmiIpcAwait(
    _In_ NTSTATUS Status,
    _In_ HANDLE Event,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    if (Status == STATUS_PENDING)
    {
        ZwWaitForSingleObject(Event, FALSE, NULL);
        Status = IoStatus->Status;
    }
    return Status;
}

/* Destination address prefixes the body on the write path */
static
NTSTATUS
QmiIpcSend(
    _In_ PQMI_TRANSPORT Transport,
    _In_ const QMI_ADDR *Destination,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    PQMI_IPC Ipc = QmiIpcFromTransport(Transport);
    IO_STATUS_BLOCK IoStatus;
    HANDLE Event;
    PUCHAR Buffer;
    NTSTATUS Status;

    if (Length > QMI_IPC_MAX_MESSAGE)
        return STATUS_INVALID_BUFFER_SIZE;

    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    Buffer = ExAllocatePoolZero(NonPagedPool, IPC_ADDR_PREFIX + Length, QMI_TAG);
    if (!Buffer)
    {
        ZwClose(Event);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ((PULONG)Buffer)[0] = Destination->Node;
    ((PULONG)Buffer)[1] = Destination->Port;
    RtlCopyMemory(Buffer + IPC_ADDR_PREFIX, Message, Length);

    Status = ZwWriteFile(Ipc->Handle,
                         Event,
                         NULL,
                         NULL,
                         &IoStatus,
                         Buffer,
                         IPC_ADDR_PREFIX + Length,
                         NULL,
                         NULL);
    Status = QmiIpcAwait(Status, Event, &IoStatus);

    if (Length >= sizeof(QMI_HEADER))
    {
        DPRINT1("[qmi] TX %lu:%lu -> %lu:%lu type %u txn %u msg 0x%04x len %lu -> 0x%lx\n",
                Ipc->Self.Node, Ipc->Self.Port, Destination->Node, Destination->Port,
                ((const QMI_HEADER *)Message)->Type, ((const QMI_HEADER *)Message)->TransactionId,
                ((const QMI_HEADER *)Message)->MessageId, Length, Status);
    }
    else
    {
        DPRINT1("[qmi] TX %lu bytes -> %lu:%lu -> 0x%lx\n", Length, Destination->Node, Destination->Port, Status);
    }

    ExFreePoolWithTag(Buffer, QMI_TAG);
    ZwClose(Event);
    return Status;
}

static
NTSTATUS
QmiIpcNameIoctl(
    _In_ PQMI_IPC Ipc,
    _In_ ULONG Code,
    _In_ ULONG Service,
    _In_ ULONG Instance,
    _Out_opt_ PIPC_ADDR Address)
{
    IO_STATUS_BLOCK IoStatus;
    IPC_NAME Name;
    HANDLE Event;
    NTSTATUS Status;

    Name.Service = Service;
    Name.Instance = Instance;

    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwDeviceIoControlFile(Ipc->Handle,
                                   Event,
                                   NULL,
                                   NULL,
                                   &IoStatus,
                                   Code,
                                   &Name,
                                   sizeof(Name),
                                   Address,
                                   Address ? sizeof(*Address) : 0);
    Status = QmiIpcAwait(Status, Event, &IoStatus);

    ZwClose(Event);
    return Status;
}

/* The router name is only service and instance, the QMI version is not part
 * of it, so it does not travel to the router here */
static
NTSTATUS
QmiIpcRegister(
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG Service,
    _In_ ULONG Version,
    _In_ ULONG Instance)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Version);

    Status = QmiIpcNameIoctl(QmiIpcFromTransport(Transport), IPC_IOCTL_BIND_NAME, Service, Instance, NULL);
    DPRINT1("[qmi] register service 0x%lx instance %lu -> 0x%lx\n", Service, Instance, Status);
    return Status;
}

static
NTSTATUS
QmiIpcUnregister(
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG Service,
    _In_ ULONG Version,
    _In_ ULONG Instance)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Version);

    Status = QmiIpcNameIoctl(QmiIpcFromTransport(Transport), IPC_IOCTL_UNBIND_NAME, Service, Instance, NULL);
    DPRINT1("[qmi] unregister service 0x%lx instance %lu -> 0x%lx\n", Service, Instance, Status);
    return Status;
}

static
NTSTATUS
QmiIpcLookup(
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG Service,
    _In_ ULONG Version,
    _In_ ULONG Instance,
    _Out_ PQMI_ADDR Address)
{
    PQMI_IPC Ipc = QmiIpcFromTransport(Transport);
    IPC_ADDR Found;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Version);

    Status = QmiIpcNameIoctl(Ipc, IPC_IOCTL_LOOKUP_SERVER, Service, Instance, &Found);
    if (NT_SUCCESS(Status))
    {
        Address->Node = Found.Node;
        Address->Port = Found.Port;
        DPRINT1("[qmi] lookup service 0x%lx instance %lu -> %lu:%lu\n", Service, Instance, Found.Node, Found.Port);
    }
    else
    {
        DPRINT1("[qmi] lookup service 0x%lx instance %lu FAILED 0x%lx\n", Service, Instance, Status);
    }
    return Status;
}

static const QMI_TRANSPORT_OPS QmiIpcOps =
{
    QmiIpcSend,
    QmiIpcRegister,
    QmiIpcUnregister,
    QmiIpcLookup
};

/* Blocks in the router read queue and hands every message to the QMI layer */
static
KSTART_ROUTINE QmiIpcRxThread;

static
VOID
NTAPI
QmiIpcRxThread(
    _In_ PVOID Context)
{
    PQMI_IPC Ipc = Context;
    IO_STATUS_BLOCK IoStatus;
    QMI_ADDR Source;
    HANDLE Event;
    NTSTATUS Status;
    ULONG Length;

    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        PsTerminateSystemThread(STATUS_SUCCESS);

    while (InterlockedCompareExchange(&Ipc->Stopping, 0, 0) == 0)
    {
        Status = ZwReadFile(Ipc->Handle,
                            Event,
                            NULL,
                            NULL,
                            &IoStatus,
                            Ipc->RxBuffer,
                            IPC_ADDR_PREFIX + QMI_IPC_MAX_MESSAGE,
                            NULL,
                            NULL);
        Status = QmiIpcAwait(Status, Event, &IoStatus);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("[qmi] RX read ended 0x%lx (stopping %ld)\n", Status, Ipc->Stopping);
            break;
        }

        if (IoStatus.Information < IPC_ADDR_PREFIX)
        {
            DPRINT1("[qmi] RX short read %Iu\n", IoStatus.Information);
            continue;
        }

        Source.Node = ((PULONG)Ipc->RxBuffer)[0];
        Source.Port = ((PULONG)Ipc->RxBuffer)[1];
        Length = (ULONG)IoStatus.Information - IPC_ADDR_PREFIX;

        if (Length >= sizeof(QMI_HEADER))
        {
            const QMI_HEADER *Header = (const QMI_HEADER *)(Ipc->RxBuffer + IPC_ADDR_PREFIX);
            DPRINT1("[qmi] RX %lu:%lu <- %lu:%lu type %u txn %u msg 0x%04x len %lu\n",
                    Ipc->Self.Node, Ipc->Self.Port, Source.Node, Source.Port,
                    Header->Type, Header->TransactionId, Header->MessageId, Length);
        }
        else
        {
            DPRINT1("[qmi] RX %lu bytes <- %lu:%lu\n", Length, Source.Node, Source.Port);
        }

        QmiTransportDeliver(&Ipc->Base, &Source, Ipc->RxBuffer + IPC_ADDR_PREFIX, Length);
    }

    ZwClose(Event);
    DPRINT1("[qmi] RX thread exit\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
QmiIpcCreate(
    _Outptr_ PQMI_TRANSPORT *Transport)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING DeviceName;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES ThreadAttributes;
    HANDLE ThreadHandle;
    PQMI_IPC Ipc;
    NTSTATUS Status;

    *Transport = NULL;

    Ipc = ExAllocatePoolZero(NonPagedPool, sizeof(*Ipc), QMI_TAG);
    if (!Ipc)
        return STATUS_INSUFFICIENT_RESOURCES;

    Ipc->Base.Ops = &QmiIpcOps;
    Ipc->Base.Context = Ipc;

    Ipc->RxBuffer = ExAllocatePoolZero(NonPagedPool, IPC_ADDR_PREFIX + QMI_IPC_MAX_MESSAGE, QMI_TAG);
    if (!Ipc->RxBuffer)
    {
        ExFreePoolWithTag(Ipc, QMI_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitUnicodeString(&DeviceName, IPC_DEVICE_NAME);
    InitializeObjectAttributes(&ObjectAttributes,
                               &DeviceName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    Status = ZwCreateFile(&Ipc->Handle,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          0,
                          FILE_OPEN,
                          0,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[qmi] open %ls FAILED 0x%lx\n", IPC_DEVICE_NAME, Status);
        ExFreePoolWithTag(Ipc->RxBuffer, QMI_TAG);
        ExFreePoolWithTag(Ipc, QMI_TAG);
        return Status;
    }

    /* GET_SELF ignores the input, the name fields are simply unused here */
    Status = QmiIpcNameIoctl(Ipc, IPC_IOCTL_GET_SELF, 0, 0, &Ipc->Self);
    DPRINT1("[qmi] transport open, self %lu:%lu (0x%lx)\n", Ipc->Self.Node, Ipc->Self.Port, Status);

    InitializeObjectAttributes(&ThreadAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  &ThreadAttributes,
                                  NULL,
                                  NULL,
                                  QmiIpcRxThread,
                                  Ipc);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Ipc->Handle);
        ExFreePoolWithTag(Ipc->RxBuffer, QMI_TAG);
        ExFreePoolWithTag(Ipc, QMI_TAG);
        return Status;
    }

    ObReferenceObjectByHandle(ThreadHandle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                              (PVOID *)&Ipc->RxThread, NULL);
    ZwClose(ThreadHandle);

    *Transport = &Ipc->Base;
    return STATUS_SUCCESS;
}

VOID
QmiIpcDestroy(
    _In_ PQMI_TRANSPORT Transport)
{
    PQMI_IPC Ipc = QmiIpcFromTransport(Transport);

    /* Closing the handle unblocks the read so the thread can see the flag */
    InterlockedExchange(&Ipc->Stopping, 1);
    ZwClose(Ipc->Handle);

    KeWaitForSingleObject(Ipc->RxThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Ipc->RxThread);

    ExFreePoolWithTag(Ipc->RxBuffer, QMI_TAG);
    ExFreePoolWithTag(Ipc, QMI_TAG);
}
