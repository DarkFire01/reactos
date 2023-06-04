/*
 * PROJECT:     ReactOS Boot Video Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Definitions for framebuffer-specific DisplayController
 *              device boot-time configuration data stored in the
 *              \Registry\Machine\Hardware\Description ARC tree.
 * COPYRIGHT:   Copyright 2023 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#ifndef _BOOTVID_FRAMEBUF_
#define _BOOTVID_FRAMEBUF_

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NOTE: Legacy video display adapter and monitor peripheral configuration
 * data is specified with the VIDEO_HARDWARE_CONFIGURATION_DATA (see video.h
 * -- with data starting at the 'Version' field and following) and the
 * MONITOR_CONFIGURATION_DATA structures (see arc.h), respectively, with
 * Version == 0 or 1, Revision == 0.
 *
 * New configuration data is provided with a CM_PARTIAL_RESOURCE_LIST.
 * The Irql/Vector interrupt settings are now provided with a
 * CmResourceTypeInterrupt resource descriptor. Both ControlBase/Size
 * and CursorBase/Size memory I/O ports are provided with successive
 * CmResourceTypePort resource descriptors (respectively in this order),
 * while the framebuffer's FrameBase/Size
 * is provided (in a 64-bit compatible way) with a CmResourceTypeMemory
 * resource descriptor.
 * Extended video/framebuffer-specific data is specified with the
 * ReactOS-specific CM_FRAMEBUF_DEVICE_DATA structure (see below),
 * while extended monitor configuration data is provided with a
 * CM_MONITOR_DEVICE_DATA structure.
 */


/*
 * Note: The DDK video.h VIDEO_HARDWARE_CONFIGURATION_DATA structure
 * has a similar layout, where:
 * - The first two fields, InterfaceType and BusNumber, are common
 *   with the CM_FULL_RESOURCE_DESCRIPTOR header;
 * - The Version and Revision fields correspond to the first two
 *   fields of CM_PARTIAL_RESOURCE_LIST.
 *
 * The other fields are of legacy layout, instead of the newer
 * CM_PARTIAL_RESOURCE_LIST one.
 */
#include <pshpack1.h>
typedef struct _MONITOR_HARDWARE_CONFIGURATION_DATA
{
    INTERFACE_TYPE InterfaceType;
    ULONG BusNumber;
    MONITOR_CONFIGURATION_DATA;
} MONITOR_HARDWARE_CONFIGURATION_DATA, *PMONITOR_HARDWARE_CONFIGURATION_DATA;
#include <poppack.h>


/**
 * @brief   Framebuffer-specific device data.
 *
 * Supplemental data, extends CM_VIDEO_DEVICE_DATA.
 * Gets appended to the standard configuration resource list.
 * Any optional Irql/Vector interrupt settings are specified with
 * a CmResourceTypeInterrupt descriptor, while any other I/O port
 * is specified with a CmResourceTypePort descriptor.
 * The framebuffer base and size are specified with a CmResourceTypeMemory
 * descriptor.
 **/
typedef struct _CM_FRAMEBUF_DEVICE_DATA
{
    CM_VIDEO_DEVICE_DATA;

    // PHYSICAL_ADDRESS FrameBase; // FrameBufferBase // BaseAddress
    // ULONG FrameSize; // FrameBufferSize // BufferSize

    /* NOTE: FrameBufferSize == PixelsPerScanLine x VerticalResolution x PixelElementSize */

    ULONG ScreenWidth;      // HorizontalResolution in pixels
    ULONG ScreenHeight;     // VerticalResolution in pixels

    /* Number of pixel elements per video memory line */
    ULONG PixelsPerScanLine;

    /* Physical format of the pixel */
    // PIXEL_FORMAT PixelFormat; // RGBX or BGRX 8-bit per color, or BitMask
    ULONG PixelFormat;

    // /*
    //  * This bit-mask is only valid if PixelFormat is set to PixelPixelBitMask.
    //  * A bit being set defines what bits are used for what purpose such as
    //  * Red, Green, Blue, or Reserved.
    //  */
    // struct
    // {
    //     ULONG RedMask;
    //     ULONG GreenMask;
    //     ULONG BlueMask;
    //     ULONG ReservedMask;
    // } /*PIXEL_BITMASK*/ PixelInformation;

    ULONG BitsPerPixel; // PixelDepth

} CM_FRAMEBUF_DEVICE_DATA, *PCM_FRAMEBUF_DEVICE_DATA;

#ifdef __cplusplus
}
#endif

#endif // _BOOTVID_FRAMEBUF_

/* EOF */
