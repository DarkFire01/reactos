/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resets, cancelling what is in flight and the command watchdog
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "uaspstor.h"

#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

typedef struct _UASP_RECOVERY_WORK
{
    PIO_WORKITEM WorkItem;
    PUASP_ADAPTER_EXTENSION Adapter;
    UASP_RECOVERY_KIND Kind;
    PVOID Srb;
} UASP_RECOVERY_WORK, *PUASP_RECOVERY_WORK;

/* FUNCTIONS ******************************************************************/

/**
 * @brief Cancels every transfer of every tag that is handed out.
 *
 * The lock is not held across the cancels, since a completion takes it.
 * The queue is frozen first, so a tag freed in between is not handed out again.
 */
static
VOID
UaspCancelBusyRequests(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    ULONG BusyMask = 0;
    KIRQL OldIrql;
    USHORT Index;
    ULONG Kind;

    C_ASSERT(UASP_MAX_REQUESTS <= 32);

    if (Adapter->Requests == NULL)
        return;

    KeAcquireSpinLock(&Adapter->QueueLock, &OldIrql);
    for (Index = 0; Index < Adapter->RequestCount; Index++)
    {
        if (Adapter->Requests[Index].Busy)
            BusyMask |= 1UL << Index;
    }
    KeReleaseSpinLock(&Adapter->QueueLock, OldIrql);

    for (Index = 0; Index < Adapter->RequestCount; Index++)
    {
        if (!(BusyMask & (1UL << Index)))
            continue;

        for (Kind = 0; Kind < UaspTransferMax; Kind++)
            IoCancelIrp(Adapter->Requests[Index].Transfer[Kind].Irp);
    }
}

/**
 * @brief Cancels what is in flight and waits for every tag to come back.
 *
 * @return TRUE when no tag is handed out any more.
 */
BOOLEAN
UaspDrainRequests(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Interval;
    ULONG Waited = 0;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    Interval.QuadPart = -10000LL * 10;

    for (;;)
    {
        if (UaspBusyRequestCount(Adapter) == 0)
            return TRUE;

        if (Waited >= TimeoutMs)
            return FALSE;

        /* Again on every pass, a data stage can be posted after the first cancel */
        UaspCancelBusyRequests(Adapter);

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        Waited += 10;
    }
}

/**
 * @brief Waits for the work items queued against the adapter to finish.
 */
VOID
UaspWaitForWork(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    LARGE_INTEGER Interval;
    ULONG Waited = 0;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    Interval.QuadPart = -10000LL * 10;

    while (Adapter->WorkCount != 0)
    {
        if (Waited >= 2 * UASP_DRAIN_TIMEOUT_MS)
        {
            DPRINT1("%ld work item(s) still running at teardown\n", Adapter->WorkCount);
            return;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        Waited += 10;
    }
}

static
NTSTATUS
NTAPI
UaspEventCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Starts a bulk transfer on a transfer slot that signals an event when done.
 */
static
NTSTATUS
UaspStartEventTransfer(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PUASP_TRANSFER Transfer,
    _In_ USBD_PIPE_HANDLE Pipe,
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _In_ ULONG TransferFlags,
    _In_ PKEVENT Event)
{
    PIO_STACK_LOCATION IoStack;
    PURB Urb = Transfer->Urb;

    RtlZeroMemory(Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));

    Urb->UrbBulkOrInterruptTransfer.Hdr.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    Urb->UrbBulkOrInterruptTransfer.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Urb->UrbBulkOrInterruptTransfer.PipeHandle = Pipe;
    Urb->UrbBulkOrInterruptTransfer.TransferFlags = TransferFlags;
    Urb->UrbBulkOrInterruptTransfer.TransferBufferLength = Length;
    Urb->UrbBulkOrInterruptTransfer.TransferBuffer = Buffer;

    KeClearEvent(Event);
    IoReuseIrp(Transfer->Irp, STATUS_SUCCESS);

    IoStack = IoGetNextIrpStackLocation(Transfer->Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

    USBD_AssignUrbToIoStackLocation(Adapter->UsbdHandle, IoStack, Urb);

    IoSetCompletionRoutine(Transfer->Irp, UaspEventCompletion, Event, TRUE, TRUE, TRUE);

    return IoCallDriver(Adapter->LowerDeviceObject, Transfer->Irp);
}

/**
 * @brief Waits for a transfer started by UaspStartEventTransfer, cancelling it on timeout.
 */
static
NTSTATUS
UaspWaitEventTransfer(
    _In_ PUASP_TRANSFER Transfer,
    _In_ PKEVENT Event,
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -10000LL * TimeoutMs;

    Status = KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        IoCancelIrp(Transfer->Irp);
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
        return STATUS_IO_TIMEOUT;
    }

    Status = Transfer->Irp->IoStatus.Status;
    if (NT_SUCCESS(Status) && !USBD_SUCCESS(Transfer->Urb->UrbHeader.Status))
        Status = STATUS_UNSUCCESSFUL;

    return Status;
}

/**
 * @brief Sends a LOGICAL UNIT RESET task management unit and reads the answer.
 *
 * The answer read is posted before the unit goes out, as on the command path.
 */
static
BOOLEAN
UaspSendLunReset(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR Lun)
{
    PUAS_TASK_MANAGEMENT_IU Task;
    PUAS_RESPONSE_IU Response;
    PUASP_REQUEST Request;
    USBD_PIPE_HANDLE StatusPipe;
    KEVENT CommandEvent, StatusEvent;
    NTSTATUS CommandStatus, Status;
    ULONG Received;
    BOOLEAN Ok = FALSE;

    Request = UaspAcquireInternalRequest(Adapter);
    if (Request == NULL)
    {
        DPRINT1("No free tag for a logical unit reset\n");
        return FALSE;
    }

    KeInitializeEvent(&CommandEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&StatusEvent, NotificationEvent, FALSE);

    Task = (PUAS_TASK_MANAGEMENT_IU)Request->CommandIu;
    RtlZeroMemory(Task, sizeof(UAS_COMMAND_IU));
    Task->Header.Id = UAS_IU_TASK_MANAGEMENT;
    UaspStoreBigEndian16(Task->Header.Tag, Request->Tag);
    Task->Function = UAS_TMF_LOGICAL_UNIT_RESET;
    Task->Lun[1] = Lun;

    RtlZeroMemory(Request->StatusIu, UASP_STATUS_IU_LENGTH);
    StatusPipe = Adapter->StreamsOpen ? Request->StatusStream : Adapter->StatusPipe;

    UaspStartEventTransfer(Adapter,
                           &Request->Transfer[UaspTransferStatus],
                           StatusPipe,
                           Request->StatusIu,
                           UASP_STATUS_IU_LENGTH,
                           USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK,
                           &StatusEvent);

    UaspStartEventTransfer(Adapter,
                           &Request->Transfer[UaspTransferCommand],
                           Adapter->CommandPipe,
                           Task,
                           sizeof(*Task),
                           0,
                           &CommandEvent);

    CommandStatus = UaspWaitEventTransfer(&Request->Transfer[UaspTransferCommand],
                                          &CommandEvent,
                                          UASP_USB_TIMEOUT_MS);
    if (!NT_SUCCESS(CommandStatus))
    {
        DPRINT1("Task management unit for LUN %u never went out (Status 0x%08lx)\n",
                Lun, CommandStatus);
        IoCancelIrp(Request->Transfer[UaspTransferStatus].Irp);
    }

    Status = UaspWaitEventTransfer(&Request->Transfer[UaspTransferStatus],
                                   &StatusEvent,
                                   UASP_INTERNAL_TIMEOUT_MS);

    if (NT_SUCCESS(CommandStatus) && NT_SUCCESS(Status))
    {
        Received = Request->Transfer[UaspTransferStatus].Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        Response = Request->StatusIu;

        if (Received < sizeof(*Response) || Response->Header.Id != UAS_IU_RESPONSE)
        {
            DPRINT1("Logical unit reset answered with unit %u (%lu bytes)\n",
                    Response->Header.Id, Received);
        }
        else if (UaspReadBigEndian16(Response->Header.Tag) != Request->Tag)
        {
            DPRINT1("Logical unit reset answer carries tag %u, expected %u\n",
                    UaspReadBigEndian16(Response->Header.Tag), Request->Tag);
        }
        else
        {
            DPRINT1("Logical unit reset of LUN %u answered with code 0x%02x\n",
                    Lun, Response->ResponseCode);
            Ok = (Response->ResponseCode == UAS_RESPONSE_TMF_COMPLETE ||
                  Response->ResponseCode == UAS_RESPONSE_TMF_SUCCEEDED);
        }
    }
    else if (NT_SUCCESS(CommandStatus))
    {
        DPRINT1("No answer to the logical unit reset (Status 0x%08lx)\n", Status);
    }

    UaspReleaseRequest(Request);

    return Ok;
}

/**
 * @brief Resets the port the device hangs off, which aborts everything on the device.
 */
static
BOOLEAN
UaspResetPort(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    ULONG PortStatus = 0;
    NTSTATUS Status;

    Status = UaspSendInternalIoctl(Adapter, IOCTL_INTERNAL_USB_GET_PORT_STATUS, &PortStatus);
    DPRINT1("Port status 0x%lx (Status 0x%08lx)\n", PortStatus, Status);

    if (NT_SUCCESS(Status) && !(PortStatus & USBD_PORT_CONNECTED))
    {
        DPRINT1("The device is no longer connected, not resetting\n");
        return FALSE;
    }

    Status = UaspSendInternalIoctl(Adapter, IOCTL_INTERNAL_USB_RESET_PORT, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Port reset failed (Status 0x%08lx)\n", Status);
        return FALSE;
    }

    DPRINT1("Port reset done\n");
    return TRUE;
}

static
VOID
NTAPI
UaspRecoveryWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PUASP_RECOVERY_WORK Work = Context;
    PUASP_ADAPTER_EXTENSION Adapter;
    BOOLEAN Ok = FALSE;
    UCHAR Lun = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Work == NULL)
        return;

    Adapter = Work->Adapter;
    if (Work->Srb != NULL)
        Lun = SrbGetLun(Work->Srb);

    DPRINT1("%s reset starts, %lu command(s) in flight\n",
            (Work->Kind == UaspRecoverLun) ? "Logical unit" : "Bus",
            UaspBusyRequestCount(Adapter));

    UaspFreezeQueue(Adapter);

    if (!Adapter->Removing)
    {
        if (!UaspDrainRequests(Adapter, UASP_DRAIN_TIMEOUT_MS))
            DPRINT1("%lu command(s) did not answer the cancel\n", UaspBusyRequestCount(Adapter));

        if (Work->Kind == UaspRecoverLun)
        {
            Ok = UaspSendLunReset(Adapter, Lun);
            if (!Ok)
                DPRINT1("Logical unit reset failed, resetting the port\n");
        }

        if (!Ok)
        {
            Ok = UaspResetPort(Adapter);
            UaspDrainRequests(Adapter, UASP_DRAIN_TIMEOUT_MS);
        }
    }

    UaspThawQueue(Adapter);

    if (Work->Srb != NULL)
        UaspCompleteSrb(Adapter, Work->Srb, Ok ? SRB_STATUS_SUCCESS : SRB_STATUS_ERROR);

    DPRINT1("Reset %s\n", Ok ? "done" : "failed");

    InterlockedExchange(&Adapter->Recovering, 0);
    InterlockedDecrement(&Adapter->WorkCount);

    IoFreeWorkItem(Work->WorkItem);
    ExFreePoolWithTag(Work, TAG_UASP);
}

/**
 * @brief Queues a reset to run at PASSIVE_LEVEL.
 *
 * @param[in] Srb
 * The reset request to complete once it is done, NULL for one the driver started.
 *
 * @return FALSE when a reset is already running or none can be queued.
 */
BOOLEAN
UaspScheduleRecovery(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ UASP_RECOVERY_KIND Kind,
    _In_opt_ PVOID Srb)
{
    PUASP_RECOVERY_WORK Work;

    if (Adapter->Removing || !Adapter->Started)
        return FALSE;

    if (InterlockedCompareExchange(&Adapter->Recovering, 1, 0) != 0)
    {
        DPRINT1("A reset is already running\n");
        return FALSE;
    }

    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), TAG_UASP);
    if (Work == NULL)
    {
        InterlockedExchange(&Adapter->Recovering, 0);
        return FALSE;
    }

    Work->WorkItem = IoAllocateWorkItem(Adapter->DeviceObject);
    if (Work->WorkItem == NULL)
    {
        ExFreePoolWithTag(Work, TAG_UASP);
        InterlockedExchange(&Adapter->Recovering, 0);
        return FALSE;
    }

    Work->Adapter = Adapter;
    Work->Kind = Kind;
    Work->Srb = Srb;

    InterlockedIncrement(&Adapter->WorkCount);
    IoQueueWorkItem(Work->WorkItem, UaspRecoveryWorker, DelayedWorkQueue, Work);

    return TRUE;
}

/**
 * @brief Counts down the timeout of every command and resets the bus when one runs out.
 *
 * The port driver does not time out requests, so the miniport does it itself.
 */
static
VOID
NTAPI
UaspWatchdog(
    _In_ PVOID DeviceExtension)
{
    PUASP_ADAPTER_EXTENSION Adapter = DeviceExtension;
    PUASP_REQUEST Request;
    BOOLEAN Expired = FALSE;
    USHORT ExpiredTag = 0;
    KIRQL OldIrql;
    USHORT Index;

    if (Adapter->Removing || !Adapter->WatchdogArmed)
        return;

    KeAcquireSpinLock(&Adapter->QueueLock, &OldIrql);
    for (Index = 0; Index < Adapter->RequestCount; Index++)
    {
        Request = &Adapter->Requests[Index];

        if (!Request->Busy || Request->Srb == NULL || Request->SecondsLeft == 0)
            continue;

        if (--Request->SecondsLeft == 0 && !Expired)
        {
            Expired = TRUE;
            ExpiredTag = Request->Tag;
        }
    }
    KeReleaseSpinLock(&Adapter->QueueLock, OldIrql);

    if (Expired)
    {
        DPRINT1("Tag %u timed out, resetting the bus\n", ExpiredTag);
        UaspScheduleRecovery(Adapter, UaspRecoverBus, NULL);
    }

    StorPortNotification(RequestTimerCall, Adapter, UaspWatchdog, UASP_WATCHDOG_PERIOD_US);
}

VOID
UaspStartWatchdog(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    Adapter->WatchdogArmed = TRUE;
    StorPortNotification(RequestTimerCall, Adapter, UaspWatchdog, UASP_WATCHDOG_PERIOD_US);
}

VOID
UaspStopWatchdog(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    if (!Adapter->WatchdogArmed)
        return;

    Adapter->WatchdogArmed = FALSE;
    StorPortNotification(RequestTimerCall, Adapter, UaspWatchdog, 0);
}
