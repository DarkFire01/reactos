/*
 * PROJECT:     ReactOS Storage Stack / SD host controller port driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Request submission, interrupt servicing and completion
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Command timeout 1s, data transfer timeout 10s. */
#define SDPORT_COMMAND_TIMEOUT_MS  1000
#define SDPORT_TRANSFER_TIMEOUT_MS 10000

/* The periodic pump runs only while asynchronous requests are outstanding:
   it is needed when the controller line interrupt is not delivered, and idles
   the host when there is no work. */
static
VOID
SdPortPollAcquire(
    _In_ PSDPORT_FDO_EXTENSION FdoExtension)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&FdoExtension->PollLock, &OldIrql);
    if (FdoExtension->PollOutstanding++ == 0 && !FdoExtension->PollActive)
    {
        LARGE_INTEGER DueTime;
        DueTime.QuadPart = -10000LL;
        FdoExtension->PollActive = TRUE;
        KeSetTimerEx(&FdoExtension->PollTimer, DueTime, 1, &FdoExtension->PollDpc);
    }
    KeReleaseSpinLock(&FdoExtension->PollLock, OldIrql);
}

static
VOID
SdPortPollRelease(
    _In_ PSDPORT_FDO_EXTENSION FdoExtension)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&FdoExtension->PollLock, &OldIrql);
    if (FdoExtension->PollOutstanding != 0 &&
        --FdoExtension->PollOutstanding == 0 &&
        FdoExtension->PollActive)
    {
        FdoExtension->PollActive = FALSE;
        KeCancelTimer(&FdoExtension->PollTimer);
    }
    KeReleaseSpinLock(&FdoExtension->PollLock, OldIrql);
}

static
VOID
NTAPI
SdPortTimeoutDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PSDPORT_WORK_REQUEST Work = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    SdPortCompleteRequest(&Work->Public, STATUS_IO_TIMEOUT);
}

NTSTATUS
NTAPI
SdPortGetProperty(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Inout_ PSDPORT_REQUEST Request)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    ULONG Value = 0;

    switch (Request->Data.Property.PropertyId)
    {
        case SdPropertySlotCount:
            Value = Slot->FdoExtension->SlotCount;
            break;

        case SdPropertyCardDetect:
            Value = (InitData->GetCardDetectState != NULL) ?
                    InitData->GetCardDetectState(Slot->PrivateExtension) : TRUE;
            break;

        case SdPropertyWriteProtect:
            Value = (InitData->GetWriteProtectState != NULL) ?
                    InitData->GetWriteProtectState(Slot->PrivateExtension) : FALSE;
            break;

        case SdPropertyBusWidth:
            Value = Slot->BusWidth;
            break;

        case SdPropertyClock:
            Value = Slot->ClockFrequencyKhz;
            break;

        case SdPropertyMaxBlockCount:
            Value = Slot->Capabilities.MaximumBlockCount;
            break;

        case SdPropertyCapabilities0:
            Value = *(PULONG)&Slot->Capabilities.Flags;
            break;

        case SdPropertyHighSpeed:
            Value = Slot->Capabilities.Flags.HighSpeed;
            break;

        case SdPropertyHighCapacity:
            Value = Slot->HighCapacity;
            break;

        case SdPropertyRca:
            Value = Slot->Rca;
            break;

        case SdPropertyCid:
        case SdPropertyCsd:
        {
            PUCHAR Source = (Request->Data.Property.PropertyId == SdPropertyCid) ?
                            Slot->Cid : Slot->Csd;

            if (Request->Data.Property.OutputBuffer == NULL ||
                Request->Data.Property.OutputBufferLength < sizeof(Slot->Cid))
            {
                Request->Status = STATUS_BUFFER_TOO_SMALL;
                return STATUS_BUFFER_TOO_SMALL;
            }

            RtlCopyMemory(Request->Data.Property.OutputBuffer, Source, sizeof(Slot->Cid));
            Request->BytesTransferred = sizeof(Slot->Cid);
            Request->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }

        default:
            Request->Status = STATUS_INVALID_PARAMETER;
            return STATUS_INVALID_PARAMETER;
    }

    if (Request->Data.Property.OutputBuffer == NULL ||
        Request->Data.Property.OutputBufferLength < sizeof(ULONG))
    {
        Request->Status = STATUS_BUFFER_TOO_SMALL;
        return STATUS_BUFFER_TOO_SMALL;
    }

    *(PULONG)Request->Data.Property.OutputBuffer = Value;
    Request->BytesTransferred = sizeof(ULONG);
    Request->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static
NTSTATUS
SdPortSubmitCommand(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _In_ PIRP Irp,
    _In_ PSDPORT_REQUEST UserRequest)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    PSDPORT_WORK_REQUEST Work;
    KIRQL OldIrql;
    LARGE_INTEGER DueTime;
    NTSTATUS Status;

    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), SDPORT_POOL_TAG);
    if (Work == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Work, sizeof(*Work));
    RtlCopyMemory(&Work->Public, UserRequest, sizeof(SDPORT_REQUEST));
    Work->Slot = Slot;
    Work->Irp = Irp;
    Work->UserRequest = UserRequest;
    KeInitializeEvent(&Work->CompletionEvent, NotificationEvent, FALSE);
    KeInitializeTimer(&Work->TimeoutTimer);
    KeInitializeDpc(&Work->TimeoutDpc, SdPortTimeoutDpc, Work);

    KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
    InsertTailList(&Slot->ActiveRequests, &Work->QueueEntry);
    KeReleaseSpinLock(&Slot->QueueLock, OldIrql);

    SdPortPollAcquire(Slot->FdoExtension);

    IoMarkIrpPending(Irp);

    if (!Slot->CrashdumpMode)
    {
        LONG TimeoutMs = (UserRequest->Type == SdRequestCommandWithTransfer) ?
                         SDPORT_TRANSFER_TIMEOUT_MS : SDPORT_COMMAND_TIMEOUT_MS;
        DueTime.QuadPart = -10000LL * TimeoutMs;
        Work->TimerArmed = TRUE;
        KeSetTimer(&Work->TimeoutTimer, DueTime, &Work->TimeoutDpc);
    }

    Status = InitData->IssueRequest(Slot->PrivateExtension, &Work->Public);

    /* The internal completion guard dedups against a miniport that already
       completed the request from inside IssueRequest. */
    if (Status != STATUS_PENDING)
        SdPortCompleteRequest(&Work->Public, Status);

    return STATUS_PENDING;
}

NTSTATUS
NTAPI
SdPortDispatchInternalDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PSDPORT_SLOT_EXTENSION Slot = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSDPORT_INITIALIZATION_DATA InitData;
    PSDPORT_REQUEST Request;
    NTSTATUS Status;

    if (Slot->ExtensionType != SdportExtensionPdo)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IoStack->Parameters.DeviceIoControl.IoControlCode != IOCTL_SDPORT_SUBMIT_REQUEST)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(SDPORT_REQUEST))
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    Status = IoAcquireRemoveLock(&Slot->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    Request = IoStack->Parameters.DeviceIoControl.Type3InputBuffer;
    InitData = Slot->FdoExtension->InitData;

    switch (Request->Type)
    {
        case SdRequestGetProperty:
            Status = SdPortGetProperty(Slot, Request);
            Irp->IoStatus.Status = Status;
            Irp->IoStatus.Information = Request->BytesTransferred;
            IoReleaseRemoveLock(&Slot->RemoveLock, Irp);
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        case SdRequestSetProperty:
            if (InitData->IssueBusOperation != NULL)
                Status = InitData->IssueBusOperation(Slot->PrivateExtension,
                                                     &Request->Data.BusOperation);
            else
                Status = STATUS_NOT_SUPPORTED;
            Request->Status = Status;
            Irp->IoStatus.Status = Status;
            IoReleaseRemoveLock(&Slot->RemoveLock, Irp);
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        case SdRequestCommand:
        case SdRequestCommandWithTransfer:
            return SdPortSubmitCommand(Slot, Irp, Request);

        default:
            Status = STATUS_INVALID_PARAMETER;
            Request->Status = Status;
            Irp->IoStatus.Status = Status;
            IoReleaseRemoveLock(&Slot->RemoveLock, Irp);
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
    }
}

NTSTATUS
NTAPI
SdPortDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PSDPORT_FDO_EXTENSION FdoExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (IoStack->MajorFunction == IRP_MJ_CREATE ||
        IoStack->MajorFunction == IRP_MJ_CLOSE ||
        IoStack->MajorFunction == IRP_MJ_CLEANUP)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    if (FdoExtension->ExtensionType == SdportExtensionFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(FdoExtension->LowerDevice, Irp);
    }

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

BOOLEAN
NTAPI
SdPortInterruptRoutine(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    PSDPORT_FDO_EXTENSION FdoExtension = ServiceContext;
    PSDPORT_INITIALIZATION_DATA InitData = FdoExtension->InitData;
    BOOLEAN Handled = FALSE;
    UCHAR Index;

    UNREFERENCED_PARAMETER(Interrupt);

    for (Index = 0; Index < FdoExtension->SlotCount; Index++)
    {
        PSDPORT_SLOT_EXTENSION Slot = FdoExtension->Slots[Index];
        ULONG Events = 0;
        ULONG Errors = 0;
        BOOLEAN CardChange = FALSE;
        BOOLEAN SdioInterrupt = FALSE;
        BOOLEAN Tuning = FALSE;

        if (Slot == NULL)
            continue;

        if (InitData->Interrupt(Slot->PrivateExtension,
                                &Events,
                                &Errors,
                                &CardChange,
                                &SdioInterrupt,
                                &Tuning))
        {
            Handled = TRUE;

            if (Events != 0)
                InterlockedOr(&Slot->IsrEvents, (LONG)Events);
            if (Errors != 0)
                InterlockedOr(&Slot->IsrErrors, (LONG)Errors);
            if (CardChange)
                InterlockedOr(&Slot->PendingEvents, SDPORT_EVENT_CARD_CHANGE);
            if (SdioInterrupt)
                InterlockedOr(&Slot->PendingEvents, SDPORT_EVENT_SDIO);
            if (Tuning)
                InterlockedOr(&Slot->PendingEvents, SDPORT_EVENT_TUNING);
        }
    }

    if (Handled)
        KeInsertQueueDpc(&FdoExtension->IsrDpc, NULL, NULL);

    return Handled;
}

VOID
NTAPI
SdPortIsrDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PSDPORT_FDO_EXTENSION FdoExtension = Context;
    PSDPORT_INITIALIZATION_DATA InitData = FdoExtension->InitData;
    UCHAR Index;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    for (Index = 0; Index < FdoExtension->SlotCount; Index++)
    {
        PSDPORT_SLOT_EXTENSION Slot = FdoExtension->Slots[Index];
        ULONG Events;
        ULONG Errors;
        ULONG Pending;
        KIRQL OldIrql;
        PLIST_ENTRY Entry;

        if (Slot == NULL)
            continue;

        Events = (ULONG)InterlockedExchange(&Slot->IsrEvents, 0);
        Errors = (ULONG)InterlockedExchange(&Slot->IsrErrors, 0);
        Pending = (ULONG)InterlockedExchange(&Slot->PendingEvents, 0);

        if (Events != 0 || Errors != 0)
        {
            KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
            Entry = Slot->ActiveRequests.Flink;
            while (Entry != &Slot->ActiveRequests)
            {
                PSDPORT_WORK_REQUEST Work = CONTAINING_RECORD(Entry, SDPORT_WORK_REQUEST, QueueEntry);
                Entry = Entry->Flink;
                KeReleaseSpinLock(&Slot->QueueLock, OldIrql);

                if (InitData->RequestDpc != NULL)
                    InitData->RequestDpc(Slot->PrivateExtension, &Work->Public, Events, Errors);

                KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
            }
            KeReleaseSpinLock(&Slot->QueueLock, OldIrql);
        }

        if (Pending & SDPORT_EVENT_CARD_CHANGE)
            IoInvalidateDeviceRelations(FdoExtension->Pdo, BusRelations);

        if ((Pending & SDPORT_EVENT_TUNING) && InitData->IssueBusOperation != NULL)
        {
            SDPORT_BUS_OPERATION BusOperation;
            RtlZeroMemory(&BusOperation, sizeof(BusOperation));
            BusOperation.Type = SdExecuteTuning;
            InitData->IssueBusOperation(Slot->PrivateExtension, &BusOperation);
        }
    }
}

VOID
NTAPI
SdPortCompletionDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PSDPORT_WORK_REQUEST Work = SystemArgument1;
    PSDPORT_SLOT_EXTENSION Slot;
    PSDPORT_INITIALIZATION_DATA InitData;
    PIRP Irp;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (Work == NULL)
        return;

    Slot = Work->Slot;
    InitData = Slot->FdoExtension->InitData;
    Irp = Work->Irp;

    KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
    RemoveEntryList(&Work->QueueEntry);
    KeReleaseSpinLock(&Slot->QueueLock, OldIrql);

    SdPortPollRelease(Slot->FdoExtension);

    if (NT_SUCCESS(Work->Public.Status) &&
        Work->Public.Data.Command.ResponseType != SdResponseTypeNone &&
        InitData->GetResponse != NULL)
    {
        InitData->GetResponse(Slot->PrivateExtension,
                              &Work->Public.Data.Command,
                              Work->Public.ResponseBuffer);
    }

    Work->UserRequest->Status = Work->Public.Status;
    Work->UserRequest->BytesTransferred = Work->Public.BytesTransferred;
    RtlCopyMemory(Work->UserRequest->ResponseBuffer,
                  Work->Public.ResponseBuffer,
                  sizeof(Work->Public.ResponseBuffer));

    Irp->IoStatus.Status = Work->Public.Status;
    Irp->IoStatus.Information = Work->Public.BytesTransferred;

    IoReleaseRemoveLock(&Slot->RemoveLock, Irp);
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);

    ExFreePoolWithTag(Work, SDPORT_POOL_TAG);
}

/* Harvest and dispatch controller events without the ISR. The host on some
   platforms does not deliver the controller line interrupt, so the synchronous
   bring-up path drives completion by polling the same miniport callbacks the
   ISR and its DPC use. */
static
VOID
SdPortPumpSlot(
    _In_ PSDPORT_SLOT_EXTENSION Slot)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    ULONG Events = 0;
    ULONG Errors = 0;
    BOOLEAN CardChange = FALSE;
    BOOLEAN SdioInterrupt = FALSE;
    BOOLEAN Tuning = FALSE;
    KIRQL OldIrql;
    PLIST_ENTRY Entry;

    if (!InitData->Interrupt(Slot->PrivateExtension, &Events, &Errors,
                             &CardChange, &SdioInterrupt, &Tuning))
        return;

    if (Events == 0 && Errors == 0)
        return;

    KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
    Entry = Slot->ActiveRequests.Flink;
    while (Entry != &Slot->ActiveRequests)
    {
        PSDPORT_WORK_REQUEST Work = CONTAINING_RECORD(Entry, SDPORT_WORK_REQUEST, QueueEntry);
        Entry = Entry->Flink;
        KeReleaseSpinLock(&Slot->QueueLock, OldIrql);

        if (InitData->RequestDpc != NULL)
            InitData->RequestDpc(Slot->PrivateExtension, &Work->Public, Events, Errors);

        KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
    }
    KeReleaseSpinLock(&Slot->QueueLock, OldIrql);
}

VOID
NTAPI
SdPortPollDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PSDPORT_FDO_EXTENSION FdoExtension = Context;
    UCHAR Index;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (FdoExtension == NULL)
        return;

    for (Index = 0; Index < FdoExtension->SlotCount; Index++)
    {
        PSDPORT_SLOT_EXTENSION Slot = FdoExtension->Slots[Index];
        if (Slot != NULL)
            SdPortPumpSlot(Slot);
    }
}

NTSTATUS
NTAPI
SdPortIssueSynchronousRequest(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Inout_ PSDPORT_REQUEST Request)
{
    PSDPORT_INITIALIZATION_DATA InitData = Slot->FdoExtension->InitData;
    SDPORT_WORK_REQUEST Work;
    KIRQL OldIrql;
    LARGE_INTEGER Poll;
    ULONG Spins;
    NTSTATUS Status;

    RtlZeroMemory(&Work, sizeof(Work));
    RtlCopyMemory(&Work.Public, Request, sizeof(SDPORT_REQUEST));
    Work.Slot = Slot;
    Work.Internal = TRUE;
    KeInitializeEvent(&Work.CompletionEvent, NotificationEvent, FALSE);

    KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
    InsertTailList(&Slot->ActiveRequests, &Work.QueueEntry);
    KeReleaseSpinLock(&Slot->QueueLock, OldIrql);

    Status = InitData->IssueRequest(Slot->PrivateExtension, &Work.Public);
    if (Status != STATUS_PENDING)
        SdPortCompleteRequest(&Work.Public, Status);

    Poll.QuadPart = -10LL * 1000;
    Status = STATUS_TIMEOUT;
    for (Spins = 0; Spins < SDPORT_TRANSFER_TIMEOUT_MS; Spins++)
    {
        SdPortPumpSlot(Slot);
        Status = KeWaitForSingleObject(&Work.CompletionEvent,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       &Poll);
        if (Status == STATUS_SUCCESS)
            break;
    }
    if (Status != STATUS_SUCCESS)
        Work.Public.Status = STATUS_IO_TIMEOUT;

    KeAcquireSpinLock(&Slot->QueueLock, &OldIrql);
    RemoveEntryList(&Work.QueueEntry);
    KeReleaseSpinLock(&Slot->QueueLock, OldIrql);

    if (NT_SUCCESS(Work.Public.Status) &&
        Work.Public.Data.Command.ResponseType != SdResponseTypeNone &&
        InitData->GetResponse != NULL)
    {
        InitData->GetResponse(Slot->PrivateExtension,
                              &Work.Public.Data.Command,
                              Work.Public.ResponseBuffer);
    }

    RtlCopyMemory(Request, &Work.Public, sizeof(SDPORT_REQUEST));
    return Work.Public.Status;
}
