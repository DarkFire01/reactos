
#include "GenericFramebuffer.h"
#include <debug.h>
typedef enum _CONFIGURATION_TYPE {
  ArcSystem,
  CentralProcessor,
  FloatingPointProcessor,
  PrimaryIcache,
  PrimaryDcache,
  SecondaryIcache,
  SecondaryDcache,
  SecondaryCache,
  EisaAdapter,
  TcAdapter,
  ScsiAdapter,
  DtiAdapter,
  MultiFunctionAdapter,
  DiskController,
  TapeController,
  CdromController,
  WormController,
  SerialController,
  NetworkController,
  DisplayController,
  ParallelController,
  PointerController,
  KeyboardController,
  AudioController,
  OtherController,
  DiskPeripheral,
  FloppyDiskPeripheral,
  TapePeripheral,
  ModemPeripheral,
  MonitorPeripheral,
  PrinterPeripheral,
  PointerPeripheral,
  KeyboardPeripheral,
  TerminalPeripheral,
  OtherPeripheral,
  LinePeripheral,
  NetworkPeripheral,
  SystemMemory,
  DockingInformation,
  RealModeIrqRoutingTable,
  RealModePCIEnumeration,
  MaximumType
} CONFIGURATION_TYPE, *PCONFIGURATION_TYPE;
typedef ULONG_PTR PFN_NUMBER, *PPFN_NUMBER;
/************************************ Globals *********************************************/
#include <arc/arc.h>
typedef struct _ROSEFI_FRAMEBUFFER_DATA
{
    ULONG_PTR    BaseAddress;
    ULONG        BufferSize;
    UINT32       ScreenWidth;
    UINT32       ScreenHeight;
    UINT32       PixelsPerScanLine;
    UINT32       PixelFormat;
} ROSEFI_FRAMEBUFFER_DATA, *PROSEFI_FRAMEBUFFER_DATA;
/************************************ Private *********************************************/
ROSEFI_FRAMEBUFFER_DATA refiFbData;
BOOLEAN
FASTCALL
GenericFramebufferMapVideoMemory(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PVIDEO_MEMORY RequestedAddress,
    PVIDEO_MEMORY_INFORMATION MapInformation,
    PSTATUS_BLOCK StatusBlock)
{
    PHYSICAL_ADDRESS FrameBuffer;
    ULONG inIoSpace = VIDEO_MEMORY_SPACE_MEMORY;

    StatusBlock->Information = sizeof(VIDEO_MEMORY_INFORMATION);

    /* Reuse framebuffer that was set up by firmware */
    FrameBuffer.QuadPart = refiFbData.BaseAddress; //GetFramebuffer
    MapInformation->VideoRamBase = RequestedAddress->RequestedVirtualAddress;
    MapInformation->VideoRamLength = refiFbData.BufferSize;//NV2A_VIDEO_MEMORY_SIZE;

    VideoPortMapMemory(
        DeviceExtension,
        FrameBuffer,
        &MapInformation->VideoRamLength,
        &inIoSpace,
        &MapInformation->VideoRamBase);

    MapInformation->FrameBufferBase = MapInformation->VideoRamBase;
    MapInformation->FrameBufferLength = MapInformation->VideoRamLength;

    DPRINT1("Mapped 0x%x bytes of phys mem at 0x%lx to virt addr 0x%p\n",
        MapInformation->VideoRamLength, FrameBuffer.u.LowPart, MapInformation->VideoRamBase);

    return TRUE;
}

/*
 * VBEUnmapVideoMemory
 *
 * Releases a mapping between the virtual address space and the adapter's
 * frame buffer and video RAM.
 */

BOOLEAN
FASTCALL
GenericFramebufferUnmapVideoMemory(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PVIDEO_MEMORY VideoMemory,
    PSTATUS_BLOCK StatusBlock)
{
    VideoPortUnmapMemory(
        DeviceExtension,
        VideoMemory->RequestedVirtualAddress,
        NULL);

    return TRUE;
}

BOOLEAN
FASTCALL
GenericFramebufferQueryCurrentMode(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PVIDEO_MODE_INFORMATION VideoMode,
    PSTATUS_BLOCK StatusBlock)
{
    UCHAR BytesPerPixel;

    VideoMode->Length = sizeof(VIDEO_MODE_INFORMATION);
    VideoMode->ModeIndex = 0;

    VideoMode->VisScreenWidth = refiFbData.ScreenWidth;// READ_REGISTER_ULONG((ULONG_PTR)DeviceExtension->VirtControlStart + NV2A_RAMDAC_FP_HVALID_END) + 1;
    VideoMode->VisScreenHeight = refiFbData.ScreenHeight; //READ_REGISTER_ULONG((ULONG_PTR)DeviceExtension->VirtControlStart + NV2A_RAMDAC_FP_VVALID_END) + 1;

    if (VideoMode->VisScreenWidth <= 1 || VideoMode->VisScreenHeight <= 1)
    {
        DPRINT1("Cannot obtain current screen resolution!\n");
        return FALSE;
    }

    BytesPerPixel = 4;//NvGetBytesPerPixel(DeviceExtension, VideoMode->VisScreenWidth);
    ASSERT(BytesPerPixel >= 1 && BytesPerPixel <= 4);

    VideoMode->ScreenStride = VideoMode->VisScreenWidth * BytesPerPixel;
    VideoMode->NumberOfPlanes = 1;
    VideoMode->BitsPerPlane = BytesPerPixel * 8;
    VideoMode->Frequency = 1;
    VideoMode->XMillimeter = 0; /* FIXME */
    VideoMode->YMillimeter = 0; /* FIXME */
    if (BytesPerPixel >= 3)
    {
        VideoMode->NumberRedBits = 8;
        VideoMode->NumberGreenBits = 8;
        VideoMode->NumberBlueBits = 8;
        VideoMode->RedMask = 0xFF0000;
        VideoMode->GreenMask = 0x00FF00;
        VideoMode->BlueMask = 0x0000FF;
    }
    else
    {
        /* FIXME: not implemented */
        DPRINT1("BytesPerPixel %d - not implemented\n", BytesPerPixel);
    }
    VideoMode->VideoMemoryBitmapWidth = VideoMode->VisScreenWidth;
    VideoMode->VideoMemoryBitmapHeight = VideoMode->VisScreenHeight;
    VideoMode->AttributeFlags = VIDEO_MODE_GRAPHICS | VIDEO_MODE_COLOR |
        VIDEO_MODE_NO_OFF_SCREEN;
    VideoMode->DriverSpecificAttributeFlags = 0;

    StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);

    return TRUE;
}

BOOLEAN
FASTCALL
GenericFramebufferResetDevice(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PSTATUS_BLOCK StatusBlock)
{
    /* There is nothing to be done here */

    return TRUE;
}


BOOLEAN
FASTCALL
GenericFramebufferQueryNumAvailModes(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PVIDEO_NUM_MODES Modes,
    PSTATUS_BLOCK StatusBlock)
{
    Modes->NumModes = 1;
    Modes->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
    StatusBlock->Information = sizeof(VIDEO_NUM_MODES);
    return TRUE;
}

/*
 * GenericFramebufferQueryAvailModes
 *
 * Returns information about each video mode supported by the adapter.
 */

BOOLEAN
FASTCALL
GenericFramebufferQueryAvailModes(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PVIDEO_MODE_INFORMATION VideoMode,
    PSTATUS_BLOCK StatusBlock)
{
    return GenericFramebufferQueryCurrentMode(DeviceExtension, VideoMode, StatusBlock);
}

BOOLEAN
FASTCALL
GenericFramebufferSetCurrentMode(
    PGENERICFRAMEBUFFER_DEVICE_EXTENSION DeviceExtension,
    PVIDEO_MODE RequestedMode,
    PSTATUS_BLOCK StatusBlock)
{
    if (RequestedMode->RequestedMode != 0)
    {
        return FALSE;
    }

    /* Nothing to do, really. We only support a single mode and we're already
     * in that mode
     */
    return TRUE;
}


/************************************ Public  *********************************************/

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenericFramebufferFindAdapter(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID HwContext,
    _In_ PWSTR ArgumentString,
    _In_ PVIDEO_PORT_CONFIG_INFO ConfigInfo,
    _In_ PUCHAR Again)
{

    return NO_ERROR;
}

CODE_SEG("PAGE")
BOOLEAN NTAPI
GenericFramebufferInitialize(
    _In_ PVOID HwDeviceExtension)
{
    return TRUE;
}

CODE_SEG("PAGE")
BOOLEAN NTAPI
GenericFramebufferStartIO             (
    _In_ PVOID HwDeviceExtension,
    _Inout_ PVIDEO_REQUEST_PACKET RequestPacket)
{
    BOOLEAN Result;

    RequestPacket->StatusBlock->Status = ERROR_INVALID_PARAMETER;

    switch (RequestPacket->IoControlCode)
    {
        case IOCTL_VIDEO_SET_CURRENT_MODE:
        {
            DPRINT1("GenericFramebuffferStartIO IOCTL_VIDEO_SET_CURRENT_MODE\n");

            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MODE))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return TRUE;
            }

            Result = GenericFramebufferSetCurrentMode(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                (PVIDEO_MODE)RequestPacket->InputBuffer,
                RequestPacket->StatusBlock);
            break;
        }
        case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        {
            DPRINT1("GenericFramebuffferStartIO IOCTL_VIDEO_MAP_VIDEO_MEMORY\n");

            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION) ||
                RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return TRUE;
            }

            Result = GenericFramebufferMapVideoMemory(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                (PVIDEO_MEMORY)RequestPacket->InputBuffer,
                (PVIDEO_MEMORY_INFORMATION)RequestPacket->OutputBuffer,
                RequestPacket->StatusBlock);
            break;
        }
        case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        {
            DPRINT1("GenericFramebuffferStartIO IOCTL_VIDEO_UNMAP_VIDEO_MEMORY\n");

            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return TRUE;
            }

            Result = GenericFramebufferUnmapVideoMemory(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                (PVIDEO_MEMORY)RequestPacket->InputBuffer,
                RequestPacket->StatusBlock);
            break;
        }
        case IOCTL_VIDEO_RESET_DEVICE:
        {
            DPRINT1("GenericFramebufferStartIO  IOCTL_VIDEO_RESET_DEVICE\n");

            Result = GenericFramebufferResetDevice(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                RequestPacket->StatusBlock);
            break;
        }
        case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        {
            DPRINT1("GenericFramebufferStartIO  IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES\n");

            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_NUM_MODES))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return TRUE;
            }

            Result = GenericFramebufferQueryNumAvailModes(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                (PVIDEO_NUM_MODES)RequestPacket->OutputBuffer,
                RequestPacket->StatusBlock);
            break;
        }
        case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        {
            DPRINT1("GenericFramebufferStartIO   IOCTL_VIDEO_QUERY_AVAIL_MODES\n");

            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return TRUE;
            }

            Result = GenericFramebufferQueryAvailModes(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer,
                RequestPacket->StatusBlock);
            break;
        }
        case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        {
            DPRINT1("GenericFramebufferStartIO   IOCTL_VIDEO_QUERY_CURRENT_MODE\n");

            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return TRUE;
            }

            Result = GenericFramebufferQueryCurrentMode(
                (PGENERICFRAMEBUFFER_DEVICE_EXTENSION)HwDeviceExtension,
                (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer,
                RequestPacket->StatusBlock);
            break;
        }

        default:
        {
            DPRINT1("GenericFramebufferStartIO 0x%x not implemented\n", RequestPacket->IoControlCode);

            RequestPacket->StatusBlock->Status = ERROR_INVALID_FUNCTION;
            return FALSE;
        }
    }

    if (Result)
    {
        RequestPacket->StatusBlock->Status = NO_ERROR;
    }

    return TRUE;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenericFramebufferSetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _In_ PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    /* Unused */
    return NO_ERROR;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenericFramebufferGetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _Out_ PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    /* Unused */
    return NO_ERROR;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenericFramebufferGetVideoChildDescriptor(
    _In_ PVOID HwDeviceExtension,
    _In_ PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
    _Out_ PVIDEO_CHILD_TYPE VideoChildType,
    _Out_ PUCHAR pChildDescriptor,
    _Out_ PULONG UId,
    _Out_ PULONG pUnused)
{
    /* Unused */
    return NO_ERROR;
}
REACTOS_BGCONTEXT
HalGrabUefiContext();
ULONG NTAPI
DriverEntry(PVOID Context1, PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA VideoInitData;
    REACTOS_BGCONTEXT HalContext = HalGrabUefiContext();
    VideoDebugPrint((Info, "GenericFramebuffer: DriverEntry\n"));
    VideoPortZeroMemory(&VideoInitData, sizeof(VideoInitData));
    VideoInitData.HwInitDataSize = sizeof(VideoInitData);
    VideoInitData.HwFindAdapter = GenericFramebufferFindAdapter;
    VideoInitData.HwInitialize = GenericFramebufferInitialize;
    VideoInitData.HwStartIO = GenericFramebufferStartIO;
    VideoInitData.HwDeviceExtensionSize = sizeof(GENERICFRAMEBUFFER_DEVICE_EXTENSION);
    VideoInitData.HwSetPowerState = GenericFramebufferSetPowerState;
    VideoInitData.HwGetPowerState = GenericFramebufferGetPowerState;
    VideoInitData.HwGetVideoChildDescriptor = GenericFramebufferGetVideoChildDescriptor;

    refiFbData.BaseAddress        = (ULONG_PTR)HalContext.BaseAddress      ;
    refiFbData.BufferSize         = HalContext.BufferSize       ;
    refiFbData.ScreenWidth        = HalContext.ScreenWidth      ;
    refiFbData.ScreenHeight       = HalContext.ScreenHeight     ;
    refiFbData.PixelsPerScanLine  = HalContext.PixelsPerScanLine;
    refiFbData.PixelFormat        = HalContext.PixelFormat      ;

    return VideoPortInitialize(Context1, Context2, &VideoInitData, NULL);
}
