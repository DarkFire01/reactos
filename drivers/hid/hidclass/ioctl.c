/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device control requests on collections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE HidClassAsyncCompletion;
static IO_COMPLETION_ROUTINE HidClassResetCompletion;
static DRIVER_CANCEL HidClassCancelPowerEvent;
static DRIVER_CANCEL HidClassCancelResetNotification;

/* Type3InputBuffer of a string request, language in the high word */
#define HIDCLASS_STRING_REQUEST(Id) \
    ((PVOID)(ULONG_PTR)(((ULONG)HIDCLASS_STRING_LANGID << 16) | ((Id) & 0xFFFF)))

static
NTSTATUS
NTAPI
HidClassAsyncCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    HidClassEndIo(Context);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Passes a request whose next stack location is ready to the minidriver and
 * ends the I/O accounting when it completes.
 */
static
NTSTATUS
NTAPI
HidClassSendAsync(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    IoSetCompletionRoutine(Irp, HidClassAsyncCompletion, Fdo, TRUE, TRUE, TRUE);
    return HidClassCallMinidriver(Fdo, Irp);
}

static
VOID
NTAPI
HidClassPrepareForward(
    _In_ PIRP Irp,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID Type3InputBuffer)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION NextStack = IoGetNextIrpStackLocation(Irp);

    RtlZeroMemory(NextStack, sizeof(*NextStack));
    NextStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    NextStack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    NextStack->Parameters.DeviceIoControl.OutputBufferLength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    NextStack->Parameters.DeviceIoControl.Type3InputBuffer = Type3InputBuffer;
}

/**
 * @brief
 * Reads one of the fixed device strings into the caller buffer.
 */
static
NTSTATUS
NTAPI
HidClassGetDeviceString(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp,
    _In_ USHORT StringId)
{
    NTSTATUS Status;

    Irp->UserBuffer = HidClassMapIrpBuffer(Irp);
    if (!Irp->UserBuffer)
    {
        Status = STATUS_INVALID_USER_BUFFER;
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        HidClassEndIo(Fdo);
        return Status;
    }

    HidClassPrepareForward(Irp, IOCTL_HID_GET_STRING, HIDCLASS_STRING_REQUEST(StringId));
    return HidClassSendAsync(Fdo, Irp);
}

/**
 * @brief
 * Feature, input and output report requests. They are synchronous calls into
 * the minidriver with a HID_XFER_PACKET in Irp->UserBuffer.
 */
static
NTSTATUS
NTAPI
HidClassGetSetReport(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp,
    _In_ ULONG IoControlCode)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FILE_EXTENSION FileExtension = NULL;
    PHIDP_COLLECTION_DESC CollectionDesc;
    PHIDP_REPORT_IDS ReportId;
    PHID_XFER_PACKET Packet;
    PIO_STACK_LOCATION NextStack;
    USHORT CollectionLength;
    USHORT ReportLength;
    PUCHAR Buffer;
    ULONG Length;
    PVOID SavedUserBuffer;
    BOOLEAN SetRequest;
    NTSTATUS Status;
    UCHAR Id = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (IoStack->FileObject)
        FileExtension = IoStack->FileObject->FsContext;

    if (!FileExtension)
        return STATUS_PRIVILEGE_NOT_HELD;

    CollectionDesc = HidClassFindCollectionDesc(Fdo, FileExtension->CollectionNumber);
    if (!CollectionDesc)
        return STATUS_DEVICE_NOT_CONNECTED;

    SetRequest = (IoControlCode == IOCTL_HID_SET_FEATURE || IoControlCode == IOCTL_HID_SET_OUTPUT_REPORT);
    if (SetRequest)
    {
        Buffer = Irp->AssociatedIrp.SystemBuffer;
        Length = IoStack->Parameters.DeviceIoControl.InputBufferLength;
    }
    else
    {
        Buffer = HidClassMapIrpBuffer(Irp);
        Length = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    }

    switch (IoControlCode)
    {
        case IOCTL_HID_SET_FEATURE:
        case IOCTL_HID_GET_FEATURE:
            CollectionLength = CollectionDesc->FeatureLength;
            break;

        case IOCTL_HID_GET_INPUT_REPORT:
            CollectionLength = CollectionDesc->InputLength;
            break;

        default:
            CollectionLength = CollectionDesc->OutputLength;
            break;
    }

    if (!CollectionLength)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (!Buffer || !Length)
        return STATUS_INVALID_USER_BUFFER;

    if (HidClassDeviceUsesReportIds(Fdo))
    {
        Id = Buffer[0];
    }
    else
    {
        Buffer++;
        Length--;
    }

    ReportId = HidClassFindReportId(Fdo, Id);
    if (!ReportId)
        return STATUS_INVALID_PARAMETER;

    if (ReportId->CollectionNumber != FileExtension->CollectionNumber)
        return STATUS_INVALID_DEVICE_REQUEST;

    switch (IoControlCode)
    {
        case IOCTL_HID_SET_FEATURE:
        case IOCTL_HID_GET_FEATURE:
            ReportLength = ReportId->FeatureLength;
            break;

        case IOCTL_HID_GET_INPUT_REPORT:
            ReportLength = ReportId->InputLength;
            break;

        default:
            ReportLength = ReportId->OutputLength;
            break;
    }

    if (!ReportLength || Length < ReportLength)
        return STATUS_INVALID_PARAMETER;

    Packet = ExAllocatePoolZero(NonPagedPool, sizeof(*Packet), HIDCLASS_TAG);
    if (!Packet)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* A set sends exactly one report, a get offers the whole buffer */
    Packet->reportBuffer = Buffer;
    Packet->reportBufferLen = SetRequest ? ReportLength : Length;
    Packet->reportId = Id;

    SavedUserBuffer = Irp->UserBuffer;
    Irp->UserBuffer = Packet;

    NextStack = IoGetNextIrpStackLocation(Irp);
    RtlZeroMemory(NextStack, sizeof(*NextStack));
    NextStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    NextStack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    if (SetRequest)
        NextStack->Parameters.DeviceIoControl.InputBufferLength = sizeof(*Packet);
    else
        NextStack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(*Packet);

    Status = HidClassCallMinidriverSynchronous(Fdo, Irp);

    Irp->UserBuffer = SavedUserBuffer;
    ExFreePoolWithTag(Packet, HIDCLASS_TAG);
    return Status;
}

static
VOID
NTAPI
HidClassCancelPowerEvent(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PHIDCLASS_COLLECTION Collection;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    Collection = &DeviceExtension->Pdo.Fdo->Collections[DeviceExtension->Pdo.CollectionIndex];

    KeAcquireSpinLock(&Collection->PowerEventLock, &OldIrql);
    if (Collection->PowerEventIrp == Irp)
        Collection->PowerEventIrp = NULL;
    KeReleaseSpinLock(&Collection->PowerEventLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/**
 * @brief
 * Parks an IOCTL_GET_SYS_BUTTON_EVENT until a power button report arrives.
 * A collection holds one at a time.
 */
static
NTSTATUS
NTAPI
HidClassQueuePowerEvent(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PIRP Irp)
{
    NTSTATUS Status = STATUS_PENDING;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Collection->PowerEventLock, &OldIrql);

    IoSetCancelRoutine(Irp, HidClassCancelPowerEvent);
    if (Irp->Cancel)
    {
        if (IoSetCancelRoutine(Irp, NULL))
            Status = STATUS_CANCELLED;
        else
            IoMarkIrpPending(Irp);
    }
    else if (Collection->PowerEventIrp)
    {
        if (IoSetCancelRoutine(Irp, NULL))
            Status = STATUS_UNSUCCESSFUL;
        else
            IoMarkIrpPending(Irp);
    }
    else
    {
        Collection->PowerEventIrp = Irp;
        IoMarkIrpPending(Irp);
    }

    KeReleaseSpinLock(&Collection->PowerEventLock, OldIrql);
    return Status;
}

static
VOID
NTAPI
HidClassCancelResetNotification(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PHIDCLASS_FDO_EXTENSION Fdo = DeviceExtension->Pdo.Fdo;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
NTAPI
HidClassFreeResetRequest(
    _In_ PHIDCLASS_RESET_REQUEST Request)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Request->Fdo;
    PIRP Irp = Request->Irp;

    IoFreeIrp(Irp);
    ExFreePoolWithTag(Request, HIDCLASS_TAG);
    IoReleaseRemoveLock(&Fdo->RemoveLock, Irp);
}

/**
 * @brief
 * Moves every queued client reset notification of a PDO, or of all PDOs
 * when Pdo is NULL, to a private list. Called with the reset lock held.
 */
static
VOID
NTAPI
HidClassTakeResetIrps(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_opt_ PHIDCLASS_PDO_EXTENSION Pdo,
    _Inout_ PLIST_ENTRY Taken)
{
    PLIST_ENTRY Entry = Fdo->ResetIrps.Flink;

    while (Entry != &Fdo->ResetIrps)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        Entry = Entry->Flink;
        if (Pdo && Irp->Tail.Overlay.DriverContext[0] != Pdo)
            continue;

        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL))
            InsertTailList(Taken, &Irp->Tail.Overlay.ListEntry);
        else
            InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }
}

static
VOID
NTAPI
HidClassCompleteIrpList(
    _Inout_ PLIST_ENTRY List,
    _In_ NTSTATUS Status)
{
    while (!IsListEmpty(List))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(List), IRP, Tail.Overlay.ListEntry);

        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

static
NTSTATUS
NTAPI
HidClassResetCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_RESET_REQUEST Request = Context;
    PHIDCLASS_FDO_EXTENSION Fdo = Request->Fdo;
    LIST_ENTRY Taken;
    LONG OldState;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    InitializeListHead(&Taken);

    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);
    HidClassTakeResetIrps(Fdo, NULL, &Taken);
    if (Fdo->ResetRequest == Request)
        Fdo->ResetRequest = NULL;
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    HidClassCompleteIrpList(&Taken, Irp->IoStatus.Status);

    /* Whoever finishes last between this and a drain frees the request */
    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);
    OldState = Request->State;
    Request->State = 1;
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    if (OldState == 0 || OldState == 3)
        HidClassFreeResetRequest(Request);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Queues an IOCTL_HID_DEVICERESET_NOTIFICATION from a client. All of them
 * complete when the one request kept at the minidriver comes back.
 */
static
NTSTATUS
NTAPI
HidClassQueueResetNotification(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PHIDCLASS_RESET_REQUEST Request;
    PIO_STACK_LOCATION IoStack;
    PIRP ResetIrp;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);

    if (Fdo->ResetDrained)
    {
        KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);
        Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_UNSUCCESSFUL;
    }

    IoMarkIrpPending(Irp);
    Irp->Tail.Overlay.DriverContext[0] = Pdo;
    InsertTailList(&Fdo->ResetIrps, &Irp->Tail.Overlay.ListEntry);

    IoSetCancelRoutine(Irp, HidClassCancelResetNotification);
    if (Irp->Cancel)
    {
        if (IoSetCancelRoutine(Irp, NULL))
        {
            RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
            KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);
            Irp->IoStatus.Status = STATUS_CANCELLED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }
        else
        {
            KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);
        }
        return STATUS_PENDING;
    }

    if (!Fdo->DeviceResetSupported || Fdo->ResetRequest)
    {
        KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);
        return STATUS_PENDING;
    }

    Request = ExAllocatePoolZero(NonPagedPool, sizeof(*Request), HIDCLASS_TAG);
    if (!Request)
    {
        KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);
        return STATUS_PENDING;
    }

    ResetIrp = IoAllocateIrp(Fdo->DeviceObject->StackSize - 1, FALSE);
    if (!ResetIrp || !NT_SUCCESS(IoAcquireRemoveLock(&Fdo->RemoveLock, ResetIrp)))
    {
        if (ResetIrp)
            IoFreeIrp(ResetIrp);
        ExFreePoolWithTag(Request, HIDCLASS_TAG);
        KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);
        return STATUS_PENDING;
    }

    Request->Irp = ResetIrp;
    Request->Fdo = Fdo;
    Request->State = 0;

    ResetIrp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoStack = IoGetNextIrpStackLocation(ResetIrp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_DEVICERESET_NOTIFICATION;
    IoSetCompletionRoutine(ResetIrp, HidClassResetCompletion, Request, TRUE, TRUE, TRUE);

    Fdo->ResetRequest = Request;
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    HidClassCallMinidriver(Fdo, ResetIrp);
    return STATUS_PENDING;
}

VOID
NTAPI
HidClassCancelResetNotifications(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    LIST_ENTRY Taken;
    KIRQL OldIrql;

    InitializeListHead(&Taken);

    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);
    HidClassTakeResetIrps(Fdo, Pdo, &Taken);
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    HidClassCompleteIrpList(&Taken, STATUS_CANCELLED);
}

/**
 * @brief
 * Stops reset notifications for good when the device goes away.
 */
VOID
NTAPI
HidClassDrainResetNotifications(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    PHIDCLASS_RESET_REQUEST Request = NULL;
    LIST_ENTRY Taken;
    LONG OldState;
    KIRQL OldIrql;

    InitializeListHead(&Taken);

    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);
    if (!Fdo->ResetDrained)
    {
        Fdo->ResetDrained = TRUE;
        if (Fdo->ResetRequest && Fdo->ResetRequest->State == 0)
        {
            Request = Fdo->ResetRequest;
            Request->State = 2;
            Fdo->ResetRequest = NULL;
        }
        HidClassTakeResetIrps(Fdo, NULL, &Taken);
    }
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    HidClassCompleteIrpList(&Taken, STATUS_CANCELLED);

    if (!Request)
        return;

    IoCancelIrp(Request->Irp);

    KeAcquireSpinLock(&Fdo->ResetLock, &OldIrql);
    OldState = Request->State;
    Request->State = 3;
    KeReleaseSpinLock(&Fdo->ResetLock, OldIrql);

    if (OldState == 1)
        HidClassFreeResetRequest(Request);
}

/**
 * @brief
 * Recomputes the longest S0 idle timeout any collection asked for.
 */
static
VOID
NTAPI
HidClassUpdateClientIdleTimeout(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Timeout = 0;
    ULONG Index;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->ClientPdoLock, &OldIrql);
    if (Fdo->ClientPdos)
    {
        for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
        {
            PHIDCLASS_PDO_EXTENSION Pdo = Fdo->ClientPdos[Index];

            if (Pdo && Pdo->S0IdleTimeout > Timeout)
                Timeout = Pdo->S0IdleTimeout;
        }
    }
    KeReleaseSpinLock(&Fdo->ClientPdoLock, OldIrql);

    if (Timeout != Fdo->ClientIdleTimeout)
    {
        Fdo->ClientIdleTimeout = Timeout;
        HidClassUpdateIdlePolicy(Fdo);
    }
}

static
NTSTATUS
NTAPI
HidClassGetCollectionDescriptorIoctl(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Length = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS Status;

    if (!Irp->UserBuffer)
        return STATUS_INVALID_BUFFER_SIZE;

    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
            ProbeForWrite(Irp->UserBuffer, Length, 1);

        Status = HidClassCopyCollectionDescriptor(Fdo,
                                                  Pdo->CollectionNumber,
                                                  Irp->UserBuffer,
                                                  &Length,
                                                  Irp->RequestorMode != KernelMode);
        Irp->IoStatus.Information = Length;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        Irp->IoStatus.Information = 0;
    }
    _SEH2_END;

    return Status;
}

NTSTATUS
NTAPI
HidClassDeviceControl(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    ULONG IoControlCode = IoStack->Parameters.DeviceIoControl.IoControlCode;
    ULONG InputLength = IoStack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    PHIDCLASS_FILE_EXTENSION FileExtension = NULL;
    PHIDCLASS_COLLECTION Collection;
    PHIDCLASS_PDO_EXTENSION Pdo;
    PHIDCLASS_FDO_EXTENSION Fdo;
    BOOLEAN Counted = FALSE;
    BOOLEAN Sent;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!DeviceExtension->IsClientPdo)
    {
        Status = STATUS_INVALID_PARAMETER_1;
        goto Complete;
    }

    Pdo = &DeviceExtension->Pdo;
    Fdo = Pdo->Fdo;

    if (Fdo->IoState == HidClassIoFail || Pdo->State != HidClassPdoStarted)
    {
        Status = STATUS_DEVICE_NOT_CONNECTED;
        goto Complete;
    }

    Irp->IoStatus.Information = 0;
    if (IoStack->FileObject)
        FileExtension = IoStack->FileObject->FsContext;

    /* Requests that never wait for the device to power up */
    switch (IoControlCode)
    {
        case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
            Status = HidClassGetCollectionDescriptorIoctl(Fdo, Pdo, Irp);
            goto Complete;

        case IOCTL_HID_ENABLE_WAKE_ON_SX:
            if (InputLength != sizeof(BOOLEAN) || !SystemBuffer)
            {
                Status = STATUS_INVALID_PARAMETER;
                goto Complete;
            }
            Pdo->WakeOnSx = *(PBOOLEAN)SystemBuffer;
            Status = STATUS_SUCCESS;
            goto Complete;

        case IOCTL_HID_SET_S0_IDLE_TIMEOUT:
            if (InputLength != sizeof(ULONG) || !SystemBuffer || *(PULONG)SystemBuffer > HIDCLASS_MAX_S0_IDLE_TIMEOUT)
            {
                Status = STATUS_INVALID_PARAMETER;
                goto Complete;
            }
            Pdo->S0IdleTimeout = *(PULONG)SystemBuffer;
            HidClassUpdateClientIdleTimeout(Fdo);
            Status = STATUS_SUCCESS;
            goto Complete;

        case IOCTL_HID_DEVICERESET_NOTIFICATION:
            if (InputLength)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                goto Complete;
            }
            return HidClassQueueResetNotification(Fdo, Pdo, Irp);

        default:
            break;
    }

    Status = HidClassBeginIo(DeviceExtension, Irp, &Counted);
    if (Status == STATUS_PENDING)
        return Status;

    if (!NT_SUCCESS(Status))
        goto Complete;

    Collection = HidClassFindCollection(Fdo, Pdo->CollectionNumber);

    switch (IoControlCode)
    {
        case IOCTL_HID_GET_DRIVER_CONFIG:
        case IOCTL_HID_SET_DRIVER_CONFIG:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case IOCTL_HID_SET_FEATURE:
        case IOCTL_HID_GET_FEATURE:
        case IOCTL_HID_SET_OUTPUT_REPORT:
        case IOCTL_HID_GET_INPUT_REPORT:
        case IOCTL_HID_GET_OUTPUT_REPORT:
            Status = HidClassGetSetReport(Fdo, Irp, IoControlCode);
            break;

        case IOCTL_HID_FLUSH_QUEUE:
            if (!FileExtension)
            {
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }
            HidClassFlushReports(FileExtension);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HID_GET_POLL_FREQUENCY_MSEC:
            if (!Collection || !Collection->Information.Polled)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }
            if (!SystemBuffer || OutputLength < sizeof(ULONG))
            {
                Status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            *(PULONG)SystemBuffer = Collection->PollIntervalMsec;
            Irp->IoStatus.Information = sizeof(ULONG);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HID_SET_POLL_FREQUENCY_MSEC:
            if (!Collection || !Collection->Information.Polled)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }
            if (!SystemBuffer || InputLength < sizeof(ULONG))
            {
                Status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            if (!FileExtension)
            {
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }

            /* Zero switches the handle to reads that poll on demand */
            if (*(PULONG)SystemBuffer)
            {
                Collection->PollIntervalMsec = min(*(PULONG)SystemBuffer, HIDCLASS_MAX_POLL_MSEC);
                FileExtension->OpportunisticPolling = FALSE;
                HidClassRestartPolling(Fdo, Collection);
            }
            else
            {
                FileExtension->OpportunisticPolling = TRUE;
            }
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_GET_PHYSICAL_DESCRIPTOR:
        case IOCTL_HID_GET_MS_GENRE_DESCRIPTOR:
            HidClassPrepareForward(Irp, IoControlCode, NULL);
            return HidClassSendAsync(Fdo, Irp);

        case IOCTL_HID_GET_HARDWARE_ID:
        {
            PVOID Buffer = HidClassMapIrpBuffer(Irp);
            ULONG ResultLength = 0;

            if (!Buffer || !OutputLength)
            {
                Status = STATUS_INVALID_USER_BUFFER;
                break;
            }

            Status = IoGetDeviceProperty(HidClassExtensionFromFdo(Fdo)->Hid.PhysicalDeviceObject,
                                         DevicePropertyHardwareID,
                                         OutputLength,
                                         Buffer,
                                         &ResultLength);
            if (NT_SUCCESS(Status))
                Irp->IoStatus.Information = ResultLength;
            break;
        }

        case IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS:
            if (!SystemBuffer || OutputLength < sizeof(ULONG))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            if (!FileExtension)
            {
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }
            *(PULONG)SystemBuffer = FileExtension->MaxReportCount;
            Irp->IoStatus.Information = sizeof(ULONG);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS:
            if (!SystemBuffer || InputLength < sizeof(ULONG))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            if (!FileExtension)
            {
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }
            if (*(PULONG)SystemBuffer < HIDCLASS_MIN_INPUT_BUFFERS || *(PULONG)SystemBuffer > HIDCLASS_MAX_INPUT_BUFFERS)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            FileExtension->MaxReportCount = *(PULONG)SystemBuffer;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HID_GET_COLLECTION_INFORMATION:
            if (!SystemBuffer)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            Status = HidClassCopyCollectionInformation(Fdo, Pdo->CollectionNumber, SystemBuffer, &OutputLength);
            Irp->IoStatus.Information = OutputLength;
            break;

        case IOCTL_HID_GET_MANUFACTURER_STRING:
            return HidClassGetDeviceString(Fdo, Irp, HID_STRING_ID_IMANUFACTURER);

        case IOCTL_HID_GET_PRODUCT_STRING:
            return HidClassGetDeviceString(Fdo, Irp, HID_STRING_ID_IPRODUCT);

        case IOCTL_HID_GET_SERIALNUMBER_STRING:
            return HidClassGetDeviceString(Fdo, Irp, HID_STRING_ID_ISERIALNUMBER);

        case IOCTL_HID_GET_INDEXED_STRING:
            if (!SystemBuffer || InputLength < sizeof(ULONG))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            HidClassPrepareForward(Irp, IOCTL_HID_GET_INDEXED_STRING, HIDCLASS_STRING_REQUEST(*(PULONG)SystemBuffer));
            return HidClassSendAsync(Fdo, Irp);

        case IOCTL_HID_ENABLE_SECURE_READ:
        case IOCTL_HID_DISABLE_SECURE_READ:
            if (!FileExtension)
            {
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }
            if (!Collection)
            {
                Status = STATUS_UNSUCCESSFUL;
                break;
            }
            if (!FileExtension->HasTcbPrivilege)
            {
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }

            KeAcquireSpinLock(&Collection->SecureReadLock, &OldIrql);
            if (IoControlCode == IOCTL_HID_ENABLE_SECURE_READ)
            {
                FileExtension->SecureReadCount++;
                Collection->SecureReadCount++;
            }
            else if (FileExtension->SecureReadCount)
            {
                FileExtension->SecureReadCount--;
                Collection->SecureReadCount--;
            }
            KeReleaseSpinLock(&Collection->SecureReadLock, OldIrql);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HIDCLASS_WRITE_REPORT_EX:
        {
            PVOID Buffer = IoStack->Parameters.DeviceIoControl.Type3InputBuffer;

            /* Only a kernel caller that described its buffer with an MDL */
            if (!Irp->MdlAddress || MmGetMdlVirtualAddress(Irp->MdlAddress) != Buffer)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            Status = HidClassSendWriteReport(Fdo, Pdo, Irp, Buffer, InputLength, &Sent);
            if (Sent)
                return Status;
            break;
        }

        case IOCTL_GET_SYS_BUTTON_CAPS:
        {
            ULONG Caps = 0;

            if (!Collection)
            {
                Status = STATUS_DEVICE_NOT_CONNECTED;
                break;
            }
            if (OutputLength < sizeof(ULONG))
            {
                Irp->IoStatus.Information = sizeof(ULONG);
                Status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }

            Status = HidP_SysPowerCaps(Collection->PreparsedData, &Caps);
            if (NT_SUCCESS(Status))
            {
                *(PULONG)SystemBuffer = Caps;
                Irp->IoStatus.Information = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_GET_SYS_BUTTON_EVENT:
            if (!Collection)
            {
                Status = STATUS_DEVICE_NOT_CONNECTED;
                break;
            }
            Status = HidClassQueuePowerEvent(Collection, Irp);
            if (Status == STATUS_PENDING)
            {
                HidClassEndIo(Fdo);
                return Status;
            }
            break;

        default:
            DPRINT1("HIDCLASS: unknown IOCTL %lx\n", IoControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

Complete:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (Counted)
        HidClassEndIo(HidClassGetFdo(DeviceExtension));

    return Status;
}

/**
 * @brief
 * Collections accept no internal device control requests.
 */
NTSTATUS
NTAPI
HidClassInternalDeviceControl(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    NTSTATUS Status;

    if (!DeviceExtension->IsClientPdo)
    {
        Status = STATUS_INVALID_PARAMETER_1;
    }
    else
    {
        Irp->IoStatus.Information = 0;
        if (DeviceExtension->Pdo.Fdo->IoState == HidClassIoFail)
            Status = STATUS_DEVICE_NOT_CONNECTED;
        else
            Status = Irp->IoStatus.Status;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
