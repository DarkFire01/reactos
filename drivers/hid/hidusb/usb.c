/*
 * PROJECT:     ReactOS USB HID Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Synchronous USB requests and recovery from transfer errors
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidusb.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE HidUsbCallUsbCompletion;
static IO_WORKITEM_ROUTINE HidUsbResetWorker;

static
NTSTATUS
NTAPI
HidUsbCallUsbCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Sends a URB down the stack and waits for it.
 *
 * @param[in] DeviceObject
 * The hidclass FDO.
 *
 * @param[in,out] Urb
 * The request. Its header status is filled in by the USB stack.
 *
 * @return
 * STATUS_IO_TIMEOUT when the stack did not finish within five seconds,
 * otherwise the status of the IRP.
 */
NTSTATUS
NTAPI
HidUsbCallUsb(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PURB Urb)
{
    PDEVICE_OBJECT NextDevice;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Timeout;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    NextDevice = HidUsbGetNextDevice(DeviceObject);
    RtlZeroMemory(&IoStatus, sizeof(IoStatus));
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
                                        NextDevice,
                                        NULL,
                                        0,
                                        NULL,
                                        0,
                                        TRUE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoSetCompletionRoutineEx(DeviceObject,
                                      Irp,
                                      HidUsbCallUsbCompletion,
                                      &Event,
                                      TRUE,
                                      TRUE,
                                      TRUE);
    if (!NT_SUCCESS(Status))
    {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoGetNextIrpStackLocation(Irp)->Parameters.Others.Argument1 = Urb;

    Status = IoCallDriver(NextDevice, Irp);
    if (Status != STATUS_PENDING)
    {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    Timeout.QuadPart = HIDUSB_SYNC_URB_TIMEOUT;
    if (KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        DPRINT1("URB %p timed out, canceling\n", Urb);
        IoCancelIrp(Irp);
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        IoStatus.Status = STATUS_IO_TIMEOUT;
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return IoStatus.Status;
}

/**
 * @brief
 * Reads a descriptor over the default pipe.
 *
 * @param[in] Function
 * One of the URB_FUNCTION_GET_DESCRIPTOR_FROM_* codes.
 *
 * @param[in,out] Buffer
 * Caller buffer, or NULL to have one allocated. An allocated buffer is
 * freed again and set back to NULL when the request fails.
 *
 * @param[in,out] BufferLength
 * Size of the buffer on input, bytes transferred on output.
 *
 * @param[in] LanguageId
 * Language of a string, or the wIndex of an interface or endpoint request.
 *
 * @return
 * STATUS_UNSUCCESSFUL when the URB itself failed.
 */
NTSTATUS
NTAPI
HidUsbGetDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ USHORT Function,
    _In_ UCHAR DescriptorType,
    _Inout_ PVOID *Buffer,
    _Inout_ PULONG BufferLength,
    _In_ UCHAR Index,
    _In_ USHORT LanguageId)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST *Request;
    BOOLEAN Allocated;
    ULONG Transferred;
    NTSTATUS Status;

    Request = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Request), HIDUSB_TAG);
    if (Request == NULL)
        return STATUS_NO_MEMORY;

    Allocated = FALSE;
    if (*Buffer == NULL)
    {
        *Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, *BufferLength, HIDUSB_TAG);
        if (*Buffer == NULL)
        {
            ExFreePoolWithTag(Request, HIDUSB_TAG);
            return STATUS_NO_MEMORY;
        }

        Allocated = TRUE;
    }
    else
    {
        RtlZeroMemory(*Buffer, *BufferLength);
    }

    Request->Hdr.Length = sizeof(*Request);
    Request->Hdr.Function = Function;
    Request->TransferBuffer = *Buffer;
    Request->TransferBufferLength = *BufferLength;
    Request->TransferBufferMDL = NULL;
    Request->UrbLink = NULL;
    Request->Index = Index;
    Request->DescriptorType = DescriptorType;
    Request->LanguageId = LanguageId;

    Transferred = 0;
    Status = HidUsbCallUsb(DeviceObject, (PURB)Request);
    if (NT_SUCCESS(Status))
    {
        if (USBD_SUCCESS(Request->Hdr.Status))
        {
            Transferred = Request->TransferBufferLength;
        }
        else
        {
            DPRINT1("Descriptor 0x%x URB failed 0x%lx\n", DescriptorType, Request->Hdr.Status);
            Status = STATUS_UNSUCCESSFUL;
        }
    }

    if (!NT_SUCCESS(Status) && Allocated)
    {
        ExFreePoolWithTag(*Buffer, HIDUSB_TAG);
        *Buffer = NULL;
    }

    *BufferLength = Transferred;
    ExFreePoolWithTag(Request, HIDUSB_TAG);
    return Status;
}

/**
 * @brief
 * Finds the first interrupt pipe of the selected interface going in the given direction.
 */
PUSBD_PIPE_INFORMATION
NTAPI
HidUsbFindInterruptPipe(
    _In_ PHIDUSB_EXTENSION Extension,
    _In_ BOOLEAN DirectionIn)
{
    PUSBD_INTERFACE_INFORMATION Interface;
    PUSBD_PIPE_INFORMATION Pipe;
    ULONG Index;

    Interface = Extension->InterfaceInfo;
    if (Interface == NULL)
        return NULL;

    for (Index = 0; Index < Interface->NumberOfPipes; Index++)
    {
        Pipe = &Interface->Pipes[Index];

        if (Pipe->PipeType != UsbdPipeTypeInterrupt)
            continue;

        if (((Pipe->EndpointAddress & USB_ENDPOINT_DIRECTION_MASK) != 0) == DirectionIn)
            return Pipe;
    }

    return NULL;
}

static
NTSTATUS
NTAPI
HidUsbPipeRequest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ USHORT Function,
    _In_ USBD_PIPE_HANDLE PipeHandle)
{
    struct _URB_PIPE_REQUEST *Request;
    NTSTATUS Status;

    Request = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Request), HIDUSB_TAG);
    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Request->Hdr.Length = sizeof(*Request);
    Request->Hdr.Function = Function;
    Request->PipeHandle = PipeHandle;

    Status = HidUsbCallUsb(DeviceObject, (PURB)Request);
    ExFreePoolWithTag(Request, HIDUSB_TAG);
    return Status;
}

/**
 * @brief
 * Aborts the transfers outstanding on the first pipe of the interface.
 *
 * @return
 * STATUS_NO_SUCH_DEVICE when no interface is configured.
 */
NTSTATUS
NTAPI
HidUsbAbortPendingRequests(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PUSBD_INTERFACE_INFORMATION Interface;
    NTSTATUS Status;

    Interface = HidUsbGetExtension(DeviceObject)->InterfaceInfo;

    if ((Interface == NULL) || (Interface->NumberOfPipes == 0) ||
        (Interface->Pipes[0].PipeHandle == NULL))
    {
        return STATUS_NO_SUCH_DEVICE;
    }

    Status = HidUsbPipeRequest(DeviceObject,
                               URB_FUNCTION_ABORT_PIPE,
                               Interface->Pipes[0].PipeHandle);
    if (!NT_SUCCESS(Status))
        DPRINT1("Abort pipe failed 0x%lx\n", Status);

    return Status;
}

static
NTSTATUS
NTAPI
HidUsbResetInterruptPipe(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PUSBD_PIPE_INFORMATION Pipe;

    Pipe = HidUsbFindInterruptPipe(HidUsbGetExtension(DeviceObject), TRUE);
    if (Pipe == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    return HidUsbPipeRequest(DeviceObject,
                             URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL,
                             Pipe->PipeHandle);
}

static
NTSTATUS
NTAPI
HidUsbPortRequest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID Argument)
{
    PDEVICE_OBJECT NextDevice;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    NextDevice = HidUsbGetNextDevice(DeviceObject);
    RtlZeroMemory(&IoStatus, sizeof(IoStatus));
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        NextDevice,
                                        NULL,
                                        0,
                                        NULL,
                                        0,
                                        TRUE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoGetNextIrpStackLocation(Irp)->Parameters.Others.Argument1 = Argument;

    Status = IoCallDriver(NextDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

/**
 * @brief
 * Completes the pending device reset notification, if hidclass left one with us.
 */
VOID
NTAPI
HidUsbCompleteResetNotification(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ NTSTATUS Status)
{
    PHIDUSB_EXTENSION Extension;
    KIRQL OldIrql;
    PIRP Irp;

    Extension = HidUsbGetExtension(DeviceObject);

    KeAcquireSpinLock(&Extension->ResetNotifyLock, &OldIrql);
    Irp = Extension->ResetNotifyIrp;
    Extension->ResetNotifyIrp = NULL;
    KeReleaseSpinLock(&Extension->ResetNotifyLock, OldIrql);

    if (Irp == NULL)
        return;

    /* The cancel routine finds the slot empty and leaves the IRP to us */
    IoSetCancelRoutine(Irp, NULL);

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
}

/* Port reset sequence run at passive level after a transfer failed */
static
VOID
NTAPI
HidUsbResetWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHIDUSB_RESET_CONTEXT ResetContext;
    PHIDUSB_EXTENSION Extension;
    PDEVICE_OBJECT Fdo;
    BOOLEAN PortWasReset;
    ULONG PortStatus;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    ResetContext = (PHIDUSB_RESET_CONTEXT)Context;
    Fdo = ResetContext->DeviceObject;
    Extension = HidUsbGetExtension(Fdo);
    PortWasReset = FALSE;

    if (NT_SUCCESS(HidUsbIncrementPendingRequests(Extension)))
    {
        PortStatus = 0;
        Status = HidUsbPortRequest(Fdo, IOCTL_INTERNAL_USB_GET_PORT_STATUS, &PortStatus);

        /* A device that is gone is left to PnP */
        if (NT_SUCCESS(Status) && (PortStatus & USBD_PORT_CONNECTED))
        {
            Status = HidUsbAbortPendingRequests(Fdo);
            if (NT_SUCCESS(Status))
            {
                Status = HidUsbPortRequest(Fdo, IOCTL_INTERNAL_USB_RESET_PORT, NULL);
                if (Status == STATUS_DEVICE_DATA_ERROR)
                {
                    DPRINT1("Port reset failed with a data error, failing the device\n");
                    Extension->State = HidUsbStateFailed;
                    IoInvalidateDeviceState(HidUsbGetHidExtension(Fdo)->PhysicalDeviceObject);
                }
                else if (NT_SUCCESS(Status))
                {
                    PortWasReset = TRUE;
                    HidUsbResetInterruptPipe(Fdo);
                }
                else
                {
                    DPRINT1("Port reset failed 0x%lx\n", Status);
                }
            }
        }

        HidUsbDecrementPendingRequests(Extension);
    }

    InterlockedExchangePointer(&Extension->ResetContext, NULL);

    if (PortWasReset)
        HidUsbCompleteResetNotification(Fdo, STATUS_SUCCESS);

    /* The IRP still carries the error that started the reset */
    IoCompleteRequest(ResetContext->Irp, IO_NO_INCREMENT);

    IoFreeWorkItem(ResetContext->WorkItem);
    ExFreePoolWithTag(ResetContext, HIDUSB_TAG);

    HidUsbDecrementPendingRequests(Extension);
    IoReleaseRemoveLock(&Extension->RemoveLock, (PVOID)HIDUSB_RESET_LOCK_TAG);
}

/**
 * @brief
 * Holds back a failed IRP and queues the port reset sequence.
 *
 * @param[in] Irp
 * The failed request. It is completed by the work item.
 *
 * @return
 * STATUS_MORE_PROCESSING_REQUIRED when the work item owns the IRP, otherwise
 * STATUS_SUCCESS and the IRP completes normally. Only one reset runs at a time.
 */
NTSTATUS
NTAPI
HidUsbQueueResetWorkItem(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PHIDUSB_RESET_CONTEXT ResetContext;
    PHIDUSB_EXTENSION Extension;

    Extension = HidUsbGetExtension(DeviceObject);

    if (!NT_SUCCESS(HidUsbIncrementPendingRequests(Extension)))
        return STATUS_SUCCESS;

    ResetContext = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ResetContext), HIDUSB_TAG);
    if (ResetContext == NULL)
    {
        HidUsbDecrementPendingRequests(Extension);
        return STATUS_SUCCESS;
    }

    ResetContext->WorkItem = IoAllocateWorkItem(Extension->DeviceObject);
    if (ResetContext->WorkItem == NULL)
    {
        ExFreePoolWithTag(ResetContext, HIDUSB_TAG);
        HidUsbDecrementPendingRequests(Extension);
        return STATUS_SUCCESS;
    }

    if (InterlockedCompareExchangePointer(&Extension->ResetContext, ResetContext, NULL) != NULL)
    {
        IoFreeWorkItem(ResetContext->WorkItem);
        ExFreePoolWithTag(ResetContext, HIDUSB_TAG);
        HidUsbDecrementPendingRequests(Extension);
        return STATUS_SUCCESS;
    }

    ResetContext->Signature = HIDUSB_RESET_SIGNATURE;
    ResetContext->Irp = Irp;
    ResetContext->DeviceObject = DeviceObject;

    IoMarkIrpPending(Irp);
    IoAcquireRemoveLock(&Extension->RemoveLock, (PVOID)HIDUSB_RESET_LOCK_TAG);
    IoQueueWorkItem(ResetContext->WorkItem, HidUsbResetWorker, DelayedWorkQueue, ResetContext);

    return STATUS_MORE_PROCESSING_REQUIRED;
}
