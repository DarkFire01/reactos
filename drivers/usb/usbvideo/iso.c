/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/iso.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#define NDEBUG
#include "usbvideo.h"

NTSTATUS
USBVideoQueueIsoRead(
    IN PKSPIN Pin,
    IN USBD_PIPE_HANDLE hIsoPipe,
    IN PUCHAR TransferBuffer,
    IN ULONG TransferLength,
    IN PIRP Irp,
    IN PURB Urb,
    IN PFRAME_CONTEXT FrameCtx)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    ULONG Index;
    PIO_STACK_LOCATION IoStack;

    /* get device extension */
    DeviceExtension = Pin->Context;

    if (DeviceExtension->StopStreaming)
    {
        InterlockedIncrement(&DeviceExtension->StoppedStreamingIrps);

        if (InterlockedCompareExchange(&DeviceExtension->StoppedStreamingIrps, 0, URB_POOL_COUNT))
        {
            KeSetEvent(&DeviceExtension->StoppedStreamingEvent, 0, FALSE);
        }
        return STATUS_SUCCESS;
    }


    //DPRINT1("USBVideoQueueIsoRead MaxTransferSize %x dwMaxPayloadTransferSize %x\n", DeviceExtension->MaximumPacketSize, DeviceExtension->dwMaxPayloadTransferSize);

    /* initialize irp */
    IoInitializeIrp(Irp, IoSizeOfIrp(DeviceExtension->LowerDevice->StackSize), DeviceExtension->LowerDevice->StackSize);

    /* set irp members */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    Irp->Flags = 0;
    Irp->UserBuffer = NULL;

    RtlZeroMemory(Urb, GET_ISO_URB_SIZE(ISO_PACKET_COUNT));

   // ASSERT(DeviceExtension->InterfaceInfo->Pipes[0].MaximumPacketSize == DeviceExtension->MaximumPacketSize);

    /* init urb */
    Urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    Urb->UrbIsochronousTransfer.Hdr.Length = GET_ISO_URB_SIZE(ISO_PACKET_COUNT);
    Urb->UrbIsochronousTransfer.PipeHandle = DeviceExtension->hPipe;
    Urb->UrbIsochronousTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_START_ISO_TRANSFER_ASAP;
    Urb->UrbIsochronousTransfer.TransferBufferLength = DeviceExtension->dwMaxPayloadTransferSize * ISO_PACKET_COUNT;
    Urb->UrbIsochronousTransfer.TransferBuffer = TransferBuffer;
    Urb->UrbIsochronousTransfer.NumberOfPackets = ISO_PACKET_COUNT;
    Urb->UrbIsochronousTransfer.StartFrame = 0;

    for (Index = 0; Index < ISO_PACKET_COUNT; Index++)
    {
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = Index * DeviceExtension->dwMaxPayloadTransferSize;
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Length = DeviceExtension->dwMaxPayloadTransferSize;
    }
    /* setup next stack location */
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.Others.Argument1 = Urb;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    Irp->Tail.Overlay.DriverContext[0] = Urb;
    Irp->Tail.Overlay.DriverContext[1] = FrameCtx;

    IoSetCompletionRoutine(Irp, USBVideoIsoReadComplete, (PVOID)Pin, TRUE, TRUE, TRUE);
    return IoCallDriver(DeviceExtension->LowerDevice, Irp);
}

NTSTATUS
NTAPI
USBVideoIsoReadComplete(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context)
{
    ULONG  BytesReceived;
    PUCHAR Data;
    ULONG  Offset = 0;
    PUVC_PAYLOAD_HEADER Hdr;
    UCHAR HeaderLen;
    ULONG Index;
    PKSPIN Pin = (PKSPIN)Context;
    PUSB_VIDEO_DEVICE_EXTENSION  DeviceExtension = Pin->Context;
    PURB Urb = Irp->Tail.Overlay.DriverContext[0];
    PFRAME_CONTEXT Frame = Irp->Tail.Overlay.DriverContext[1];

    /* check for success */
    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        DPRINT1("Irp Failed with %x\n", Irp->IoStatus.Status);
        USBVideoQueueIsoRead(Pin,
                              DeviceExtension->hPipe,
                              (PUCHAR)Urb->UrbIsochronousTransfer.TransferBuffer,
                              32 * 1024,
                              Irp,
                              Urb,
                              Frame);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    BytesReceived = 0;

    for(Index = 0; Index < ISO_PACKET_COUNT; Index++)
    {
        //DPRINT1("Index %u Length %u\n", Index, Urb->UrbIsochronousTransfer.IsoPacket[Index].Length);
        BytesReceived += Urb->UrbIsochronousTransfer.IsoPacket[Index].Length;
    }
    Data         = (PUCHAR)Urb->UrbIsochronousTransfer.TransferBuffer;
    Offset        = 0;
    for(Index = 0; Index < ISO_PACKET_COUNT; Index++)
    {
        if (Urb->UrbIsochronousTransfer.IsoPacket[Index].Status != USBD_STATUS_SUCCESS)
        {
            DPRINT("Status failed for packet %x\n", Urb->UrbIsochronousTransfer.IsoPacket[Index].Status);
            continue;
        }
        Offset = Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset;
        Hdr = (PUVC_PAYLOAD_HEADER)(Data + Offset);
        HeaderLen = Hdr->bHeaderLength;

        /* validate header length */
        if (HeaderLen < 2 || HeaderLen > 12 || Offset + HeaderLen > BytesReceived)
        {
            DPRINT("Invalid packet atIso Index %u Offset %u HeaderLen %u BytesReceived %u\n", Index,
                    Offset, HeaderLen, BytesReceived);
            break;
        }
#if 0
        /* discard frame if err bit is set */
        if (Hdr->ERR) {
            //PUCHAR Value = AllocFunction(sizeof(UCHAR)*2);
            //NTSTATUS Status = USBVideoTransferControlPacket(Pin, Value, sizeof(UCHAR)*2, USBD_TRANSFER_DIRECTION_IN, 0x81, 0x0200, 0x00);
            DPRINT1("ERR bit set at IsoPacket Index %u\n", Index);//, Value[0], Status);
            Frame->FrameStarted = FALSE;
            Frame->FrameSize    = 0;
            break;
        }
#endif
        /* check FID */
        if (Frame->FrameStarted && (Hdr->FID != Frame->LastFid || Hdr->EOF)) {
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

            /* update buffer */
            Frame->FrameSize   += PayloadDataLen;
            Frame->FrameStarted = TRUE;
        }
    }

    /* requeue irp */
    USBVideoQueueIsoRead(Pin,
                          DeviceExtension->hPipe,
                          (PUCHAR)Urb->UrbIsochronousTransfer.TransferBuffer,
                          32 * 1024,
                          Irp,
                          Urb,
                          Frame);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

