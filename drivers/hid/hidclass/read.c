/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Input report reads, routing and polled devices
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE HidClassReadCompletion;
static IO_COMPLETION_ROUTINE HidClassPolledReadCompletion;
static IO_COMPLETION_ROUTINE HidClassTimedPolledReadCompletion;
static KDEFERRED_ROUTINE HidClassBackoffDpc;
static KDEFERRED_ROUTINE HidClassPollDpc;
static DRIVER_CANCEL HidClassCancelRead;
static DRIVER_CANCEL HidClassCancelPolledRead;
static IO_WORKITEM_ROUTINE HidClassCompleteReadWorker;

static
NTSTATUS
NTAPI
HidClassSubmitRead(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PINGPONG PingPong,
    _Out_ PBOOLEAN Sent);

/**
 * @brief
 * Largest input report among the report IDs that belong to a collection.
 */
static
ULONG
NTAPI
HidClassComputeMaxReportLength(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Index;

    Fdo->MaxReportLength = 0;
    for (Index = 0; Index < Fdo->DeviceDesc.ReportIDsLength; Index++)
    {
        PHIDP_REPORT_IDS ReportId = &Fdo->DeviceDesc.ReportIDs[Index];

        if (!HidClassFindCollection(Fdo, ReportId->CollectionNumber))
            continue;

        if (ReportId->InputLength > Fdo->MaxReportLength)
            Fdo->MaxReportLength = ReportId->InputLength;
    }

    if (Fdo->MaxReportLength == 0)
        DPRINT1("HIDCLASS: no collection has an input report\n");

    return Fdo->MaxReportLength;
}

static
ULONG
NTAPI
HidClassMaxCollectionInputLength(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Length = 0;
    ULONG Index;

    for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
        Length = max(Length, Fdo->DeviceDesc.CollectionDesc[Index].InputLength);

    return Length;
}

static
VOID
NTAPI
HidClassReleasePingPongs(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Index;

    if (!Fdo->PingPongs)
        return;

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        PHIDCLASS_PINGPONG PingPong = &Fdo->PingPongs[Index];

        if (PingPong->Irp)
            IoFreeIrp(PingPong->Irp);

        if (PingPong->Buffer)
            ExFreePoolWithTag(PingPong->Buffer, HIDCLASS_TAG);

        if (PingPong->Report)
            ExFreePoolWithTag(PingPong->Report, HIDCLASS_TAG);
    }

    ExFreePoolWithTag(Fdo->PingPongs, HIDCLASS_TAG);
    Fdo->PingPongs = NULL;
}

static
NTSTATUS
NTAPI
HidClassBuildPingPongs(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG ReportLength;
    ULONG Index;

    Fdo->PingPongs = ExAllocatePoolZero(NonPagedPool,
                                        Fdo->PingPongCount * sizeof(*Fdo->PingPongs),
                                        HIDCLASS_TAG);
    if (!Fdo->PingPongs)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Room for a report ID byte in front of the longest collection report */
    ReportLength = max(HidClassMaxCollectionInputLength(Fdo), Fdo->MaxReportLength + 1);

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        PHIDCLASS_PINGPONG PingPong = &Fdo->PingPongs[Index];

        PingPong->Fdo = Fdo;
        PingPong->State = HidClassPingPongIdle;
        PingPong->BackoffDelay.QuadPart = HIDCLASS_BACKOFF_STEP;
        KeInitializeTimer(&PingPong->BackoffTimer);
        KeInitializeDpc(&PingPong->BackoffDpc, HidClassBackoffDpc, PingPong);
        KeInitializeEvent(&PingPong->SubmitDone, NotificationEvent, TRUE);
        KeInitializeEvent(&PingPong->ReadDone, NotificationEvent, TRUE);

        PingPong->Buffer = ExAllocatePoolZero(NonPagedPool, Fdo->MaxReportLength, HIDCLASS_TAG);
        PingPong->Report = ExAllocatePoolZero(NonPagedPool, ReportLength, HIDCLASS_TAG);
        PingPong->Irp = IoAllocateIrp(Fdo->DeviceObject->StackSize - 1, FALSE);
        if (!PingPong->Buffer || !PingPong->Report || !PingPong->Irp)
        {
            HidClassReleasePingPongs(Fdo);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Allocates the reads kept outstanding on an interrupt driven device. Two
 * are used, or one when memory is short.
 */
NTSTATUS
NTAPI
HidClassAllocatePingPongs(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Count;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fdo->DriverExtension->DevicesArePolled)
    {
        Fdo->PingPongCount = 0;
        Fdo->PingPongs = NULL;
        return STATUS_SUCCESS;
    }

    if (HidClassComputeMaxReportLength(Fdo) == 0)
        return STATUS_SUCCESS;

    for (Count = HIDCLASS_PINGPONG_COUNT; Count > 0; Count >>= 1)
    {
        Fdo->PingPongCount = Count;
        Status = HidClassBuildPingPongs(Fdo);
        if (NT_SUCCESS(Status))
            break;
    }

    if (!NT_SUCCESS(Status))
    {
        Fdo->PingPongCount = 0;
        return Status;
    }

    InterlockedExchange(&Fdo->ReadsState, HidClassReadsStopped);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Cancels every outstanding read and waits until none of them runs. The reads
 * can be started again afterwards.
 */
VOID
NTAPI
HidClassStopReads(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Index;

    if (!Fdo->PingPongs)
        return;

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        PHIDCLASS_PINGPONG PingPong = &Fdo->PingPongs[Index];

        InterlockedIncrement(&PingPong->CancelCount);

        /* A read waiting for its backoff will not be sent again */
        if (KeCancelTimer(&PingPong->BackoffTimer))
        {
            KeSetEvent(&PingPong->SubmitDone, IO_NO_INCREMENT, FALSE);
            KeSetEvent(&PingPong->ReadDone, IO_NO_INCREMENT, FALSE);
        }
    }

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        PHIDCLASS_PINGPONG PingPong = &Fdo->PingPongs[Index];

        KeWaitForSingleObject(&PingPong->SubmitDone, Executive, KernelMode, FALSE, NULL);
        IoCancelIrp(PingPong->Irp);
    }

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        PHIDCLASS_PINGPONG PingPong = &Fdo->PingPongs[Index];

        KeWaitForSingleObject(&PingPong->ReadDone, Executive, KernelMode, FALSE, NULL);
        InterlockedDecrement(&PingPong->CancelCount);
    }

    InterlockedExchange(&Fdo->ReadsState, HidClassReadsStopped);
}

VOID
NTAPI
HidClassFreePingPongs(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    InterlockedExchange(&Fdo->ReadsState, HidClassReadsNone);

    if (!Fdo->PingPongs)
        return;

    HidClassStopReads(Fdo);
    InterlockedExchange(&Fdo->ReadsState, HidClassReadsNone);
    HidClassReleasePingPongs(Fdo);
    Fdo->PingPongCount = 0;
}

/**
 * @brief
 * Sends every read that is not outstanding yet. Does nothing while the reads
 * already run or before they were allocated.
 */
NTSTATUS
NTAPI
HidClassStartReads(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN Sent;
    ULONG Index;

    if (Fdo->NoInputCollections)
        return STATUS_SUCCESS;

    if (Fdo->DevicePowerState != PowerDeviceD0)
        return STATUS_INVALID_DEVICE_STATE;

    if (InterlockedCompareExchange(&Fdo->ReadsState, HidClassReadsRunning, HidClassReadsStopped) != HidClassReadsStopped)
        return STATUS_SUCCESS;

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        PHIDCLASS_PINGPONG PingPong = &Fdo->PingPongs[Index];

        if (!KeReadStateEvent(&PingPong->ReadDone))
            continue;

        PingPong->State = HidClassPingPongPending;
        KeResetEvent(&PingPong->ReadDone);

        Status = HidClassSubmitRead(Fdo, PingPong, &Sent);
        if (!NT_SUCCESS(Status))
        {
            /* A read that went down retries on its own through the backoff timer */
            if (!Sent)
                break;
            Status = STATUS_SUCCESS;
        }
    }

    return (Status == STATUS_PENDING) ? STATUS_SUCCESS : Status;
}

/**
 * @brief
 * Sends one read to the minidriver. A read the minidriver completes inside
 * the call is sent again from here instead of from the completion routine.
 */
static
NTSTATUS
NTAPI
HidClassSubmitRead(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PINGPONG PingPong,
    _Out_ PBOOLEAN Sent)
{
    PIO_STACK_LOCATION IoStack;
    PIRP Irp = PingPong->Irp;
    NTSTATUS Status = STATUS_SUCCESS;

    *Sent = FALSE;

    while (NT_SUCCESS(Status))
    {
        HidClassResetIdleTimer(Fdo);

        InterlockedExchange(&PingPong->State, HidClassPingPongSubmitting);

        IoReuseIrp(Irp, STATUS_NOT_SUPPORTED);
        Irp->UserBuffer = PingPong->Buffer;
        IoStack = IoGetNextIrpStackLocation(Irp);
        IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_READ_REPORT;
        IoStack->Parameters.DeviceIoControl.OutputBufferLength = Fdo->MaxReportLength;
        IoStack->Parameters.DeviceIoControl.InputBufferLength = 0;
        IoSetCompletionRoutine(Irp, HidClassReadCompletion, Fdo, TRUE, TRUE, TRUE);

        KeResetEvent(&PingPong->SubmitDone);
        if (PingPong->CancelCount)
        {
            KeSetEvent(&PingPong->SubmitDone, IO_NO_INCREMENT, FALSE);
            KeSetEvent(&PingPong->ReadDone, IO_NO_INCREMENT, FALSE);
            return STATUS_CANCELLED;
        }

        Status = HidClassCallMinidriver(Fdo, Irp);
        KeSetEvent(&PingPong->SubmitDone, IO_NO_INCREMENT, FALSE);
        *Sent = TRUE;

        if (InterlockedExchange(&PingPong->State, HidClassPingPongPending) != HidClassPingPongCompletedInline)
            return Status;

        Status = Irp->IoStatus.Status;
    }

    if (PingPong->CancelCount)
    {
        KeSetEvent(&PingPong->ReadDone, IO_NO_INCREMENT, FALSE);
        return Status;
    }

    DPRINT("HIDCLASS: read failed %lx, retrying after backoff\n", Status);
    KeSetTimer(&PingPong->BackoffTimer, PingPong->BackoffDelay, &PingPong->BackoffDpc);
    return Status;
}

static
VOID
NTAPI
HidClassBackoffDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PHIDCLASS_PINGPONG PingPong = DeferredContext;
    BOOLEAN Sent;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* Each failure in a row waits one second longer, up to five */
    if (PingPong->BackoffDelay.QuadPart > HIDCLASS_BACKOFF_LIMIT)
        PingPong->BackoffDelay.QuadPart += HIDCLASS_BACKOFF_STEP;

    HidClassSubmitRead(PingPong->Fdo, PingPong, &Sent);
}

static
VOID
NTAPI
HidClassQueueReportLocked(
    _In_ PLIST_ENTRY ListHead,
    _Inout_ PULONG Count,
    _In_ ULONG Limit,
    _In_reads_bytes_(Length) PUCHAR Report,
    _In_ ULONG Length)
{
    PHIDCLASS_REPORT Entry;
    PLIST_ENTRY Oldest = NULL;

    Entry = ExAllocatePoolWithTag(NonPagedPool, FIELD_OFFSET(HIDCLASS_REPORT, Data[Length]), HIDCLASS_TAG);
    if (!Entry)
        return;

    Entry->Length = Length;
    RtlCopyMemory(Entry->Data, Report, Length);

    /* A full queue loses its oldest report */
    if (*Count >= Limit)
    {
        Oldest = RemoveHeadList(ListHead);
        (*Count)--;
    }

    InsertTailList(ListHead, &Entry->Link);
    (*Count)++;

    if (Oldest)
        ExFreePoolWithTag(CONTAINING_RECORD(Oldest, HIDCLASS_REPORT, Link), HIDCLASS_TAG);
}

/**
 * @brief
 * Takes the oldest read waiting on a file object whose cancel routine has not
 * started yet. Called with the file lock held.
 */
PIRP
NTAPI
HidClassDequeuePendingRead(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension)
{
    while (!IsListEmpty(&FileExtension->PendingReads))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&FileExtension->PendingReads);
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (IoSetCancelRoutine(Irp, NULL))
        {
            InterlockedDecrement(&Collection->PendingReadCount);
            return Irp;
        }

        /* The cancel routine owns it and unlinks an empty entry */
        InitializeListHead(Entry);
    }

    return NULL;
}

/**
 * @brief
 * Gives one report to a file object. A waiting read gets it right away,
 * otherwise it goes into the ring buffer of the file.
 *
 * @return
 * The read to complete once the locks are released, or NULL.
 */
static
PIRP
NTAPI
HidClassDeliverToFile(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension,
    _In_reads_bytes_(Length) PUCHAR Report,
    _In_ ULONG Length)
{
    PHIDP_COLLECTION_DESC CollectionDesc;
    PHIDP_REPORT_IDS ReportId;
    PIO_STACK_LOCATION IoStack;
    PUCHAR Buffer;
    KIRQL OldIrql;
    PIRP Irp;

    KeAcquireSpinLock(&FileExtension->Lock, &OldIrql);

    Irp = HidClassDequeuePendingRead(Collection, FileExtension);
    if (!Irp)
    {
        HidClassQueueReportLocked(&FileExtension->Reports,
                                  &FileExtension->ReportCount,
                                  FileExtension->MaxReportCount,
                                  Report,
                                  Length);
        KeReleaseSpinLock(&FileExtension->Lock, OldIrql);
        return NULL;
    }

    Buffer = HidClassMapIrpBuffer(Irp);
    if (!Buffer)
    {
        Irp->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        KeReleaseSpinLock(&FileExtension->Lock, OldIrql);
        return Irp;
    }

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Irp->IoStatus.Information = IoStack->Parameters.Read.Length;
    Irp->IoStatus.Status = STATUS_DEVICE_DATA_ERROR;

    ReportId = HidClassFindReportId(Fdo, Report[0]);
    if (ReportId)
    {
        CollectionDesc = HidClassFindCollectionDesc(Fdo, ReportId->CollectionNumber);
        if (CollectionDesc && HidClassFindCollection(Fdo, ReportId->CollectionNumber))
        {
            if (IoStack->Parameters.Read.Length < CollectionDesc->InputLength)
            {
                Irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
            }
            else
            {
                RtlCopyMemory(Buffer, Report, CollectionDesc->InputLength);
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }
            Irp->IoStatus.Information = CollectionDesc->InputLength;
        }
    }

    KeReleaseSpinLock(&FileExtension->Lock, OldIrql);
    return Irp;
}

/**
 * @brief
 * Hands a report of a collection to every file object allowed to see it.
 *
 * @param[in] SessionOnly
 * Only handles opened from the active console session get the report.
 */
static
VOID
NTAPI
HidClassDistributeReport(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_reads_bytes_(Length) PUCHAR Report,
    _In_ ULONG Length,
    _In_ BOOLEAN SessionOnly)
{
    LIST_ENTRY Completed;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    InitializeListHead(&Completed);

    KeAcquireSpinLock(&Collection->FileLock, &OldIrql);

    if (IsListEmpty(&Collection->FileList))
    {
        /* Keystrokes typed before anyone opened the keyboard are kept */
        if (Collection->Kind == HidClassCollectionKeyboard)
        {
            HidClassQueueReportLocked(&Collection->UnreadReports,
                                      &Collection->UnreadReportCount,
                                      HIDCLASS_MAX_UNREAD_REPORTS,
                                      Report,
                                      Length);
        }
    }
    else
    {
        for (Entry = Collection->FileList.Flink; Entry != &Collection->FileList; Entry = Entry->Flink)
        {
            PHIDCLASS_FILE_EXTENSION FileExtension;
            PIRP Irp;

            FileExtension = CONTAINING_RECORD(Entry, HIDCLASS_FILE_EXTENSION, CollectionLink);

            if (Collection->SecureReadCount && !FileExtension->HasTcbPrivilege)
                continue;

            if (SessionOnly && FileExtension->SessionId != SharedUserData->ActiveConsoleId)
                continue;

            Irp = HidClassDeliverToFile(Fdo, Collection, FileExtension, Report, Length);
            if (Irp)
                InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);
        }
    }

    KeReleaseSpinLock(&Collection->FileLock, OldIrql);

    while (!IsListEmpty(&Completed))
    {
        Entry = RemoveHeadList(&Completed);
        IoCompleteRequest(CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry), IO_KEYBOARD_INCREMENT);
    }
}

/**
 * @brief
 * Completes a waiting IOCTL_GET_SYS_BUTTON_EVENT when a report carries a
 * system power button.
 */
VOID
NTAPI
HidClassSignalPowerEvent(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_reads_bytes_(Length) PUCHAR Report,
    _In_ USHORT Length)
{
    ULONG Event = 0;
    PIRP Irp = NULL;
    KIRQL OldIrql;

    if (!NT_SUCCESS(HidP_SysPowerEvent((PCHAR)Report, Length, Collection->PreparsedData, &Event)) || Event == 0)
        return;

    KeAcquireSpinLock(&Collection->PowerEventLock, &OldIrql);
    if (Collection->PowerEventIrp)
    {
        if (IoSetCancelRoutine(Collection->PowerEventIrp, NULL))
            Irp = Collection->PowerEventIrp;
        Collection->PowerEventIrp = NULL;
    }
    KeReleaseSpinLock(&Collection->PowerEventLock, OldIrql);

    if (!Irp)
        return;

    *(PULONG)Irp->AssociatedIrp.SystemBuffer = Event;
    Irp->IoStatus.Information = sizeof(ULONG);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/**
 * @brief
 * Splits what one read returned into reports and routes each one to its
 * collection by report ID.
 */
static
VOID
NTAPI
HidClassProcessReadBuffer(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PINGPONG PingPong,
    _In_ ULONG Length)
{
    PUCHAR Data = PingPong->Buffer;
    LONG Remaining = (LONG)Length;

    while (Remaining > 0)
    {
        PHIDP_COLLECTION_DESC CollectionDesc;
        PHIDCLASS_PDO_EXTENSION Pdo = NULL;
        PHIDCLASS_COLLECTION Collection;
        PHIDP_REPORT_IDS ReportId;
        BOOLEAN Deliver = FALSE;
        LONG DataLength;
        UCHAR Id = 0;
        KIRQL OldIrql;

        if (HidClassDeviceUsesReportIds(Fdo))
        {
            Id = *Data++;
            Remaining--;
        }

        ReportId = HidClassFindReportId(Fdo, Id);
        if (!ReportId)
        {
            DPRINT1("HIDCLASS: dropping report with unknown ID %u\n", Id);
            break;
        }

        /* The ID byte counts toward the report length only when present */
        DataLength = ReportId->InputLength;
        if (Id)
            DataLength--;

        if (DataLength <= 0 || DataLength > Remaining)
        {
            DPRINT1("HIDCLASS: report %u is %ld bytes, %ld left\n", Id, DataLength, Remaining);
            break;
        }

        Collection = HidClassFindCollection(Fdo, ReportId->CollectionNumber);
        CollectionDesc = HidClassFindCollectionDesc(Fdo, ReportId->CollectionNumber);
        if (!Collection || !CollectionDesc)
            break;

        RtlZeroMemory(PingPong->Report, CollectionDesc->InputLength);
        PingPong->Report[0] = Id;
        RtlCopyMemory(PingPong->Report + 1, Data, DataLength);

        KeAcquireSpinLock(&Fdo->ClientPdoLock, &OldIrql);
        if (Fdo->ClientPdos)
            Pdo = Fdo->ClientPdos[Collection->CollectionIndex];
        if (Pdo)
            Deliver = (Pdo->State == HidClassPdoStarted) || (InterlockedCompareExchange(&Fdo->IsPresent, 0, 0) == 1);
        KeReleaseSpinLock(&Fdo->ClientPdoLock, OldIrql);

        if (!Pdo)
        {
            if (Collection->Kind == HidClassCollectionKeyboard)
            {
                KeAcquireSpinLock(&Collection->FileLock, &OldIrql);
                HidClassQueueReportLocked(&Collection->UnreadReports,
                                          &Collection->UnreadReportCount,
                                          HIDCLASS_MAX_UNREAD_REPORTS,
                                          PingPong->Report,
                                          CollectionDesc->InputLength);
                KeReleaseSpinLock(&Collection->FileLock, OldIrql);
            }
        }
        else if (Deliver)
        {
            HidClassSignalPowerEvent(Collection, PingPong->Report, CollectionDesc->InputLength);
            HidClassDistributeReport(Fdo,
                                     Collection,
                                     PingPong->Report,
                                     CollectionDesc->InputLength,
                                     Pdo->SessionSecurity);
        }

        Data += DataLength;
        Remaining -= DataLength;
    }

    PingPong->BackoffDelay.QuadPart = HIDCLASS_BACKOFF_STEP;
}

static
NTSTATUS
NTAPI
HidClassReadCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;
    PHIDCLASS_PINGPONG PingPong = NULL;
    BOOLEAN Inline;
    BOOLEAN Sent;
    ULONG Index;

    UNREFERENCED_PARAMETER(DeviceObject);

    for (Index = 0; Index < Fdo->PingPongCount; Index++)
    {
        if (Fdo->PingPongs[Index].Irp == Irp)
        {
            PingPong = &Fdo->PingPongs[Index];
            break;
        }
    }

    if (!PingPong)
    {
        DPRINT1("HIDCLASS: completion for unknown read %p\n", Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    Inline = (InterlockedCompareExchange(&PingPong->State,
                                         HidClassPingPongCompletedInline,
                                         HidClassPingPongSubmitting) == HidClassPingPongSubmitting);

    if (NT_SUCCESS(Irp->IoStatus.Status))
        HidClassProcessReadBuffer(Fdo, PingPong, (ULONG)Irp->IoStatus.Information);

    /* The submitting thread is still in its call and sends the next read */
    if (Inline)
        return STATUS_MORE_PROCESSING_REQUIRED;

    if (PingPong->CancelCount)
    {
        KeSetEvent(&PingPong->ReadDone, IO_NO_INCREMENT, FALSE);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        HidClassSubmitRead(Fdo, PingPong, &Sent);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    DPRINT("HIDCLASS: read completed with %lx\n", Irp->IoStatus.Status);
    KeSetTimer(&PingPong->BackoffTimer, PingPong->BackoffDelay, &PingPong->BackoffDpc);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
NTAPI
HidClassCancelRead(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PHIDCLASS_FILE_EXTENSION FileExtension;
    PHIDCLASS_COLLECTION Collection;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    FileExtension = IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;
    Collection = &DeviceExtension->Pdo.Fdo->Collections[DeviceExtension->Pdo.CollectionIndex];

    KeAcquireSpinLock(&FileExtension->Lock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    InterlockedDecrement(&Collection->PendingReadCount);
    KeReleaseSpinLock(&FileExtension->Lock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/**
 * @brief
 * Parks a read on its file object. Called with the file lock held.
 */
static
NTSTATUS
NTAPI
HidClassQueueRead(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension,
    _In_ PIRP Irp)
{
    IoSetCancelRoutine(Irp, HidClassCancelRead);

    if (Irp->Cancel)
    {
        if (IoSetCancelRoutine(Irp, NULL))
            return STATUS_CANCELLED;

        /* The cancel routine already runs and unlinks this empty entry */
        InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }
    else
    {
        InsertTailList(&FileExtension->PendingReads, &Irp->Tail.Overlay.ListEntry);
    }

    InterlockedIncrement(&Collection->PendingReadCount);
    IoMarkIrpPending(Irp);
    return STATUS_PENDING;
}

static
VOID
NTAPI
HidClassCompleteReadWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PIRP Irp = Context;
    PIO_WORKITEM WorkItem = Irp->Tail.Overlay.DriverContext[0];

    UNREFERENCED_PARAMETER(DeviceObject);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoFreeWorkItem(WorkItem);
}

/**
 * @brief
 * Completes a read finished in the dispatch routine. A caller that keeps
 * sending reads from its completion routine gets them back through a work
 * item once it nests too deep.
 */
static
NTSTATUS
NTAPI
HidClassFinishRead(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension,
    _In_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    PIO_WORKITEM WorkItem;
    LONG Depth;

    Depth = InterlockedIncrement(&FileExtension->CompletionDepth);
    Irp->IoStatus.Status = Status;

    if (Depth <= HIDCLASS_MAX_INLINE_COMPLETIONS)
    {
        IoCompleteRequest(Irp, IO_KEYBOARD_INCREMENT);
    }
    else
    {
        WorkItem = IoAllocateWorkItem(Pdo->DeviceObject);
        if (WorkItem)
        {
            Irp->Tail.Overlay.DriverContext[0] = WorkItem;
            IoMarkIrpPending(Irp);
            IoQueueWorkItem(WorkItem, HidClassCompleteReadWorker, DelayedWorkQueue, Irp);
            Status = STATUS_PENDING;
        }
        else
        {
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }
    }

    InterlockedDecrement(&FileExtension->CompletionDepth);
    return Status;
}

static
VOID
NTAPI
HidClassCancelPolledRead(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PHIDCLASS_COLLECTION Collection;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    Collection = &DeviceExtension->Pdo.Fdo->Collections[DeviceExtension->Pdo.CollectionIndex];

    KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    InterlockedDecrement(&Collection->PendingReadCount);
    KeReleaseSpinLock(&Collection->PolledLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
HidClassQueuePolledRead(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PIRP Irp)
{
    NTSTATUS Status = STATUS_PENDING;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);

    IoSetCancelRoutine(Irp, HidClassCancelPolledRead);
    if (Irp->Cancel)
    {
        if (IoSetCancelRoutine(Irp, NULL))
        {
            Status = STATUS_CANCELLED;
            goto Exit;
        }
        InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }
    else
    {
        InsertTailList(&Collection->PolledReads, &Irp->Tail.Overlay.ListEntry);
    }

    InterlockedIncrement(&Collection->PendingReadCount);
    IoMarkIrpPending(Irp);

Exit:
    KeReleaseSpinLock(&Collection->PolledLock, OldIrql);
    return Status;
}

typedef enum _HIDCLASS_POLLED_FILTER
{
    HidClassPolledAll,
    HidClassPolledSession,
    HidClassPolledTcb
} HIDCLASS_POLLED_FILTER;

/**
 * @brief
 * Takes the oldest waiting polled read a report may go to.
 */
static
PIRP
NTAPI
HidClassDequeuePolledRead(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ HIDCLASS_POLLED_FILTER Filter)
{
    PLIST_ENTRY Entry;
    PIRP Found = NULL;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);

    Entry = Collection->PolledReads.Flink;
    while (Entry != &Collection->PolledReads)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        PHIDCLASS_FILE_EXTENSION FileExtension;

        Entry = Entry->Flink;
        FileExtension = IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;

        if (Filter == HidClassPolledSession && FileExtension->SessionId != SharedUserData->ActiveConsoleId)
            continue;

        if (Filter == HidClassPolledTcb && !FileExtension->HasTcbPrivilege)
            continue;

        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL))
        {
            InterlockedDecrement(&Collection->PendingReadCount);
            Found = Irp;
            break;
        }

        InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }

    KeReleaseSpinLock(&Collection->PolledLock, OldIrql);
    return Found;
}

static
VOID
NTAPI
HidClassCompletePolledReads(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_reads_bytes_(Length) PUCHAR Report,
    _In_ ULONG Length,
    _In_ NTSTATUS Status)
{
    HIDCLASS_POLLED_FILTER Filter;
    LIST_ENTRY Completed;
    PIRP Irp;

    InitializeListHead(&Completed);

    if (Collection->SecureReadCount)
        Filter = HidClassPolledTcb;
    else if (Pdo->SessionSecurity)
        Filter = HidClassPolledSession;
    else
        Filter = HidClassPolledAll;

    while ((Irp = HidClassDequeuePolledRead(Collection, Filter)) != NULL)
        InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);

    while (!IsListEmpty(&Completed))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Completed);
        NTSTATUS IrpStatus = Status;
        PUCHAR Buffer;

        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (NT_SUCCESS(IrpStatus))
        {
            Buffer = HidClassMapIrpBuffer(Irp);
            if (Buffer && IoGetCurrentIrpStackLocation(Irp)->Parameters.Read.Length >= Length)
            {
                RtlCopyMemory(Buffer, Report, Length);
                Irp->IoStatus.Information = Length;
            }
            else
            {
                IrpStatus = STATUS_INVALID_USER_BUFFER;
            }
        }

        Irp->IoStatus.Status = IrpStatus;
        IoCompleteRequest(Irp, IO_KEYBOARD_INCREMENT);
    }
}

static
NTSTATUS
NTAPI
HidClassPolledReadCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_PDO_EXTENSION Pdo = Context;
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PHIDCLASS_COLLECTION Collection;
    PHIDP_REPORT_IDS ReportId;
    PUCHAR Report;
    ULONG Length;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Put back the zero ID byte kept in front of the buffer */
    if (!HidClassDeviceUsesReportIds(Fdo))
    {
        Irp->UserBuffer = (PUCHAR)Irp->UserBuffer - 1;
        *(PUCHAR)Irp->UserBuffer = 0;
        if (NT_SUCCESS(Irp->IoStatus.Status))
            Irp->IoStatus.Information++;
    }

    Report = Irp->UserBuffer;
    Length = (ULONG)Irp->IoStatus.Information;

    ReportId = HidClassFindReportId(Fdo, Report[0]);
    if (ReportId)
    {
        Collection = HidClassFindCollection(Fdo, ReportId->CollectionNumber);
        if (Collection && HidClassFindCollectionDesc(Fdo, ReportId->CollectionNumber))
        {
            if (NT_SUCCESS(Irp->IoStatus.Status))
            {
                HidClassSignalPowerEvent(Collection, Report, (USHORT)Length);

                if (Collection->SecureReadCount)
                {
                    Collection->PolledReportStale = TRUE;
                }
                else
                {
                    KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);
                    if (Collection->PolledReport)
                    {
                        Collection->PolledReportLength = min(Length, max(HidClassMaxCollectionInputLength(Fdo), Fdo->MaxReportLength + 1));
                        RtlCopyMemory(Collection->PolledReport, Report, Collection->PolledReportLength);
                        Collection->PolledReportStale = FALSE;
                    }
                    KeReleaseSpinLock(&Collection->PolledLock, OldIrql);
                }
            }

            HidClassCompletePolledReads(Collection, Pdo, Report, Length, Irp->IoStatus.Status);
        }
    }

    ExFreePoolWithTag(Report, HIDCLASS_TAG);
    IoFreeIrp(Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
NTAPI
HidClassTimedPolledReadCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_PDO_EXTENSION Pdo = Context;
    PHIDCLASS_COLLECTION Collection;
    LARGE_INTEGER DueTime;

    HidClassPolledReadCompletion(DeviceObject, Irp, Context);

    if (Pdo->State == HidClassPdoStarted)
    {
        Collection = HidClassFindCollection(Pdo->Fdo, Pdo->CollectionNumber);
        if (Collection)
        {
            DueTime.QuadPart = -10000LL * Collection->PollIntervalMsec;
            KeSetTimer(&Collection->PollTimer, DueTime, &Collection->PollDpc);
        }
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Sends one read to a polled device on behalf of a collection.
 *
 * @return
 * TRUE once the read went down to the minidriver.
 */
BOOLEAN
NTAPI
HidClassReadPolledDevice(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ BOOLEAN TimerDriven)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PHIDP_COLLECTION_DESC CollectionDesc;
    PIO_STACK_LOCATION IoStack;
    PUCHAR Buffer;
    ULONG Length;
    PIRP Irp;

    CollectionDesc = HidClassFindCollectionDesc(Fdo, Pdo->CollectionNumber);
    if (!CollectionDesc)
        return FALSE;

    Irp = IoAllocateIrp(Fdo->DeviceObject->StackSize, FALSE);
    if (!Irp)
        return FALSE;

    Length = CollectionDesc->InputLength;
    Buffer = ExAllocatePoolZero(NonPagedPool, Length, HIDCLASS_TAG);
    if (!Buffer)
    {
        IoFreeIrp(Irp);
        return FALSE;
    }

    /* Without report IDs the minidriver fills in after a zero ID byte */
    Irp->UserBuffer = Buffer;
    if (!HidClassDeviceUsesReportIds(Fdo))
    {
        Buffer[0] = 0;
        Irp->UserBuffer = Buffer + 1;
        Length--;
    }

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_READ_REPORT;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = Length;
    IoSetCompletionRoutine(Irp,
                           TimerDriven ? HidClassTimedPolledReadCompletion : HidClassPolledReadCompletion,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    HidClassCallMinidriver(Fdo, Irp);
    return TRUE;
}

static
VOID
NTAPI
HidClassPollDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PHIDCLASS_PDO_EXTENSION Pdo = DeferredContext;
    PHIDCLASS_COLLECTION Collection;
    LARGE_INTEGER DueTime;
    BOOLEAN Empty;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (Pdo->State != HidClassPdoStarted)
        return;

    Collection = HidClassFindCollection(Pdo->Fdo, Pdo->CollectionNumber);
    if (!Collection)
        return;

    KeAcquireSpinLockAtDpcLevel(&Collection->PolledLock);
    Empty = IsListEmpty(&Collection->PolledReads);
    KeReleaseSpinLockFromDpcLevel(&Collection->PolledLock);

    /* Nobody waits, the cached report no longer reflects the device */
    if (Empty)
        Collection->PolledReportStale = TRUE;
    else if (HidClassReadPolledDevice(Pdo, TRUE))
        return;

    DueTime.QuadPart = -10000LL * Collection->PollIntervalMsec;
    KeSetTimer(&Collection->PollTimer, DueTime, &Collection->PollDpc);
}

/**
 * @brief
 * Starts polling a collection with the default interval.
 */
VOID
NTAPI
HidClassStartPolling(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_COLLECTION Collection)
{
    PHIDCLASS_PDO_EXTENSION Pdo;
    LARGE_INTEGER DueTime;

    if (Collection->PollingStarted || !Fdo->ClientPdos)
        return;

    Pdo = Fdo->ClientPdos[Collection->CollectionIndex];
    if (!Pdo)
        return;

    Collection->PollIntervalMsec = HIDCLASS_DEFAULT_POLL_MSEC;
    if (HidClassComputeMaxReportLength(Fdo) == 0)
        return;

    if (!Collection->PolledReport)
    {
        Collection->PolledReport = ExAllocatePoolZero(NonPagedPool,
                                                      max(HidClassMaxCollectionInputLength(Fdo), Fdo->MaxReportLength + 1),
                                                      HIDCLASS_TAG);
        if (!Collection->PolledReport)
            return;
    }
    Collection->PolledReportStale = TRUE;
    Collection->PollingStarted = TRUE;

    KeInitializeDpc(&Collection->PollDpc, HidClassPollDpc, Pdo);
    DueTime.QuadPart = -10000LL * Collection->PollIntervalMsec;
    KeSetTimer(&Collection->PollTimer, DueTime, &Collection->PollDpc);
}

VOID
NTAPI
HidClassRestartPolling(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_COLLECTION Collection)
{
    PHIDCLASS_PDO_EXTENSION Pdo;
    LARGE_INTEGER DueTime;
    KIRQL OldIrql;

    if (!Fdo->ClientPdos || !(Pdo = Fdo->ClientPdos[Collection->CollectionIndex]))
        return;

    KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);
    KeCancelTimer(&Collection->PollTimer);
    KeInitializeDpc(&Collection->PollDpc, HidClassPollDpc, Pdo);
    KeReleaseSpinLock(&Collection->PolledLock, OldIrql);

    DueTime.QuadPart = -10000LL * Collection->PollIntervalMsec;
    KeSetTimer(&Collection->PollTimer, DueTime, &Collection->PollDpc);
}

VOID
NTAPI
HidClassStopPolling(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ BOOLEAN CompleteReads)
{
    KIRQL OldIrql;
    PIRP Irp;

    KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);
    KeCancelTimer(&Collection->PollTimer);
    KeReleaseSpinLock(&Collection->PolledLock, OldIrql);
    Collection->PollingStarted = FALSE;

    if (!CompleteReads)
        return;

    while ((Irp = HidClassDequeuePolledRead(Collection, HidClassPolledAll)) != NULL)
    {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

static
NTSTATUS
NTAPI
HidClassReadPolled(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension,
    _In_ PIRP Irp,
    _In_ BOOLEAN Queue)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;
    PUCHAR Buffer;
    KIRQL OldIrql;

    if (Queue)
        return HidClassQueuePolledRead(Collection, Irp);

    /* An opportunistic reader takes the last report while it is fresh */
    if (FileExtension->OpportunisticPolling &&
        !Collection->PolledReportStale &&
        IoStack->Parameters.Read.Length >= Collection->PolledReportLength)
    {
        Buffer = HidClassMapIrpBuffer(Irp);
        if (!Buffer)
            return STATUS_INVALID_USER_BUFFER;

        KeAcquireSpinLock(&Collection->PolledLock, &OldIrql);
        RtlCopyMemory(Buffer, Collection->PolledReport, Collection->PolledReportLength);
        Irp->IoStatus.Information = Collection->PolledReportLength;
        KeReleaseSpinLock(&Collection->PolledLock, OldIrql);
        return STATUS_SUCCESS;
    }

    Status = HidClassQueuePolledRead(Collection, Irp);
    if (NT_SUCCESS(Status) && FileExtension->OpportunisticPolling)
        HidClassReadPolledDevice(Pdo, FALSE);

    return Status;
}

/**
 * @brief
 * IRP_MJ_READ on a collection. Reports already queued on the handle are
 * returned at once, as many as fit, otherwise the read waits for input.
 */
NTSTATUS
NTAPI
HidClassRead(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PHIDCLASS_FILE_EXTENSION FileExtension = NULL;
    PHIDP_COLLECTION_DESC CollectionDesc;
    PHIDCLASS_COLLECTION Collection;
    HIDCLASS_IO_STATE IoState;
    NTSTATUS Status = STATUS_DEVICE_NOT_CONNECTED;
    BOOLEAN Queue;
    KIRQL OldIrql;

    if (IoStack->FileObject)
        FileExtension = IoStack->FileObject->FsContext;

    if (!FileExtension)
    {
        Irp->IoStatus.Status = STATUS_PRIVILEGE_NOT_HELD;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_PRIVILEGE_NOT_HELD;
    }

    Collection = HidClassFindCollection(Fdo, Pdo->CollectionNumber);
    CollectionDesc = HidClassFindCollectionDesc(Fdo, Pdo->CollectionNumber);

    if (Collection && Collection->Kind == HidClassCollectionKeyboard && !(FileExtension->DesiredAccess & FILE_READ_DATA))
        DPRINT1("HIDCLASS: keyboard read on a handle without read access\n");

    FileExtension->HasRead = TRUE;

    IoState = Fdo->IoState;
    if ((IoState != HidClassIoPass && IoState != HidClassIoQueue) ||
        Pdo->State < HidClassPdoStarted || Pdo->State > HidClassPdoStopped)
    {
        goto Finish;
    }

    Queue = (IoState == HidClassIoQueue) || (Pdo->State == HidClassPdoStopPending) || (Pdo->State == HidClassPdoStopped);
    Irp->IoStatus.Information = 0;

    if (!Collection || !CollectionDesc)
        goto Finish;

    if (IoStack->Parameters.Read.Length < CollectionDesc->InputLength)
    {
        Status = STATUS_INVALID_BUFFER_SIZE;
        goto Finish;
    }

    if (Collection->Information.Polled)
    {
        Status = HidClassReadPolled(Pdo, Collection, FileExtension, Irp, Queue);
        if (Status == STATUS_PENDING)
            return Status;
        goto Finish;
    }

    KeAcquireSpinLock(&FileExtension->Lock, &OldIrql);

    if (Fdo->DevicePowerState != PowerDeviceD0)
        Queue = TRUE;

    if (Queue)
    {
        Status = HidClassQueueRead(Collection, FileExtension, Irp);
    }
    else
    {
        PUCHAR Buffer = HidClassMapIrpBuffer(Irp);
        ULONG Remaining = IoStack->Parameters.Read.Length;
        ULONG Copied = 0;

        if (!Buffer)
        {
            Status = STATUS_INVALID_USER_BUFFER;
            KeReleaseSpinLock(&FileExtension->Lock, OldIrql);
            goto Finish;
        }

        Status = STATUS_SUCCESS;
        while (Remaining && !IsListEmpty(&FileExtension->Reports))
        {
            PLIST_ENTRY Entry = RemoveHeadList(&FileExtension->Reports);
            PHIDCLASS_REPORT Report = CONTAINING_RECORD(Entry, HIDCLASS_REPORT, Link);
            PHIDP_COLLECTION_DESC ReportDesc;
            PHIDP_REPORT_IDS ReportId;
            ULONG ReportLength = 0;

            if (Report->Length > Remaining)
            {
                InsertHeadList(&FileExtension->Reports, Entry);
                break;
            }
            FileExtension->ReportCount--;

            Status = STATUS_DEVICE_DATA_ERROR;
            ReportId = HidClassFindReportId(Fdo, Report->Data[0]);
            if (ReportId)
            {
                ReportDesc = HidClassFindCollectionDesc(Fdo, ReportId->CollectionNumber);
                if (ReportDesc && HidClassFindCollection(Fdo, ReportId->CollectionNumber))
                {
                    ReportLength = ReportDesc->InputLength;
                    if (Remaining < ReportLength)
                    {
                        Status = STATUS_INVALID_BUFFER_SIZE;
                    }
                    else
                    {
                        RtlCopyMemory(Buffer + Copied, Report->Data, ReportLength);
                        Status = STATUS_SUCCESS;
                    }
                }
            }

            ExFreePoolWithTag(Report, HIDCLASS_TAG);
            if (!NT_SUCCESS(Status))
                break;

            Copied += ReportLength;
            Remaining -= ReportLength;
        }

        if (NT_SUCCESS(Status))
        {
            if (Copied)
                Irp->IoStatus.Information = Copied;
            else
                Status = HidClassQueueRead(Collection, FileExtension, Irp);
        }
    }

    KeReleaseSpinLock(&FileExtension->Lock, OldIrql);

    if (Status == STATUS_PENDING)
        return Status;

Finish:
    return HidClassFinishRead(Pdo, FileExtension, Irp, Status);
}

/**
 * @brief
 * Fails every read waiting on handles of a collection PDO.
 */
VOID
NTAPI
HidClassCompletePendingReads(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDCLASS_COLLECTION Collection;
    LIST_ENTRY Completed;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    if (!Pdo->Fdo->Collections)
        return;

    Collection = &Pdo->Fdo->Collections[Pdo->CollectionIndex];
    InitializeListHead(&Completed);

    KeAcquireSpinLock(&Collection->FileLock, &OldIrql);
    for (Entry = Collection->FileList.Flink; Entry != &Collection->FileList; Entry = Entry->Flink)
    {
        PHIDCLASS_FILE_EXTENSION FileExtension;
        PIRP Irp;

        FileExtension = CONTAINING_RECORD(Entry, HIDCLASS_FILE_EXTENSION, CollectionLink);
        if (FileExtension->Pdo != Pdo)
            continue;

        KeAcquireSpinLockAtDpcLevel(&FileExtension->Lock);
        while ((Irp = HidClassDequeuePendingRead(Collection, FileExtension)) != NULL)
            InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLockFromDpcLevel(&FileExtension->Lock);
    }
    KeReleaseSpinLock(&Collection->FileLock, OldIrql);

    while (!IsListEmpty(&Completed))
    {
        PIRP Irp;

        Entry = RemoveHeadList(&Completed);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

VOID
NTAPI
HidClassFlushReports(
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&FileExtension->Lock, &OldIrql);
    while (!IsListEmpty(&FileExtension->Reports))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&FileExtension->Reports);

        ExFreePoolWithTag(CONTAINING_RECORD(Entry, HIDCLASS_REPORT, Link), HIDCLASS_TAG);
        FileExtension->ReportCount--;
    }
    KeReleaseSpinLock(&FileExtension->Lock, OldIrql);
}
