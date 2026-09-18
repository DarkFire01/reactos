/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Talking to the USB stack below us
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "uaspstor.h"

#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

/*
 * A pipe that has to be reset once the thread that noticed can get down to
 * PASSIVE_LEVEL. The work item carries its own memory so several stalls can
 * be outstanding at once.
 */
typedef struct _UASP_RESET_WORK
{
    PIO_WORKITEM WorkItem;
    PUASP_ADAPTER_EXTENSION Adapter;
    USBD_PIPE_HANDLE Pipe;
} UASP_RESET_WORK, *PUASP_RESET_WORK;

/* FUNCTIONS ******************************************************************/

static
NTSTATUS
NTAPI
UaspSynchronousCompletion(
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
 * @brief Sends one request block down and waits for the answer.
 *
 * @param FromUsbdHandle Set when the block came from USBD_UrbAllocate, which
 *                       is how the stack tells those apart from plain ones.
 *
 * Only used while bringing the device up, where commands run one at a time
 * and blocking costs nothing.
 */
NTSTATUS
UaspSendUrbSynchronously(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PURB Urb,
    _In_ BOOLEAN FromUsbdHandle)
{
    PIO_STACK_LOCATION IoStack;
    LARGE_INTEGER Timeout;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoAllocateIrp(Adapter->LowerDeviceObject->StackSize, FALSE);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

    if (FromUsbdHandle)
        USBD_AssignUrbToIoStackLocation(Adapter->UsbdHandle, IoStack, Urb);
    else
        IoStack->Parameters.Others.Argument1 = Urb;

    IoStack->CompletionRoutine = UaspSynchronousCompletion;
    IoStack->Context = &Event;
    IoStack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

    Status = IoCallDriver(Adapter->LowerDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        Timeout.QuadPart = -10000LL * UASP_USB_TIMEOUT_MS;

        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout);
        if (Status == STATUS_TIMEOUT)
        {
            /* Take the request back and let the cancel run to completion */
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = STATUS_IO_TIMEOUT;
        }
        else
        {
            Status = Irp->IoStatus.Status;
        }
    }

    IoFreeIrp(Irp);

    return Status;
}

/**
 * @brief Reads one descriptor from the device into freshly allocated memory.
 *
 * The caller owns the buffer that comes back and frees it with the driver
 * pool tag.
 */
NTSTATUS
UaspGetDescriptor(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ UCHAR DescriptorType,
    _In_ UCHAR Index,
    _In_ ULONG Length,
    _Outptr_result_maybenull_ PVOID *Descriptor)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST Urb;
    NTSTATUS Status;
    PVOID Buffer;

    *Descriptor = NULL;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_UASP);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Buffer, Length);
    RtlZeroMemory(&Urb, sizeof(Urb));

    UsbBuildGetDescriptorRequest((PURB)&Urb,
                                 sizeof(Urb),
                                 DescriptorType,
                                 Index,
                                 0,
                                 Buffer,
                                 NULL,
                                 Length,
                                 NULL);

    Status = UaspSendUrbSynchronously(Adapter, (PURB)&Urb, FALSE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Descriptor %u index %u failed (Status 0x%08lx, USBD 0x%08lx)\n",
                DescriptorType, Index, Status, Urb.Hdr.Status);
        ExFreePoolWithTag(Buffer, TAG_UASP);
        return Status;
    }

    if (Urb.TransferBufferLength == 0)
    {
        ExFreePoolWithTag(Buffer, TAG_UASP);
        return STATUS_DEVICE_DATA_ERROR;
    }

    *Descriptor = Buffer;

    return STATUS_SUCCESS;
}

/**
 * @brief Clears a halt the device raised on one pipe.
 *
 * Both sides have to agree the pipe is running again, so this resets the data
 * toggle here as well as clearing the feature on the device.
 */
NTSTATUS
UaspResetPipe(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ USBD_PIPE_HANDLE Pipe)
{
    struct _URB_PIPE_REQUEST Urb;
    NTSTATUS Status;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    RtlZeroMemory(&Urb, sizeof(Urb));

    Urb.Hdr.Length = sizeof(Urb);
    Urb.Hdr.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
    Urb.PipeHandle = Pipe;

    Status = UaspSendUrbSynchronously(Adapter, (PURB)&Urb, FALSE);
    if (!NT_SUCCESS(Status))
        DPRINT1("Resetting pipe %p failed (Status 0x%08lx)\n", Pipe, Status);

    return Status;
}

static
VOID
NTAPI
UaspResetPipeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PUASP_RESET_WORK Work = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Work == NULL)
        return;

    if (!Work->Adapter->Removing)
        UaspResetPipe(Work->Adapter, Work->Pipe);

    IoFreeWorkItem(Work->WorkItem);
    ExFreePoolWithTag(Work, TAG_UASP);
}

/**
 * @brief Asks for a pipe to be reset from a thread that may do so.
 *
 * A stall is noticed in a completion routine, which is no place to talk to
 * the device, so the work is handed off.
 */
VOID
UaspQueuePipeReset(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ USBD_PIPE_HANDLE Pipe)
{
    PUASP_RESET_WORK Work;

    if (Pipe == NULL || Adapter->Removing)
        return;

    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), TAG_UASP);
    if (Work == NULL)
        return;

    Work->WorkItem = IoAllocateWorkItem(Adapter->DeviceObject);
    if (Work->WorkItem == NULL)
    {
        ExFreePoolWithTag(Work, TAG_UASP);
        return;
    }

    Work->Adapter = Adapter;
    Work->Pipe = Pipe;

    IoQueueWorkItem(Work->WorkItem, UaspResetPipeWorker, DelayedWorkQueue, Work);
}

/**
 * @brief Tells whether a transfer failed because the device halted the pipe.
 */
BOOLEAN
UaspIsPipeStalled(
    _In_ USBD_STATUS UsbdStatus)
{
    switch (UsbdStatus)
    {
        case USBD_STATUS_STALL_PID:
        case USBD_STATUS_ENDPOINT_HALTED:
        case USBD_STATUS_BABBLE_DETECTED:
        case USBD_STATUS_DATA_OVERRUN:
            return TRUE;

        default:
            return FALSE;
    }
}

/**
 * @brief Turns the outcome of a transfer into something the class layer reads.
 */
UCHAR
UaspTranslateStatus(
    _In_ NTSTATUS Status)
{
    if (NT_SUCCESS(Status))
        return SRB_STATUS_SUCCESS;

    switch (Status)
    {
        case STATUS_DEVICE_NOT_CONNECTED:
        case STATUS_NO_SUCH_DEVICE:
        case STATUS_DEVICE_DOES_NOT_EXIST:
            return SRB_STATUS_NO_DEVICE;

        case STATUS_CANCELLED:
            return SRB_STATUS_ABORTED;

        case STATUS_IO_TIMEOUT:
        case STATUS_TIMEOUT:
            return SRB_STATUS_TIMEOUT;

        case STATUS_INSUFFICIENT_RESOURCES:
            return SRB_STATUS_BUSY;

        default:
            return SRB_STATUS_ERROR;
    }
}
