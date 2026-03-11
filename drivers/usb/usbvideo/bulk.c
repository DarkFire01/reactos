/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/pin.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#define NDEBUG
#include "usbvideo.h"


NTSTATUS
USBVideoQueueBulkRead(
    IN PKSPIN Pin,
    IN USBD_PIPE_HANDLE hBulkPipe,
    IN PUCHAR TransferBuffer,
    IN ULONG TransferLength,
    IN PIRP Irp,
    IN PURB Urb)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION IoStack;

    /* get device extension */
    DeviceExtension = Pin->Context;

    if (DeviceExtension->StopStreaming) {
       InterlockedIncrement(&DeviceExtension->StoppedStreamingIrps);
       return STATUS_SUCCESS;
    }

    /* initialize irp */
    IoInitializeIrp(Irp, IoSizeOfIrp(DeviceExtension->LowerDevice->StackSize), DeviceExtension->LowerDevice->StackSize);

    /* set irp members */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    Irp->Flags = 0;
    Irp->UserBuffer = NULL;

    /* build bulk transfer */
    UsbBuildInterruptOrBulkTransferRequest(
        Urb,
        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
        hBulkPipe,
        TransferBuffer,
        NULL,
        TransferLength,
        USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK,
        NULL);

    /* setup next stack location */
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.Others.Argument1 = Urb;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    Irp->Tail.Overlay.DriverContext[0] = Urb;

    IoSetCompletionRoutine(Irp, USBVideoBulkReadComplete, (PVOID)Pin, TRUE, TRUE, TRUE);
    return IoCallDriver(DeviceExtension->LowerDevice, Irp);
}

NTSTATUS
NTAPI
USBVideoBulkReadComplete(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context)
{
    ULONG  BytesReceived;
    PUCHAR Data;
    ULONG  Offset = 0;
    PUVC_PAYLOAD_HEADER Hdr;
    UCHAR HeaderLen;

    PKSPIN Pin = (PKSPIN)Context;
    PUSB_VIDEO_DEVICE_EXTENSION  DeviceExtension = Pin->Context;
    PFRAME_CONTEXT Frame     = DeviceExtension->FrameCtx;
    PURB Urb = Irp->Tail.Overlay.DriverContext[0];

    DPRINT("USBVideoBulkReadComplete: Called, status=%x\n", Irp->IoStatus.Status);

    /* check for success */
    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        DPRINT1("Irp Failed with %x\n", Irp->IoStatus.Status);
        USBVideoQueueBulkRead(Pin,
                              DeviceExtension->hPipe,
                              (PUCHAR)Urb->UrbBulkOrInterruptTransfer.TransferBuffer,
                              32 * 1024,
                              Irp,
                              Urb);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    BytesReceived = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    DPRINT("USBVideoBulkReadComplete: Received %u bytes\n", BytesReceived);
    Data         = (PUCHAR)Urb->UrbBulkOrInterruptTransfer.TransferBuffer;
    Offset        = 0;
    while (Offset < BytesReceived)
    {
        if (Offset + 2 > BytesReceived)
            break;

        Hdr = (PUVC_PAYLOAD_HEADER)(Data + Offset);
        HeaderLen = Hdr->bHeaderLength;

        /* validate header length */
        if (HeaderLen < 2 || HeaderLen > 12 || Offset + HeaderLen > BytesReceived)
            break;

        /* discard frame if err bit is set */
        if (Hdr->ERR) {
            Frame->FrameStarted = FALSE;
            Frame->FrameSize    = 0;
            Offset += HeaderLen;
            continue;
        }
        /* check FID */
        if (Frame->FrameStarted && Hdr->FID != Frame->LastFid) {
            USBVideoDeliverFrame(DeviceExtension,
                            Frame->FrameBuffer,
                            Frame->FrameSize);
            Frame->FrameSize    = 0;
            Frame->FrameStarted = FALSE;
        }
        Frame->LastFid = Hdr->FID;

        /* copy payload */
        ULONG PayloadDataOffset = Offset + HeaderLen;
        ULONG PayloadDataLen    = DeviceExtension->dwMaxPayloadTransferSize - HeaderLen;

        /* validate payload length*/
        if (PayloadDataOffset + PayloadDataLen > BytesReceived)
            PayloadDataLen = BytesReceived - PayloadDataOffset;

        if (PayloadDataLen > 0 && Frame->FrameSize + PayloadDataLen <= Frame->MaxFrameSize) {

            RtlCopyMemory(
                Frame->FrameBuffer + Frame->FrameSize,
                Data + PayloadDataOffset,
                PayloadDataLen);

            /* reset buffer */
            Frame->FrameSize   += PayloadDataLen;
            Frame->FrameStarted = TRUE;
        }

        /* check end of frame bit */
        if (Hdr->EOF && Frame->FrameStarted) {
            USBVideoDeliverFrame(DeviceExtension,
                            Frame->FrameBuffer,
                            Frame->FrameSize);
            /* reset buffer */
            Frame->FrameSize    = 0;
            Frame->FrameStarted = FALSE;
        }

        /* move to next frame */
        Offset += DeviceExtension->dwMaxPayloadTransferSize;
    }

    /* requeue irp */
    USBVideoQueueBulkRead(Pin,
                          DeviceExtension->hPipe,
                          (PUCHAR)Urb->UrbBulkOrInterruptTransfer.TransferBuffer,
                          32 * 1024,
                          Irp,
                          Urb);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

