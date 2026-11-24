/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI data block consumers and event delivery
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <wmistr.h>
#include "wmip.h"

#define NDEBUG
#include <debug.h>

#define TAG_WMI_EVENT 'EimW'

/* Events a GUID object holds for user mode stop growing past this, as in Windows */
#define WMIP_EVENT_QUEUE_LIMIT  0x80000
#define WMIP_EVENT_QUEUE_GRAIN  0x4000

/* Receive requests naming few handles keep their object list on the stack */
#define WMIP_LOCAL_OBJECTS      16

#define WMIP_ALIGN8(Value)      (((ULONGLONG)(Value) + 7) & ~(ULONGLONG)7)

C_ASSERT(sizeof(WMIP_IRP_CONTEXT) <= RTL_FIELD_SIZE(IRP, Tail.Overlay.DriverContext));

/* One event IoWMIWriteEvent took over, waiting for the delivery worker */
typedef struct _WMIP_PENDING_EVENT
{
    LIST_ENTRY Link;
    PWNODE_HEADER Wnode;
} WMIP_PENDING_EVENT, *PWMIP_PENDING_EVENT;

/* Serializes the consumer list, the switch requests sent for it and event delivery */
static KMUTEX WmipConsumerMutex;
static LIST_ENTRY WmipConsumerListHead;
static volatile LONG WmipConsumerCount;

/* Guards the pending receive IRP of every GUID object against its cancel routine */
static KSPIN_LOCK WmipIrpLock;

static LIST_ENTRY WmipEventListHead;
static KSPIN_LOCK WmipEventLock;
static volatile LONG WmipEventWorkCount;
static WORK_QUEUE_ITEM WmipEventWorkItem;

static WORKER_THREAD_ROUTINE WmipEventWorker;
static DRIVER_CANCEL WmipNotificationIrpCancel;


/* FUNCTIONS *****************************************************************/

VOID
NTAPI
WmipInitializeConsumers(
    VOID)
{
    KeInitializeMutex(&WmipConsumerMutex, 0);
    InitializeListHead(&WmipConsumerListHead);
    KeInitializeSpinLock(&WmipIrpLock);
    InitializeListHead(&WmipEventListHead);
    KeInitializeSpinLock(&WmipEventLock);
    ExInitializeWorkItem(&WmipEventWorkItem, WmipEventWorker, NULL);
}

VOID
NTAPI
WmipAcquireConsumerLock(
    VOID)
{
    PAGED_CODE();
    KeWaitForSingleObject(&WmipConsumerMutex, Executive, KernelMode, FALSE, NULL);
}

VOID
NTAPI
WmipReleaseConsumerLock(
    VOID)
{
    KeReleaseMutex(&WmipConsumerMutex, FALSE);
}

BOOLEAN
NTAPI
WmipHasAnyConsumer(
    VOID)
{
    return (WmipConsumerCount != 0);
}

BOOLEAN
NTAPI
WmipHasConsumerLocked(
    _In_ LPCGUID Guid,
    _In_ ULONG OpenType)
{
    PWMIP_GUID_OBJECT GuidObject;
    PLIST_ENTRY Entry;

    for (Entry = WmipConsumerListHead.Flink; Entry != &WmipConsumerListHead; Entry = Entry->Flink)
    {
        GuidObject = CONTAINING_RECORD(Entry, WMIP_GUID_OBJECT, ConsumerLink);
        if ((GuidObject->OpenType == OpenType) && IsEqualGUID(&GuidObject->Guid, Guid))
            return TRUE;
    }

    return FALSE;
}

/* Unhooks every GUID object a receive IRP waits on. */
static
VOID
WmipClearIrpObjectsLocked(
    _Inout_ PIRP Irp)
{
    PWMIP_IRP_CONTEXT IrpContext = (PWMIP_IRP_CONTEXT)Irp->Tail.Overlay.DriverContext;
    PWMIP_GUID_OBJECT GuidObject;
    PLIST_ENTRY Entry;

    while (!IsListEmpty(&IrpContext->GuidObjectListHead))
    {
        Entry = RemoveHeadList(&IrpContext->GuidObjectListHead);
        GuidObject = CONTAINING_RECORD(Entry, WMIP_GUID_OBJECT, IrpLink);
        ASSERT(GuidObject->Irp == Irp);
        GuidObject->Irp = NULL;
    }
}

/**
 * @brief Takes the receive IRP waiting on a GUID object away from all the objects it waits on.
 *
 * Returns NULL when there is none or its cancel routine already owns it, the object is unhooked either way.
 */
static
PIRP
WmipTakeObjectIrp(
    _Inout_ PWMIP_GUID_OBJECT GuidObject)
{
    KIRQL OldIrql;
    PIRP Irp;

    KeAcquireSpinLock(&WmipIrpLock, &OldIrql);

    Irp = GuidObject->Irp;
    if (Irp)
    {
        if (IoSetCancelRoutine(Irp, NULL))
        {
            WmipClearIrpObjectsLocked(Irp);
        }
        else
        {
            RemoveEntryList(&GuidObject->IrpLink);
            GuidObject->Irp = NULL;
            Irp = NULL;
        }
    }

    KeReleaseSpinLock(&WmipIrpLock, OldIrql);
    return Irp;
}

static
VOID
NTAPI
WmipNotificationIrpCancel(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&WmipIrpLock, &OldIrql);
    WmipClearIrpObjectsLocked(Irp);
    KeReleaseSpinLock(&WmipIrpLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/**
 * @brief Links a freshly opened GUID object to the consumers of its GUID.
 *
 * The first event consumer of a GUID enables its events, the first query consumer its collection.
 */
NTSTATUS
NTAPI
WmipAttachConsumer(
    _Inout_ PWMIP_GUID_OBJECT GuidObject,
    _In_ ULONG OpenType,
    _In_ KPROCESSOR_MODE AccessMode)
{
    BOOLEAN First;

    PAGED_CODE();

    GuidObject->OpenType = OpenType;
    if (OpenType == WMIP_OPEN_PLAIN)
        return STATUS_SUCCESS;

    if ((OpenType == WMIP_OPEN_QUERYSET) && !WmipIsQuerySetGuid(&GuidObject->Guid))
        return STATUS_WMI_GUID_NOT_FOUND;

    /* Kernel event consumers get a callback instead of a queue */
    GuidObject->KernelConsumer = (AccessMode == KernelMode) && (OpenType == WMIP_OPEN_EVENTS);

    WmipAcquireConsumerLock();

    First = !WmipHasConsumerLocked(&GuidObject->Guid, OpenType);
    InsertTailList(&WmipConsumerListHead, &GuidObject->ConsumerLink);
    GuidObject->Linked = TRUE;
    InterlockedIncrement(&WmipConsumerCount);

    if (First)
    {
        WmipControlGuidLocked(&GuidObject->Guid,
                              (OpenType == WMIP_OPEN_EVENTS) ? IRP_MN_ENABLE_EVENTS : IRP_MN_ENABLE_COLLECTION);
    }

    WmipReleaseConsumerLock();
    return STATUS_SUCCESS;
}

/**
 * @brief Undoes everything a GUID object stands for once it is deleted.
 *
 * A waiting receive IRP fails with STATUS_INVALID_HANDLE, the last consumer of a GUID switches it off.
 */
VOID
NTAPI
WmipDetachConsumer(
    _Inout_ PWMIP_GUID_OBJECT GuidObject)
{
    PIRP Irp;

    PAGED_CODE();

    if (GuidObject->Linked)
    {
        WmipAcquireConsumerLock();

        RemoveEntryList(&GuidObject->ConsumerLink);
        GuidObject->Linked = FALSE;
        InterlockedDecrement(&WmipConsumerCount);

        if (!WmipHasConsumerLocked(&GuidObject->Guid, GuidObject->OpenType))
        {
            WmipControlGuidLocked(&GuidObject->Guid,
                                  (GuidObject->OpenType == WMIP_OPEN_EVENTS) ?
                                      IRP_MN_DISABLE_EVENTS : IRP_MN_DISABLE_COLLECTION);
        }

        WmipReleaseConsumerLock();
    }

    Irp = WmipTakeObjectIrp(GuidObject);
    if (Irp)
    {
        Irp->IoStatus.Status = STATUS_INVALID_HANDLE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    if (GuidObject->Queue.Buffer)
    {
        ExFreePoolWithTag(GuidObject->Queue.Buffer, TAG_WMI_EVENT);
        GuidObject->Queue.Buffer = NULL;
    }
}

/* Appends one event to the queue of a GUID object, counting it lost when the queue is full. */
static
NTSTATUS
WmipQueueEventLocked(
    _Inout_ PWMIP_GUID_OBJECT GuidObject,
    _In_ PWNODE_HEADER Wnode)
{
    PWMIP_EVENT_QUEUE Queue = &GuidObject->Queue;
    PWNODE_HEADER Queued;
    ULONGLONG End;
    ULONG NewSize;
    PUCHAR Buffer;

    End = WMIP_ALIGN8((ULONGLONG)Queue->Used + Wnode->BufferSize);
    if (End > Queue->Size)
    {
        if (End > WMIP_EVENT_QUEUE_LIMIT)
        {
            Queue->EventsLost++;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        NewSize = (ULONG)ALIGN_UP_BY(End, WMIP_EVENT_QUEUE_GRAIN);
        Buffer = ExAllocatePoolWithTag(PagedPool, NewSize, TAG_WMI_EVENT);
        if (!Buffer)
        {
            Queue->EventsLost++;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (Queue->Buffer)
        {
            RtlCopyMemory(Buffer, Queue->Buffer, Queue->Used);
            ExFreePoolWithTag(Queue->Buffer, TAG_WMI_EVENT);
        }

        Queue->Buffer = Buffer;
        Queue->Size = NewSize;
    }

    Queued = (PWNODE_HEADER)(Queue->Buffer + Queue->Used);
    RtlCopyMemory(Queued, Wnode, Wnode->BufferSize);
    Queued->Linkage = 0;

    if (Queue->Used)
        ((PWNODE_HEADER)(Queue->Buffer + Queue->LastOffset))->Linkage = Queue->Used - Queue->LastOffset;

    Queue->LastOffset = Queue->Used;
    Queue->Used = (ULONG)End;

    KeSetEvent(&GuidObject->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

/* Hands an event to a waiting receive IRP, or queues it until the next one comes. */
static
VOID
WmipWriteEventToObjectLocked(
    _Inout_ PWMIP_GUID_OBJECT GuidObject,
    _In_ PWNODE_HEADER Wnode)
{
    PIO_STACK_LOCATION Stack;
    PWNODE_TOO_SMALL TooSmall;
    BOOLEAN Delivered = FALSE;
    PWNODE_HEADER Output;
    PIRP Irp;

    Irp = WmipTakeObjectIrp(GuidObject);
    if (Irp)
    {
        Stack = IoGetCurrentIrpStackLocation(Irp);
        Output = Irp->AssociatedIrp.SystemBuffer;

        if (Wnode->BufferSize <= Stack->Parameters.DeviceIoControl.OutputBufferLength)
        {
            RtlCopyMemory(Output, Wnode, Wnode->BufferSize);
            Output->Linkage = 0;
            Irp->IoStatus.Information = Wnode->BufferSize;
            Delivered = TRUE;
        }
        else
        {
            /* The event stays queued for a retry with a buffer of the size reported */
            TooSmall = (PWNODE_TOO_SMALL)Output;
            RtlZeroMemory(TooSmall, sizeof(*TooSmall));
            TooSmall->WnodeHeader.BufferSize = sizeof(*TooSmall);
            TooSmall->WnodeHeader.Flags = WNODE_FLAG_TOO_SMALL;
            TooSmall->SizeNeeded = Wnode->BufferSize;
            Irp->IoStatus.Information = sizeof(*TooSmall);
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    if (!Delivered)
        WmipQueueEventLocked(GuidObject, Wnode);
}

static
VOID
NTAPI
WmipDeliverEvent(
    _In_ PWNODE_HEADER Wnode)
{
    PWMIP_GUID_OBJECT GuidObject;
    PLIST_ENTRY Entry, Next;

    WmipAcquireConsumerLock();

    for (Entry = WmipConsumerListHead.Flink; Entry != &WmipConsumerListHead; Entry = Next)
    {
        Next = Entry->Flink;
        GuidObject = CONTAINING_RECORD(Entry, WMIP_GUID_OBJECT, ConsumerLink);
        if ((GuidObject->OpenType != WMIP_OPEN_EVENTS) || !IsEqualGUID(&GuidObject->Guid, &Wnode->Guid))
            continue;

        if (GuidObject->KernelConsumer)
        {
            if (GuidObject->NotificationCallback)
                GuidObject->NotificationCallback(Wnode, GuidObject->NotificationContext);
        }
        else
        {
            WmipWriteEventToObjectLocked(GuidObject, Wnode);
        }
    }

    WmipReleaseConsumerLock();
}

static
VOID
NTAPI
WmipEventWorker(
    _In_ PVOID Context)
{
    PWMIP_PENDING_EVENT Pending;
    PWNODE_HEADER Wnode, Prepared;
    PLIST_ENTRY Entry;

    UNREFERENCED_PARAMETER(Context);

    PAGED_CODE();

    do
    {
        Entry = ExInterlockedRemoveHeadList(&WmipEventListHead, &WmipEventLock);
        if (!Entry)
            continue;

        Pending = CONTAINING_RECORD(Entry, WMIP_PENDING_EVENT, Link);
        Wnode = Pending->Wnode;
        ExFreePoolWithTag(Pending, TAG_WMI_EVENT);

        Wnode->Linkage = 0;
        Wnode->ClientContext = 0;

        /* Events nobody listens to are only freed */
        if (WmipHasAnyConsumer())
        {
            Prepared = WmipPrepareEvent(Wnode);
            if (Prepared)
            {
                WmipDeliverEvent(Prepared);
                if (Prepared != Wnode)
                    WmipFreeEventCopy(Prepared);
            }
        }

        ExFreePool(Wnode);
    } while (InterlockedDecrement(&WmipEventWorkCount) != 0);
}

/**
 * @brief Takes over an event a driver fired and delivers it to the consumers of its GUID.
 *
 * Delivery happens on a worker thread, the buffer is freed afterwards unless this fails.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
IoWMIWriteEvent(
    _Inout_ _When_(return == 0, __drv_aliasesMem) PVOID WnodeEventItem)
{
    PWNODE_HEADER Wnode = WnodeEventItem;
    PWMIP_PENDING_EVENT Pending;

    if (!WmipServiceDeviceObject)
        return STATUS_UNSUCCESSFUL;

    if (!Wnode)
        return STATUS_INVALID_PARAMETER;

    /* Event tracing owns these and the caller keeps the buffer */
    if (Wnode->Flags & WNODE_FLAG_TRACED_GUID)
        return STATUS_SUCCESS;

    if (Wnode->BufferSize < sizeof(*Wnode))
        return STATUS_BUFFER_TOO_SMALL;

    Pending = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Pending), TAG_WMI_EVENT);
    if (!Pending)
        return STATUS_INSUFFICIENT_RESOURCES;

    Pending->Wnode = Wnode;
    ExInterlockedInsertTailList(&WmipEventListHead, &Pending->Link, &WmipEventLock);

    if (InterlockedIncrement(&WmipEventWorkCount) == 1)
        ExQueueWorkItem(&WmipEventWorkItem, DelayedWorkQueue);

    return STATUS_SUCCESS;
}

/**
 * @brief Records the routine that receives the events of a data block a driver opened for events.
 */
NTSTATUS
NTAPI
IoWMISetNotificationCallback(
    _Inout_ PVOID Object,
    _In_ WMI_NOTIFICATION_CALLBACK Callback,
    _In_opt_ PVOID Context)
{
    PWMIP_GUID_OBJECT GuidObject = Object;

    PAGED_CODE();

    WmipAcquireConsumerLock();
    GuidObject->NotificationCallback = Callback;
    GuidObject->NotificationContext = Context;
    WmipReleaseConsumerLock();

    return STATUS_SUCCESS;
}

/* Moves every queued event of the given objects into one WNODE chain. */
static
ULONG
WmipDrainQueuesLocked(
    _In_reads_(Count) PWMIP_GUID_OBJECT *Objects,
    _In_ ULONG Count,
    _Out_ PUCHAR Output)
{
    PWMIP_EVENT_QUEUE Queue;
    ULONG Offset = 0, PreviousLast = MAXULONG;
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        Queue = &Objects[Index]->Queue;
        if (!Queue->Used)
            continue;

        RtlCopyMemory(Output + Offset, Queue->Buffer, Queue->Used);
        if (PreviousLast != MAXULONG)
            ((PWNODE_HEADER)(Output + PreviousLast))->Linkage = Offset - PreviousLast;

        PreviousLast = Offset + Queue->LastOffset;
        Offset += Queue->Used;

        ExFreePoolWithTag(Queue->Buffer, TAG_WMI_EVENT);
        Queue->Buffer = NULL;
        Queue->Size = 0;
        Queue->Used = 0;
        Queue->LastOffset = 0;
    }

    return Offset;
}

/**
 * @brief Handles IOCTL_WMI_RECEIVE_NOTIFICATIONS.
 *
 * Queued events of the named GUID objects come back at once, otherwise the IRP waits for the next one.
 * Events that do not fit leave a WNODE_TOO_SMALL with the size needed and stay queued.
 */
NTSTATUS
NTAPI
WmipReceiveNotifications(
    _Inout_ PIRP Irp,
    _In_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWMIP_GUID_OBJECT LocalObjects[WMIP_LOCAL_OBJECTS];
    PWMI_NOTIFICATION_HANDLES Request = Buffer;
    PWMIP_GUID_OBJECT *Objects = LocalObjects;
    PWMIP_IRP_CONTEXT IrpContext;
    PWMIP_GUID_OBJECT GuidObject;
    PWNODE_TOO_SMALL TooSmall;
    KPROCESSOR_MODE PreviousMode;
    ULONG HandleCount, Count = 0, Index, Other;
    ULONGLONG Total = 0;
    BOOLEAN Canceled = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;
    PIRP OldIrp;

    PAGED_CODE();

    if ((InputLength < FIELD_OFFSET(WMI_NOTIFICATION_HANDLES, Handles) + sizeof(ULONG64)) ||
        (*OutputLength < sizeof(WNODE_TOO_SMALL)))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    HandleCount = Request->HandleCount;
    if ((HandleCount == 0) ||
        (HandleCount > (InputLength - FIELD_OFFSET(WMI_NOTIFICATION_HANDLES, Handles)) / sizeof(ULONG64)))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (HandleCount > WMIP_LOCAL_OBJECTS)
    {
        Objects = ExAllocatePoolWithTag(PagedPool, HandleCount * sizeof(*Objects), TAG_WMI_EVENT);
        if (!Objects)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The handles are taken before the output overwrites them */
    PreviousMode = ExGetPreviousMode();
    for (Index = 0; Index < HandleCount; Index++)
    {
        Status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Request->Handles[Index],
                                           WMIGUID_NOTIFICATION,
                                           WmipGuidObjectType,
                                           PreviousMode,
                                           (PVOID *)&GuidObject,
                                           NULL);
        if (!NT_SUCCESS(Status))
            goto Exit;

        for (Other = 0; Other < Count; Other++)
        {
            if (Objects[Other] == GuidObject)
                break;
        }

        if (Other < Count)
            ObDereferenceObject(GuidObject);
        else
            Objects[Count++] = GuidObject;
    }

    WmipAcquireConsumerLock();

    /* A new request replaces whatever request still waits on these objects */
    for (Index = 0; Index < Count; Index++)
    {
        OldIrp = WmipTakeObjectIrp(Objects[Index]);
        if (OldIrp)
        {
            OldIrp->IoStatus.Status = STATUS_INVALID_HANDLE;
            OldIrp->IoStatus.Information = 0;
            IoCompleteRequest(OldIrp, IO_NO_INCREMENT);
        }

        Total += Objects[Index]->Queue.Used;
    }

    if (Total > *OutputLength)
    {
        TooSmall = Buffer;
        RtlZeroMemory(TooSmall, sizeof(*TooSmall));
        TooSmall->WnodeHeader.BufferSize = sizeof(*TooSmall);
        TooSmall->WnodeHeader.Flags = WNODE_FLAG_TOO_SMALL;
        TooSmall->SizeNeeded = (Total > MAXULONG) ? MAXULONG : (ULONG)Total;
        *OutputLength = sizeof(*TooSmall);
    }
    else if (Total)
    {
        *OutputLength = WmipDrainQueuesLocked(Objects, Count, Buffer);
    }
    else
    {
        /* Marked first, an event may complete the IRP as soon as it is hooked up */
        IoMarkIrpPending(Irp);
        IrpContext = (PWMIP_IRP_CONTEXT)Irp->Tail.Overlay.DriverContext;
        InitializeListHead(&IrpContext->GuidObjectListHead);

        KeAcquireSpinLock(&WmipIrpLock, &OldIrql);
        for (Index = 0; Index < Count; Index++)
        {
            Objects[Index]->Irp = Irp;
            InsertTailList(&IrpContext->GuidObjectListHead, &Objects[Index]->IrpLink);
        }

        IoSetCancelRoutine(Irp, WmipNotificationIrpCancel);
        if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
        {
            WmipClearIrpObjectsLocked(Irp);
            Canceled = TRUE;
        }
        KeReleaseSpinLock(&WmipIrpLock, OldIrql);

        if (Canceled)
        {
            Irp->IoStatus.Status = STATUS_CANCELLED;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }

        *OutputLength = 0;
        Status = STATUS_PENDING;
    }

    WmipReleaseConsumerLock();

Exit:
    for (Index = 0; Index < Count; Index++)
        ObDereferenceObject(Objects[Index]);

    if (Objects != LocalObjects)
        ExFreePoolWithTag(Objects, TAG_WMI_EVENT);

    return Status;
}
