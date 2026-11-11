/*
 * PROJECT:     ReactOS Qualcomm QMI framework
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service side: register, dispatch requests, answer and indicate
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/qmisvc.h>

#define QMI_TAG                     'imQc'
#define QMI_RESPONSE_MAX            2048

#define NDEBUG
#include <debug.h>

typedef struct _QMI_SERVICE_CLIENT
{
    LIST_ENTRY Link;
    QMI_ADDR Addr;
} QMI_SERVICE_CLIENT, *PQMI_SERVICE_CLIENT;

static
VOID
QmiServiceTrackClient(
    _Inout_ PQMI_SERVICE Service,
    _In_ const QMI_ADDR *Addr)
{
    PQMI_SERVICE_CLIENT Client;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Service->Lock, &OldIrql);
    for (Entry = Service->Clients.Flink; Entry != &Service->Clients; Entry = Entry->Flink)
    {
        Client = CONTAINING_RECORD(Entry, QMI_SERVICE_CLIENT, Link);
        if (Client->Addr.Node == Addr->Node && Client->Addr.Port == Addr->Port)
        {
            KeReleaseSpinLock(&Service->Lock, OldIrql);
            return;
        }
    }
    KeReleaseSpinLock(&Service->Lock, OldIrql);

    Client = ExAllocatePoolZero(NonPagedPool, sizeof(*Client), QMI_TAG);
    if (!Client)
        return;

    Client->Addr = *Addr;
    KeAcquireSpinLock(&Service->Lock, &OldIrql);
    InsertTailList(&Service->Clients, &Client->Link);
    KeReleaseSpinLock(&Service->Lock, OldIrql);
    DPRINT1("[qmi] svc 0x%lx: new client %lu:%lu\n", Service->ServiceId, Addr->Node, Addr->Port);
}

static
VOID
QmiServiceDeliver(
    _In_ PVOID Sink,
    _In_ const QMI_ADDR *Source,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    PQMI_SERVICE Service = Sink;
    const QMI_HEADER *Header = Message;
    QMI_ENCODER Response;
    NTSTATUS Status;
    PVOID Buffer;
    ULONG Final;

    if (!QmiMessageValid(Message, Length))
    {
        DPRINT1("[qmi] svc 0x%lx: malformed message (%lu bytes) from %lu:%lu\n",
                Service->ServiceId, Length, Source->Node, Source->Port);
        return;
    }

    if (Header->Type != QMI_TYPE_REQUEST)
    {
        DPRINT1("[qmi] svc 0x%lx: ignoring type %u msg 0x%04x\n", Service->ServiceId, Header->Type, Header->MessageId);
        return;
    }

    DPRINT1("[qmi] svc 0x%lx: request msg 0x%04x txn %u from %lu:%lu\n",
            Service->ServiceId, Header->MessageId, Header->TransactionId, Source->Node, Source->Port);
    QmiServiceTrackClient(Service, Source);

    Buffer = ExAllocatePoolZero(NonPagedPool, QMI_RESPONSE_MAX, QMI_TAG);
    if (!Buffer)
        return;

    QmiEncodeBegin(&Response, Buffer, QMI_RESPONSE_MAX, QMI_TYPE_RESPONSE,
                   Header->TransactionId, Header->MessageId);

    Status = Service->Handler(Service,
                              Source,
                              Header->MessageId,
                              Message,
                              Length,
                              &Response,
                              Service->HandlerContext);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[qmi] svc 0x%lx: handler for msg 0x%04x FAILED 0x%lx, sending failure result\n",
                Service->ServiceId, Header->MessageId, Status);

        /* Discard whatever the handler added and answer with a failure result */
        QmiEncodeBegin(&Response, Buffer, QMI_RESPONSE_MAX, QMI_TYPE_RESPONSE,
                       Header->TransactionId, Header->MessageId);
        QmiEncodeResult(&Response, QMI_RESULT_FAILURE, (USHORT)(Status & 0xFFFF));
    }

    Final = QmiEncodeEnd(&Response);
    DPRINT1("[qmi] svc 0x%lx: response msg 0x%04x txn %u, %lu bytes\n",
            Service->ServiceId, Header->MessageId, Header->TransactionId, Final);
    Service->Transport->Ops->Send(Service->Transport, Source, Buffer, Final);
    ExFreePoolWithTag(Buffer, QMI_TAG);
}

NTSTATUS
QmiServiceCreate(
    _Out_ PQMI_SERVICE Service,
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG ServiceId,
    _In_ ULONG Version,
    _In_ ULONG Instance,
    _In_ PQMI_REQUEST_HANDLER Handler,
    _In_opt_ PVOID Context)
{
    NTSTATUS Status;

    RtlZeroMemory(Service, sizeof(*Service));
    Service->Transport = Transport;
    Service->ServiceId = ServiceId;
    Service->Version = Version;
    Service->Instance = Instance;
    Service->Handler = Handler;
    Service->HandlerContext = Context;
    KeInitializeSpinLock(&Service->Lock);
    InitializeListHead(&Service->Clients);

    Transport->Sink = Service;
    Transport->Deliver = QmiServiceDeliver;

    Status = Transport->Ops->RegisterService(Transport, ServiceId, Version, Instance);
    DPRINT1("[qmi] service 0x%lx v%lu inst %lu create -> 0x%lx\n", ServiceId, Version, Instance, Status);
    if (!NT_SUCCESS(Status))
    {
        Transport->Sink = NULL;
        Transport->Deliver = NULL;
    }
    return Status;
}

VOID
QmiServiceDestroy(
    _Inout_ PQMI_SERVICE Service)
{
    PQMI_SERVICE_CLIENT Client;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    Service->Transport->Ops->UnregisterService(Service->Transport,
                                               Service->ServiceId,
                                               Service->Version,
                                               Service->Instance);
    Service->Transport->Deliver = NULL;
    Service->Transport->Sink = NULL;

    KeAcquireSpinLock(&Service->Lock, &OldIrql);
    while (!IsListEmpty(&Service->Clients))
    {
        Entry = RemoveHeadList(&Service->Clients);
        Client = CONTAINING_RECORD(Entry, QMI_SERVICE_CLIENT, Link);
        ExFreePoolWithTag(Client, QMI_TAG);
    }
    KeReleaseSpinLock(&Service->Lock, OldIrql);
}

NTSTATUS
QmiServiceSendIndication(
    _In_ PQMI_SERVICE Service,
    _In_ const QMI_ADDR *Client,
    _In_ USHORT MessageId,
    _In_reads_bytes_(BodyLength) const VOID *Body,
    _In_ ULONG BodyLength)
{
    QMI_ENCODER Encoder;
    NTSTATUS Status;
    PVOID Buffer;
    ULONG Final;

    Buffer = ExAllocatePoolZero(NonPagedPool, sizeof(QMI_HEADER) + BodyLength, QMI_TAG);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    QmiEncodeBegin(&Encoder, Buffer, sizeof(QMI_HEADER) + BodyLength, QMI_TYPE_INDICATION, 0, MessageId);
    RtlCopyMemory((PUCHAR)Buffer + sizeof(QMI_HEADER), Body, BodyLength);
    Encoder.Offset += BodyLength;
    Final = QmiEncodeEnd(&Encoder);

    Status = Service->Transport->Ops->Send(Service->Transport, Client, Buffer, Final);
    ExFreePoolWithTag(Buffer, QMI_TAG);
    return Status;
}
