/*
 * PROJECT:     ReactOS Network Programming Interface
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A work queue the network modules hand deferred work to
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntifs.h>

#include <reactos/debug.h>

/* TYPES **********************************************************************/

typedef struct _NETIO_WORK_QUEUE_ITEM
{
    struct _NETIO_WORK_QUEUE_ITEM *Next;
} NETIO_WORK_QUEUE_ITEM, *PNETIO_WORK_QUEUE_ITEM;

typedef VOID
(NTAPI *PNETIO_WORK_QUEUE_ROUTINE)(
    _In_opt_ PVOID Context,
    _In_ PNETIO_WORK_QUEUE_ITEM Item);

typedef struct _NETIO_WORK_QUEUE
{
    KSPIN_LOCK Lock;
    PNETIO_WORK_QUEUE_ROUTINE Routine;
    PNETIO_WORK_QUEUE_ITEM Head;
    PNETIO_WORK_QUEUE_ITEM Tail;
    PIO_WORKITEM WorkItem;
    PVOID Context;
    PDEVICE_OBJECT DeviceObject;
} NETIO_WORK_QUEUE, *PNETIO_WORK_QUEUE;

/*
 * The queue holds items the caller owns, threaded through a link each item
 * carries at its own front, and runs them on one work item that the queue owns
 * for its whole life. A queue that was empty is what schedules that work item,
 * so a run that is already on its way picks up whatever arrives behind it and
 * nothing is scheduled twice.
 */

#define NETIO_WORK_QUEUE_TAG 'QWeN'

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
NetioWorkQueueRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Parameter)
{
    PNETIO_WORK_QUEUE Queue = (PNETIO_WORK_QUEUE)Parameter;
    PNETIO_WORK_QUEUE_ITEM Item;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Queue == NULL)
        return;

    for (;;)
    {
        KeAcquireSpinLock(&Queue->Lock, &OldIrql);

        Item = Queue->Head;
        if (Item == NULL)
        {
            /* Nothing left, so the next insert schedules the work item again */
            Queue->Tail = NULL;
            KeReleaseSpinLock(&Queue->Lock, OldIrql);
            return;
        }

        Queue->Head = Item->Next;
        if (Queue->Head == NULL)
            Queue->Tail = NULL;

        KeReleaseSpinLock(&Queue->Lock, OldIrql);

        Item->Next = NULL;
        Queue->Routine(Queue->Context, Item);
    }
}

/*
 * @implemented
 */
VOID
NTAPI
NetioInitializeWorkQueue(
    _Out_ PNETIO_WORK_QUEUE Queue,
    _In_ PNETIO_WORK_QUEUE_ROUTINE Routine,
    _In_opt_ PVOID Context,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    RtlZeroMemory(Queue, sizeof(*Queue));

    KeInitializeSpinLock(&Queue->Lock);

    Queue->Routine = Routine;
    Queue->Context = Context;
    Queue->DeviceObject = DeviceObject;

    /*
     * The work item is taken once rather than per run, because the queue has
     * to be able to schedule itself when there is no memory left to ask for.
     */
    Queue->WorkItem = ExAllocatePoolZero(NonPagedPoolNx,
                                         IoSizeofWorkItem(),
                                         NETIO_WORK_QUEUE_TAG);
    if (Queue->WorkItem != NULL)
        IoInitializeWorkItem(DeviceObject, Queue->WorkItem);
}

/*
 * @implemented
 */
VOID
NTAPI
NetioInsertWorkQueue(
    _Inout_ PNETIO_WORK_QUEUE Queue,
    _Inout_ PNETIO_WORK_QUEUE_ITEM Item)
{
    BOOLEAN WasEmpty;
    KIRQL OldIrql;

    Item->Next = NULL;

    KeAcquireSpinLock(&Queue->Lock, &OldIrql);

    WasEmpty = (Queue->Tail == NULL);
    if (WasEmpty)
        Queue->Head = Item;
    else
        Queue->Tail->Next = Item;

    Queue->Tail = Item;

    KeReleaseSpinLock(&Queue->Lock, OldIrql);

    /* Only the item that found it empty asks for a run */
    if (WasEmpty && (Queue->WorkItem != NULL))
    {
        IoQueueWorkItem(Queue->WorkItem,
                        NetioWorkQueueRoutine,
                        DelayedWorkQueue,
                        Queue);
    }
}

/*
 * @implemented
 */
VOID
NTAPI
NetioShutdownWorkQueue(
    _Inout_ PNETIO_WORK_QUEUE Queue)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Queue->Lock, &OldIrql);

    /* What is still queued is left alone, the caller owns those items */
    Queue->Head = NULL;
    Queue->Tail = NULL;

    KeReleaseSpinLock(&Queue->Lock, OldIrql);

    if (Queue->WorkItem != NULL)
    {
        IoUninitializeWorkItem(Queue->WorkItem);
        ExFreePoolWithTag(Queue->WorkItem, NETIO_WORK_QUEUE_TAG);
        Queue->WorkItem = NULL;
    }
}

/*
 * The transport layer lets a provider leave any request it does not serve
 * pointing here, so every one of these names is the same refusal.
 *
 * @implemented
 */
NTSTATUS
NTAPI
TlDefaultRequestConnectAndSend(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
TlDefaultRequestMessage(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
TlDefaultRequestQueryDispatch(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
TlDefaultRequestQueryDispatchEndpoint(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
TlDefaultRequestResume(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
