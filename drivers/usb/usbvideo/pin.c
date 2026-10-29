/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/pin.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#include "usbvideo.h"

extern GUID KSDATAFORMAT_SUBTYPE_MJPEG_LOCAL;

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
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES(NULL, OBJ_KERNEL_HANDLE);

    /* get device extension */
    DeviceExtension = Pin->Context;

    DPRINT1("USBVideoPinSetDeviceState State %u\n", ToState);

    if (ToState == KSSTATE_RUN)
    {
        DeviceExtension->StopStreaming = FALSE;
        KeClearEvent(&DeviceExtension->StopStreamingEvent);
        Status = PsCreateSystemThread(&DeviceExtension->IrpSchedulerThread,
                                      0,
                                      &ObjectAttributes,
                                      NULL,
                                      NULL,
                                      IrpSchedulerThread,
                                      DeviceExtension);
    }
    else if (ToState == KSSTATE_STOP)
    {
        DeviceExtension->StopStreaming = TRUE;
        KeSetEvent(&DeviceExtension->StopStreamingEvent, 0, FALSE);
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
    OUT ULONG * dwFrameInterval,
    OUT PKS_DATARANGE_VIDEO *DataRange)
{
    UNIMPLEMENTED;
    *FormatIndex = 1;
    *FrameIndex = 1;
    *dwFrameInterval = 3333333;
    *DataRange = (PKS_DATARANGE_VIDEO)DataRanges[0];
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
    OUT ULONG * dwFrameInterval,
    OUT PKS_DATARANGE_VIDEO *DataRange)
{
    ULONG DataRangeIndex;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PKS_DATAFORMAT_VIDEOINFOHEADER RequestedFormat;
    PKS_DATARANGE_VIDEO CurrentDataRange;
    /* get device extension */
    DeviceExtension = Pin->Context;

    for(DataRangeIndex = 0; DataRangeIndex < DataRangesCount; DataRangeIndex++)
    {
        if (DataRanges[DataRangeIndex] == NULL)
            continue;

        CurrentDataRange = (PKS_DATARANGE_VIDEO)DataRanges[DataRangeIndex];
        RequestedFormat = (PKS_DATAFORMAT_VIDEOINFOHEADER)ConnectionFormat;

         /* compare guids */
         if (!IsEqualGUIDAligned(&CurrentDataRange->DataRange.MajorFormat, &ConnectionFormat->MajorFormat) ||
             !IsEqualGUIDAligned(&CurrentDataRange->DataRange.SubFormat, &ConnectionFormat->SubFormat) ||
             !IsEqualGUIDAligned(&CurrentDataRange->DataRange.Specifier, &ConnectionFormat->Specifier))
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
            *DataRange = CurrentDataRange;
            return STATUS_SUCCESS;
         }

    }
    //HACK return first available format
    *FormatIndex = DeviceExtension->VideoFormatInfo[2].bFormatIndex;
    *FrameIndex = DeviceExtension->VideoFormatInfo[2].bFrameIndex;
    *dwFrameInterval = DeviceExtension->VideoFormatInfo[2].dwFrameInterval;
    *DataRange = (PKS_DATARANGE_VIDEO)DataRanges[0];
    return STATUS_SUCCESS;
}

VOID
USBVideoSetStreamingDefaults(
    IN PKSPIN Pin,
    IN PKS_DATARANGE_VIDEO DataRange)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = Pin->Context;

    if (DeviceExtension->PipeType == UsbdPipeTypeBulk)
    {
        /* bulk pipe */
        DeviceExtension->BulkTransferSize = 32 * 1024;
        DeviceExtension->FrameContextCount = 1;
        DeviceExtension->UrbPoolCount = 4;
        if (DataRange->VideoInfoHeader.dwBitRate == 0)
        {
            DeviceExtension->FrameContextSize = 512 * 1024;
            DPRINT1("[USBVIDEO] received dwBitRate 0\n");
        }
        else
        {
            DeviceExtension->FrameContextSize = min(DataRange->VideoInfoHeader.dwBitRate, 512 * 1024);
        }

        DeviceExtension->IsMjpegFormat = IsEqualGUIDAligned(&DataRange->DataRange.SubFormat, &KSDATAFORMAT_SUBTYPE_MJPEG_LOCAL);

        DPRINT1("BulkTransferSize %u\n", DeviceExtension->BulkTransferSize);
        DPRINT1("FrameContextCount %u\n", DeviceExtension->FrameContextCount);
        DPRINT1("FrameContextSize %u\n", DeviceExtension->FrameContextSize);
        DPRINT1("UrbPoolCount %u\n", DeviceExtension->UrbPoolCount);
        DPRINT1("IsoPacketCount %u\n", DeviceExtension->IsoPacketCount);
        DPRINT1("IsMjpegFormat %u\n", DeviceExtension->IsMjpegFormat);
    }
    else
    {
        ASSERT(DeviceExtension->PipeType == UsbdPipeTypeIsochronous);
        if (DataRange->bFixedSizeSamples)
        {
            /* uncompressed format have fixed samples */
            ULONG TransferSize = DeviceExtension->dwMaxVideoFrameSize * 2;
            ULONG Payload = DeviceExtension->dwMaxPayloadTransferSize;
            ULONG PacketCount = TransferSize / Payload;
            ULONG Rest = TransferSize % Payload;
            ULONG UrbPoolCount;
            if (Rest != 0)
            {
                PacketCount++;
            }
            PacketCount = ROUND_UP(PacketCount, 8);
            UrbPoolCount = 1;
            TransferSize = PacketCount * Payload;
            DeviceExtension->IsoTransferSize = TransferSize;
            DeviceExtension->FrameContextCount = 1;
            DeviceExtension->FrameContextSize = DeviceExtension->dwMaxVideoFrameSize * 2;
            DeviceExtension->UrbPoolCount = UrbPoolCount;
            DeviceExtension->IsoPacketCount = PacketCount;
            DeviceExtension->IsMjpegFormat = FALSE;
        }
        else
        {
            ULONG TransferSize = DeviceExtension->dwMaxVideoFrameSize * 2;
            ULONG Payload = DeviceExtension->dwMaxPayloadTransferSize;
            ULONG PacketCount = TransferSize / Payload;
            ULONG Rest = TransferSize % Payload;
            ULONG UrbPoolCount;
            if (Rest != 0)
            {
                PacketCount++;
            }
            PacketCount = ROUND_UP(PacketCount, 8);
            UrbPoolCount = 1;
            TransferSize = PacketCount * Payload;
            DeviceExtension->IsoTransferSize = TransferSize;
            DeviceExtension->FrameContextCount = 1;
            DeviceExtension->FrameContextSize = DeviceExtension->dwMaxVideoFrameSize * 2;
            DeviceExtension->UrbPoolCount = UrbPoolCount;
            DeviceExtension->IsoPacketCount = PacketCount;
            DeviceExtension->IsMjpegFormat = IsEqualGUIDAligned(&DataRange->DataRange.SubFormat, &KSDATAFORMAT_SUBTYPE_MJPEG_LOCAL);

        }
        DPRINT1("bFixedSizeSamples %u\n", DataRange->bFixedSizeSamples);
        DPRINT1("dwMaxVideoFrameSize %u\n", DeviceExtension->dwMaxVideoFrameSize);
        DPRINT1("IsoTransferSize %u\n", DeviceExtension->IsoTransferSize);
        DPRINT1("FrameContextCount %u\n", DeviceExtension->FrameContextCount);
        DPRINT1("FrameContextSize %u\n", DeviceExtension->FrameContextSize);
        DPRINT1("UrbPoolCount %u\n", DeviceExtension->UrbPoolCount);
        DPRINT1("IsoPacketCount %u\n", DeviceExtension->IsoPacketCount);
        DPRINT1("IsMjpegFormat %u\n", DeviceExtension->IsMjpegFormat);
    }
}


NTSTATUS
USBVideoSetStreamingFormat(
    PKSPIN Pin,
    PKSDATAFORMAT ConnectionFormat)
{
    PKS_DATARANGE_VIDEO DataRange = NULL;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    UCHAR FormatIndex, FrameIndex;
    ULONG dwFrameInterval;
    NTSTATUS Status;
    PURB Urb;
    ULONG Length;
    UCHAR InterfaceNumber;
    UCHAR AlternateSetting;
    VS_PROBE_COMMIT_CONTROL ProbeCommit;
    ULONG wCompQuality;

    Status = USBVideoGetDataRangeIndexForFormat(
        Pin,
        ConnectionFormat,
        Pin->Descriptor->PinDescriptor.DataRanges,
        Pin->Descriptor->PinDescriptor.DataRangesCount,
        &FormatIndex,
        &FrameIndex,
        &dwFrameInterval,
        &DataRange
    );

    if (!NT_SUCCESS(Status))
        return Status;

    /* get device extension */
    DeviceExtension = Pin->Context;

    Status = USBVideoGetInterfaceDescriptorForStreaming(DeviceExtension, &InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        // failed to retrieve interface number
        DPRINT1("USBVideoGetInterfaceDescriptorForStreaming failed\n");
        return Status;
    }

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

    wCompQuality = 10000; // Max Quality
    //dwFrameInterval = 0x001E8480;
    do
    {
        RtlZeroMemory(&ProbeCommit, sizeof(VS_PROBE_COMMIT_CONTROL));
        ProbeCommit.bmHint = 0x00;
        ProbeCommit.bFormatIndex    = FormatIndex;
        ProbeCommit.bFrameIndex     = FrameIndex;
        ProbeCommit.dwFrameInterval = dwFrameInterval;
        ProbeCommit.wCompQuality = wCompQuality;

        DPRINT1("[USBVIDEO] Probeing with bFormatIndex %u bFrameIndex %u dwFrameFrameInterval %u wCompQuality %u\n", FormatIndex, FrameIndex, dwFrameInterval, wCompQuality);

        UCHAR ProbeControl = 0x01;
        USHORT ProbeValue = 0x100;
        Status = USBVideoTransferControlPacket(DeviceExtension, &ProbeCommit, Length, USBD_TRANSFER_DIRECTION_OUT, ProbeControl, ProbeValue, InterfaceNumber);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("USBVideoSetFormat Probe failed with %x\n", Status);
            if (wCompQuality > 1600)
            {
                wCompQuality -= 1600;
                continue;
            }
            return Status;
        }

        /* get current format */
        UCHAR GetCurFormat = 0x81;
        USHORT CurFormatValue = 0x100;
        Status = USBVideoTransferControlPacket(DeviceExtension, &ProbeCommit, Length, USBD_TRANSFER_DIRECTION_IN, GetCurFormat, CurFormatValue, InterfaceNumber);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("USBVideoSetFormat GetCur failed with %x\n", Status);
            if (wCompQuality > 1600)
            {
                wCompQuality -= 1600;
                continue;
            }
            DPRINT1("[USBVIDEO] Failed to find a valid quality setting\n");
            return Status;
        }
        if (ProbeCommit.dwMaxVideoFrameSize == 0 || ProbeCommit.dwMaxPayloadTransferSize == 0)
        {
            DPRINT1("USBVideoSetFormat dwMaxVideoFrameSize %u dwMaxPayloadTransferSize %u is invalid\n",
                ProbeCommit.dwMaxVideoFrameSize, ProbeCommit.dwMaxPayloadTransferSize);

            if (wCompQuality > 1600)
            {
                wCompQuality -= 1600;
                continue;
            }
            DPRINT1("[USBVIDEO] Failed to find a valid quality setting\n");
            return STATUS_UNSUCCESSFUL;
        }

        /* set format */
        UCHAR SetFormat = 0x01;
        USHORT CommitControl = 0x200;
        Status = USBVideoTransferControlPacket(DeviceExtension, &ProbeCommit, Length, USBD_TRANSFER_DIRECTION_OUT, SetFormat, CommitControl, InterfaceNumber);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("USBVideoSetFormat SetCur failed with %x\n", Status);
            if (wCompQuality > 1600)
            {
                wCompQuality -= 1600;
                continue;
            }
            return Status;
        }

        DPRINT1("bmHint %x\n", ProbeCommit.bmHint);
        DPRINT1("bFormatIndex %x\n", ProbeCommit.bFormatIndex);
        DPRINT1("bFrameIndex %x\n", ProbeCommit.bFrameIndex);
        DPRINT1("dwFrameInterval %x\n", ProbeCommit.dwFrameInterval);
        DPRINT1("wKeyFrameRate %x\n", ProbeCommit.wKeyFrameRate);
        DPRINT1("wPFrameRate %x\n", ProbeCommit.wPFrameRate);
        DPRINT1("wCompQuality %u\n", ProbeCommit.wCompQuality);
        DPRINT1("wCompWindowSize %u\n", ProbeCommit.wCompWindowSize);
        DPRINT1("wDelay %x\n", ProbeCommit.wDelay);
        DPRINT1("dwMaxVideoFrameSize %u\n", ProbeCommit.dwMaxVideoFrameSize);
        DPRINT1("dwMaxPayloadTransferSize %u\n", ProbeCommit.dwMaxPayloadTransferSize);
        DeviceExtension->dwMaxVideoFrameSize = ProbeCommit.dwMaxVideoFrameSize;
        DeviceExtension->dwMaxPayloadTransferSize = ProbeCommit.dwMaxPayloadTransferSize;
        break;
    } while (TRUE);

    Urb = (PURB)AllocFunction(GET_SELECT_INTERFACE_REQUEST_SIZE(1));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = USBVideoFindStreamingInterfaceDescriptor(Pin, ProbeCommit.dwMaxPayloadTransferSize, InterfaceNumber, &AlternateSetting, &DeviceExtension->dwMaxPayloadTransferSize);
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
    DPRINT1("MaximumPacketSize %u\n", DeviceExtension->MaximumPacketSize);
    USBVideoSetStreamingDefaults(Pin, DataRange);
    DPRINT1("USBVideoSetFormat success\n");
    return Status;
}


NTSTATUS
USBVideoSetStillImageFormat(
    PKSPIN Pin,
    PKSDATAFORMAT ConnectionFormat)
{
    PKS_DATARANGE_VIDEO DataRange;
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
        &dwFrameInterval,
        &DataRange
    );

    if (!NT_SUCCESS(Status))
        return Status;

    /* get device extension */
    DeviceExtension = Pin->Context;

    Status = USBVideoGetInterfaceDescriptorForStreaming(DeviceExtension, &InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        // failed to retrieve interface number
        DPRINT1("USBVideoGetInterfaceDescriptorForStreaming failed\n");
        return Status;
    }

    ASSERT(DeviceExtension);
    ASSERT(DeviceExtension->InterfaceHeaderDescriptor);

    STILL_PROBE_COMMIT ProbeCommit;
    RtlZeroMemory(&ProbeCommit, sizeof(STILL_PROBE_COMMIT));
    ProbeCommit.bFormatIndex = FormatIndex;
    ProbeCommit.bFrameIndex = FrameIndex;

    UCHAR ProbeControl = 0x01;
    USHORT ProbeValue = 0x300;
    //ProbeControl = 0x3;

    Status = USBVideoTransferControlPacket(DeviceExtension, &ProbeCommit, sizeof(ProbeCommit), USBD_TRANSFER_DIRECTION_OUT, ProbeControl, ProbeValue, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat Probe failed with %x\n", Status);
        return Status;
    }
    /* get current format */
    UCHAR GetCurFormat = 0x81;
    USHORT CurFormatValue = 0x300;
    Status = USBVideoTransferControlPacket(DeviceExtension, &ProbeCommit, sizeof(ProbeCommit), USBD_TRANSFER_DIRECTION_IN, GetCurFormat, CurFormatValue, InterfaceNumber);
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
    Status = USBVideoTransferControlPacket(DeviceExtension, &ProbeCommit, sizeof(ProbeCommit), USBD_TRANSFER_DIRECTION_OUT, SetFormat, CommitControl, InterfaceNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoSetFormat SetCur failed with %x\n", Status);
        return Status;
    }
    /* set format */
    UCHAR KeyFrame = 1;
    Status = USBVideoTransferControlPacket(DeviceExtension, &KeyFrame, sizeof(UCHAR), USBD_TRANSFER_DIRECTION_OUT, 0x01, 0x0500, InterfaceNumber);
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

    DeviceExtension->dwMaxVideoFrameSize = ProbeCommit.dwMaxVideoFrameSize;
    DeviceExtension->dwMaxPayloadTransferSize = ProbeCommit.dwMaxPayloadTransferSize;

    Status = USBVideoFindStreamingInterfaceDescriptor(Pin, ProbeCommit.dwMaxPayloadTransferSize, InterfaceNumber, &AlternateSetting, &DeviceExtension->dwMaxPayloadTransferSize);
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
    USBVideoSetStreamingDefaults(Pin, DataRange);
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

    if (Pin->Id == 0)
    {
        return USBVideoSetStreamingFormat(Pin, ConnectionFormat);
    }
    else
    {
        return USBVideoSetStillImageFormat(Pin, ConnectionFormat);
    }
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
    DeviceExtension->FrameCtx = AllocFunction(sizeof(FRAME_CONTEXT) * DeviceExtension->FrameContextCount);
    if (!DeviceExtension->FrameCtx)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DeviceExtension->Irp = AllocFunction(sizeof(PIRP) * DeviceExtension->UrbPoolCount);
        if (!DeviceExtension->Irp)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DeviceExtension->Urb = AllocFunction(sizeof(PURB) * DeviceExtension->UrbPoolCount);
    if (!DeviceExtension->Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DeviceExtension->Buffer = AllocFunction(sizeof(PUCHAR) * DeviceExtension->UrbPoolCount);
    if (!DeviceExtension->Buffer)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DeviceExtension->Mdl = AllocFunction(sizeof(PMDL) * DeviceExtension->UrbPoolCount);
    if (!DeviceExtension->Mdl)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for(ULONG i = 0; i < DeviceExtension->FrameContextCount; i++)
    {
        DeviceExtension->FrameCtx[i].FrameBuffer = AllocFunction(DeviceExtension->FrameContextSize);
        if (!DeviceExtension->FrameCtx[i].FrameBuffer)
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        DeviceExtension->FrameCtx[i].MaxFrameSize = DeviceExtension->FrameContextSize;
    }

    for (ULONG i = 0; i < DeviceExtension->UrbPoolCount; i++)
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
            DeviceExtension->Urb[i] = AllocFunction(GET_ISO_URB_SIZE(DeviceExtension->IsoPacketCount));
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

        if (DeviceExtension->PipeType == UsbdPipeTypeIsochronous)
        {
            DeviceExtension->Buffer[i] = AllocFunction(DeviceExtension->IsoTransferSize);
            DeviceExtension->Mdl[i] = IoAllocateMdl(DeviceExtension->Buffer[i], DeviceExtension->IsoTransferSize, FALSE, FALSE, NULL);
            MmBuildMdlForNonPagedPool(DeviceExtension->Mdl[i]);
        }
        else
        {
            ASSERT(DeviceExtension->PipeType == UsbdPipeTypeBulk);
            DeviceExtension->Buffer[i] = AllocFunction(DeviceExtension->BulkTransferSize);
            DeviceExtension->Mdl[i] = IoAllocateMdl(DeviceExtension->Buffer[i], DeviceExtension->BulkTransferSize, FALSE, FALSE, NULL);
            MmBuildMdlForNonPagedPool(DeviceExtension->Mdl[i]);
        }

        if (!DeviceExtension->Buffer[i])
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }

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


    if (!DeviceExtension->StopStreaming || DeviceExtension->StoppedStreamingIrps != DeviceExtension->UrbPoolCount)
    {
        /* set stop streaming flag */
        DeviceExtension->StopStreaming = TRUE;

        DPRINT1("USBVideoPinClose before wait\n");

        /* wait for completion */
        KeWaitForSingleObject(&DeviceExtension->StoppedStreamingEvent, Executive, KernelMode, FALSE, NULL);
    }


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

    Status = USBVideoGetInterfaceDescriptorForStreaming(DeviceExtension, &InterfaceIndex);
    ASSERT(NT_SUCCESS(Status));
    UsbBuildSelectInterfaceRequest(Urb,
        GET_SELECT_INTERFACE_REQUEST_SIZE(1),
        DeviceExtension->ConfigurationHandle,
        InterfaceIndex,
        0);

    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    FreeFunction(Urb);
    DPRINT1("USBVideoPinClose Status %x\n", Status);

    for(ULONG i = 0; i < DeviceExtension->FrameContextCount; i++)
    {
        FreeFunction(DeviceExtension->FrameCtx[i].FrameBuffer);
    }

    for (ULONG i = 0; i < DeviceExtension->UrbPoolCount; i++)
    {
        IoFreeMdl(DeviceExtension->Mdl[i]);
        FreeFunction(DeviceExtension->Irp[i]);
        FreeFunction(DeviceExtension->Urb[i]);
        FreeFunction(DeviceExtension->Buffer[i]);
    }
    FreeFunction(DeviceExtension->Mdl);
    FreeFunction(DeviceExtension->FrameCtx);
    FreeFunction(DeviceExtension->Irp);
    FreeFunction(DeviceExtension->Urb);
    FreeFunction(DeviceExtension->Buffer);

    DeviceExtension->FrameCtx = NULL;
    DeviceExtension->Urb = NULL;
    DeviceExtension->Irp = NULL;
    DeviceExtension->Buffer = NULL;
    DeviceExtension->Mdl = NULL;

    IoStopTimer(DeviceExtension->FunctionalDeviceObject);

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
