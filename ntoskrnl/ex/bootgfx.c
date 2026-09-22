/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boot framebuffer description for SystemBootGraphicsInformation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include <drivers/bootvid/framebuf.c>

/* GLOBALS ********************************************************************/

static SYSTEM_BOOT_GRAPHICS_INFORMATION ExpBootGraphicsInformation;
static BOOLEAN ExpBootGraphicsAvailable;

/* PRIVATE FUNCTIONS **********************************************************/

static
SYSTEM_PIXEL_FORMAT
ExpGetBootPixelFormat(
    _In_ PCM_FRAMEBUF_DEVICE_DATA VideoData)
{
    BOOLEAN HasPadding;

    if (VideoData->BitsPerPixel == 32)
        HasPadding = TRUE;
    else if (VideoData->BitsPerPixel == 24)
        HasPadding = FALSE;
    else
        return SystemPixelFormatUnknown;

    if (VideoData->PixelMasks.GreenMask != 0x0000FF00)
        return SystemPixelFormatUnknown;

    /* Blue in the low byte is B8G8R8 in memory order */
    if ((VideoData->PixelMasks.RedMask == 0x00FF0000) &&
        (VideoData->PixelMasks.BlueMask == 0x000000FF))
        return HasPadding ? SystemPixelFormatB8G8R8X8 : SystemPixelFormatB8G8R8;

    if ((VideoData->PixelMasks.RedMask == 0x000000FF) &&
        (VideoData->PixelMasks.BlueMask == 0x00FF0000))
        return HasPadding ? SystemPixelFormatR8G8B8X8 : SystemPixelFormatR8G8B8;

    return SystemPixelFormatUnknown;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Records the framebuffer the loader left on screen. Must run before the
 * loader block is freed.
 */
CODE_SEG("INIT")
VOID
NTAPI
ExpInitializeBootGraphicsInformation(VOID)
{
    CM_FRAMEBUF_DEVICE_DATA VideoData;
    PHYSICAL_ADDRESS VramAddress;
    PHYSICAL_ADDRESS FrameBuffer;
    INTERFACE_TYPE Interface;
    ULONG AddressSpace = 0;
    ULONG BusNumber;
    ULONG VramSize;

    if (!NT_SUCCESS(FindBootDisplay(&VramAddress,
                                    &VramSize,
                                    &VideoData,
                                    NULL,
                                    &Interface,
                                    &BusNumber)))
        return;

    /* Report the CPU physical address, the same one bootvid maps */
    FrameBuffer.QuadPart = VramAddress.QuadPart + VideoData.FrameBufferOffset;
    if (!BootTranslateBusAddress(Interface, BusNumber, FrameBuffer, &AddressSpace, &FrameBuffer) ||
        (AddressSpace != 0))
    {
        DPRINT1("Boot framebuffer 0x%I64X has no memory translation\n", FrameBuffer.QuadPart);
        return;
    }

    ExpBootGraphicsInformation.FrameBuffer = FrameBuffer;
    ExpBootGraphicsInformation.Width = VideoData.ScreenWidth;
    ExpBootGraphicsInformation.Height = VideoData.ScreenHeight;
    ExpBootGraphicsInformation.PixelStride = VideoData.PixelsPerScanLine;
    ExpBootGraphicsInformation.Flags = 0;
    ExpBootGraphicsInformation.Format = ExpGetBootPixelFormat(&VideoData);
    ExpBootGraphicsInformation.DisplayRotation = 0;
    ExpBootGraphicsAvailable = TRUE;
}

/**
 * @brief
 * Returns the boot framebuffer description.
 *
 * @param[out] Information
 * Receives the framebuffer address, size, stride and pixel format.
 *
 * @return
 * STATUS_UNSUCCESSFUL above PASSIVE_LEVEL or when the loader left no
 * framebuffer, otherwise STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
ExpQueryBootGraphicsInformation(
    _Out_ PSYSTEM_BOOT_GRAPHICS_INFORMATION Information)
{
    if ((KeGetCurrentIrql() != PASSIVE_LEVEL) || !ExpBootGraphicsAvailable)
        return STATUS_UNSUCCESSFUL;

    *Information = ExpBootGraphicsInformation;
    return STATUS_SUCCESS;
}

/* EOF */
