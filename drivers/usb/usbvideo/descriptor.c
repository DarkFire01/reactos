/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/descriptor.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#include "usbvideo.h"

GUID PKSNODETYPE_VIDEO_CAMERA_TERMINAL = { STATIC_KSNODETYPE_VIDEO_CAMERA_TERMINAL };
GUID PKSNODETYPE_VIDEO_STREAMING = { STATIC_KSNODETYPE_VIDEO_STREAMING };
GUID PKSNODETYPE_VIDEO_PROCESSING = { STATIC_KSNODETYPE_VIDEO_PROCESSING };
GUID PKSNODETYPE_DEV_SPECIFIC = { STATIC_KSNODETYPE_DEV_SPECIFIC };
GUID PPINNAME_CAPTURE = { STATIC_PINNAME_CAPTURE };
GUID PPINNAME_VIDEO_STILL = { STATIC_PINNAME_VIDEO_STILL };
GUID gKSDATAFORMAT_TYPE_VIDEO = { STATIC_KSDATAFORMAT_TYPE_VIDEO};
GUID gKSDATAFORMAT_SPECIFIER_VIDEOINFO = { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO};
GUID gKSDATAFORMAT_SPECIFIER_VIDEOINFO2 = { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO2};
GUID KSDATAFORMAT_SUBTYPE_MJPEG_LOCAL = {0x47504a4d, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
GUID MEDIASUBTYPE_YUY2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

KSPIN_INTERFACE StandardPinInterface =
{
     {STATIC_KSINTERFACESETID_Standard},
     KSINTERFACE_STANDARD_STREAMING,
     0
};

KSPIN_MEDIUM StandardPinMedium =
{
     {STATIC_KSMEDIUMSETID_Standard},
     KSMEDIUM_TYPE_ANYINSTANCE,
     0
};

static KSPIN_DISPATCH UsbVideoPinDispatch =
{
    USBVideoPinCreate,
    USBVideoPinClose,
    USBVideoPinProcess,
    USBVideoPinReset,
    USBVideoPinSetDataFormat,
    USBVideoPinSetDeviceState,
    NULL,
    NULL,
    NULL,
    NULL
};


NTSTATUS
USBVideoFindStreamingInterfaceDescriptor(
    IN PKSPIN Pin,
    IN ULONG dwMaxPayloadTransferSize,
    IN UCHAR InterfaceNumber,
    OUT PUCHAR AlternateSetting)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PUSB_ENDPOINT_DESCRIPTOR EndpointDescriptor;
    ULONG AltSetting;

    /* get device extension */
    DeviceExtension = Pin->Context;

    /* init alt setting */
    AltSetting = 1;

    /* find first descriptor */
    Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor,
        InterfaceNumber, AltSetting,
        USB_DEVICE_CLASS_VIDEO,
        0x02, // INTERFACE_STREAMING_SUBCLASS
        -1
    );

    while(Descriptor != NULL)
    {
        EndpointDescriptor = (PUSB_ENDPOINT_DESCRIPTOR)((ULONG_PTR)Descriptor + Descriptor->bLength);
        ASSERT(EndpointDescriptor->bDescriptorType == 0x05);
        ASSERT(EndpointDescriptor->bLength == 0x07);
        if (EndpointDescriptor->wMaxPacketSize >= dwMaxPayloadTransferSize)
        {
            /* found alternate setting */
            DPRINT1("wMaxPacketSize %x\n", EndpointDescriptor->wMaxPacketSize);
            *AlternateSetting = AltSetting;
            return STATUS_SUCCESS;
        }
        AltSetting++;

        /* find next descriptor */
        Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor,
                                                        InterfaceNumber, AltSetting,
                                                        USB_DEVICE_CLASS_VIDEO,
                                                        0x02, // INTERFACE_STREAMING_SUBCLASS
                                                        -1);
    }
    /* falling back to bulk */
    DPRINT1("Fallback to bulk\n");
    *AlternateSetting = 0;
    return STATUS_SUCCESS;
}



NTSTATUS
USBVideoGetInterfaceDescriptorForStreaming(
    IN PKSPIN Pin,
    OUT PUCHAR InterfaceNumber)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    /* get device extension */
    DeviceExtension = Pin->Context;

    Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor,
        -1, -1,
        USB_DEVICE_CLASS_VIDEO,
        0x02, // INTERFACE_STREAMING_SUBCLASS
        -1
    );
    if (Descriptor)
    {
        *InterfaceNumber = Descriptor->bInterfaceNumber;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}

NTSTATUS
UsbVideoGetDataRangesForStreaming(
    IN PKSDEVICE Device,
    OUT PKSDATARANGE** OutDataRanges,
    OUT PULONG OutDataRangesCount)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PVS_VIDEO_INPUT_HEADER_DESCRIPTOR VideoInputHeaderDescriptor;
    PVC_INTERFACE_COMMON_DESCRIPTOR CommonDescriptor;
    PVS_MJPEG_FORMAT_TYPE_DESCRIPTOR MjpegFormatTypeDescriptor;
    PVS_STILL_IMAGE_FRAME_TYPE_DESCRIPTOR StillImageFormatDescriptor;
    PVS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR UncompressedFormatDescriptor;
    PVS_MJPEG_FRAME_TYPE_DESCRIPTOR MjpegFrameTypeDescriptor;
    PVS_UNCOMPRESSED_FRAME_TYPE_DESCRIPTOR UncompressedFrameTypeDescriptor;
    PKS_DATARANGE_VIDEO DataRangeVideo;
    PKS_DATARANGE_VIDEO2 DataRangeVideo2;
    PKSDATARANGE *DataRangeVideoArray;
    ULONG FormatCount, FormatIndex, VideoFormatIndex;

    /* get device extension */
    DeviceExtension = Device->Context;
    ASSERT(DeviceExtension);
    VideoInputHeaderDescriptor = DeviceExtension->VideoInputHeaderDescriptor;
    CommonDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)((ULONG_PTR)VideoInputHeaderDescriptor + VideoInputHeaderDescriptor->bLength);

    /* enumerate all formats */
    FormatCount = 0;
    do
    {
        if (CommonDescriptor->Common.bDescriptorType == VS_INTERFACE_HEADER_DESCRIPTOR_TYPE)
        {
            MjpegFormatTypeDescriptor = (PVS_MJPEG_FORMAT_TYPE_DESCRIPTOR)CommonDescriptor;
            StillImageFormatDescriptor = (PVS_STILL_IMAGE_FRAME_TYPE_DESCRIPTOR)CommonDescriptor;
            UncompressedFormatDescriptor = (PVS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR)CommonDescriptor;
            if (MjpegFormatTypeDescriptor->bDescriptorSubtype == VS_MJPEG_FORMAT_TYPE_DESCRIPTOR_SUBTYPE)
            {
                DPRINT1("Found MJPEG Format Type descriptor numFormats %u bFormatIndex %u\n", MjpegFormatTypeDescriptor->bNumFrameDescriptors, MjpegFormatTypeDescriptor->bFormatIndex);
                FormatCount += (MjpegFormatTypeDescriptor->bNumFrameDescriptors * 2); // KS_DATARANGE_VIDEO + KS_DATARANGE_VIDEO2
            }
#if 0
            else if (StillImageFormatDescriptor->bDescriptorSubtype == VS_STILL_IMAGE_FRAME_TYPE_DESCRIPTOR_SUBTYPE )
            {
                DPRINT1("Found Still Image Format Type descriptor bNumImageSizePatterns: %u\n", StillImageFormatDescriptor->bNumImageSizePatterns);
                FormatCount += StillImageFormatDescriptor->bNumImageSizePatterns;
                ASSERT(FALSE);
            }
#endif
            else if (UncompressedFormatDescriptor->bDescriptorSubtype == VS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR_SUBTYPE)
            {
                DPRINT1("Found Uncompressed Format Type descriptor bNumFrameDescriptors %u\n", UncompressedFormatDescriptor->bNumFrameDescriptors);
                if (IsEqualGUIDAligned(&UncompressedFormatDescriptor->guidFormat,
                    &MEDIASUBTYPE_YUY2))
                {
                  FormatCount += UncompressedFormatDescriptor->bNumFrameDescriptors * 2;
                }
            }
            CommonDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->Common.bLength);
        }

    } while (((ULONG_PTR)CommonDescriptor) <((ULONG_PTR)VideoInputHeaderDescriptor + VideoInputHeaderDescriptor->wTotalLength));

    DataRangeVideoArray = AllocFunction(sizeof(PVOID) * FormatCount);
    if (DataRangeVideoArray == NULL)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DeviceExtension->VideoFormatInfo = AllocFunction(sizeof(VIDEO_FORMAT_INFO)* FormatCount);
    if (DeviceExtension->VideoFormatInfo == NULL)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* now build datarange video array */
    CommonDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)((ULONG_PTR)VideoInputHeaderDescriptor + VideoInputHeaderDescriptor->bLength);
    FormatIndex = 0;
    VideoFormatIndex = 0;
    do
    {
        if (CommonDescriptor->Common.bDescriptorType == VS_INTERFACE_HEADER_DESCRIPTOR_TYPE)
        {
            if (CommonDescriptor->bDescriptorSubtype == VS_MJPEG_FORMAT_TYPE_DESCRIPTOR_SUBTYPE)
            {
                MjpegFormatTypeDescriptor = (PVS_MJPEG_FORMAT_TYPE_DESCRIPTOR)CommonDescriptor;
                VideoFormatIndex = MjpegFormatTypeDescriptor->bFormatIndex;
            }
            else if (CommonDescriptor->bDescriptorSubtype == VS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR_SUBTYPE)
            {
                UncompressedFormatDescriptor = (PVS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR)CommonDescriptor;
                VideoFormatIndex = UncompressedFormatDescriptor->bFormatIndex;
            }
            if (CommonDescriptor->bDescriptorSubtype == VS_MJPEG_FRAME_TYPE_DESCRIPTOR_SUBTYPE)
            {
                MjpegFrameTypeDescriptor = (PVS_MJPEG_FRAME_TYPE_DESCRIPTOR)CommonDescriptor;

                DPRINT1("MjpegFrameTypeDescriptor ", MjpegFrameTypeDescriptor->bFrameIndex);
                DataRangeVideo = AllocFunction(sizeof(KS_DATARANGE_VIDEO));
                DataRangeVideo2 = AllocFunction(sizeof(KS_DATARANGE_VIDEO2));

                if (DataRangeVideo == NULL || DataRangeVideo2 == NULL)
                {
                    /* insufficient resources */
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                /* init video range */
                DataRangeVideo->DataRange.FormatSize = sizeof(KS_DATARANGE_VIDEO);
                DataRangeVideo->DataRange.MajorFormat = gKSDATAFORMAT_TYPE_VIDEO;
                DataRangeVideo->DataRange.SampleSize = MjpegFrameTypeDescriptor->wWidth * MjpegFrameTypeDescriptor->wHeight * 3; // 3 bytes per pixel
                DataRangeVideo->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_MJPEG_LOCAL;
                DataRangeVideo->DataRange.Specifier = gKSDATAFORMAT_SPECIFIER_VIDEOINFO;
                DataRangeVideo->ConfigCaps.guid = gKSDATAFORMAT_SPECIFIER_VIDEOINFO;
                DataRangeVideo->ConfigCaps.InputSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo->ConfigCaps.InputSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo->ConfigCaps.MinCroppingSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo->ConfigCaps.MinCroppingSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo->ConfigCaps.MaxCroppingSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo->ConfigCaps.MaxCroppingSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo->ConfigCaps.MinOutputSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo->ConfigCaps.MinOutputSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo->ConfigCaps.MaxOutputSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo->ConfigCaps.MaxOutputSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo->ConfigCaps.OutputGranularityX = 1;
                DataRangeVideo->ConfigCaps.OutputGranularityY = 1;
                DataRangeVideo->ConfigCaps.CropGranularityX = 1;
                DataRangeVideo->ConfigCaps.CropGranularityY = 1;
                DataRangeVideo->ConfigCaps.CropAlignX = 1;
                DataRangeVideo->ConfigCaps.CropAlignY = 1;
                /* todo Alignment, MaxBitsPerSecond, MinBitsPerSecond, MinFrameInterval, MaxFrameInterval */
                DataRangeVideo->VideoInfoHeader.AvgTimePerFrame = 333333; // FIXME
                DataRangeVideo->VideoInfoHeader.dwBitRate = MjpegFrameTypeDescriptor->wHeight * MjpegFrameTypeDescriptor->wWidth * 3; // FIXME
                DataRangeVideo->VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
                DataRangeVideo->VideoInfoHeader.bmiHeader.biWidth = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo->VideoInfoHeader.bmiHeader.biHeight = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo->VideoInfoHeader.bmiHeader.biPlanes = 1;
                DataRangeVideo->VideoInfoHeader.bmiHeader.biBitCount = 24;
                DataRangeVideo->VideoInfoHeader.bmiHeader.biCompression = MAKEFOURCC('M','J','P','G');
                DataRangeVideo->VideoInfoHeader.bmiHeader.biSizeImage = MjpegFrameTypeDescriptor->wWidth * MjpegFrameTypeDescriptor->wHeight * 3;

                DataRangeVideo2->DataRange.FormatSize = sizeof(KS_DATARANGE_VIDEO2);
                DataRangeVideo2->DataRange.MajorFormat = gKSDATAFORMAT_TYPE_VIDEO;
                DataRangeVideo2->DataRange.SampleSize = MjpegFrameTypeDescriptor->wWidth * MjpegFrameTypeDescriptor->wHeight * 3; // 3 bytes per pixel
                DataRangeVideo2->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_MJPEG_LOCAL;
                DataRangeVideo2->DataRange.Specifier = gKSDATAFORMAT_SPECIFIER_VIDEOINFO2;
                DataRangeVideo2->ConfigCaps.guid = gKSDATAFORMAT_SPECIFIER_VIDEOINFO2;
                DataRangeVideo2->ConfigCaps.InputSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo2->ConfigCaps.InputSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo2->ConfigCaps.MinCroppingSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo2->ConfigCaps.MinCroppingSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo2->ConfigCaps.MaxCroppingSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo2->ConfigCaps.MaxCroppingSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo2->ConfigCaps.MinOutputSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo2->ConfigCaps.MinOutputSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo2->ConfigCaps.MaxOutputSize.cx = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo2->ConfigCaps.MaxOutputSize.cy = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo2->ConfigCaps.OutputGranularityX = 1;
                DataRangeVideo2->ConfigCaps.OutputGranularityY = 1;
                DataRangeVideo2->ConfigCaps.CropGranularityX = 1;
                DataRangeVideo2->ConfigCaps.CropGranularityY = 1;
                DataRangeVideo2->ConfigCaps.CropAlignX = 1;
                DataRangeVideo2->ConfigCaps.CropAlignY = 1;
                /* todo Alignment, MaxBitsPerSecond, MinBitsPerSecond, MinFrameInterval, MaxFrameInterval */
                DataRangeVideo2->VideoInfoHeader.AvgTimePerFrame = 333333; // FIXME
                DataRangeVideo2->VideoInfoHeader.dwBitRate = MjpegFrameTypeDescriptor->wHeight * MjpegFrameTypeDescriptor->wWidth * 3; // FIXME
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biWidth = MjpegFrameTypeDescriptor->wWidth;
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biHeight = MjpegFrameTypeDescriptor->wHeight;
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biPlanes = 1;
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biBitCount = 24;
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biCompression = MAKEFOURCC('M','J','P','G');
                DataRangeVideo2->VideoInfoHeader.bmiHeader.biSizeImage = MjpegFrameTypeDescriptor->wWidth * MjpegFrameTypeDescriptor->wHeight * 3;

                DeviceExtension->VideoFormatInfo[FormatIndex].bFormatIndex = VideoFormatIndex;
                DeviceExtension->VideoFormatInfo[FormatIndex+1].bFormatIndex = VideoFormatIndex;
                DeviceExtension->VideoFormatInfo[FormatIndex].bFrameIndex = MjpegFrameTypeDescriptor->bFrameIndex;
                DeviceExtension->VideoFormatInfo[FormatIndex+1].bFrameIndex = MjpegFrameTypeDescriptor->bFrameIndex;
                DeviceExtension->VideoFormatInfo[FormatIndex].dwFrameInterval = MjpegFrameTypeDescriptor->dwFrameInterval[0];
                DeviceExtension->VideoFormatInfo[FormatIndex+1].dwFrameInterval = MjpegFrameTypeDescriptor->dwFrameInterval[0];
                DeviceExtension->VideoFormatInfo[FormatIndex].Descriptor = (PUSB_COMMON_DESCRIPTOR)MjpegFrameTypeDescriptor;
                DeviceExtension->VideoFormatInfo[FormatIndex+1].Descriptor = (PUSB_COMMON_DESCRIPTOR)MjpegFrameTypeDescriptor;

                DataRangeVideoArray[FormatIndex] = (PKSDATARANGE)DataRangeVideo;
                DataRangeVideoArray[FormatIndex+1] = (PKSDATARANGE)DataRangeVideo2;
                FormatIndex += 2;
            }
            else if (CommonDescriptor->bDescriptorSubtype == VS_UNCOMPRESSED_FRAME_TYPE_DESCRIPTOR_SUBTYPE)
            {
                if (IsEqualGUIDAligned(&UncompressedFormatDescriptor->guidFormat,
                                      &MEDIASUBTYPE_YUY2))
                {
                    UncompressedFrameTypeDescriptor = (PVS_UNCOMPRESSED_FRAME_TYPE_DESCRIPTOR)CommonDescriptor;

                    DPRINT1("Uncompressed FormatType Descriptor %x", UncompressedFrameTypeDescriptor->bFrameIndex);
                    DataRangeVideo = AllocFunction(sizeof(KS_DATARANGE_VIDEO));
                    DataRangeVideo2 = AllocFunction(sizeof(KS_DATARANGE_VIDEO2));

                    if (DataRangeVideo == NULL || DataRangeVideo2 == NULL)
                    {
                        /* insufficient resources */
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    /* init video range */
                    DataRangeVideo->DataRange.FormatSize = sizeof(KS_DATARANGE_VIDEO);
                    DataRangeVideo->DataRange.MajorFormat = gKSDATAFORMAT_TYPE_VIDEO;
                    DataRangeVideo->DataRange.SampleSize = UncompressedFrameTypeDescriptor->wWidth * UncompressedFrameTypeDescriptor->wHeight * 3; // 3 bytes per pixel
                    DataRangeVideo->DataRange.SubFormat = MEDIASUBTYPE_YUY2;
                    DataRangeVideo->DataRange.Specifier = gKSDATAFORMAT_SPECIFIER_VIDEOINFO;
                    DataRangeVideo->ConfigCaps.guid = gKSDATAFORMAT_SPECIFIER_VIDEOINFO;
                    DataRangeVideo->ConfigCaps.InputSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo->ConfigCaps.InputSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo->ConfigCaps.MinCroppingSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo->ConfigCaps.MinCroppingSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo->ConfigCaps.MaxCroppingSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo->ConfigCaps.MaxCroppingSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo->ConfigCaps.MinOutputSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo->ConfigCaps.MinOutputSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo->ConfigCaps.MaxOutputSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo->ConfigCaps.MaxOutputSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo->ConfigCaps.OutputGranularityX = 1;
                    DataRangeVideo->ConfigCaps.OutputGranularityY = 1;
                    DataRangeVideo->ConfigCaps.CropGranularityX = 1;
                    DataRangeVideo->ConfigCaps.CropGranularityY = 1;
                    DataRangeVideo->ConfigCaps.CropAlignX = 1;
                    DataRangeVideo->ConfigCaps.CropAlignY = 1;
                    /* todo Alignment, MaxBitsPerSecond, MinBitsPerSecond, MinFrameInterval, MaxFrameInterval */
                    DataRangeVideo->VideoInfoHeader.AvgTimePerFrame = 333333; // FIXME
                    DataRangeVideo->VideoInfoHeader.dwBitRate = UncompressedFrameTypeDescriptor->wHeight * UncompressedFrameTypeDescriptor->wWidth * 3; // FIXME
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biWidth = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biHeight = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biPlanes = 1;
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biBitCount = 16;
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biCompression = MAKEFOURCC('Y','U','Y','2');
                    DataRangeVideo->VideoInfoHeader.bmiHeader.biSizeImage = UncompressedFrameTypeDescriptor->wWidth * UncompressedFrameTypeDescriptor->wHeight * 3;
                    DataRangeVideo->bFixedSizeSamples = TRUE;


                    DataRangeVideo2->DataRange.FormatSize = sizeof(KS_DATARANGE_VIDEO2);
                    DataRangeVideo2->DataRange.MajorFormat = gKSDATAFORMAT_TYPE_VIDEO;
                    DataRangeVideo2->DataRange.SampleSize = UncompressedFrameTypeDescriptor->wWidth * UncompressedFrameTypeDescriptor->wHeight * 3; // 3 bytes per pixel
                    DataRangeVideo2->DataRange.SubFormat = MEDIASUBTYPE_YUY2;
                    DataRangeVideo2->DataRange.Specifier = gKSDATAFORMAT_SPECIFIER_VIDEOINFO2;
                    DataRangeVideo2->ConfigCaps.guid = gKSDATAFORMAT_SPECIFIER_VIDEOINFO2;
                    DataRangeVideo2->ConfigCaps.InputSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo2->ConfigCaps.InputSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo2->ConfigCaps.MinCroppingSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo2->ConfigCaps.MinCroppingSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo2->ConfigCaps.MaxCroppingSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo2->ConfigCaps.MaxCroppingSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo2->ConfigCaps.MinOutputSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo2->ConfigCaps.MinOutputSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo2->ConfigCaps.MaxOutputSize.cx = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo2->ConfigCaps.MaxOutputSize.cy = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo2->ConfigCaps.OutputGranularityX = 1;
                    DataRangeVideo2->ConfigCaps.OutputGranularityY = 1;
                    DataRangeVideo2->ConfigCaps.CropGranularityX = 1;
                    DataRangeVideo2->ConfigCaps.CropGranularityY = 1;
                    DataRangeVideo2->ConfigCaps.CropAlignX = 1;
                    DataRangeVideo2->ConfigCaps.CropAlignY = 1;
                    /* todo Alignment, MaxBitsPerSecond, MinBitsPerSecond, MinFrameInterval, MaxFrameInterval */
                    DataRangeVideo2->VideoInfoHeader.AvgTimePerFrame = 333333; // FIXME
                    DataRangeVideo2->VideoInfoHeader.dwBitRate = UncompressedFrameTypeDescriptor->wHeight * UncompressedFrameTypeDescriptor->wWidth * 3; // FIXME
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biWidth = UncompressedFrameTypeDescriptor->wWidth;
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biHeight = UncompressedFrameTypeDescriptor->wHeight;
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biPlanes = 1;
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biBitCount = 16;
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biCompression = MAKEFOURCC('Y','U','Y','2');
                    DataRangeVideo2->VideoInfoHeader.bmiHeader.biSizeImage = UncompressedFrameTypeDescriptor->wWidth * UncompressedFrameTypeDescriptor->wHeight * 3;
                    DataRangeVideo2->bFixedSizeSamples = TRUE;


                    DeviceExtension->VideoFormatInfo[FormatIndex].bFormatIndex = VideoFormatIndex;
                    DeviceExtension->VideoFormatInfo[FormatIndex+1].bFormatIndex = VideoFormatIndex;
                    DeviceExtension->VideoFormatInfo[FormatIndex].bFrameIndex = UncompressedFrameTypeDescriptor->bFrameIndex;
                    DeviceExtension->VideoFormatInfo[FormatIndex+1].bFrameIndex = UncompressedFrameTypeDescriptor->bFrameIndex;
                    DeviceExtension->VideoFormatInfo[FormatIndex].dwFrameInterval = UncompressedFrameTypeDescriptor->dwFrameInterval[0];
                    DeviceExtension->VideoFormatInfo[FormatIndex+1].dwFrameInterval = UncompressedFrameTypeDescriptor->dwFrameInterval[0];
                    DeviceExtension->VideoFormatInfo[FormatIndex].Descriptor = (PUSB_COMMON_DESCRIPTOR)UncompressedFrameTypeDescriptor;
                    DeviceExtension->VideoFormatInfo[FormatIndex+1].Descriptor = (PUSB_COMMON_DESCRIPTOR)UncompressedFrameTypeDescriptor;

                    DataRangeVideoArray[FormatIndex] = (PKSDATARANGE)DataRangeVideo;
                    DataRangeVideoArray[FormatIndex+1] = (PKSDATARANGE)DataRangeVideo2;
                    FormatIndex += 2;
                }
            }
            CommonDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->Common.bLength);
        }

    } while (((ULONG_PTR)CommonDescriptor) <((ULONG_PTR)VideoInputHeaderDescriptor + VideoInputHeaderDescriptor->wTotalLength));

    *OutDataRanges = DataRangeVideoArray;
    *OutDataRangesCount = FormatIndex;

    return STATUS_SUCCESS;
}


NTSTATUS
UsbVideoGetDataRangesForStillImages(
    IN PKSDEVICE Device,
    OUT PKSDATARANGE** OutDataRanges,
    OUT PULONG OutDataRangesCount)
{
    UNIMPLEMENTED;
    // TODO parse still image descriptor
    *OutDataRangesCount = 0;
    *OutDataRanges = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
SubmitUrbSync(
    IN PDEVICE_OBJECT DeviceObject,
    IN PURB Urb)
{
    PIRP Irp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    // init event
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    // build irp
    Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
        DeviceObject,
        NULL,
        0,
        NULL,
        0,
        TRUE,
        &Event,
        &IoStatus);

    if (!Irp)
    {
        //
        // no memory
        //
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get next stack location
    IoStack = IoGetNextIrpStackLocation(Irp);

    // store urb
    IoStack->Parameters.Others.Argument1 = Urb;

    // call driver
    Status = IoCallDriver(DeviceObject, Irp);

    // wait for the request to finish
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    // done
    return Status;
}

NTSTATUS
USBVideoTransferControlPacket(
    IN PKSPIN Pin,
    IN PVOID TransferBuffer,
    IN ULONG TransferBufferLength,
    IN ULONG TransferFlags,
    IN UCHAR           bRequest,
    IN USHORT          wValue,
    IN USHORT          wIndex)
{
    PURB Urb;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = Pin->Context;
    ASSERT(DeviceExtension);

    Urb = AllocFunction(sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UsbBuildVendorRequest(
    Urb,
    URB_FUNCTION_CLASS_INTERFACE,
    sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
    USBD_TRANSFER_DIRECTION_OUT | TransferFlags,
    0,
    bRequest,
    wValue,
    wIndex,
    TransferBuffer,
    NULL,
    TransferBufferLength,
    NULL);

    return SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
}



NTSTATUS
USBVideoFindVideoStreamingDescriptor(
    IN PKSDEVICE Device,
    IN UCHAR TerminalLink)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    PVS_VIDEO_INPUT_HEADER_DESCRIPTOR VideoInputHeaderDescriptor;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;

    /* get device extension */
    DeviceExtension = Device->Context;

    InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor,
        DeviceExtension->ConfigurationDescriptor, -1, -1, 0x0E, // Video Interface Class
        0x02, -1); // Video Streaming Interface Subclass
    if (!InterfaceDescriptor)
    {
        /* not found */
        return STATUS_UNSUCCESSFUL;
    }


    /* point to first descriptor */
    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)InterfaceDescriptor + InterfaceDescriptor->bLength);

    /* enum descriptors */
    while (((ULONG_PTR)CommonDescriptor) < ((ULONG_PTR)DeviceExtension->ConfigurationDescriptor + DeviceExtension->ConfigurationDescriptor->wTotalLength))
    {
        if (CommonDescriptor->bDescriptorType == VC_INTERFACE_HEADER_DESCRIPTOR_TYPE)
        {
            VideoInputHeaderDescriptor = (PVS_VIDEO_INPUT_HEADER_DESCRIPTOR)CommonDescriptor;
            if (VideoInputHeaderDescriptor->bDescriptorSubtype == VC_INTERFACE_HEADER_DESCRIPTOR_SUBTYPE &&
                VideoInputHeaderDescriptor->bTerminalLink == TerminalLink)
            {
                DeviceExtension->VideoInputHeaderDescriptor = VideoInputHeaderDescriptor;
                return STATUS_SUCCESS;
            }
        }
        CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
    }
    DPRINT1("USBVideoEnumVideoStreamingDescriptor not found\n");
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
BuildUSBVideoFilterTopology(
    PKSDEVICE Device,
    PKSFILTER_DESCRIPTOR FilterDescriptor)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;
    ULONG OutputTerminalDescriptorIndex;
    PVC_OUTPUT_TERMINAL_DESCRIPTOR OutputTerminalDescriptor;
    ULONG NodesCount = 0, TerminalIndex, NodeIndex, NodeConnectionCount;
    UCHAR SourceId;
    PVC_INTERFACE_COMMON_DESCRIPTOR CommonTerminalDescriptor;
    PVC_INPUT_TERMINAL_DESCRIPTOR InputTerminalDescriptor;
    PVC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR ProcessingUnitDescriptor;
    PVC_EXTENSION_TERMINAL_DESCRIPTOR ExtensionDescriptor;
    PKSNODE_DESCRIPTOR NodeDescriptors;
    PKSTOPOLOGY_CONNECTION Connections;

    /* enum terminal descriptors */
    Status = USBVideoEnumTerminalDescriptors(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoEnumTerminalDescriptors failed with %x\n", Status);
        return Status;
    }

    /* get device extension */
    DeviceExtension = Device->Context;

    /* find output terminal descriptor */
    Status = USBVideoFindTerminalDescriptorWithSubType(Device, VC_OUTPUT_TERMINAL_DESCRIPTOR_SUBTYPE, &OutputTerminalDescriptorIndex);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBVideoFindTerminalDescriptorWithSubType failed with %x\n", Status);
        return Status;
    }
    OutputTerminalDescriptor = (PVC_OUTPUT_TERMINAL_DESCRIPTOR)DeviceExtension->TerminalDescriptors[OutputTerminalDescriptorIndex];

    DPRINT1("USBVIDEO OutputTerminalId %x\n", OutputTerminalDescriptor->bTerminalID);
    if (OutputTerminalDescriptor->wTerminalType != 0x101) // TT_STREAMING
    {
        // bogus configuration descriptor or multiple output terminal descriptors
        DPRINT1("USBVideo unexpected wTerminalType %x\n", OutputTerminalDescriptor->wTerminalType);
        return STATUS_UNSUCCESSFUL;
    }
    NodesCount = 1;

    /* find video streaming descriptor */
    Status = USBVideoFindVideoStreamingDescriptor(Device, OutputTerminalDescriptor->bTerminalID);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        DPRINT1("USBVideo failed to find video streaming descriptor with %x\n", Status);
        return Status;
    }

    /* iterate until input terminal descriptor is found */
    SourceId = OutputTerminalDescriptor->bSourceID;
    do
    {
        Status = USBVideoFindTerminalDescriptorWithTerminalId(Device, SourceId, &TerminalIndex);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to find terminal descriptor with id %x\n", SourceId);
            return Status;
        }
        CommonTerminalDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)DeviceExtension->TerminalDescriptors[TerminalIndex];
        if (CommonTerminalDescriptor->bDescriptorSubtype == VC_INPUT_TERMINAL_DESCRIPTOR_SUBTYPE)
        {
            InputTerminalDescriptor = (PVC_INPUT_TERMINAL_DESCRIPTOR)CommonTerminalDescriptor;
            if (InputTerminalDescriptor->wTerminalType != 0x201) // ITT_CAMERA
            {
                DPRINT1("Unexpected input terminal type %x\n", InputTerminalDescriptor->wTerminalType);
                return STATUS_UNSUCCESSFUL;
            }
            NodesCount++;
            DPRINT1("Found InputTerminal Id %x\n", InputTerminalDescriptor->bTerminalID);
            break;
        }
        else if (CommonTerminalDescriptor->bDescriptorSubtype == VC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR_SUBTYPE)
        {
            ProcessingUnitDescriptor = (PVC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR)CommonTerminalDescriptor;
            DPRINT1("Found ProcessingUnit %p Id %x\n", ProcessingUnitDescriptor, ProcessingUnitDescriptor->bUnitID);
            NodesCount++;
            SourceId = ProcessingUnitDescriptor->bSourceID;
        }
        else if (CommonTerminalDescriptor->bDescriptorSubtype == VC_MULTIPLEXER_TERMINAL_DESCRIPTOR_SUBTYPE )
        {
            /* unsupported multiplexer control */
            DPRINT1("USBVIDEO Unsupported multiplexer control\n");
            return STATUS_UNSUCCESSFUL;
        }
        else if (CommonTerminalDescriptor->bDescriptorSubtype == VC_EXTENSION_TERMINAL_DESCRIPTOR_SUBTYPE)
        {
            /* device specific extension unit */
           ExtensionDescriptor = (PVC_EXTENSION_TERMINAL_DESCRIPTOR)CommonTerminalDescriptor;
           DPRINT1("Found Extension Terminal %p Id %x\n", ExtensionDescriptor, ExtensionDescriptor->bUnitID);
           NodesCount++;
           ASSERT(ExtensionDescriptor->bNrInPins == 1);
           SourceId = ExtensionDescriptor->baSourceID[0];
        }
        else if (CommonTerminalDescriptor->bDescriptorSubtype == 0x07) // UVC 1.5
        {
            UNIMPLEMENTED;
            return STATUS_NOT_IMPLEMENTED;
        }
    } while (TRUE);

    NodeDescriptors = (PKSNODE_DESCRIPTOR)AllocFunction(sizeof(KSNODE_DESCRIPTOR) * NodesCount);
    if (!NodeDescriptors)
    {
        /* insufficient resources */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NodeIndex = NodesCount - 1;
    do
    {
        if (NodeIndex + 1 == NodesCount)
        {
            NodeDescriptors[NodeIndex].Name = &PKSNODETYPE_VIDEO_STREAMING;
            NodeDescriptors[NodeIndex].Type = &PKSNODETYPE_VIDEO_STREAMING;
            NodeDescriptors[NodeIndex].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));
            SourceId = OutputTerminalDescriptor->bSourceID;
        }
        else
        {
            Status = USBVideoFindTerminalDescriptorWithTerminalId(Device, SourceId, &TerminalIndex);
            ASSERT(NT_SUCCESS(Status));

            CommonTerminalDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)DeviceExtension->TerminalDescriptors[TerminalIndex];
            if (CommonTerminalDescriptor->bDescriptorSubtype == VC_INPUT_TERMINAL_DESCRIPTOR_SUBTYPE)
            {
                NodeDescriptors[NodeIndex].Name = &PKSNODETYPE_VIDEO_CAMERA_TERMINAL;
                NodeDescriptors[NodeIndex].Type = &PKSNODETYPE_VIDEO_CAMERA_TERMINAL;
                NodeDescriptors[NodeIndex].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));
                break;
            }
            else if (CommonTerminalDescriptor->bDescriptorSubtype == VC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR_SUBTYPE)
            {
                /* processing unit */
                NodeDescriptors[NodeIndex].Name = &PKSNODETYPE_VIDEO_PROCESSING;
                NodeDescriptors[NodeIndex].Type = &PKSNODETYPE_VIDEO_PROCESSING;
                NodeDescriptors[NodeIndex].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));
                ProcessingUnitDescriptor = (PVC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR)CommonTerminalDescriptor;
                SourceId = ProcessingUnitDescriptor->bSourceID;
            }
            else if (CommonTerminalDescriptor->bDescriptorSubtype == VC_EXTENSION_TERMINAL_DESCRIPTOR_SUBTYPE)
            {
                /* device specific extension unit */
                NodeDescriptors[NodeIndex].Name = &PKSNODETYPE_DEV_SPECIFIC;
                NodeDescriptors[NodeIndex].Type = &PKSNODETYPE_DEV_SPECIFIC;
                NodeDescriptors[NodeIndex].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));
                ExtensionDescriptor = (PVC_EXTENSION_TERMINAL_DESCRIPTOR)CommonTerminalDescriptor;
                ASSERT(ExtensionDescriptor->bNrInPins == 1);
                SourceId = ExtensionDescriptor->baSourceID[0];
            }
            else
            {
                UNIMPLEMENTED;
                return STATUS_NOT_IMPLEMENTED;
            }
        }
        NodeIndex--;
    }while(TRUE);

    FilterDescriptor->NodeDescriptorsCount = NodesCount;
    FilterDescriptor->NodeDescriptorSize = sizeof(KSNODE_DESCRIPTOR);
    FilterDescriptor->NodeDescriptors = NodeDescriptors;

    NodeConnectionCount = NodesCount + 2; // for bridge pin and streaming pin
    if (DeviceExtension->VideoInputHeaderDescriptor->bStillCaptureMethod == 0x02)
    {
        // still capture supported with dedicated endpoint
        NodeConnectionCount++;
    }

    Connections = AllocFunction(sizeof(KSTOPOLOGY_CONNECTION) * NodeConnectionCount);
    if(!Connections)
        return STATUS_INSUFFICIENT_RESOURCES;

    FilterDescriptor->Connections = Connections;
    FilterDescriptor->ConnectionsCount = NodeConnectionCount;

    /* bridge pin connection */
    Connections[0].FromNode = -1;
    Connections[0].FromNodePin = 1;
    Connections[0].ToNode = 0;
    Connections[0].ToNodePin = 1;
    for(NodeIndex = 0; NodeIndex < NodesCount; NodeIndex++)
    {
        /* inter node connection */
        Connections[NodeIndex+1].FromNode = NodeIndex;
        Connections[NodeIndex+1].FromNodePin = 0;
        Connections[NodeIndex+1].ToNode = NodeIndex+1;
        Connections[NodeIndex+1].ToNodePin = 1;
    }

    /* streaming pin connection */
    Connections[NodesCount+1].FromNode = NodesCount;
    Connections[NodesCount+1].FromNodePin = 0;
    Connections[NodesCount+1].ToNode = -1;
    Connections[NodesCount+1].ToNodePin = 0;

    if (DeviceExtension->VideoInputHeaderDescriptor->bStillCaptureMethod == 0x02)
    {
        /* still pin connection */
        Connections[NodesCount+2].FromNode = NodesCount;
        Connections[NodesCount+2].FromNodePin = 1;
        Connections[NodesCount+2].ToNode = -1;
        Connections[NodesCount+2].ToNodePin = 2;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
USBVideoPinBuildDescriptors(
    PKSDEVICE Device,
    PKSPIN_DESCRIPTOR_EX *PinDescriptors,
    PULONG PinDescriptorsCount,
    PULONG PinDescriptorSize)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PKSPIN_DESCRIPTOR_EX Pins;
    ULONG PinCount;
    NTSTATUS Status;

    /* get device extension */
    DeviceExtension = Device->Context;

    if (DeviceExtension->VideoInputHeaderDescriptor->bStillCaptureMethod == 0x02)
    {
        /* bridge pin + video capture pin + still image pin */
        PinCount = 3;
    }
    else
    {
        /* bridge pin + video capture pin */
        PinCount = 2;
    }

    Pins = AllocFunction(PinCount * sizeof(KSPIN_DESCRIPTOR_EX));
    if (!Pins)
    {
        /* no memory */
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }

    /* initialize streaming pin */
    Pins[0].Dispatch = &UsbVideoPinDispatch;
    Pins[0].PinDescriptor.InterfacesCount = 1;
    Pins[0].PinDescriptor.Interfaces = &StandardPinInterface;
    Pins[0].PinDescriptor.MediumsCount = 1;
    Pins[0].PinDescriptor.Mediums = &StandardPinMedium;
    Pins[0].PinDescriptor.Category = &PPINNAME_CAPTURE;
    Pins[0].PinDescriptor.Communication = KSPIN_COMMUNICATION_BOTH;
    Pins[0].PinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
    Pins[0].Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING;
    Pins[0].InstancesPossible = 1;

    /* enum dataranges for streaming */
    Status = UsbVideoGetDataRangesForStreaming(Device, (PKSDATARANGE**)&Pins[0].PinDescriptor.DataRanges, &Pins[0].PinDescriptor.DataRangesCount);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to retrieve data ranges\n");
        return Status;
    }

    /* initialize bridge pin */
    Pins[1].PinDescriptor.InterfacesCount = 1;
    Pins[1].PinDescriptor.Interfaces = &StandardPinInterface;
    Pins[1].PinDescriptor.MediumsCount = 1;
    Pins[1].PinDescriptor.Mediums = &StandardPinMedium;
    Pins[1].PinDescriptor.DataRanges = NULL;
    Pins[1].PinDescriptor.DataRangesCount = 0;
    Pins[1].PinDescriptor.Communication = KSPIN_COMMUNICATION_BRIDGE;
    Pins[1].PinDescriptor.Category = NULL; // FIXME
    Pins[1].PinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;

    if (PinCount == 3)
    {
        /* initialize still image pin */
        Pins[2].Dispatch = &UsbVideoPinDispatch;
        Pins[2].PinDescriptor.InterfacesCount = 1;
        Pins[2].PinDescriptor.Interfaces = &StandardPinInterface;
        Pins[2].PinDescriptor.MediumsCount = 1;
        Pins[2].PinDescriptor.Mediums = &StandardPinMedium;
        Pins[2].PinDescriptor.Category = &PPINNAME_VIDEO_STILL;
        Pins[2].PinDescriptor.Communication = KSPIN_COMMUNICATION_BOTH;
        Pins[2].PinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
        Pins[2].Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING;
        Pins[2].InstancesPossible = 1;

        Status = UsbVideoGetDataRangesForStillImages(Device, (PKSDATARANGE**)&Pins[2].PinDescriptor.DataRanges, &Pins[2].PinDescriptor.DataRangesCount);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to retrieve data ranges\n");
            return Status;
        }
    }

    *PinDescriptors = Pins;
    *PinDescriptorsCount = PinCount;
    *PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);
    return STATUS_SUCCESS;
}

NTSTATUS
USBVideoCheckUVCVersion(
    PKSDEVICE Device,
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    PVC_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;

    /* get device extension */
    DeviceExtension = Device->Context;

    /* find video control interface descriptor */
    InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(
        ConfigurationDescriptor, ConfigurationDescriptor,
        -1, -1,
        USB_DEVICE_CLASS_VIDEO,
        0x01, // video control interface subclass
        -1
    );

    if (!InterfaceDescriptor)
    {
        DPRINT1("USBVideoCheckUVCVersion interface descriptor not found\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* point to first descriptor */
    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)InterfaceDescriptor + InterfaceDescriptor->bLength);

    /* enum descriptors */
    while (((ULONG_PTR)CommonDescriptor) < ((ULONG_PTR)ConfigurationDescriptor + ConfigurationDescriptor->wTotalLength))
    {
        if (CommonDescriptor->bDescriptorType == VC_INTERFACE_HEADER_DESCRIPTOR_TYPE)
        {
            InterfaceHeaderDescriptor = (PVC_INTERFACE_HEADER_DESCRIPTOR)CommonDescriptor;
            if (InterfaceHeaderDescriptor->bDescriptorSubType == VC_INTERFACE_HEADER_DESCRIPTOR_SUBTYPE)
            {
                DeviceExtension->InterfaceHeaderDescriptor = InterfaceHeaderDescriptor;
                DPRINT1("UsbVideo UVCVersion %x\n", InterfaceHeaderDescriptor->bcdUVC);
                if (InterfaceHeaderDescriptor->bcdUVC == 0x0100 || /* version 1.0 */
                    InterfaceHeaderDescriptor->bcdUVC == 0x0110 || /* version 1.1 */
                    InterfaceHeaderDescriptor->bcdUVC == 0x0150)   /* version 1.5 */
                {
                    return STATUS_SUCCESS;
                }
            }
        }
        CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
USBVideoEnumTerminalDescriptors(
    PKSDEVICE Device)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PVC_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;
    ULONG DescriptorCount = 0;

    /* get device extension */
    DeviceExtension = Device->Context;

    /* get interface header */
    InterfaceHeaderDescriptor = DeviceExtension->InterfaceHeaderDescriptor;
    ASSERT(InterfaceHeaderDescriptor);
    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength);
    while(((ULONG_PTR) CommonDescriptor) < ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
    {
        DescriptorCount++;
        CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
    }
    if (!DescriptorCount)
    {
        DPRINT1("EnumTerminalDescriptors failed to enum terminal descriptors\n");
        return STATUS_UNSUCCESSFUL;
    }
    DeviceExtension->TerminalDescriptors = AllocFunction(sizeof(PUSB_COMMON_DESCRIPTOR) * DescriptorCount);
    if (!DeviceExtension->TerminalDescriptors)
    {
        DPRINT1("Failed to allocate terminal descriptos Count %u\n", DescriptorCount);
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }
    DPRINT1("EnumTerminalDescriptors Descriptors Count %u\n", DescriptorCount);

    DeviceExtension->TerminalDescriptorsCount = 0;
    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength);
    while(((ULONG_PTR) CommonDescriptor) < ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
    {
        DeviceExtension->TerminalDescriptors[DeviceExtension->TerminalDescriptorsCount] = CommonDescriptor;
        DeviceExtension->TerminalDescriptorsCount++;

        CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
USBVideoFindTerminalDescriptorWithSubType(
    IN PKSDEVICE Device,
    IN UCHAR bDescriptorSubType,
    OUT PULONG TerminalDescriptorIndex)
{
    ULONG Index;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PVC_INTERFACE_COMMON_DESCRIPTOR CommonTerminalDescriptor;

    /* get device extension */
    DeviceExtension = Device->Context;

    for(Index = 0; Index < DeviceExtension->TerminalDescriptorsCount; Index++)
    {
        CommonTerminalDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)DeviceExtension->TerminalDescriptors[Index];
        if (CommonTerminalDescriptor->bDescriptorSubtype == bDescriptorSubType)
        {
            *TerminalDescriptorIndex = Index;
            return STATUS_SUCCESS;
        }
    }
    *TerminalDescriptorIndex = 0;
    return STATUS_NOT_FOUND;
}


NTSTATUS
USBVideoFindTerminalDescriptorWithTerminalId(
    IN PKSDEVICE Device,
    IN UCHAR bTerminalId,
    OUT PULONG TerminalDescriptorIndex)
{
    ULONG Index;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PVC_INTERFACE_COMMON_DESCRIPTOR CommonTerminalDescriptor;

    /* get device extension */
    DeviceExtension = Device->Context;

    for(Index = 0; Index < DeviceExtension->TerminalDescriptorsCount; Index++)
    {
        CommonTerminalDescriptor = (PVC_INTERFACE_COMMON_DESCRIPTOR)DeviceExtension->TerminalDescriptors[Index];
        if (CommonTerminalDescriptor->bTerminalID == bTerminalId)
        {
            *TerminalDescriptorIndex = Index;
            return STATUS_SUCCESS;
        }
    }
    *TerminalDescriptorIndex = 0;
    return STATUS_NOT_FOUND;
}

