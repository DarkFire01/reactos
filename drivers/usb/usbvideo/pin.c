/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/pin.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#include "usbvideo.h"

VOID
NTAPI
USBVideoPinReset(
    _In_ PKSPIN Pin)
{
    UNIMPLEMENTED;
}


NTSTATUS
NTAPI
USBVideoPinSetDataFormat(
    _In_ PKSPIN Pin,
    _In_opt_ PKSDATAFORMAT OldFormat,
    _In_opt_ PKSMULTIPLE_ITEM OldAttributeList,
    _In_ const KSDATARANGE* DataRange,
    _In_opt_ const KSATTRIBUTE_LIST* AttributeRange)
{
    return USBVideoSetFormat(Pin, (PKSDATARANGE)DataRange);
}


NTSTATUS
NTAPI
USBVideoPinSetDeviceState(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    ULONG Index;

    /* get device extension */
    DeviceExtension = Pin->Context;

    DPRINT1("USBVideoPinSetDeviceState State %u\n", ToState);

    if (ToState == KSSTATE_RUN)
    {
        for (Index = 0; Index < URB_POOL_COUNT; Index++)
        {
            if (DeviceExtension->PipeType == UsbdPipeTypeIsochronous)
            {
                USBVideoQueueIsoRead(
                    Pin,
                    DeviceExtension->hPipe,
                    DeviceExtension->BulkBuffer[Index],
                    32* 1024,
                    DeviceExtension->Irp[Index],
                    DeviceExtension->Urb[Index],
                    &DeviceExtension->FrameCtx[Index]);

            }
            else
            {
                ASSERT(DeviceExtension->PipeType == UsbdPipeTypeBulk);
                USBVideoQueueBulkRead(
                        Pin,
                        DeviceExtension->hPipe,
                        DeviceExtension->BulkBuffer[Index],
                        32* 1024,
                        DeviceExtension->Irp[Index],
                        DeviceExtension->Urb[Index]);

            }

        }
    }
    return STATUS_SUCCESS;
}


NTSTATUS
USBVideoGetDataRangeIndexForFormatStillImage(
    IN PKSPIN Pin,
    IN PKSDATARANGE ConnectionFormat,
    IN const PKSDATARANGE * DataRanges,
    IN ULONG DataRangesCount,
    OUT UCHAR * FormatIndex,
    OUT UCHAR * FrameIndex,
    OUT ULONG * dwFrameInterval)
{
    UNIMPLEMENTED;
    *FormatIndex = 1;
    *FrameIndex = 1;
    *dwFrameInterval = 3333333;
    return STATUS_SUCCESS;
}

NTSTATUS
USBVideoGetDataRangeIndexForFormat(
    IN PKSPIN Pin,
    IN PKSDATARANGE ConnectionFormat,
    IN const PKSDATARANGE * DataRanges,
    IN ULONG DataRangesCount,
    OUT UCHAR * FormatIndex,
    OUT UCHAR * FrameIndex,
    OUT ULONG * dwFrameInterval)
{
    ULONG DataRangeIndex;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PKS_DATAFORMAT_VIDEOINFOHEADER CurrentDataRange, RequestedFormat;

    /* get device extension */
    DeviceExtension = Pin->Context;

    for(DataRangeIndex = 0; DataRangeIndex < DataRangesCount; DataRangeIndex++)
    {
        if (DataRanges[DataRangeIndex] == NULL)
            continue;

        CurrentDataRange = (PKS_DATAFORMAT_VIDEOINFOHEADER)DataRanges[DataRangeIndex];
        RequestedFormat = (PKS_DATAFORMAT_VIDEOINFOHEADER)ConnectionFormat;

         /* compare guids */
         if (!IsEqualGUIDAligned(&CurrentDataRange->DataFormat.MajorFormat, &ConnectionFormat->MajorFormat) ||
             !IsEqualGUIDAligned(&CurrentDataRange->DataFormat.SubFormat, &ConnectionFormat->SubFormat) ||
             !IsEqualGUIDAligned(&CurrentDataRange->DataFormat.Specifier, &ConnectionFormat->Specifier))
         {
             /* no match */
             continue;
         }
         if (RequestedFormat->VideoInfoHeader.bmiHeader.biHeight == CurrentDataRange->VideoInfoHeader.bmiHeader.biHeight &&
            RequestedFormat->VideoInfoHeader.bmiHeader.biWidth == CurrentDataRange->VideoInfoHeader.bmiHeader.biWidth)
         {
            DPRINT1("Returning bFormatIndex %x bFrameIndex %x dwFrameInterval %x\n",
                DeviceExtension->VideoFormatInfo[DataRangeIndex].bFormatIndex,
                DeviceExtension->VideoFormatInfo[DataRangeIndex].bFrameIndex,
                DeviceExtension->VideoFormatInfo[DataRangeIndex].dwFrameInterval
            );
            *FormatIndex = DeviceExtension->VideoFormatInfo[DataRangeIndex].bFormatIndex;
            *FrameIndex = DeviceExtension->VideoFormatInfo[DataRangeIndex].bFrameIndex;
            *dwFrameInterval = DeviceExtension->VideoFormatInfo[DataRangeIndex].dwFrameInterval;
            return STATUS_SUCCESS;
         }

    }
    //HACK return first available format
    *FormatIndex = DeviceExtension->VideoFormatInfo[0].bFormatIndex;
    *FrameIndex = DeviceExtension->VideoFormatInfo[0].bFrameIndex;
    *dwFrameInterval = DeviceExtension->VideoFormatInfo[0].dwFrameInterval;

    ASSERT(FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS
USBVideoSetStreamingFormat(
    PKSPIN Pin,
    PKSDATAFORMAT ConnectionFormat)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    UCHAR FormatIndex, FrameIndex;
    ULONG dwFrameInterval;
    NTSTATUS Status;
    PURB Urb;
    ULONG Length;
    UCHAR InterfaceNumber;
    UCHAR AlternateSetting;
    VS_PROBE_COMMIT_CONTROL ProbeCommit = {0};

    Status = USBVideoGetDataRangeIndexForFormat(
        Pin,
        ConnectionFormat,
        Pin->Descriptor->PinDescriptor.DataRanges,
        Pin->Descriptor->PinDescriptor.DataRangesCount,
        &FormatIndex,
        &FrameIndex,
        &dwFrameInterval
    );

    if (!NT_SUCCESS(Status))
        return Status;

    Status = USBVideoGetInterfaceDescriptorForStreaming(Pin, &InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        // failed to retrieve interface number
        DPRINT1("USBVideoGetInterfaceDescriptorForStreaming failed\n");
        return Status;
    }

    /* get device extension */
    DeviceExtension = Pin->Context;

    ASSERT(DeviceExtension);
    ASSERT(DeviceExtension->InterfaceHeaderDescriptor);
    switch(DeviceExtension->InterfaceHeaderDescriptor->bcdUVC)
    {
        case 0x0100: // 1.0
            Length = 26;
            break;
        case 0x0110: // 1.1
            Length = 34;
            break;
        case 0x0150: // 1.5
            Length = 48;
            break;
        default:
            DPRINT1("Unknown UVC version: %x\n", DeviceExtension->InterfaceHeaderDescriptor->bcdUVC);
            // fallback to 1.0
            Length = 26;
            break;
    }
    RtlZeroMemory(&ProbeCommit, sizeof(VS_PROBE_COMMIT_CONTROL));
    ProbeCommit.bmHint = 0x0001;
    ProbeCommit.bFormatIndex    = FormatIndex;
    ProbeCommit.bFrameIndex     = FrameIndex;
    ProbeCommit.dwFrameInterval = dwFrameInterval;
    //ProbeCommit.wCompQuality = 10000;

    UCHAR ProbeControl = 0x01;
    USHORT ProbeValue = 0x100;
    Status = USBVideoTransferControlPacket(Pin, &ProbeCommit, Length, USBD_TRANSFER_DIRECTION_OUT, ProbeControl, ProbeValue, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat Probe failed with %x\n", Status);
        return Status;
    }
    /* get current format */
    UCHAR GetCurFormat = 0x81;
    USHORT CurFormatValue = 0x100;
    Status = USBVideoTransferControlPacket(Pin, &ProbeCommit, Length, USBD_TRANSFER_DIRECTION_IN, GetCurFormat, CurFormatValue, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat GetCur failed with %x\n", Status);
        return Status;
    }
    /* todo check if format matches request */
    DPRINT1("dwMaxPayloadTransferSize %x\n", ProbeCommit.dwMaxPayloadTransferSize);
    DeviceExtension->dwMaxPayloadTransferSize = ProbeCommit.dwMaxPayloadTransferSize;
    /* set format */
    UCHAR SetFormat = 0x01;
    USHORT CommitControl = 0x200;
    Status = USBVideoTransferControlPacket(Pin, &ProbeCommit, Length, USBD_TRANSFER_DIRECTION_OUT, SetFormat, CommitControl, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat SetCur failed with %x\n", Status);
        return Status;
    }

    Urb = (PURB)AllocFunction(GET_SELECT_INTERFACE_REQUEST_SIZE(1));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = USBVideoFindStreamingInterfaceDescriptor(Pin, ProbeCommit.dwMaxPayloadTransferSize, InterfaceNumber, &AlternateSetting);
    if (!NT_SUCCESS(Status))
    {
        // failed to retrieve interface number
        DPRINT1("USBVideoFindStreamingInterfaceDescriptor failed\n");
        return Status;
    }
    DPRINT1("InterfaceNumber %x AlternateSetting %x\n", InterfaceNumber, AlternateSetting);

    UsbBuildSelectInterfaceRequest(Urb,
                                   GET_SELECT_INTERFACE_REQUEST_SIZE(1),
                                   DeviceExtension->ConfigurationHandle,
                                   InterfaceNumber,
                                   AlternateSetting);

    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to select interface with %x\n", Status);
        ASSERT(FALSE);
        return Status;
    }

    /* store pipe handle */
    DeviceExtension->PipeType = Urb->UrbSelectInterface.Interface.Pipes[0].PipeType;
    DeviceExtension->hPipe = Urb->UrbSelectInterface.Interface.Pipes[0].PipeHandle;
    DeviceExtension->MaximumPacketSize = Urb->UrbSelectInterface.Interface.Pipes[0].MaximumPacketSize;
    DPRINT1("USBVideoSetFormat success\n");
    return Status;
}


NTSTATUS
USBVideoSetStillImageFormat(
    PKSPIN Pin,
    PKSDATAFORMAT ConnectionFormat)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    UCHAR FormatIndex, FrameIndex;
    ULONG dwFrameInterval;
    NTSTATUS Status;
    PURB Urb;
    UCHAR InterfaceNumber;
    UCHAR AlternateSetting;

    Status = USBVideoGetDataRangeIndexForFormatStillImage(
        Pin,
        ConnectionFormat,
        Pin->Descriptor->PinDescriptor.DataRanges,
        Pin->Descriptor->PinDescriptor.DataRangesCount,
        &FormatIndex,
        &FrameIndex,
        &dwFrameInterval
    );

    if (!NT_SUCCESS(Status))
        return Status;

    Status = USBVideoGetInterfaceDescriptorForStreaming(Pin, &InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        // failed to retrieve interface number
        DPRINT1("USBVideoGetInterfaceDescriptorForStreaming failed\n");
        return Status;
    }
    /* get device extension */
    DeviceExtension = Pin->Context;

    ASSERT(DeviceExtension);
    ASSERT(DeviceExtension->InterfaceHeaderDescriptor);

    STILL_PROBE_COMMIT ProbeCommit;
    RtlZeroMemory(&ProbeCommit, sizeof(STILL_PROBE_COMMIT));
    ProbeCommit.bFormatIndex = FormatIndex;
    ProbeCommit.bFrameIndex = FrameIndex;

    UCHAR ProbeControl = 0x01;
    USHORT ProbeValue = 0x300;
    //ProbeControl = 0x3;

    Status = USBVideoTransferControlPacket(Pin, &ProbeCommit, sizeof(ProbeCommit), USBD_TRANSFER_DIRECTION_OUT, ProbeControl, ProbeValue, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat Probe failed with %x\n", Status);
        return Status;
    }
    /* get current format */
    UCHAR GetCurFormat = 0x81;
    USHORT CurFormatValue = 0x300;
    Status = USBVideoTransferControlPacket(Pin, &ProbeCommit, sizeof(ProbeCommit), USBD_TRANSFER_DIRECTION_IN, GetCurFormat, CurFormatValue, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat GetCur failed with %x\n", Status);
        return Status;
    }

    /* todo check if format matches request */
    DPRINT1("dwMaxPayloadTransferSize %x\n", ProbeCommit.dwMaxPayloadTransferSize);
    DeviceExtension->dwMaxPayloadTransferSize = ProbeCommit.dwMaxPayloadTransferSize;
    /* set format */
    UCHAR SetFormat = 0x01;
    USHORT CommitControl = 0x400;
    Status = USBVideoTransferControlPacket(Pin, &ProbeCommit, sizeof(ProbeCommit), USBD_TRANSFER_DIRECTION_OUT, SetFormat, CommitControl, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat SetCur failed with %x\n", Status);
        return Status;
    }
    /* set format */
    UCHAR KeyFrame = 1;
    Status = USBVideoTransferControlPacket(Pin, &KeyFrame, sizeof(UCHAR), USBD_TRANSFER_DIRECTION_OUT, 0x01, 0x0500, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat set keyframe failed with %x\n", Status);
        return Status;
    }
    Urb = (PURB)AllocFunction(GET_SELECT_INTERFACE_REQUEST_SIZE(1));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = USBVideoFindStreamingInterfaceDescriptor(Pin, ProbeCommit.dwMaxPayloadTransferSize, InterfaceNumber, &AlternateSetting);
    if (!NT_SUCCESS(Status))
    {
        // failed to retrieve interface number
        DPRINT1("USBVideoFindStreamingInterfaceDescriptor failed\n");
        return Status;
    }
    DPRINT1("InterfaceNumber %x AlternateSetting %x\n", InterfaceNumber, AlternateSetting);

    UsbBuildSelectInterfaceRequest(Urb,
                                   GET_SELECT_INTERFACE_REQUEST_SIZE(1),
                                   DeviceExtension->ConfigurationHandle,
                                   InterfaceNumber,
                                   AlternateSetting);

    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to select interface with %x\n", Status);
        ASSERT(FALSE);
        return Status;
    }

    /* store pipe handle */
    DeviceExtension->PipeType = Urb->UrbSelectInterface.Interface.Pipes[0].PipeType;
    DeviceExtension->hPipe = Urb->UrbSelectInterface.Interface.Pipes[0].PipeHandle;
    DeviceExtension->MaximumPacketSize = Urb->UrbSelectInterface.Interface.Pipes[0].MaximumPacketSize;
    DPRINT1("USBVideoSetStillImageFormat success\n");
    return Status;
}

NTSTATUS
USBVideoSetFormat(
    PKSPIN Pin,
    PKSDATAFORMAT ConnectionFormat)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PKSDEVICE Device;
    PKSFILTER Filter;

    Filter = KsPinGetParentFilter(Pin);
    if (Filter == NULL)
    {
        /* invalid parameter */
        ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    Device = KsFilterGetDevice(Filter);
    if (Device == NULL)
    {
        /* invalid parameter */
        ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    /* get device extension */
    DeviceExtension = Device->Context;

    /* store as context */
    Pin->Context = DeviceExtension;

    /* store pin in context */
    DeviceExtension->Pin = Pin;
    DeviceExtension->StopStreaming = FALSE;

    //USBVideoSetStillImageFormat(Pin, ConnectionFormat);
    return USBVideoSetStreamingFormat(Pin, ConnectionFormat);
}

NTSTATUS
NTAPI
USBVideoPinCreate(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PKSDEVICE Device;
    PKSFILTER Filter;

    Filter = KsPinGetParentFilter(Pin);
    if (Filter == NULL)
    {
        /* invalid parameter */
        ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    Device = KsFilterGetDevice(Filter);
    if (Device == NULL)
    {
        /* invalid parameter */
        ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    /* store context */
    Pin->Context = Device->Context;
    ASSERT(Pin->Context);

    /* get device extension */
    DeviceExtension = Pin->Context;

    /* init frame context */
    DeviceExtension->FrameCtx = AllocFunction(sizeof(FRAME_CONTEXT) * URB_POOL_COUNT);
    if (!DeviceExtension->FrameCtx)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DeviceExtension->Irp = AllocFunction(sizeof(PIRP) * URB_POOL_COUNT);
        if (!DeviceExtension->Irp)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DeviceExtension->Urb = AllocFunction(sizeof(PURB) * URB_POOL_COUNT);
    if (!DeviceExtension->Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DeviceExtension->BulkBuffer = AllocFunction(sizeof(PUCHAR) * URB_POOL_COUNT);
    if (!DeviceExtension->BulkBuffer)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG i = 0; i < URB_POOL_COUNT; i++)
    {
        /* allocate irp */
        PIRP Irp = AllocFunction(IoSizeOfIrp(DeviceExtension->LowerDevice->StackSize));
        if (!Irp)
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        /* initialize irp */
        IoInitializeIrp(Irp, IoSizeOfIrp(DeviceExtension->LowerDevice->StackSize), DeviceExtension->LowerDevice->StackSize);

        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        Irp->Flags = 0;
        Irp->UserBuffer = NULL;
        DeviceExtension->Irp[i] = Irp;

        if (DeviceExtension->PipeType == UsbdPipeTypeIsochronous)
        {
            DeviceExtension->Urb[i] = AllocFunction(GET_ISO_URB_SIZE(ISO_PACKET_COUNT));
        }
        else
        {
            ASSERT(DeviceExtension->PipeType == UsbdPipeTypeBulk);
            DeviceExtension->Urb[i] = AllocFunction(sizeof(URB));
        }
        if (!DeviceExtension->Urb[i])
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DeviceExtension->BulkBuffer[i] = AllocFunction(32 * 1024);
        if (!DeviceExtension->BulkBuffer[i])
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DeviceExtension->FrameCtx[i].FrameBuffer = AllocFunction(720 * 1280 * 3);
        if (!DeviceExtension->FrameCtx[i].FrameBuffer)
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        DeviceExtension->FrameCtx[i].MaxFrameSize = 720 * 1280 * 3;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBVideoPinClose(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PURB Urb;
    UCHAR InterfaceIndex;
    NTSTATUS Status;

    /* get device extension */
    DeviceExtension = Pin->Context;

    /* set stop streaming flag */
    DeviceExtension->StopStreaming = TRUE;

    /* wait for completion */
    KeWaitForSingleObject(&DeviceExtension->StoppedStreamingEvent, Executive, KernelMode, FALSE, NULL);

    /* now abort pipe*/
    Urb = AllocFunction(sizeof(URB));
    if (!Urb)
    {
        /* no memory */
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Urb->UrbHeader.Length   = sizeof(struct _URB_PIPE_REQUEST);
    Urb->UrbHeader.Function = URB_FUNCTION_ABORT_PIPE;
    Urb->UrbPipeRequest.PipeHandle = DeviceExtension->hPipe;
    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        DPRINT1("USBVideoPinClose Status %x\n", Status);
        FreeFunction(Urb);
        Irp->IoStatus.Status = Status;
        return Status;
    }

    Status = USBVideoGetInterfaceDescriptorForStreaming(Pin, &InterfaceIndex);
    ASSERT(NT_SUCCESS(Status));
    UsbBuildSelectInterfaceRequest(Urb,
        GET_SELECT_INTERFACE_REQUEST_SIZE(1),
        DeviceExtension->ConfigurationHandle,
        InterfaceIndex,
        0);

    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    FreeFunction(Urb);
    DPRINT1("USBVideoPinClose Status %x\n", Status);

    /* todo cleanup irps, urbs, etc */
    Irp->IoStatus.Status = Status;
    return Status;
}


NTSTATUS
NTAPI
USBVideoPinProcess(
    _In_ PKSPIN Pin)
{
    /* For pull-mode pins, the KS framework calls this when user buffers are available.
       However, since we're delivering frames directly from the USB completion callback
       (via USBVideoDeliverFrame), this function doesn't need to do much. */
    return STATUS_SUCCESS;
}
