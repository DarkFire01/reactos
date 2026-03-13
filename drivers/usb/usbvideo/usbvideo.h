#pragma once

#include <ntddk.h>
#include <portcls.h>
#include <ksmedia.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <usbioctl.h>
#include <usb.h>
#include <usbdlib.h>
#define YDEBUG
#include <debug.h>

#define USBVIDEO_TAG 'VbsU'

#include <pshpack1.h>

typedef struct _VS_PROBE_COMMIT_CONTROL {
    USHORT  bmHint;
    UCHAR   bFormatIndex;
    UCHAR   bFrameIndex;
    ULONG   dwFrameInterval;
    USHORT  wKeyFrameRate;
    USHORT  wPFrameRate;
    USHORT  wCompQuality;
    USHORT  wCompWindowSize;
    USHORT  wDelay;
    ULONG   dwMaxVideoFrameSize;
    ULONG   dwMaxPayloadTransferSize;
 #if 0
    // UVC 1.1+
    DWORD  dwClockFrequency;
    UCHAR   bmFramingInfo;
    UCHAR   bPreferedVersion;
    UCHAR   bMinVersion;
    UCHAR   bMaxVersion;
#endif
}VS_PROBE_COMMIT_CONTROL;

typedef struct _JFIF_APP0 {
    UCHAR  marker[2];
    USHORT length;
    UCHAR  identifier[5];
    UCHAR  versionMajor;
    UCHAR  versionMinor;
    UCHAR  pixelAspect;
    USHORT xDensity;
    USHORT yDensity;
    UCHAR  xThumbnail;
    UCHAR  yThumbnail;
} JFIF_APP0;

typedef struct
{
        UCHAR bFormatIndex;
        UCHAR bFrameIndex;
        UCHAR bCompressionIndex;
        ULONG dwMaxVideoFrameSize;
        ULONG dwMaxPayloadTransferSize;
}STILL_PROBE_COMMIT, *PSTILL_PROBE_COMMIT;;


#define ISO_PACKET_COUNT 16 // FIXME determine packet count by bInterval
#define BULK_TRANSFER_SIZE  (64*1024)
#define ISO_TRANSFER_SIZE (64*1024)
#define URB_POOL_COUNT    4


typedef struct _FRAME_CONTEXT {
    PUCHAR  FrameBuffer;
    ULONG   FrameSize;
    ULONG   MaxFrameSize;
    UCHAR   LastFid;
    BOOLEAN FrameStarted;
} FRAME_CONTEXT, *PFRAME_CONTEXT;

typedef struct
{
    UCHAR bHeaderLength;
    union {
        UCHAR bmHeaderInfo;
        struct {
            UCHAR FID  : 1;
            UCHAR EOF  : 1;
            UCHAR PTS  : 1;
            UCHAR SCR  : 1;
            UCHAR RES  : 1;
            UCHAR STI  : 1;
            UCHAR ERR  : 1;
            UCHAR EOH  : 1;
        };
    };
} UVC_PAYLOAD_HEADER, *PUVC_PAYLOAD_HEADER;

typedef struct
{
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    UCHAR  bDescriptorSubType;
    USHORT bcdUVC;
    USHORT wTotalLength;
    ULONG  dwClockFrequency;
    UCHAR  bInCollection;
    UCHAR  baInterfaceNr[1];
} VC_INTERFACE_HEADER_DESCRIPTOR, *PVC_INTERFACE_HEADER_DESCRIPTOR;

typedef struct
{
    USB_COMMON_DESCRIPTOR Common;
    UCHAR                 bDescriptorSubtype;
    UCHAR                 bTerminalID;
} VC_INTERFACE_COMMON_DESCRIPTOR, *PVC_INTERFACE_COMMON_DESCRIPTOR;
C_ASSERT(sizeof(USB_COMMON_DESCRIPTOR) + sizeof(UCHAR) * 2 == sizeof(VC_INTERFACE_COMMON_DESCRIPTOR));

typedef struct {
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bTerminalID;
    USHORT wTerminalType;
    UCHAR bAssocTerminal;
    UCHAR bSourceID;
    UCHAR iTerminal;
}VC_OUTPUT_TERMINAL_DESCRIPTOR, *PVC_OUTPUT_TERMINAL_DESCRIPTOR;
C_ASSERT(sizeof(VC_OUTPUT_TERMINAL_DESCRIPTOR) == 0x09);

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bTerminalID;
    USHORT wTerminalType;
    UCHAR bAssocTerminal;
    UCHAR iTerminal;
    USHORT wObjectiveFocalLengthMin;
    USHORT wObjectiveFocalLengthMax;
    USHORT wOcularFocalLength;
    UCHAR bControlSize;
    UCHAR bmControls[0];
}VC_INPUT_TERMINAL_DESCRIPTOR, *PVC_INPUT_TERMINAL_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bUnitID;
    UCHAR bSourceID;
    USHORT wMaxMultiplier;
    UCHAR bControlSize;
    UCHAR bmControls[1];
    UCHAR iProcessing;
}VC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR, *PVC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubType;
    UCHAR bUnitID;
    UCHAR bNrInPins;
    UCHAR baSourceID[1];
    UCHAR iSelector;
}VC_MULTIPLEXER_TERMINAL_DESCRIPTOR, *PVC_MULITPLEXER_TERMINAL_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubType;
    UCHAR bUnitID;
    GUID guidExtensionCode;
    UCHAR bNumControls;
    UCHAR bNrInPins;
    UCHAR baSourceID[1];
    UCHAR bControlSize;
    UCHAR bmControls[1];
    UCHAR iExtension;
}VC_EXTENSION_TERMINAL_DESCRIPTOR, *PVC_EXTENSION_TERMINAL_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bNumFormats;
    USHORT wTotalLength;
    UCHAR bEndpointAddress;
    UCHAR bmInfo;
    UCHAR bTerminalLink;
    UCHAR bStillCaptureMethod;
    UCHAR bTriggerSupport;
    UCHAR bTriggerUsage;
    UCHAR bControlSize;
    UCHAR bmaControls[1];
}VS_VIDEO_INPUT_HEADER_DESCRIPTOR, *PVS_VIDEO_INPUT_HEADER_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bFormatIndex;
    UCHAR bNumFrameDescriptors;
    UCHAR bmFlags;
    UCHAR bDefaultFrameIndex;
    UCHAR bAspectRatioX;
    UCHAR bAspectRatioY;
    UCHAR bmInterlaceFlags;
    UCHAR bCopyProtect;
}VS_MJPEG_FORMAT_TYPE_DESCRIPTOR, *PVS_MJPEG_FORMAT_TYPE_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bFrameIndex;
    UCHAR bmCapabilities;
    USHORT wWidth;
    USHORT wHeight;
    ULONG dwMinBitRate;
    ULONG dwMaxBitRate;
    ULONG dwMaxVideoFrameBufferSize;
    ULONG dwDefaultFrameInterval;
    UCHAR bFrameIntervalType;
    ULONG dwFrameInterval[1];
}VS_MJPEG_FRAME_TYPE_DESCRIPTOR, *PVS_MJPEG_FRAME_TYPE_DESCRIPTOR;

typedef struct
{
    USHORT wWidth;
    USHORT wHeight;
} STILL_IMAGE_SIZE;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bEndpointAddress;
    UCHAR bNumImageSizePatterns;
    STILL_IMAGE_SIZE Size[1];
}VS_STILL_IMAGE_FRAME_TYPE_DESCRIPTOR, *PVS_STILL_IMAGE_FRAME_TYPE_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bFormatIndex;
    UCHAR bNumFrameDescriptors;
    GUID guidFormat;
    UCHAR bBitsPerPixel;
    UCHAR bDefaultFrameIndex;
    UCHAR bAspectRatioX;
    UCHAR bAspectRatioY;
    UCHAR bmInterlaceFlags;
    UCHAR bCopyProtect;
}VS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR, *PVS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bFrameIndex;
    UCHAR bmCapabilities;
    USHORT wWidth;
    USHORT wHeight;
    ULONG dwMinBitRate;
    ULONG dwMaxBitRate;
    ULONG dwMaxVideoFrameBufferSize;
    ULONG dwDefaultFrameInterval;
    UCHAR bFrameIntervalType;
    ULONG dwFrameInterval[1];
} VS_UNCOMPRESSED_FRAME_TYPE_DESCRIPTOR, *PVS_UNCOMPRESSED_FRAME_TYPE_DESCRIPTOR;

typedef struct
{
    PUSB_COMMON_DESCRIPTOR Descriptor;
    UCHAR bFrameIndex;
    UCHAR bFormatIndex;
    ULONG dwFrameInterval;
}VIDEO_FORMAT_INFO, *PVIDEO_FORMAT_INFO;


#define VC_INTERFACE_HEADER_DESCRIPTOR_TYPE 0x24
#define VS_INTERFACE_HEADER_DESCRIPTOR_TYPE 0x24


#define VC_INTERFACE_HEADER_DESCRIPTOR_SUBTYPE 0x01
#define VC_INPUT_TERMINAL_DESCRIPTOR_SUBTYPE 0x02
#define VC_OUTPUT_TERMINAL_DESCRIPTOR_SUBTYPE 0x03
#define VC_MULTIPLEXER_TERMINAL_DESCRIPTOR_SUBTYPE 0x04
#define VC_PROCESSING_UNIT_TERMINAL_DESCRIPTOR_SUBTYPE 0x05
#define VC_EXTENSION_TERMINAL_DESCRIPTOR_SUBTYPE 0x06

#define VS_STILL_IMAGE_FRAME_TYPE_DESCRIPTOR_SUBTYPE 0x03
#define VS_UNCOMPRESSED_FORMAT_TYPE_DESCRIPTOR_SUBTYPE 0x04
#define VS_UNCOMPRESSED_FRAME_TYPE_DESCRIPTOR_SUBTYPE 0x05
#define VS_MJPEG_FORMAT_TYPE_DESCRIPTOR_SUBTYPE 0x06
#define VS_MJPEG_FRAME_TYPE_DESCRIPTOR_SUBTYPE 0x07

#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
                ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) |   \
                ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24 ))


#define UVC_VERSION_100 0x100
#define UVC_VERSION_110 0x110
#define UVC_VERSION_150 0x150

#include <poppack.h>


typedef struct
{
    PDEVICE_OBJECT LowerDevice;                                  /* lower device*/
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;       /* usb configuration descriptor */
    PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;                     /* usb device descriptor */
    PUSBD_INTERFACE_INFORMATION InterfaceInfo;                   /* interface information */
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;               /* configuration handle */
    PVC_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;   /* interface header descriptor*/
    PUSB_COMMON_DESCRIPTOR * TerminalDescriptors;                /* terminal descriptors */
    ULONG TerminalDescriptorsCount;                              /* num of terminal descriptors */
    PVS_VIDEO_INPUT_HEADER_DESCRIPTOR VideoInputHeaderDescriptor; /* video input header descriptor */
    HANDLE hPipe;                                                 /* handle pipe */
    USBD_PIPE_TYPE PipeType;                                      /* pipe type */
    ULONG MaximumPacketSize;                                      /* max packet size */
    PFRAME_CONTEXT FrameCtx;
    ULONG dwMaxPayloadTransferSize;
    PVIDEO_FORMAT_INFO VideoFormatInfo;                           /* video format lookup info */
    PUCHAR * BulkBuffer;                                          /* bulk buffer array for irp queue */
    PURB * Urb;                                                   /* Urb array for irp queue */
    PIRP * Irp;                                                   /* irp array for irp queue */
    PKSPIN Pin;                                                   /* pin reference */
    ULONG StopStreaming;                                          /* stops streaming */
    ULONG StoppedStreamingIrps;                                   /* stopped streaming irp count */
    KEVENT StoppedStreamingEvent;                                  /* stopped streaming event */
} USB_VIDEO_DEVICE_EXTENSION, *PUSB_VIDEO_DEVICE_EXTENSION;


typedef struct
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;                           /* device extension */
    PDEVICE_OBJECT LowerDevice;                                  /* lower device*/
}FILTER_CONTEXT, *PFILTER_CONTEXT;

/* bulk.c */

NTSTATUS
NTAPI
USBVideoBulkReadComplete(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context);

NTSTATUS
USBVideoQueueBulkRead(
    IN PKSPIN Pin,
    IN USBD_PIPE_HANDLE hBulkPipe,
    IN PUCHAR TransferBuffer,
    IN ULONG TransferLength,
    IN PIRP Irp,
    IN PURB Urb);


/* descriptor.c */

NTSTATUS
USBVideoFindStreamingInterfaceDescriptor(
    IN PKSPIN Pin,
    IN ULONG dwMaxPayloadTransferSize,
    IN UCHAR InterfaceNumber,
    OUT PUCHAR AlternateSetting);


NTSTATUS
USBVideoGetInterfaceDescriptorForStreaming(
    IN PKSPIN Pin,
    OUT PUCHAR InterfaceNumber);

NTSTATUS
USBVideoTransferControlPacket(
    IN PKSPIN Pin,
    IN PVOID TransferBuffer,
    IN ULONG TransferBufferLength,
    IN ULONG TransferFlags,
    IN UCHAR           bRequest,
    IN USHORT          wValue,
    IN USHORT          wIndex);

NTSTATUS
SubmitUrbSync(
    IN PDEVICE_OBJECT DeviceObject,
    IN PURB Urb);


NTSTATUS
USBVideoPinBuildDescriptors(
    PKSDEVICE Device,
    PKSPIN_DESCRIPTOR_EX *PinDescriptors,
    PULONG PinDescriptorsCount,
    PULONG PinDescriptorSize);


NTSTATUS
BuildUSBVideoFilterTopology(
    PKSDEVICE Device,
    PKSFILTER_DESCRIPTOR FilterDescriptor);

NTSTATUS
USBVideoCheckUVCVersion(
    PKSDEVICE Device,
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor);

NTSTATUS
USBVideoEnumTerminalDescriptors(
    PKSDEVICE Device);


NTSTATUS
USBVideoFindTerminalDescriptorWithTerminalId(
    IN PKSDEVICE Device,
    IN UCHAR bTerminalId,
    OUT PULONG TerminalDescriptorIndex
);

NTSTATUS
USBVideoFindTerminalDescriptorWithSubType(
    IN PKSDEVICE Device,
    IN UCHAR bDescriptorSubType,
    OUT PULONG TerminalDescriptorIndex);

/* pool.c */

PVOID
NTAPI
AllocFunction(
    IN ULONG ItemSize);

VOID
NTAPI
FreeFunction(
    IN PVOID Item);

/* usbvideo.c */

NTSTATUS
NTAPI
USBVideoAddDevice(
  _In_ PKSDEVICE Device
);

NTSTATUS
NTAPI
USBVideoPnPStart(
  _In_     PKSDEVICE         Device,
  _In_     PIRP              Irp,
  _In_opt_ PCM_RESOURCE_LIST TranslatedResourceList,
  _In_opt_ PCM_RESOURCE_LIST UntranslatedResourceList
);

NTSTATUS
NTAPI
USBVideoPnPQueryStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBVideoPnPCancelStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBVideoPnPStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

NTSTATUS
NTAPI
USBVideoPnPQueryRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBVideoPnPCancelRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBVideoPnPRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

NTSTATUS
NTAPI
USBVideoPnPQueryCapabilities(
  _In_    PKSDEVICE            Device,
  _In_    PIRP                 Irp,
  _Inout_ PDEVICE_CAPABILITIES Capabilities
);

VOID
NTAPI
USBVideoPnPSurpriseRemoval(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

NTSTATUS
NTAPI
USBVideoPnPQueryPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE DeviceTo,
  _In_ DEVICE_POWER_STATE DeviceFrom,
  _In_ SYSTEM_POWER_STATE SystemTo,
  _In_ SYSTEM_POWER_STATE SystemFrom,
  _In_ POWER_ACTION       Action
);

VOID
NTAPI
USBVideoPnPSetPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE To,
  _In_ DEVICE_POWER_STATE From
);

/* filter.c */
NTSTATUS
NTAPI
USBVideoCreateFilterContext(
    PKSDEVICE Device);

NTSTATUS
NTAPI
USBVideoFilterCreate(
    PKSFILTER Filter,
    PIRP Irp);

/* pin.c */

NTSTATUS
USBVideoSetFormat(
    PKSPIN Pin,
    PKSDATAFORMAT ConnectionFormat);

NTSTATUS
NTAPI
USBVideoPinCreate(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp);


NTSTATUS
NTAPI
USBVideoPinClose(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
USBVideoPinProcess(
    _In_ PKSPIN Pin);

VOID
NTAPI
USBVideoPinReset(
    _In_ PKSPIN Pin);

NTSTATUS
NTAPI
USBVideoPinSetDataFormat(
    _In_ PKSPIN Pin,
    _In_opt_ PKSDATAFORMAT OldFormat,
    _In_opt_ PKSMULTIPLE_ITEM OldAttributeList,
    _In_ const KSDATARANGE* DataRange,
    _In_opt_ const KSATTRIBUTE_LIST* AttributeRange);

NTSTATUS
NTAPI
USBVideoPinSetDeviceState(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState);

/* iso.c */

NTSTATUS
NTAPI
USBVideoIsoReadComplete(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context);

NTSTATUS
USBVideoQueueIsoRead(
    IN PKSPIN Pin,
    IN USBD_PIPE_HANDLE hIsoPipe,
    IN PUCHAR TransferBuffer,
    IN ULONG TransferLength,
    IN PIRP Irp,
    IN PURB Urb,
    IN PFRAME_CONTEXT FrameCtx);

/* uvc.c */
VOID
NTAPI
USBVideoDeliverFrame(
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension,
    PUCHAR FrameBuffer,
    ULONG FrameSize);
