/*
 * PROJECT:     ReactOS GenericFramebuffer graphics card driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     GenericFramebuffer primary header
 * COPYRIGHT:   Copyright 2022-2023 Justin Miller <justinmiller100@gmail.com>
 *              Copyright 2023 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#include <ntifs.h>
#include <arc/arc.h>
#include <ndk/kefuncs.h>

#include <dderror.h>
#define __BROKEN__
#include <miniport.h>
#include <video.h>
#include <devioctl.h>

#include <section_attribs.h>

#include <debug.h>
// #define DPRINT(fmt, ...)    VideoDebugPrint((Info, fmt, ##__VA_ARGS__))
// #define DPRINT1(fmt, ...)   VideoDebugPrint((Error, fmt, ##__VA_ARGS__))

//#define __debugbreak()

#include <drivers/bootvid/framebuf.h>
#include <drivers/bootvid/cfgconv.h>

/********************************** Globals ***********************************/

typedef struct _BOOT_FRAMEBUF_DATA
{
    PHYSICAL_ADDRESS BaseAddress; // ULONG_PTR
    ULONG BufferSize;
    ULONG ScreenWidth;
    ULONG ScreenHeight;
    ULONG PixelsPerScanLine;
    ULONG PixelFormat;
} BOOT_FRAMEBUF_DATA, *PBOOT_FRAMEBUF_DATA;

// typedef struct
// {
//     PUCHAR Mapped;
//     PHYSICAL_ADDRESS RangeStart;
//     ULONG RangeLength;
//     UCHAR RangeInIoSpace;
// } GENFB_ADDRESS_RANGE;

typedef struct
{
    BOOT_FRAMEBUF_DATA FrameBufData;
    // GENFB_ADDRESS_RANGE FrameBuffer;
    PVOID FrameAddress; // Mapped framebuffer virtual address.

    /* Configuration data from hardware tree */
    CM_FRAMEBUF_DEVICE_DATA VideoConfigData;
    CM_MONITOR_DEVICE_DATA MonitorConfigData;

    /* The one and only video mode we support */
    VIDEO_MODE_INFORMATION CurrentVideoMode;
} GENFB_DEVICE_EXTENSION, *PGENFB_DEVICE_EXTENSION;


/********************************** Private ***********************************/

/**
 * @brief
 * Maps the video framebuffer to the requested preferred address.
 **/
static VP_STATUS
GenFbVmpMapVideoMemory(
    _In_ PGENFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PVIDEO_MEMORY RequestedAddress,
    _Out_ PVIDEO_MEMORY_INFORMATION MapInformation,
    _Out_ PSTATUS_BLOCK StatusBlock)
{
    VP_STATUS Status;
    PBOOT_FRAMEBUF_DATA FrameBufData = &DeviceExtension->FrameBufData;
    PHYSICAL_ADDRESS FrameBuffer = FrameBufData->BaseAddress;
    ULONG InIoSpace = VIDEO_MEMORY_SPACE_MEMORY;

    /* Map the framebuffer (set up by the firmware) to the
     * preferred address the user requests, if possible. */
    MapInformation->VideoRamBase = RequestedAddress->RequestedVirtualAddress;
    MapInformation->VideoRamLength = FrameBufData->BufferSize;

// NOTE: VideoRamLength == VideoMemoryBitmapHeight * ScreenStride
// and:  FrameBufferLength == VisScreenHeight * ScreenStride.

    Status = VideoPortMapMemory(DeviceExtension,
                                FrameBuffer,
                                &MapInformation->VideoRamLength,
                                &InIoSpace,
                                &MapInformation->VideoRamBase);
    if (Status != NO_ERROR)
    {
        DPRINT1("Failed to map framebuffer memory 0x%I64x\n", FrameBuffer.QuadPart);
        return Status;
    }

    /* For framebuffers, it is expected that FrameBufferBase == VideoRamBase */
    MapInformation->FrameBufferBase   = MapInformation->VideoRamBase;
    MapInformation->FrameBufferLength = MapInformation->VideoRamLength;

    DPRINT1("Mapped 0x%x bytes of phys mem at 0x%I64x to virt addr 0x%p\n",
            MapInformation->VideoRamLength,
            FrameBuffer.QuadPart,
            MapInformation->VideoRamBase);

    StatusBlock->Information = sizeof(VIDEO_MEMORY_INFORMATION);
    return Status;
}

/**
 * @brief
 * Releases the mapping between the virtual address space
 * and the adapter's framebuffer and video RAM.
 **/
static VP_STATUS
GenFbVmpUnmapVideoMemory(
    _In_ PGENFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PVIDEO_MEMORY VideoMemory,
    _Out_ PSTATUS_BLOCK StatusBlock)
{
    return VideoPortUnmapMemory(DeviceExtension,
                                VideoMemory->RequestedVirtualAddress,
                                NULL);
}


#if 0

struct ET6000ColorModes {
    UCHAR bpp;
    UCHAR redBitsNum;
    UCHAR greenBitsNum;
    UCHAR blueBitsNum;
    ULONG redMask;
    ULONG greenMask;
    ULONG blueMask;
} et6000ColorModes[] = {
    {16, 5, 5, 5, 0x00007c00, 0x000003e0, 0x0000001f}, /* 5:5:5 */
    {16, 5, 6, 5, 0x0000f800, 0x000007e0, 0x0000001f}, /* 5:6:5 */
    {24, 8, 8, 8, 0x00ff0000, 0x0000ff00, 0x000000ff}  /* 8:8:8 */
};

/**
 * From: Windows NT Video Miniport for the VirtualBox/bochs/qemu SVGA adapter
 * Copyright (c) 2012  Michal Necasek
 **/
/* Determine pixel mask given color depth and color channel */
static ULONG vmpMaskByBpp(UCHAR Bpp, COLOR_CHANNEL Channel)
{
    ULONG ulMask;

    switch (Bpp)
    {
    case 32:
    case 24: /* 8:8:8 */
        ulMask = 0x00FF0000 >> (Channel * 8);
        break;
    case 15: /* 5:5:5 */
        ulMask = 0x00007C00 >> (Channel * 5);
        break;
    case 16:
        switch (Channel)
        {
        case CHANNEL_RED:
            ulMask = 0x0000F800;
            break;
        case CHANNEL_GREEN:
            ulMask = 0x000007E0;
            break;
        case CHANNEL_BLUE:
            ulMask = 0x0000001F;
            break;
        }
        break;
    case 8:
    case 4:
    default:
        ulMask = 0; /* Palettized modes don't have a mask */
    }

    return ulMask;
}

#endif

static inline unsigned int _vid_popcount(unsigned int x)
{
#ifdef HAVE___BUILTIN_POPCOUNT
    return __popcount(x);
#else
    x -= (x >> 1) & 0x55555555;
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    return ((x + (x >> 4)) & 0x0f0f0f0f) * 0x01010101 >> 24;
#endif
}

static BOOLEAN
GenFbVmpSetupCurrentMode(
    _In_ PGENFB_DEVICE_EXTENSION DeviceExtension)
{
    PVIDEO_MODE_INFORMATION VideoMode = &DeviceExtension->CurrentVideoMode;
    PBOOT_FRAMEBUF_DATA FrameBufData  = &DeviceExtension->FrameBufData;
    PCM_FRAMEBUF_DEVICE_DATA VideoData  = &DeviceExtension->VideoConfigData;
    PCM_MONITOR_DEVICE_DATA MonitorData = &DeviceExtension->MonitorConfigData;
    UCHAR BytesPerPixel;

    VideoMode->Length = sizeof(VIDEO_MODE_INFORMATION);
    VideoMode->ModeIndex = 0;

    /* If the individual framebuffer screen sizes are not
     * already initialized by now, use monitor data. */
    if ((FrameBufData->ScreenWidth == 0) || (FrameBufData->ScreenHeight == 0))
    {
        FrameBufData->ScreenWidth  = VideoData->ScreenWidth;
        FrameBufData->ScreenHeight = VideoData->ScreenHeight;
    }
    // FIXME: Remove redundancies between those fields...
    if ((FrameBufData->ScreenWidth == 0) || (FrameBufData->ScreenHeight == 0))
    {
        VideoData->ScreenWidth  = MonitorData->HorizontalResolution;
        VideoData->ScreenHeight = MonitorData->VerticalResolution;

        FrameBufData->ScreenWidth  = MonitorData->HorizontalResolution;
        FrameBufData->ScreenHeight = MonitorData->VerticalResolution;
    }

    if (FrameBufData->ScreenWidth <= 1 || FrameBufData->ScreenHeight <= 1)
    {
        DPRINT1("Cannot obtain current screen resolution!\n");
        return FALSE;
    }

    VideoMode->VisScreenWidth  = FrameBufData->ScreenWidth;
    VideoMode->VisScreenHeight = FrameBufData->ScreenHeight;

    BytesPerPixel = VideoData->BitsPerPixel / 8;
    ASSERT(BytesPerPixel >= 1 && BytesPerPixel <= 4);

    /* Bytes per scanline */
    // FrameBufData->PixelsPerScanLine;
    VideoMode->ScreenStride = VideoMode->VisScreenWidth * BytesPerPixel; // == PixelsPerScanLine

    VideoMode->NumberOfPlanes = 1;
    VideoMode->BitsPerPlane = VideoData->BitsPerPixel;

    /* Video frequency in Hertz */
    VideoMode->Frequency = VideoData->VideoClock;
    if (VideoMode->Frequency == 0) // or 1 ?
        VideoMode->Frequency = 60; // Default value.

    /* Use metrics from the monitor, if any */
    VideoMode->XMillimeter = MonitorData->HorizontalScreenSize;
    VideoMode->YMillimeter = MonitorData->VerticalScreenSize;
    if ((VideoMode->XMillimeter == 0) || (VideoMode->YMillimeter == 0))
    {
        /* Assume 96 DPI and 25.4 millimeters per inch, round to nearest */
        static const ULONG dpi = 96;
        // VideoMode->XMillimeter = FrameBufData->ScreenWidth  * 254 / 960;
        // VideoMode->YMillimeter = FrameBufData->ScreenHeight * 254 / 960;
        VideoMode->XMillimeter = ((ULONGLONG)FrameBufData->ScreenWidth  * 254 + (dpi * 5)) / (dpi * 10);
        VideoMode->YMillimeter = ((ULONGLONG)FrameBufData->ScreenHeight * 254 + (dpi * 5)) / (dpi * 10);
    }

    // FrameBufData->PixelFormat;
    if (VideoData->BitsPerPixel > 8) // (BytesPerPixel > 1)
    {
        if (VideoData->PixelInformation.RedMask   == 0 &&
            VideoData->PixelInformation.GreenMask == 0 &&
            VideoData->PixelInformation.BlueMask  == 0)
        {
            switch (VideoData->BitsPerPixel)
            {
                case 32:
                case 24: /* 8:8:8 */
                    VideoMode->RedMask   = 0x00FF0000;
                    VideoMode->GreenMask = 0x0000FF00;
                    VideoMode->BlueMask  = 0x000000FF;
                    break;
                case 15: /* 5:5:5 */
                    VideoMode->RedMask   = 0x00007C00;
                    VideoMode->GreenMask = 0x000003E0;
                    VideoMode->BlueMask  = 0x0000001F;
                    break;
                case 16: /* 5:6:5 */
                    VideoMode->RedMask   = 0x0000F800;
                    VideoMode->GreenMask = 0x000007E0;
                    VideoMode->BlueMask  = 0x0000001F;
                    break;
                case 8:
                case 4:
                default:
                    /* Palettized modes don't have a mask */
                    VideoMode->RedMask   = 0;
                    VideoMode->GreenMask = 0;
                    VideoMode->BlueMask  = 0;
            }
        }
        else
        {
            VideoMode->RedMask   = VideoData->PixelInformation.RedMask;
            VideoMode->GreenMask = VideoData->PixelInformation.GreenMask;
            VideoMode->BlueMask  = VideoData->PixelInformation.BlueMask;
        }

        VideoMode->NumberRedBits   = _vid_popcount(VideoMode->RedMask);   // 8;
            // VideoData->PixelInformation.NumberRedBits;
        VideoMode->NumberGreenBits = _vid_popcount(VideoMode->GreenMask); // 8;
            // VideoData->PixelInformation.NumberGreenBits;
        VideoMode->NumberBlueBits  = _vid_popcount(VideoMode->BlueMask);  // 8;
            // VideoData->PixelInformation.NumberBlueBits;
    }
    else
    {
        /* FIXME: not implemented */
        DPRINT1("BitsPerPixel %d - not implemented\n", VideoData->BitsPerPixel);
    }

    VideoMode->VideoMemoryBitmapWidth  = VideoMode->VisScreenWidth;
    VideoMode->VideoMemoryBitmapHeight = VideoMode->VisScreenHeight;

    VideoMode->AttributeFlags =
        VIDEO_MODE_GRAPHICS | VIDEO_MODE_COLOR | VIDEO_MODE_NO_OFF_SCREEN |
        ((VideoData->BitsPerPixel <= 8) ? VIDEO_MODE_PALETTE_DRIVEN : 0);
    VideoMode->DriverSpecificAttributeFlags = 0;

    return TRUE;
}


/*********************************** Public ***********************************/

/* Data structure we can receive from ConfigurationData
 * for either VpControllerData or VpMonitorData. */
typedef union _VP_HARDWARE_CONFIGURATION_DATA
{
    /* For VpControllerData */
    struct
    {
        /* Legacy configuration data */
        VIDEO_HARDWARE_CONFIGURATION_DATA legacyConfigData;

        /* New configuration data via resource descriptor (if any) */
        PCM_FULL_RESOURCE_DESCRIPTOR cmDescriptor;
        ULONG cmResDescriptorLength;
    } video;

    /* For VpMonitorData */
    struct
    {
        /* Legacy configuration data */
        MONITOR_HARDWARE_CONFIGURATION_DATA legacyConfigData;

        /* New configuration data via resource descriptor (if any) */
        PCM_FULL_RESOURCE_DESCRIPTOR cmDescriptor;
        ULONG cmResDescriptorLength;
    } monitor;
} VP_HARDWARE_CONFIGURATION_DATA, *PVP_HARDWARE_CONFIGURATION_DATA;

// HACK:
// PMINIPORT_QUERY_DEVICE_ROUTINE GenFbGetDeviceDataCallback;
static VP_STATUS
NTAPI
GenFbGetDeviceDataCallback(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID Context,
    _In_ VIDEO_DEVICE_DATA_TYPE DeviceDataType,
    _In_ PVOID Identifier,
    _In_ ULONG IdentifierLength,
    _In_ PVOID ConfigurationData,
    _In_ ULONG ConfigurationDataLength,
    _Inout_ PVOID ComponentInformation,
    _In_ ULONG ComponentInformationLength);

static VP_STATUS
NTAPI
VpGetDeviceDataCallback(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID Context,
    _In_ VIDEO_DEVICE_DATA_TYPE DeviceDataType,
    _In_ PVOID Identifier,              // PWCHAR
    _In_ ULONG IdentifierLength,
    _In_ PVOID ConfigurationData,
    _In_ ULONG ConfigurationDataLength,
    _Inout_ PVOID ComponentInformation, // PCM_COMPONENT_INFORMATION
    _In_ ULONG ComponentInformationLength)
{
    /* Local buffer for conversion if needed */
    VP_HARDWARE_CONFIGURATION_DATA configData = {0};

    switch (DeviceDataType)
    {
        case VpControllerData:
        {
            DPRINT1("\n"
                    "sizeof(VIDEO_HARDWARE_CONFIGURATION_DATA) == %lu\n"
                    "sizeof(CM_FULL_RESOURCE_DESCRIPTOR) == %lu\n"
                    "sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == %lu\n",
                sizeof(VIDEO_HARDWARE_CONFIGURATION_DATA),
                sizeof(CM_FULL_RESOURCE_DESCRIPTOR),
                sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

            if (!ConfigurationData ||
                (ConfigurationDataLength < min(sizeof(VIDEO_HARDWARE_CONFIGURATION_DATA),
                                               sizeof(CM_FULL_RESOURCE_DESCRIPTOR))))
            {
                /* Unknown format, just call the user callback */
                break;
            }

            /*
             * Suppose first the configuration data is of the
             * CM_FULL_RESOURCE_DESCRIPTOR format -- the older
             * MIPS-oriented VIDEO_HARDWARE_CONFIGURATION_DATA
             * shares the first fields InterfaceType and BusNumber
             * with CM_FULL_RESOURCE_DESCRIPTOR, and Version and
             * Revision with CM_PARTIAL_RESOURCE_LIST.
             * NT OS loader for MIPS will use the old format and
             * set non-zero values for Version/Revision, while
             * the x86/x64 loader either won't set any data, or
             * may but with Version/Revision == 0. ReactOS loader
             * also does this, and will use CM_FULL_RESOURCE_DESCRIPTOR.
             * If we detect a Version/Revision == 0, assume we use
             * that newer data format, and convert it back to older
             * one, heuristically. This will allow backward compatibility
             * with Windows miniports. We will then append the actual
             * CM_FULL_RESOURCE_DESCRIPTOR data and update the data length.
             * It has been verified that standard Windows miniports
             * do not check this value.
             */
            // if (fbConfigData.cmDescriptor->PartialResourceList.Version == 0)
            if (ConfigurationDataLength != sizeof(VIDEO_HARDWARE_CONFIGURATION_DATA) &&
                ConfigurationDataLength >= sizeof(CM_FULL_RESOURCE_DESCRIPTOR))
            {
                ConvertVideoDataToLegacyConfigData(&configData.video.legacyConfigData,
                                                   ConfigurationData);
                configData.video.cmDescriptor = ConfigurationData;
                configData.video.cmResDescriptorLength = ConfigurationDataLength;
                ConfigurationData = &configData.video;
                ConfigurationDataLength = sizeof(configData.video);
            }

            break;
        }

        case VpMonitorData:
        {
            DPRINT1("\n"
                    "sizeof(MONITOR_HARDWARE_CONFIGURATION_DATA) == %lu\n"
                    "sizeof(CM_FULL_RESOURCE_DESCRIPTOR) == %lu\n"
                    "sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == %lu\n",
                sizeof(MONITOR_HARDWARE_CONFIGURATION_DATA),
                sizeof(CM_FULL_RESOURCE_DESCRIPTOR),
                sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

            if (!ConfigurationData ||
                (ConfigurationDataLength < min(sizeof(MONITOR_HARDWARE_CONFIGURATION_DATA),
                                               sizeof(CM_FULL_RESOURCE_DESCRIPTOR))))
            {
                /* Unknown format, just call the user callback */
                break;
            }

            // if (monConfigData.cmDescriptor->PartialResourceList.Version == 0)
            if (ConfigurationDataLength != sizeof(MONITOR_HARDWARE_CONFIGURATION_DATA) &&
                ConfigurationDataLength >= sizeof(CM_FULL_RESOURCE_DESCRIPTOR))
            {
                ConvertMonitorDataToLegacyConfigData(&configData.monitor.legacyConfigData,
                                                     ConfigurationData);
                configData.monitor.cmDescriptor = ConfigurationData;
                configData.monitor.cmResDescriptorLength = ConfigurationDataLength;
                ConfigurationData = &configData.monitor;
                ConfigurationDataLength = sizeof(configData.monitor);
            }

            break;
        }

        default:
            break;
    }

    /* Call the user callback */
    return GenFbGetDeviceDataCallback(HwDeviceExtension,
                                      Context,
                                      DeviceDataType,
                                      Identifier,
                                      IdentifierLength,
                                      ConfigurationData,
                                      ConfigurationDataLength,
                                      ComponentInformation,
                                      ComponentInformationLength);
}


/**
 * @brief
 * Callback routine for the VideoPortGetDeviceData function.
 *
 * @return
 * - NO_ERROR if the function completed properly.
 * - ERROR_DEV_NOT_EXIST if we did not find the device.
 * - ERROR_INVALID_PARAMETER otherwise.
 **/
static VP_STATUS
NTAPI
GenFbGetDeviceDataCallback(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID Context,
    _In_ VIDEO_DEVICE_DATA_TYPE DeviceDataType,
    _In_ PVOID Identifier,
    _In_ ULONG IdentifierLength,
    _In_ PVOID ConfigurationData,
    _In_ ULONG ConfigurationDataLength,
    _Inout_ PVOID ComponentInformation,
    _In_ ULONG ComponentInformationLength)
{
    PGENFB_DEVICE_EXTENSION hwDeviceExtension = HwDeviceExtension;
    PVIDEO_PORT_CONFIG_INFO ConfigInfo = Context;
    PWCHAR identifier = Identifier;
    PCM_COMPONENT_INFORMATION CompInfo = ComponentInformation;
    VIDEO_ACCESS_RANGE accessRanges[1]; // [2];
    VP_STATUS status;

    switch (DeviceDataType)
    {
        case VpControllerData:
        {
            PVP_HARDWARE_CONFIGURATION_DATA configData = ConfigurationData;

            DPRINT1("Getting controller information: Display: '%.*ws'\n",
                    IdentifierLength/sizeof(WCHAR), identifier);

            // if (ConfigurationDataLength == sizeof(VIDEO_HARDWARE_CONFIGURATION_DATA))
            // if (ConfigurationDataLength == sizeof(VP_HARDWARE_CONFIGURATION_DATA))
            if (!ConfigurationData ||
                (ConfigurationDataLength < sizeof(VIDEO_HARDWARE_CONFIGURATION_DATA)))
            {
                DPRINT1("Invalid display configuration data %p %lu\n",
                        ConfigurationData, ConfigurationDataLength);
                return ERROR_DEV_NOT_EXIST;
            }

            if (CompInfo && (ComponentInformationLength == sizeof(CM_COMPONENT_INFORMATION)) &&
                !(CompInfo->Flags.Output && CompInfo->Flags.ConsoleOut))
            {
                DPRINT1("Weird: this DisplayController has flags %lu\n", CompInfo->Flags);
            }

            // if (VideoPortCompareMemory(identifier, L"UEFI GOP Framebuffer", 20) != 20)
            //     return ERROR_DEV_NOT_EXIST;


            /* Initialize the display adapter parameters, converting
             * them to the new format if needed */
            if (ConfigurationDataLength == sizeof(configData->video.legacyConfigData))
            {
                /* Legacy configuration data, convert it into new format */
                CM_PARTIAL_RESOURCE_DESCRIPTOR FrameBuffer;

                ConvertLegacyVideoConfigDataToDeviceData(
                    &configData->video.legacyConfigData,
                    NULL, // Interrupt
                    NULL, // ControlPort
                    NULL, // CursorPort
                    &FrameBuffer);

                /* Save the framebuffer base and size */
                hwDeviceExtension->FrameBufData.BaseAddress = FrameBuffer.u.Memory.Start;
                hwDeviceExtension->FrameBufData.BufferSize  = FrameBuffer.u.Memory.Length;

                /* The legacy video controller configuration data does not
                 * contain any information regarding framebuffer format, etc.
                 * so set them to zero. We will later calculate default values. */
                VideoPortZeroMemory(&hwDeviceExtension->VideoConfigData,
                                    sizeof(hwDeviceExtension->VideoConfigData));
            }
            else if ((ConfigurationDataLength == sizeof(configData->video)) &&
                     configData->video.cmResDescriptorLength >= sizeof(*configData->video.cmDescriptor))
            {
                /* New configuration data */
                PCM_FULL_RESOURCE_DESCRIPTOR cmDescriptor = configData->video.cmDescriptor;
                PCM_PARTIAL_RESOURCE_LIST ResourceList = &cmDescriptor->PartialResourceList;
                PCM_PARTIAL_RESOURCE_DESCRIPTOR FrameBuffer;
                PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;

                GetVideoData(ResourceList,
                             NULL, // Interrupt
                             NULL, // ControlPort
                             NULL, // CursorPort
                             &FrameBuffer,
                             &Descriptor);

                if (FrameBuffer)
                {
                    /* Save the framebuffer base and size */
                    hwDeviceExtension->FrameBufData.BaseAddress = FrameBuffer->u.Memory.Start;
                    hwDeviceExtension->FrameBufData.BufferSize  = FrameBuffer->u.Memory.Length;
                }
                else
                {
                    /* No framebuffer base?! Zero it out */
                    hwDeviceExtension->FrameBufData.BaseAddress.QuadPart = 0;
                    hwDeviceExtension->FrameBufData.BufferSize  = 0;
                }

                if (Descriptor &&
                    (Descriptor->u.DeviceSpecificData.DataSize >= sizeof(CM_FRAMEBUF_DEVICE_DATA)))
                {
                    /* NOTE: This descriptor *MUST* be the last one.
                     * The actual device data follows the descriptor. */
                    PCM_FRAMEBUF_DEVICE_DATA VideoData = (PCM_FRAMEBUF_DEVICE_DATA)(Descriptor + 1);
                    hwDeviceExtension->VideoConfigData = *VideoData;
                }
                else
                {
                    /* The configuration does not contain any information
                     * regarding framebuffer format, etc. so set them to zero.
                     * We will later calculate default values. */
                    VideoPortZeroMemory(&hwDeviceExtension->VideoConfigData,
                                        sizeof(hwDeviceExtension->VideoConfigData));
                }
            }


            /* Fail if no framebuffer was provided */
            if ((hwDeviceExtension->FrameBufData.BaseAddress.QuadPart == 0) ||
                (hwDeviceExtension->FrameBufData.BufferSize == 0))
            {
                DPRINT1("No framebuffer found!\n");
                return ERROR_DEV_NOT_EXIST;
            }


            /*
             * Fill up the device extension and the configuration
             * information with the appropriate data.
             */

            DBG_UNREFERENCED_PARAMETER(ConfigInfo);

#if 0
            ConfigInfo->BusInterruptLevel  = fbConfigData->Irql;
            ConfigInfo->BusInterruptVector = fbConfigData->Vector;

            /*
             * Save the ranges in the range buffer allocated for us.
             */

            accessRanges[0].RangeStart.HighPart = 0;
            accessRanges[0].RangeStart.LowPart = fbConfigData->ControlBase;
            accessRanges[0].RangeLength = fbConfigData->ControlSize;
            accessRanges[0].RangeInIoSpace = FALSE;
            accessRanges[0].RangeVisible = FALSE;
            accessRanges[0].RangeShareable = FALSE;
            accessRanges[0].RangePassive = FALSE;

            accessRanges[1].RangeStart.HighPart = 0;
            accessRanges[1].RangeStart.LowPart = fbConfigData->CursorBase;
            accessRanges[1].RangeLength = fbConfigData->CursorSize;
            accessRanges[1].RangeInIoSpace = FALSE;
            accessRanges[1].RangeVisible = FALSE;
            accessRanges[1].RangeShareable = FALSE;
            accessRanges[1].RangePassive = FALSE;

            accessRanges[2].RangeStart.HighPart = 0;
            accessRanges[2].RangeStart.LowPart = fbConfigData->FrameBase;
            accessRanges[2].RangeLength = fbConfigData->FrameSize;
            accessRanges[2].RangeInIoSpace = FALSE;
            accessRanges[2].RangeVisible = FALSE;
            accessRanges[2].RangeShareable = FALSE;
            accessRanges[2].RangePassive = FALSE;

#else

            accessRanges[0].RangeStart = hwDeviceExtension->FrameBufData.BaseAddress;
            accessRanges[0].RangeLength = hwDeviceExtension->FrameBufData.BufferSize;
            accessRanges[0].RangeInIoSpace = FALSE;
            accessRanges[0].RangeVisible = FALSE;
            accessRanges[0].RangeShareable = FALSE;
            accessRanges[0].RangePassive = FALSE;
#endif

            /* Check to see if there is a hardware resource conflict */
            status = VideoPortVerifyAccessRanges(HwDeviceExtension,
                                                 RTL_NUMBER_OF(accessRanges),
                                                 accessRanges);
            if (status != NO_ERROR)
                return status;

            /* Save framebuffer information */
            // hwDeviceExtension->PhysicalFrameAddress = fbConfigData->FrameBase;
            // hwDeviceExtension->FrameLength = fbConfigData->FrameSize;

#if 0
            /* Map the video controller into the system virtual address space */
            hwDeviceExtension->VideoAddress =
                VideoPortGetDeviceBase(hwDeviceExtension,
                                       accessRanges[0].RangeStart, // Control
                                       accessRanges[0].RangeLength,
                                       accessRanges[0].RangeInIoSpace);
            if (!hwDeviceExtension->VideoAddress)
                return ERROR_INVALID_PARAMETER;

            /* Map the video memory into the system virtual
             * address space so we can clear it out. */
            hwDeviceExtension->FrameAddress =
                VideoPortGetDeviceBase(hwDeviceExtension,
                                       accessRanges[1].RangeStart, // Frame
                                       accessRanges[1].RangeLength,
                                       accessRanges[1].RangeInIoSpace);
            if (!hwDeviceExtension->FrameAddress)
                return ERROR_INVALID_PARAMETER;
#else
            hwDeviceExtension->FrameAddress =
                VideoPortGetDeviceBase(hwDeviceExtension,
                                       accessRanges[0].RangeStart, // Frame
                                       accessRanges[0].RangeLength,
                                       accessRanges[0].RangeInIoSpace);
            if (!hwDeviceExtension->FrameAddress)
                return ERROR_INVALID_PARAMETER;

            DPRINT1("GenFbVmpFindAdapter: Mapped framebuffer 0x%I64x to 0x%p - size %lu\n",
                accessRanges[0].RangeStart, hwDeviceExtension->FrameAddress, accessRanges[0].RangeLength);
#endif

            return NO_ERROR;
        }

        case VpMonitorData:
        {
            PVP_HARDWARE_CONFIGURATION_DATA configData = ConfigurationData;

            DPRINT1("Getting monitor information: Monitor: '%.*ws'\n",
                    IdentifierLength/sizeof(WCHAR), identifier);

            // if (ConfigurationDataLength == sizeof(MONITOR_HARDWARE_CONFIGURATION_DATA))
            // if (ConfigurationDataLength == sizeof(VP_HARDWARE_CONFIGURATION_DATA))
            if (!ConfigurationData ||
                (ConfigurationDataLength < sizeof(MONITOR_HARDWARE_CONFIGURATION_DATA)))
            {
                DPRINT1("Invalid monitor configuration data %p %lu\n",
                        ConfigurationData, ConfigurationDataLength);
                return ERROR_DEV_NOT_EXIST;
            }

            if (CompInfo && (ComponentInformationLength == sizeof(CM_COMPONENT_INFORMATION)) &&
                !(CompInfo->Flags.Output && CompInfo->Flags.ConsoleOut))
            {
                DPRINT1("Weird: this MonitorPeripheral has flags %lu\n", CompInfo->Flags);
            }

            /* Initialize the monitor parameters, converting
             * them to the new format if needed */
            if (ConfigurationDataLength == sizeof(configData->monitor.legacyConfigData))
            {
                /* Legacy configuration data, convert it into new format */
                ConvertLegacyMonitorConfigDataToDeviceData(
                    &configData->monitor.legacyConfigData,
                    &hwDeviceExtension->MonitorConfigData);
            }
            else if ((ConfigurationDataLength == sizeof(configData->monitor)) &&
                     configData->monitor.cmResDescriptorLength >= sizeof(*configData->monitor.cmDescriptor))
            {
                /* New configuration data */
                PCM_FULL_RESOURCE_DESCRIPTOR cmDescriptor = configData->monitor.cmDescriptor;
                PCM_PARTIAL_RESOURCE_LIST ResourceList = &cmDescriptor->PartialResourceList;
                PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;

                GetMonitorData(ResourceList, &Descriptor);

                if (Descriptor &&
                    (Descriptor->u.DeviceSpecificData.DataSize >= sizeof(CM_MONITOR_DEVICE_DATA)))
                {
                    /* NOTE: This descriptor *MUST* be the last one.
                     * The actual device data follows the descriptor. */
                    PCM_MONITOR_DEVICE_DATA MonitorData = (PCM_MONITOR_DEVICE_DATA)(Descriptor + 1);
                    hwDeviceExtension->MonitorConfigData = *MonitorData;
                }
                else
                {
                    /* The configuration does not contain any information
                     * regarding the monitor so set them to zero.
                     * We will later calculate default values. */
                    VideoPortZeroMemory(&hwDeviceExtension->MonitorConfigData,
                                        sizeof(hwDeviceExtension->MonitorConfigData));
                }
            }

            return NO_ERROR;
        }

        default:
        {
            DPRINT1("Unknown device type %lu\n", DeviceDataType);
            return ERROR_INVALID_PARAMETER;
        }
    }
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenFbVmpFindAdapter(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID HwContext,
    _In_ PWSTR ArgumentString,
    _Inout_ PVIDEO_PORT_CONFIG_INFO ConfigInfo,
    _In_ PUCHAR Again)
{
    PBOOT_FRAMEBUF_DATA FrameBufData =
        &((PGENFB_DEVICE_EXTENSION)HwDeviceExtension)->FrameBufData;

    DPRINT1("GenFbVmpFindAdapter(%p, %p, %s, %p, %p)\n",
        HwDeviceExtension, HwContext, ArgumentString, ConfigInfo, Again);

    *Again = FALSE;

    if (ConfigInfo->Length < sizeof(VIDEO_PORT_CONFIG_INFO))
        return ERROR_INVALID_PARAMETER;

    // if (ConfigInfo->AdapterInterfaceType == Internal) { ...

    /*
     * Retrieve configuration data for the boot-time display controller and monitor.
     *
     * This data is initialized by the NT bootloader and passed in the ARC tree
     * pointed by the loader block. It is available only for boot drivers and gets
     * freed later on.
     *
     * TODO: On Win8+, try first to read the BgContext structure.
     */

    VideoPortZeroMemory(FrameBufData, sizeof(*FrameBufData));

    /* Enumerate and find the boot-time console display controller */
    // ControllerClass, DisplayController
    if (VideoPortGetDeviceData(HwDeviceExtension,
                               VpControllerData,
                               VpGetDeviceDataCallback,
                               ConfigInfo) != NO_ERROR)
    {
        DPRINT1("VideoPrt get controller info failed\n");
        return ERROR_DEV_NOT_EXIST;
    }

    /* Now find the MonitorPeripheral to obtain more information.
     * It should be child of the display controller. */
    if (VideoPortGetDeviceData(HwDeviceExtension,
                               VpMonitorData,
                               VpGetDeviceDataCallback,
                               ConfigInfo) != NO_ERROR)
    {
        /* Ignore if no monitor data is given */
        DPRINT1("VideoPrt monitor info not found\n");
    }

    /* From the captured video framebuffer and monitor data,
     * synthesize our single video mode information structure. */
    if (!GenFbVmpSetupCurrentMode(HwDeviceExtension))
        return ERROR_DEV_NOT_EXIST;

    /* Zero out the emulator entries since this driver
     * does not support them (not VGA). */
    ConfigInfo->NumEmulatorAccessEntries = 0;
    ConfigInfo->EmulatorAccessEntries = NULL;
    ConfigInfo->EmulatorAccessEntriesContext = 0;
    ConfigInfo->VdmPhysicalVideoMemoryAddress.LowPart = 0;
    ConfigInfo->VdmPhysicalVideoMemoryAddress.HighPart = 0;
    ConfigInfo->VdmPhysicalVideoMemoryLength = 0;
    ConfigInfo->HardwareStateSize = 0;

#if 0
    VideoPortSetRegistryParameters(DeviceExtension,
                                   L"HardwareInformation.ChipType",
                                   AdapterChipType,
                                   sizeof(AdapterChipType));
    VideoPortSetRegistryParameters(DeviceExtension,
                                   L"HardwareInformation.DacType",
                                   AdapterDacType,
                                   sizeof(AdapterDacType));
    VideoPortSetRegistryParameters(DeviceExtension,
                                   L"HardwareInformation.MemorySize",
                                   &DeviceExtension->FrameBufferLength,
                                   sizeof(ULONG));
    VideoPortSetRegistryParameters(DeviceExtension,
                                   L"HardwareInformation.AdapterString",
                                   AdapterString,
                                   sizeof(AdapterString));
#endif

    return NO_ERROR;
}

CODE_SEG("PAGE")
BOOLEAN NTAPI
GenFbVmpInitialize(
    _In_ PVOID HwDeviceExtension)
{
    DPRINT1("GenFbVmpInitialize(%p)\n", HwDeviceExtension);
    return TRUE;
}

CODE_SEG("PAGE")
static VOID
GenFbVmpShowIOControl(ULONG IoControlCode)
{
    switch (IoControlCode)
    {
    case IOCTL_VIDEO_ENABLE_VDM:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_ENABLE_VDM\n");
        break;

    case IOCTL_VIDEO_DISABLE_VDM:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_DISABLE_VDM\n");
        break;

    case IOCTL_VIDEO_REGISTER_VDM:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_REGISTER_VDM\n");
        break;

    case IOCTL_VIDEO_SET_OUTPUT_DEVICE_POWER_STATE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_OUTPUT_DEVICE_POWER_STATE\n");
        break;

    case IOCTL_VIDEO_GET_OUTPUT_DEVICE_POWER_STATE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_GET_OUTPUT_DEVICE_POWER_STATE\n");
        break;

    case IOCTL_VIDEO_MONITOR_DEVICE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_MONITOR_DEVICE\n");
        break;

    case IOCTL_VIDEO_ENUM_MONITOR_PDO:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_ENUM_MONITOR_PDO\n");
        break;

    case IOCTL_VIDEO_INIT_WIN32K_CALLBACKS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_INIT_WIN32K_CALLBACKS\n");
        break;

    case IOCTL_VIDEO_HANDLE_VIDEOPARAMETERS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_HANDLE_VIDEOPARAMETERS\n");
        break;

    case IOCTL_VIDEO_IS_VGA_DEVICE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_IS_VGA_DEVICE\n");
        break;

    case IOCTL_VIDEO_USE_DEVICE_IN_SESSION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_USE_DEVICE_IN_SESSION\n");
        break;

    case IOCTL_VIDEO_PREPARE_FOR_EARECOVERY:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_PREPARE_FOR_EARECOVERY\n");
        break;

    case IOCTL_VIDEO_DISABLE_CURSOR:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_DISABLE_CURSOR\n");
        break;

    case IOCTL_VIDEO_DISABLE_POINTER:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_DISABLE_POINTER\n");
        break;

    case IOCTL_VIDEO_ENABLE_CURSOR:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_ENABLE_CURSOR\n");
        break;

    case IOCTL_VIDEO_ENABLE_POINTER:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_ENABLE_POINTER\n");
        break;

    case IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES\n");
        break;

    case IOCTL_VIDEO_GET_BANK_SELECT_CODE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_GET_BANK_SELECT_CODE\n");
        break;

    case IOCTL_VIDEO_GET_CHILD_STATE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_GET_CHILD_STATE\n");
        break;

    case IOCTL_VIDEO_GET_POWER_MANAGEMENT:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_GET_POWER_MANAGEMENT\n");
        break;

    case IOCTL_VIDEO_LOAD_AND_SET_FONT:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_LOAD_AND_SET_FONT\n");
        break;

    case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_MAP_VIDEO_MEMORY\n");
        break;

    case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_AVAIL_MODES\n");
        break;

    case IOCTL_VIDEO_QUERY_COLOR_CAPABILITIES:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_COLOR_CAPABILITIES\n");
        break;

    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_CURRENT_MODE\n");
        break;

    case IOCTL_VIDEO_QUERY_CURSOR_ATTR:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_CURSOR_ATTR\n");
        break;

    case IOCTL_VIDEO_QUERY_CURSOR_POSITION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_CURSOR_POSITION\n");
        break;

    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES\n");
        break;

    case IOCTL_VIDEO_QUERY_POINTER_ATTR:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_POINTER_ATTR\n");
        break;

    case IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES\n");
        break;

    case IOCTL_VIDEO_QUERY_POINTER_POSITION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_POINTER_POSITION\n");
        break;

    case IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES\n");
        break;

    case IOCTL_VIDEO_RESET_DEVICE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_RESET_DEVICE\n");
        break;

    case IOCTL_VIDEO_RESTORE_HARDWARE_STATE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_RESTORE_HARDWARE_STATE\n");
        break;

    case IOCTL_VIDEO_SAVE_HARDWARE_STATE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SAVE_HARDWARE_STATE\n");
        break;

    case IOCTL_VIDEO_SET_CHILD_STATE_CONFIGURATION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_CHILD_STATE_CONFIGURATION\n");
        break;

    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_COLOR_REGISTERS\n");
        break;

    case IOCTL_VIDEO_SET_CURRENT_MODE:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_CURRENT_MODE\n");
        break;

    case IOCTL_VIDEO_SET_CURSOR_ATTR:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_CURSOR_ATTR\n");
        break;

    case IOCTL_VIDEO_SET_CURSOR_POSITION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_CURSOR_POSITION\n");
        break;

    case IOCTL_VIDEO_SET_PALETTE_REGISTERS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_PALETTE_REGISTERS\n");
        break;

    case IOCTL_VIDEO_SET_POINTER_ATTR:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_POINTER_ATTR\n");
        break;

    case IOCTL_VIDEO_SET_POINTER_POSITION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_POINTER_POSITION\n");
        break;

    case IOCTL_VIDEO_SET_POWER_MANAGEMENT:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_POWER_MANAGEMENT\n");
        break;

    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SHARE_VIDEO_MEMORY\n");
        break;

    case IOCTL_VIDEO_SWITCH_DUALVIEW:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SWITCH_DUALVIEW\n");
        break;

    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_UNMAP_VIDEO_MEMORY\n");
        break;

    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY\n");
        break;

    case IOCTL_VIDEO_SET_COLOR_LUT_DATA:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_COLOR_LUT_DATA\n");
        break;

    case IOCTL_VIDEO_VALIDATE_CHILD_STATE_CONFIGURATION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_VALIDATE_CHILD_STATE_CONFIGURATION\n");
        break;

    case IOCTL_VIDEO_SET_BANK_POSITION:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_BANK_POSITION\n");
        break;

    case IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS\n");
        break;

    case IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS\n");
        break;

    case IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS:
        DPRINT1("GenFbVmpStartIO: IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS\n");
        break;

    default:
        DPRINT1("GenFbVmpStartIO: Unknown 0x%x\n", IoControlCode);
        break;
    }
}

CODE_SEG("PAGE")
BOOLEAN NTAPI
GenFbVmpStartIO(
    _In_ PVOID HwDeviceExtension,
    _Inout_ PVIDEO_REQUEST_PACKET RequestPacket)
{
    PSTATUS_BLOCK StatusBlock = RequestPacket->StatusBlock;
    VP_STATUS Status = ERROR_INVALID_PARAMETER;

    GenFbVmpShowIOControl(RequestPacket->IoControlCode);

    switch (RequestPacket->IoControlCode)
    {
        case IOCTL_VIDEO_SET_CURRENT_MODE:
        {
            PVIDEO_MODE RequestedMode;
            ULONG RequestedModeNum;

            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MODE))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }

            RequestedMode = (PVIDEO_MODE)RequestPacket->InputBuffer;
            RequestedModeNum = RequestedMode->RequestedMode &
                ~(VIDEO_MODE_NO_ZERO_MEMORY | VIDEO_MODE_MAP_MEM_LINEAR);

            /* There is nothing to do. We only support one
             * single mode and we are already in that mode. */
            if (RequestedModeNum != 0)
            {
                Status = ERROR_INVALID_PARAMETER;
                break;
            }

            // if (!(RequestedMode->RequestedMode & VIDEO_MODE_NO_ZERO_MEMORY))
            //     VideoPortZeroMemory(HwDeviceExtension, ...);

            Status = NO_ERROR;
            break;
        }

        case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        {
            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION) ||
                RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }

            Status = GenFbVmpMapVideoMemory(
                        (PGENFB_DEVICE_EXTENSION)HwDeviceExtension,
                        (PVIDEO_MEMORY)RequestPacket->InputBuffer,
                        (PVIDEO_MEMORY_INFORMATION)RequestPacket->OutputBuffer,
                        StatusBlock);
            break;
        }

        case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        {
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }

            Status = GenFbVmpUnmapVideoMemory(
                        (PGENFB_DEVICE_EXTENSION)HwDeviceExtension,
                        (PVIDEO_MEMORY)RequestPacket->InputBuffer,
                        StatusBlock);
            break;
        }

        case IOCTL_VIDEO_RESET_DEVICE:
        {
            /* There is nothing to be done here */
            // TODO: Maybe zero memory the framebuffer?
            Status = NO_ERROR;
            break;
        }

        case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        {
            PVIDEO_NUM_MODES Modes;

            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_NUM_MODES))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }

            Modes = (PVIDEO_NUM_MODES)RequestPacket->OutputBuffer;

            /* We only support one single mode set at boot time */
            Modes->NumModes = 1;
            Modes->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
            StatusBlock->Information = sizeof(VIDEO_NUM_MODES);
            Status = NO_ERROR;
            break;
        }

        case IOCTL_VIDEO_QUERY_AVAIL_MODES:
            /* Since we support only one single mode, return
             * only that mode that is also the active one. */
        case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        {
            PGENFB_DEVICE_EXTENSION DeviceExtension = (PGENFB_DEVICE_EXTENSION)HwDeviceExtension;
            PVIDEO_MODE_INFORMATION VideoMode;

            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }

            /* Copy back our existing current mode */
            VideoMode = (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer;
            VideoPortMoveMemory(VideoMode,
                                &DeviceExtension->CurrentVideoMode,
                                sizeof(VIDEO_MODE_INFORMATION));
            StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
            Status = NO_ERROR;
            break;
        }

        default:
        {
            DPRINT1("GenFbVmpStartIO: 0x%x not implemented\n", RequestPacket->IoControlCode);
            StatusBlock->Information = 0;
            Status = ERROR_INVALID_FUNCTION;
            break;
        }
    }

    StatusBlock->Status = Status;
    return TRUE;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenFbVmpSetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _In_ PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    /* Unused */
    __debugbreak();
    return ERROR_INVALID_FUNCTION; // NO_ERROR;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenFbVmpGetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _Out_ PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    /* Unused */
    __debugbreak();
    return ERROR_INVALID_FUNCTION; // NO_ERROR;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
GenFbVmpGetVideoChildDescriptor(
    _In_ PVOID HwDeviceExtension,
    _In_ PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
    _Out_ PVIDEO_CHILD_TYPE VideoChildType,
    _Out_ PUCHAR pChildDescriptor,
    _Out_ PULONG UId,
    _Out_ PULONG pUnused)
{
    /* Unused */
    DPRINT1("GenFbVmpGetVideoChildDescriptor(%p)\n", HwDeviceExtension);
    __debugbreak();
    return NO_ERROR; // FIXME: Should return VIDEO_ENUM_NO_MORE_DEVICES;
}

ULONG NTAPI
DriverEntry(
    _In_ PVOID Context1,
    _In_ PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA VideoInitData;
    ULONG Status;

    VideoDebugPrint((Info, "GenFbVmp: DriverEntry\n"));
    __debugbreak();

    VideoPortZeroMemory(&VideoInitData, sizeof(VideoInitData));
    VideoInitData.HwInitDataSize = sizeof(VideoInitData);
    VideoInitData.HwFindAdapter = GenFbVmpFindAdapter;
    VideoInitData.HwInitialize = GenFbVmpInitialize;
    // HwInterrupt
    VideoInitData.HwStartIO = GenFbVmpStartIO;
    VideoInitData.HwDeviceExtensionSize = sizeof(GENFB_DEVICE_EXTENSION);

    // HwResetHw = NULL;
    // HwTimer = NULL;
    // HwStartDma = NULL;

#if 0
// FIXME: For now, keep the miniport as non-PnP
    VideoInitData.HwSetPowerState = GenFbVmpSetPowerState;
    VideoInitData.HwGetPowerState = GenFbVmpGetPowerState;
    VideoInitData.HwGetVideoChildDescriptor = GenFbVmpGetVideoChildDescriptor;
#endif

    // HwQueryInterface = NULL;
    // HwChildDeviceExtensionSize = 0;
    // HwLegacyResourceList  = NULL;
    // HwLegacyResourceCount = 0;
    // HwGetLegacyResources  = NULL
    // AllowEarlyEnumeration = FALSE;


    /* Start with parameters for Device0 */
    VideoInitData.StartingDeviceNumber = 0;

    /*
     * Our general aim is to find the *only* boot-time framebuffer
     * display controller available on the system, so don't declare
     * ourselves as PnP (yet? should we?). Instead look at specific
     * buses and enumerate the internal ARC device tree set up by
     * the bootloader.
     */

    {
    INTERFACE_TYPE Types[] = {PCIBus, Internal, Isa, Eisa, MicroChannel /*, TurboChannel*/};
    ULONG i;

    // QUESTION: Loop from interface type == 0 to MaximumInterfaceType ??
    // for (Type = 0; Type < MaximumInterfaceType; Type++)
    // { }

    for (i = 0; i < RTL_NUMBER_OF(Types); ++i)
    {
        VideoInitData.AdapterInterfaceType = Types[i];
        Status = VideoPortInitialize(Context1, Context2, &VideoInitData, NULL);
        if (Status == STATUS_SUCCESS)
            break;
    }
    }

    return Status;
}
