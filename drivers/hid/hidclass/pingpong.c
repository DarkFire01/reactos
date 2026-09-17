/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Continuous input reports and the ring that holds them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* REPORT RING ****************************************************************/

/**
 * @brief
 * Gives a ring the memory to hold reports in.
 *
 * @param[out] Ring
 * The ring to set up.
 *
 * @param[in] ReportSize
 * Size of one report, in bytes.
 *
 * @param[in] ReportCount
 * How many reports the ring has to hold.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER when the size does not add up, or
 * STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
HidClass_RingInitialize(
    _Out_ PHIDCLASS_REPORT_RING Ring,
    _In_ ULONG ReportSize,
    _In_ ULONG ReportCount)
{
    ULONG SlotCount;
    ULONG Length;

    RtlZeroMemory(Ring, sizeof(*Ring));

    if (ReportSize == 0 || ReportCount == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* One slot stays empty so a full ring cannot be mistaken for an empty one */
    if (ReportCount == MAXULONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SlotCount = ReportCount + 1;
    if (ReportSize > MAXULONG / SlotCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Length = ReportSize * SlotCount;

    Ring->Reports = ExAllocatePoolWithTag(NonPagedPool, Length, HIDCLASS_TAG);
    if (Ring->Reports == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ring->ReportSize = ReportSize;
    Ring->SlotCount = SlotCount;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Releases the memory of a ring.
 *
 * @param[in,out] Ring
 * The ring to release.
 */
VOID
HidClass_RingFree(
    _Inout_ PHIDCLASS_REPORT_RING Ring)
{
    if (Ring->Reports != NULL)
    {
        ExFreePoolWithTag(Ring->Reports, HIDCLASS_TAG);
    }

    RtlZeroMemory(Ring, sizeof(*Ring));
}

/**
 * @brief
 * Puts a report into a ring, dropping the oldest one when there is no room.
 *
 * @param[in,out] Ring
 * The ring to put the report in.
 *
 * @param[in] Report
 * The report to copy in.
 *
 * @param[in] Length
 * Number of bytes to copy, at most the ring's report size.
 */
VOID
HidClass_RingPut(
    _Inout_ PHIDCLASS_REPORT_RING Ring,
    _In_reads_bytes_(Length) PVOID Report,
    _In_ ULONG Length)
{
    PUCHAR Slot;

    if (Ring->Reports == NULL)
    {
        return;
    }

    Length = min(Length, Ring->ReportSize);

    Slot = Ring->Reports + (Ring->Head * Ring->ReportSize);
    RtlCopyMemory(Slot, Report, Length);

    /* A report shorter than the slot must not show the previous one's tail */
    if (Length < Ring->ReportSize)
    {
        RtlZeroMemory(Slot + Length, Ring->ReportSize - Length);
    }

    Ring->Head = (Ring->Head + 1) % Ring->SlotCount;

    /* Catching the tail means the oldest report has just been overwritten */
    if (Ring->Head == Ring->Tail)
    {
        Ring->Tail = (Ring->Tail + 1) % Ring->SlotCount;
    }
}

/**
 * @brief
 * Takes the oldest report out of a ring.
 *
 * @param[in,out] Ring
 * The ring to take the report from.
 *
 * @param[out] Report
 * Receives the report.
 *
 * @param[in] Length
 * Number of bytes to copy, at most the ring's report size.
 *
 * @return
 * TRUE when a report was handed back, FALSE when the ring is empty.
 */
BOOLEAN
HidClass_RingGet(
    _Inout_ PHIDCLASS_REPORT_RING Ring,
    _Out_writes_bytes_(Length) PVOID Report,
    _In_ ULONG Length)
{
    PUCHAR Slot;

    if (Ring->Reports == NULL || Ring->Head == Ring->Tail)
    {
        return FALSE;
    }

    Slot = Ring->Reports + (Ring->Tail * Ring->ReportSize);
    RtlCopyMemory(Report, Slot, min(Length, Ring->ReportSize));
    Ring->Tail = (Ring->Tail + 1) % Ring->SlotCount;
    return TRUE;
}

/* READ LOOP ******************************************************************/

static
NTSTATUS
NTAPI
HidClass_ReadCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context);

/**
 * @brief
 * Hands one of the reads back to the minidriver.
 *
 * @param[in] PingPong
 * The read to issue.
 *
 * @remarks
 * Called both to prime the loop and from the completion of the previous read,
 * which is what keeps a read outstanding at all times.
 */
static
VOID
HidClass_IssueRead(
    _In_ PHIDCLASS_PING_PONG PingPong)
{
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    PIO_STACK_LOCATION IoStack;
    PIRP Irp;

    FDODeviceExtension = PingPong->FDODeviceExtension;
    Irp = PingPong->Irp;

    IoReuseIrp(Irp, STATUS_SUCCESS);

    /*
     * The minidriver hands back the report as it came off the wire. A client
     * is owed a report id in front of it, so the wire report is read one byte
     * along and the spare byte in front is handed over with it when the device
     * does not number its reports.
     */
    Irp->UserBuffer = (PUCHAR)PingPong->Report + 1;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_READ_REPORT;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = FDODeviceExtension->MaxReportSize;
    IoStack->Parameters.DeviceIoControl.InputBufferLength = 0;

    IoSetCompletionRoutine(Irp, HidClass_ReadCompletion, PingPong, TRUE, TRUE, TRUE);

    IoCallDriver(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject, Irp);
}

/**
 * @brief
 * Drops the count of reads still out and wakes whoever is waiting for the
 * last of them.
 *
 * @param[in] FDODeviceExtension
 * The device the read belonged to.
 */
static
VOID
HidClass_ReadRetired(
    _In_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension)
{
    KIRQL OldIrql;
    BOOLEAN Drained;

    KeAcquireSpinLock(&FDODeviceExtension->ReadLock, &OldIrql);
    ASSERT(FDODeviceExtension->ReadsOutstanding != 0);
    FDODeviceExtension->ReadsOutstanding--;
    Drained = (FDODeviceExtension->ReadsOutstanding == 0);
    KeReleaseSpinLock(&FDODeviceExtension->ReadLock, OldIrql);

    if (Drained)
    {
        KeSetEvent(&FDODeviceExtension->ReadsDrained, IO_NO_INCREMENT, FALSE);
    }
}

/**
 * @brief
 * Re-issues a read once the stand off after an error has run out.
 */
static
VOID
NTAPI
HidClass_BackoffDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PHIDCLASS_PING_PONG PingPong = DeferredContext;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    KIRQL OldIrql;
    BOOLEAN Running;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    FDODeviceExtension = PingPong->FDODeviceExtension;

    KeAcquireSpinLock(&FDODeviceExtension->ReadLock, &OldIrql);
    Running = FDODeviceExtension->ReadsRunning;
    KeReleaseSpinLock(&FDODeviceExtension->ReadLock, OldIrql);

    if (Running)
    {
        HidClass_IssueRead(PingPong);
    }
    else
    {
        HidClass_ReadRetired(FDODeviceExtension);
    }
}

/**
 * @brief
 * Finds the collection a report belongs to and leaves a copy of it with every
 * file object open on that collection.
 *
 * @param[in] FDODeviceExtension
 * The device the report came from.
 *
 * @param[in] ReportID
 * Identifier of the report, zero on a device that does not number them.
 *
 * @param[in] Report
 * The report, with its identifier in front, as a client expects to see it.
 *
 * @param[in] Length
 * Length of @p Report, in bytes.
 */
static
VOID
HidClass_DeliverReport(
    _In_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension,
    _In_ UCHAR ReportID,
    _In_reads_bytes_(Length) PVOID Report,
    _In_ ULONG Length)
{
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;
    PHIDCLASS_FILEOP_CONTEXT FileOp;
    PHIDP_REPORT_IDS ReportDescription;
    PDEVICE_RELATIONS DeviceRelations;
    PIO_STACK_LOCATION IoStack;
    LIST_ENTRY CompletedList;
    PLIST_ENTRY Entry;
    PIRP PendingIrp;
    PUCHAR Address;
    KIRQL OldIrql;
    ULONG Index;
    ULONG Copied;

    DeviceRelations = FDODeviceExtension->DeviceRelations;
    if (DeviceRelations == NULL || Length == 0)
    {
        return;
    }

    ReportDescription = HidClassPDO_GetReportDescriptionByReportID(
                            &FDODeviceExtension->Common.DeviceDescription,
                            ReportID);
    if (ReportDescription == NULL)
    {
        DPRINT1("[HIDCLASS] Report %u belongs to no collection\n", ReportID);
        return;
    }

    InitializeListHead(&CompletedList);

    for (Index = 0; Index < DeviceRelations->Count; Index++)
    {
        PDODeviceExtension = DeviceRelations->Objects[Index]->DeviceExtension;
        if (PDODeviceExtension->CollectionNumber != ReportDescription->CollectionNumber)
        {
            continue;
        }

        KeAcquireSpinLock(&PDODeviceExtension->FileOpLock, &OldIrql);

        for (Entry = PDODeviceExtension->FileOpListHead.Flink;
             Entry != &PDODeviceExtension->FileOpListHead;
             Entry = Entry->Flink)
        {
            FileOp = CONTAINING_RECORD(Entry, HIDCLASS_FILEOP_CONTEXT, FileOpLink);
            if (FileOp->StopInProgress)
            {
                continue;
            }

            /* Nobody is reading, so the report waits in the ring */
            if (IsListEmpty(&FileOp->PendingReadListHead))
            {
                HidClass_RingPut(&FileOp->ReportRing, Report, Length);
                continue;
            }

            PendingIrp = CONTAINING_RECORD(RemoveHeadList(&FileOp->PendingReadListHead),
                                           IRP,
                                           Tail.Overlay.ListEntry);
            InitializeListHead(&PendingIrp->Tail.Overlay.ListEntry);

            /* Losing the cancel routine means the cancel path has the request */
            if (IoSetCancelRoutine(PendingIrp, NULL) == NULL)
            {
                HidClass_RingPut(&FileOp->ReportRing, Report, Length);
                continue;
            }

            IoStack = IoGetCurrentIrpStackLocation(PendingIrp);
            Copied = min(Length, IoStack->Parameters.Read.Length);

            Address = MmGetSystemAddressForMdlSafe(PendingIrp->MdlAddress, NormalPagePriority);
            if (Address == NULL)
            {
                PendingIrp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                PendingIrp->IoStatus.Information = 0;
            }
            else
            {
                RtlCopyMemory(Address, Report, Copied);
                PendingIrp->IoStatus.Status = STATUS_SUCCESS;
                PendingIrp->IoStatus.Information = Copied;
            }

            /*
             * Completing a request under the lock would let the walk be
             * re-entered, so they are all seen to once it is over.
             */
            InsertTailList(&CompletedList, &PendingIrp->Tail.Overlay.ListEntry);
        }

        KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldIrql);
    }

    while (!IsListEmpty(&CompletedList))
    {
        PendingIrp = CONTAINING_RECORD(RemoveHeadList(&CompletedList),
                                       IRP,
                                       Tail.Overlay.ListEntry);
        IoCompleteRequest(PendingIrp, IO_NO_INCREMENT);
    }
}

/**
 * @brief
 * Takes the report a read brought back and starts the next read.
 *
 * @return
 * STATUS_MORE_PROCESSING_REQUIRED, as the IRP belongs to the read loop and is
 * handed straight back to the minidriver rather than completed.
 */
static
NTSTATUS
NTAPI
HidClass_ReadCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PHIDCLASS_PING_PONG PingPong = Context;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    LARGE_INTEGER Backoff;
    NTSTATUS Status;
    KIRQL OldIrql;
    BOOLEAN Running;

    UNREFERENCED_PARAMETER(DeviceObject);

    FDODeviceExtension = PingPong->FDODeviceExtension;
    Status = Irp->IoStatus.Status;

    KeAcquireSpinLock(&FDODeviceExtension->ReadLock, &OldIrql);
    Running = FDODeviceExtension->ReadsRunning;
    KeReleaseSpinLock(&FDODeviceExtension->ReadLock, OldIrql);

    if (NT_SUCCESS(Status) && Irp->IoStatus.Information != 0)
    {
        PUCHAR Wire = (PUCHAR)PingPong->Report + 1;
        ULONG WireLength = (ULONG)Irp->IoStatus.Information;

        if (FDODeviceExtension->UsesReportId)
        {
            /* The wire report already carries its identifier */
            HidClass_DeliverReport(FDODeviceExtension, Wire[0], Wire, WireLength);
        }
        else
        {
            /* The spare byte in front stays zero and stands in for one */
            HidClass_DeliverReport(FDODeviceExtension,
                                   0,
                                   PingPong->Report,
                                   WireLength + 1);
        }
    }

    if (!Running || Status == STATUS_DELETE_PENDING || Status == STATUS_DEVICE_NOT_CONNECTED)
    {
        HidClass_ReadRetired(FDODeviceExtension);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (NT_SUCCESS(Status))
    {
        HidClass_IssueRead(PingPong);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /*
     * A device that keeps failing would otherwise have us spin re-issuing the
     * read, so stand off before trying it again.
     */
    DPRINT1("[HIDCLASS] Read failed with %lx, standing off\n", Status);
    Backoff.QuadPart = Int32x32To64(HIDCLASS_READ_BACKOFF_MS, -10000);
    KeSetTimer(&PingPong->BackoffTimer, Backoff, &PingPong->BackoffDpc);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Works out the largest input report the device can produce, which is the
 * size every read buffer and ring slot is cut to.
 *
 * @param[in] FDODeviceExtension
 * The device to size.
 *
 * @return
 * The report size in bytes, or zero when no collection reports input.
 */
static
ULONG
HidClass_GetMaxReportSize(
    _In_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension)
{
    PHIDP_DEVICE_DESC DeviceDescription;
    ULONG MaxReportSize = 0;
    ULONG Index;

    DeviceDescription = &FDODeviceExtension->Common.DeviceDescription;

    for (Index = 0; Index < DeviceDescription->CollectionDescLength; Index++)
    {
        MaxReportSize = max(MaxReportSize, DeviceDescription->CollectionDesc[Index].InputLength);
    }

    return MaxReportSize;
}

/**
 * @brief
 * Starts the reads that keep the device's reports coming in.
 *
 * @param[in,out] FDODeviceExtension
 * The device to start reading.
 *
 * @return
 * STATUS_SUCCESS, or the reason no read could be started.
 *
 * @remarks
 * A polled device is left alone: its reports are fetched on demand rather
 * than pushed, so there is nothing to keep outstanding.
 */
NTSTATUS
HidClass_StartReads(
    _Inout_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension)
{
    PHIDCLASS_PING_PONG PingPong;
    ULONG Count;
    ULONG Index;
    CCHAR StackSize;

    if (FDODeviceExtension->Common.DriverExtension->DevicesArePolled)
    {
        return STATUS_SUCCESS;
    }

    FDODeviceExtension->MaxReportSize = HidClass_GetMaxReportSize(FDODeviceExtension);
    if (FDODeviceExtension->MaxReportSize == 0)
    {
        DPRINT("[HIDCLASS] Device reports no input, not reading it\n");
        return STATUS_SUCCESS;
    }

    /*
     * A collection is as long as its report only when that report carries an
     * identifier, otherwise the collection is the longer of the two by the
     * byte the parser puts in front.
     */
    FDODeviceExtension->UsesReportId =
        (FDODeviceExtension->Common.DeviceDescription.CollectionDesc[0].InputLength ==
         FDODeviceExtension->Common.DeviceDescription.ReportIDs[0].InputLength);

    StackSize = FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject->StackSize;

    /* Fall back to a single read when there is not room for the full set */
    for (Count = HIDCLASS_PING_PONG_COUNT; Count != 0; Count /= 2)
    {
        PingPong = ExAllocatePoolWithTag(NonPagedPool,
                                         Count * sizeof(*PingPong),
                                         HIDCLASS_TAG);
        if (PingPong == NULL)
        {
            continue;
        }

        RtlZeroMemory(PingPong, Count * sizeof(*PingPong));

        for (Index = 0; Index < Count; Index++)
        {
            PingPong[Index].FDODeviceExtension = FDODeviceExtension;
            KeInitializeTimer(&PingPong[Index].BackoffTimer);
            KeInitializeDpc(&PingPong[Index].BackoffDpc, HidClass_BackoffDpc, &PingPong[Index]);

            /* One byte more than the wire report, to hold its identifier */
            PingPong[Index].Report = ExAllocatePoolWithTag(NonPagedPool,
                                                           FDODeviceExtension->MaxReportSize + 1,
                                                           HIDCLASS_TAG);
            if (PingPong[Index].Report == NULL)
            {
                break;
            }

            RtlZeroMemory(PingPong[Index].Report, FDODeviceExtension->MaxReportSize + 1);

            PingPong[Index].Irp = IoAllocateIrp(StackSize, FALSE);
            if (PingPong[Index].Irp == NULL)
            {
                break;
            }
        }

        if (Index == Count)
        {
            break;
        }

        /* Give back what was built before trying a smaller set */
        for (Index = 0; Index < Count; Index++)
        {
            if (PingPong[Index].Irp != NULL)
            {
                IoFreeIrp(PingPong[Index].Irp);
            }

            if (PingPong[Index].Report != NULL)
            {
                ExFreePoolWithTag(PingPong[Index].Report, HIDCLASS_TAG);
            }
        }

        ExFreePoolWithTag(PingPong, HIDCLASS_TAG);
    }

    if (Count == 0)
    {
        DPRINT1("[HIDCLASS] No memory to keep a read outstanding\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    FDODeviceExtension->PingPong = PingPong;
    FDODeviceExtension->PingPongCount = Count;
    FDODeviceExtension->ReadsRunning = TRUE;
    FDODeviceExtension->ReadsOutstanding = Count;
    KeClearEvent(&FDODeviceExtension->ReadsDrained);

    for (Index = 0; Index < Count; Index++)
    {
        HidClass_IssueRead(&PingPong[Index]);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops the reads and waits for the ones already out to come back.
 *
 * @param[in,out] FDODeviceExtension
 * The device to stop reading.
 */
VOID
HidClass_StopReads(
    _Inout_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension)
{
    PHIDCLASS_PING_PONG PingPong;
    ULONG Count;
    ULONG Index;
    KIRQL OldIrql;

    KeAcquireSpinLock(&FDODeviceExtension->ReadLock, &OldIrql);
    PingPong = FDODeviceExtension->PingPong;
    Count = FDODeviceExtension->PingPongCount;
    FDODeviceExtension->ReadsRunning = FALSE;
    KeReleaseSpinLock(&FDODeviceExtension->ReadLock, OldIrql);

    if (PingPong == NULL)
    {
        return;
    }

    /*
     * Cancelling brings back a read that is sitting on the device. A read
     * waiting out a stand off is retired by its own timer instead, so the
     * timer has to lose the race before the count can settle.
     */
    for (Index = 0; Index < Count; Index++)
    {
        if (KeCancelTimer(&PingPong[Index].BackoffTimer))
        {
            HidClass_ReadRetired(FDODeviceExtension);
        }

        IoCancelIrp(PingPong[Index].Irp);
    }

    KeWaitForSingleObject(&FDODeviceExtension->ReadsDrained,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    for (Index = 0; Index < Count; Index++)
    {
        IoFreeIrp(PingPong[Index].Irp);
        ExFreePoolWithTag(PingPong[Index].Report, HIDCLASS_TAG);
    }

    ExFreePoolWithTag(PingPong, HIDCLASS_TAG);

    FDODeviceExtension->PingPong = NULL;
    FDODeviceExtension->PingPongCount = 0;
}

/* EOF */
