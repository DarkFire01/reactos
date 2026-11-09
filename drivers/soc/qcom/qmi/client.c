/*
 * PROJECT:     ReactOS Qualcomm QMI framework
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client side: connect, request and response, indications
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/qmisvc.h>

#define QMI_TAG                     'imQc'

#define NDEBUG
#include <debug.h>

typedef struct _QMI_PENDING
{
    LIST_ENTRY Link;
    USHORT Transaction;
    KEVENT Done;
    PVOID Response;
    ULONG Capacity;
    ULONG Length;
    NTSTATUS Status;
} QMI_PENDING, *PQMI_PENDING;

static
VOID
QmiClientDeliver(
    _In_ PVOID Sink,
    _In_ const QMI_ADDR *Source,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    PQMI_CLIENT Client = Sink;
    const QMI_HEADER *Header = Message;
    PQMI_PENDING Pending;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Source);

    if (!QmiMessageValid(Message, Length))
    {
        DPRINT1("[qmi] client: malformed message (%lu bytes)\n", Length);
        return;
    }

    if (Header->Type == QMI_TYPE_INDICATION)
    {
        DPRINT1("[qmi] client: indication msg 0x%04x len %lu\n", Header->MessageId, Length);
        if (Client->IndicationHandler)
            Client->IndicationHandler(Client, Header->MessageId, Message, Length, Client->IndicationContext);
        return;
    }

    if (Header->Type != QMI_TYPE_RESPONSE)
        return;

    KeAcquireSpinLock(&Client->Lock, &OldIrql);
    for (Entry = Client->Pending.Flink; Entry != &Client->Pending; Entry = Entry->Flink)
    {
        Pending = CONTAINING_RECORD(Entry, QMI_PENDING, Link);
        if (Pending->Transaction != Header->TransactionId)
            continue;

        if (Length <= Pending->Capacity)
        {
            RtlCopyMemory(Pending->Response, Message, Length);
            Pending->Length = Length;
            Pending->Status = STATUS_SUCCESS;
        }
        else
        {
            Pending->Status = STATUS_BUFFER_TOO_SMALL;
        }
        /* The waiter owns Pending and may return as soon as it is signaled */
        DPRINT1("[qmi] client: response msg 0x%04x txn %u matched (0x%lx)\n",
                Header->MessageId, Header->TransactionId, Pending->Status);
        RemoveEntryList(&Pending->Link);
        KeSetEvent(&Pending->Done, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&Client->Lock, OldIrql);
        return;
    }
    KeReleaseSpinLock(&Client->Lock, OldIrql);
    DPRINT1("[qmi] client: response txn %u has no waiter\n", Header->TransactionId);
}

NTSTATUS
QmiClientConnect(
    _Out_ PQMI_CLIENT Client,
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG ServiceId,
    _In_ ULONG Version,
    _In_ ULONG Instance,
    _In_opt_ PQMI_INDICATION_HANDLER IndicationHandler,
    _In_opt_ PVOID Context)
{
    NTSTATUS Status;

    RtlZeroMemory(Client, sizeof(*Client));
    Client->Transport = Transport;
    Client->NextTransaction = 1;
    Client->IndicationHandler = IndicationHandler;
    Client->IndicationContext = Context;
    KeInitializeSpinLock(&Client->Lock);
    InitializeListHead(&Client->Pending);

    Status = Transport->Ops->LookupService(Transport, ServiceId, Version, Instance, &Client->Server);
    DPRINT1("[qmi] client connect service 0x%lx inst %lu -> 0x%lx\n", ServiceId, Instance, Status);
    if (!NT_SUCCESS(Status))
        return Status;

    Transport->Sink = Client;
    Transport->Deliver = QmiClientDeliver;
    return STATUS_SUCCESS;
}

VOID
QmiClientDisconnect(
    _Inout_ PQMI_CLIENT Client)
{
    PQMI_PENDING Pending;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    Client->Transport->Deliver = NULL;
    Client->Transport->Sink = NULL;

    /* Wake anything still waiting so callers do not hang on teardown */
    KeAcquireSpinLock(&Client->Lock, &OldIrql);
    while (!IsListEmpty(&Client->Pending))
    {
        Entry = RemoveHeadList(&Client->Pending);
        Pending = CONTAINING_RECORD(Entry, QMI_PENDING, Link);
        Pending->Status = STATUS_CANCELLED;
        KeSetEvent(&Pending->Done, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&Client->Lock, OldIrql);
}

NTSTATUS
QmiClientRequest(
    _In_ PQMI_CLIENT Client,
    _Inout_updates_bytes_(RequestLength) PVOID Request,
    _In_ ULONG RequestLength,
    _Out_writes_bytes_to_(ResponseCapacity, *ResponseLength) PVOID Response,
    _In_ ULONG ResponseCapacity,
    _Out_ PULONG ResponseLength,
    _In_ ULONG TimeoutMs)
{
    PQMI_HEADER Header = Request;
    QMI_PENDING Pending;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    KIRQL OldIrql;

    *ResponseLength = 0;

    if (RequestLength < sizeof(QMI_HEADER))
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Pending, sizeof(Pending));
    KeInitializeEvent(&Pending.Done, NotificationEvent, FALSE);
    Pending.Response = Response;
    Pending.Capacity = ResponseCapacity;
    Pending.Status = STATUS_IO_TIMEOUT;

    KeAcquireSpinLock(&Client->Lock, &OldIrql);
    Pending.Transaction = Client->NextTransaction++;
    if (Client->NextTransaction == 0)
        Client->NextTransaction = 1;
    Header->Type = QMI_TYPE_REQUEST;
    Header->TransactionId = Pending.Transaction;
    InsertTailList(&Client->Pending, &Pending.Link);
    KeReleaseSpinLock(&Client->Lock, OldIrql);

    Status = Client->Transport->Ops->Send(Client->Transport, &Client->Server, Request, RequestLength);
    if (!NT_SUCCESS(Status))
    {
        KeAcquireSpinLock(&Client->Lock, &OldIrql);
        RemoveEntryList(&Pending.Link);
        KeReleaseSpinLock(&Client->Lock, OldIrql);
        return Status;
    }

    DPRINT1("[qmi] client request msg 0x%04x txn %u sent, waiting %lu ms\n",
            Header->MessageId, Pending.Transaction, TimeoutMs);

    Timeout.QuadPart = -(LONGLONG)TimeoutMs * 10 * 1000;
    Status = KeWaitForSingleObject(&Pending.Done, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        DPRINT1("[qmi] client request msg 0x%04x txn %u TIMED OUT\n", Header->MessageId, Pending.Transaction);
        KeAcquireSpinLock(&Client->Lock, &OldIrql);
        /* It may have completed in the race between timeout and the lock */
        if (!KeReadStateEvent(&Pending.Done))
            RemoveEntryList(&Pending.Link);
        KeReleaseSpinLock(&Client->Lock, OldIrql);
        return STATUS_IO_TIMEOUT;
    }

    if (NT_SUCCESS(Pending.Status))
        *ResponseLength = Pending.Length;

    return Pending.Status;
}
