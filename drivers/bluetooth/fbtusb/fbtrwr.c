// Copyright (c) 2004, Antony C. Roberts

// Use of this file is subject to the terms
// described in the LICENSE.TXT file that
// accompanies this file.
//
// Your use of this file indicates your
// acceptance of the terms described in
// LICENSE.TXT.
//
// http://www.freebt.net

#include "fbtusb.h"
#include "fbtpnp.h"
#include "fbtpwr.h"
#include "fbtdev.h"
#include "fbtrwr.h"
#include "fbtwmi.h"

#include "fbtusr.h"

// Completion for both ACL directions, the URB is passed as the context
NTSTATUS
NTAPI
FreeBT_TransferCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PURB                urb;
    NTSTATUS            ntStatus;
    PDEVICE_EXTENSION   deviceExtension;

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;
    urb = (PURB) Context;
    ntStatus = Irp->IoStatus.Status;

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    if (NT_SUCCESS(ntStatus))
    {
        Irp->IoStatus.Information = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_TransferCompletion: %d bytes transferred\n",
                            urb->UrbBulkOrInterruptTransfer.TransferBufferLength));

    }

    else
    {
        Irp->IoStatus.Information = 0;
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_TransferCompletion: Failed with status 0x%08x, URB status 0x%08x\n",
                            ntStatus, urb->UrbHeader.Status));

    }

    ExFreePool(urb);
    FreeBT_IoDecrement(deviceExtension);

    return ntStatus;

}

// Build and submit a bulk transfer over Pipe, described by the MDL the IRP
// already carries. This routine owns the IRP, callers must not touch it after.
NTSTATUS
NTAPI
FreeBT_SubmitTransfer(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PUSBD_PIPE_INFORMATION Pipe,
    _In_ ULONG TransferFlags,
    _In_ ULONG TransferLength)
{
    PURB                urb;
    NTSTATUS            ntStatus;
    PDEVICE_EXTENSION   deviceExtension;
    PIO_STACK_LOCATION  nextStack;

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    urb = (PURB) ExAllocatePool(NonPagedPool, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));
    if (urb == NULL)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_SubmitTransfer: Failed to alloc mem for urb\n"));
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Status = ntStatus;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return ntStatus;

    }

    // USBD splits the transfer into MaximumPacketSize chunks itself, so the
    // whole buffer goes down as one URB and no staging is needed here
    UsbBuildInterruptOrBulkTransferRequest(
                            urb,
                            sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
                            Pipe->PipeHandle,
                            NULL,
                            Irp->MdlAddress,
                            TransferLength,
                            TransferFlags,
                            NULL);

    // Reuse the read/write irp as an internal device control irp
    nextStack = IoGetNextIrpStackLocation(Irp);
    nextStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    nextStack->Parameters.Others.Argument1 = (PVOID) urb;
    nextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

    IoSetCompletionRoutine(Irp,
                           FreeBT_TransferCompletion,
                           urb,
                           TRUE,
                           TRUE,
                           TRUE);

    // Take the count before the call, the completion routine drops it and can
    // run before IoCallDriver returns
    FreeBT_IoIncrement(deviceExtension);
    IoMarkIrpPending(Irp);

    ntStatus = IoCallDriver(deviceExtension->TopOfStackDeviceObject, Irp);
    if (!NT_SUCCESS(ntStatus))
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_SubmitTransfer: IoCallDriver fails with status %X\n", ntStatus));

        // The completion routine has already run and completed the irp, so
        // only the pipe is left to recover
        if ((ntStatus != STATUS_CANCELLED) && (ntStatus != STATUS_DEVICE_NOT_CONNECTED))
        {
            if (!NT_SUCCESS(FreeBT_ResetPipe(DeviceObject, Pipe->PipeHandle)))
            {
                FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_SubmitTransfer: FreeBT_ResetPipe failed\n"));
                FreeBT_ResetDevice(DeviceObject);

            }

        }

    }

    return STATUS_PENDING;

}

NTSTATUS
NTAPI
FreeBT_DispatchRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    ULONG               totalLength;
    NTSTATUS            ntStatus;
    PDEVICE_EXTENSION   deviceExtension;

    totalLength = 0;
    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchRead: Entered\n"));

    if (deviceExtension->DeviceState != Working)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_DispatchRead: Invalid device state\n"));
        ntStatus = STATUS_INVALID_DEVICE_STATE;
        goto FreeBT_DispatchRead_Exit;

    }

    // Park the request while the device is between power states
    if (QueueRequestIfHeld(deviceExtension, Irp))
        return STATUS_PENDING;

    if (deviceExtension->DataInPipe.PipeHandle == NULL)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_DispatchRead: Device has no ACL in pipe\n"));
        ntStatus = STATUS_DEVICE_NOT_READY;
        goto FreeBT_DispatchRead_Exit;

    }

    // Make sure that any selective suspend request has been completed.
    if (deviceExtension->SSEnable)
    {
        FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchRead: Waiting on the IdleReqPendEvent\n"));
        KeWaitForSingleObject(&deviceExtension->NoIdleReqPendEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);

    }

    if (Irp->MdlAddress)
        totalLength = MmGetMdlByteCount(Irp->MdlAddress);

    FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchRead: Transfer data length = %d\n", totalLength));
    if (totalLength == 0)
    {
        ntStatus = STATUS_SUCCESS;
        goto FreeBT_DispatchRead_Exit;

    }

    // A short packet ends the transfer, which is what frames one ACL packet
    // per read
    return FreeBT_SubmitTransfer(DeviceObject,
                                 Irp,
                                 &deviceExtension->DataInPipe,
                                 USBD_SHORT_TRANSFER_OK | USBD_TRANSFER_DIRECTION_IN,
                                 totalLength);

FreeBT_DispatchRead_Exit:
    Irp->IoStatus.Status = ntStatus;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchRead: Leaving\n"));

    return ntStatus;

}

NTSTATUS
NTAPI
FreeBT_DispatchWrite(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    ULONG               totalLength;
    NTSTATUS            ntStatus;
    PDEVICE_EXTENSION   deviceExtension;

    totalLength = 0;
    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchWrite: Entered\n"));

    if (deviceExtension->DeviceState != Working)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_DispatchWrite: Invalid device state\n"));
        ntStatus = STATUS_INVALID_DEVICE_STATE;
        goto FreeBT_DispatchWrite_Exit;

    }

    // Park the request while the device is between power states
    if (QueueRequestIfHeld(deviceExtension, Irp))
        return STATUS_PENDING;

    if (deviceExtension->DataOutPipe.PipeHandle == NULL)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_DispatchWrite: Device has no ACL out pipe\n"));
        ntStatus = STATUS_DEVICE_NOT_READY;
        goto FreeBT_DispatchWrite_Exit;

    }

    // Make sure that any selective suspend request has been completed.
    if (deviceExtension->SSEnable)
    {
        FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchWrite: Waiting on the IdleReqPendEvent\n"));
        KeWaitForSingleObject(&deviceExtension->NoIdleReqPendEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);

    }

    if (Irp->MdlAddress)
        totalLength = MmGetMdlByteCount(Irp->MdlAddress);

    FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchWrite: Transfer data length = %d\n", totalLength));

    // The controller reports its own ACL limit in the buffer size parameters,
    // all that can be enforced here is what the pipe will carry
    if (totalLength > deviceExtension->DataOutPipe.MaximumTransferSize)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_DispatchWrite: Buffer exceeds pipe maximum (%d), failing IRP\n",
                            deviceExtension->DataOutPipe.MaximumTransferSize));
        ntStatus = STATUS_INVALID_BUFFER_SIZE;
        goto FreeBT_DispatchWrite_Exit;

    }

    if (totalLength < FBT_HCI_DATA_MIN_SIZE)
    {
        FreeBT_DbgPrint(1, ("FBTUSB: FreeBT_DispatchWrite: Buffer shorter than an ACL header, completing IRP\n"));
        ntStatus = STATUS_BUFFER_TOO_SMALL;
        goto FreeBT_DispatchWrite_Exit;

    }

    return FreeBT_SubmitTransfer(DeviceObject,
                                 Irp,
                                 &deviceExtension->DataOutPipe,
                                 USBD_TRANSFER_DIRECTION_OUT,
                                 totalLength);

FreeBT_DispatchWrite_Exit:
    Irp->IoStatus.Status = ntStatus;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    FreeBT_DbgPrint(3, ("FBTUSB: FreeBT_DispatchWrite: Leaving\n"));

    return ntStatus;

}
