
/* PROJECT:     ReactOS Boot Video Driver for UEFI/Framebuffer devices
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI/Framebuffer-specific source file
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* GLOBALS ********************************************************************/

static ULONG_PTR FrameBufferStart = 0;
static ULONG FrameBufferWidth, FrameBufferHeight, PanH, PanV;
static UCHAR BytesPerPixel;
static RGBQUAD CachedPalette[BV_MAX_COLORS];
static PUCHAR BackBuffer = NULL;
const UINT32 Pallet[BV_MAX_COLORS] =
{
    (0x000000), /* Black */
    (0x800000), /* Red */
    (0x008000), /* Green */
    (0x808000), /* Brown */
    (0x000080), /* Blue */
    (0x800080), /* Magenta */
    (0x008080), /* Cyan */
    (0x808080), /* Dark Gray */
    (0xC0C0C0), /* Light Gray */
    (0xFF0000), /* Light Red */
    (0x00FF00), /* Light Green */
    (0xFFFF00), /* Yellow */
    (0x0000FF), /* Light Blue */
    (0xFF00FF), /* Light Magenta */
    (0x00FFFF), /* Light Cyan */
    (0xFFFFFF), /* White */
};

PUCHAR BackBuffer;
PROSEFI_FRAMEBUFFER_DATA refiFbData;

#define BB_OFFSET(x, y)    ((y) * SCREEN_WIDTH + (x))
#define FB_OFFSET(x, y)    (((PanV + (y)) * FrameBufferWidth + PanH + (x)) * 1)

static VOID
ApplyPalette(VOID)
{
    PULONG Frame = (PULONG)FrameBufferStart;
    ULONG x, y;

    /* Top panning */
    for (x = 0; x < PanV * FrameBufferWidth; x++)
    {
        *Frame++ = CachedPalette[0];
    }

    /* Left panning */
    for (y = 0; y < FrameBufferHeight; y++)
    {
        Frame = (PULONG)(FrameBufferStart + FB_OFFSET(-PanH, y));

        for (x = 0; x < PanH; x++)
        {
            *Frame++ = CachedPalette[0];
        }
    }

    /* Screen redraw */
    PUCHAR Back = BackBuffer;
    for (y = 0; y < FrameBufferHeight; y++)
    {
        Frame = (PULONG)(FrameBufferStart + FB_OFFSET(0, y));

        for (x = 0; x < FrameBufferWidth; x++)
        {
            *Frame++ = CachedPalette[*Back++];
        }
    }

    /* Right panning */
    for (y = 0; y < FrameBufferHeight; y++)
    {
        Frame = (PULONG)(FrameBufferStart + FB_OFFSET(FrameBufferWidth, y));

        for (x = 0; x < PanH; x++)
        {
            *Frame++ = CachedPalette[0];
        }
    }

    /* Bottom panning */
    Frame = (PULONG)(FrameBufferStart + FB_OFFSET(-PanH, FrameBufferHeight));
    for (x = 0; x < PanV * FrameBufferWidth; x++)
    {
        *Frame++ = CachedPalette[0];
    }
}

BOOLEAN
NTAPI
VidInitializeUefi(
    _In_ BOOLEAN SetMode)
{
   PHYSICAL_ADDRESS FrameBufferPhyLoc;


    /* Hard coding go brr */
    refiFbData = ExAllocatePoolWithTag(NonPagedPool, sizeof(ROSEFI_FRAMEBUFFER_DATA), 'IFEU');
    refiFbData->BaseAddress        = 0x80000000;
    refiFbData->BufferSize         = 0x21c0000;
    refiFbData->ScreenWidth        = 3840;
    refiFbData->ScreenHeight       = 2160;
    refiFbData->PixelsPerScanLine  = 3840;
    refiFbData->PixelFormat        = 1;

    /* MAP FRAMEBUFFER */
    FrameBufferPhyLoc.QuadPart = refiFbData->BaseAddress;
    FrameBufferStart = (ULONG_PTR)MmMapIoSpace(FrameBufferPhyLoc, refiFbData->BufferSize, MmNonCached);
    if (!FrameBufferStart)
    {
        DPRINT1("Out of memory!\n");
        goto cleanup;
    }

    /* Setup framebuffer */
    FrameBufferWidth = refiFbData->ScreenWidth;
    FrameBufferHeight = refiFbData->ScreenHeight;

    /*
    * Reserve off-screen area for the backbuffer that contains 8-bit indexed
    * color screen image, plus preserved row data.
    */
    ULONG BackBufferSize = refiFbData->BufferSize;
    BackBuffer = ExAllocatePoolWithTag(NonPagedPool, BackBufferSize, 'IFEU');

    /* Calculate panning values */
    PanH = (FrameBufferWidth - SCREEN_WIDTH) / 2;
    PanV = (FrameBufferHeight - SCREEN_HEIGHT) / 2;

    InitializePalette();

    
cleanup:
    if (ControlStart)
        MmUnmapIoSpace((PVOID)ControlStart, ControlLength);

    return TRUE;
}

VOID
NTAPI
VidResetDisplayUefi(
    _In_ BOOLEAN HalReset)
{    /* Just fill the screen black */

}

VOID
NTAPI
InitPaletteWithTableUefi(
    _In_ PULONG Table,
    _In_ ULONG Count)
{
}

VOID
PrepareForSetPixelUefi(VOID)
{
    NOTHING;
    //
}

VOID
SetPixelUefi(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ UCHAR Color)
{
}

VOID
NTAPI
PreserveRowUefi(
    _In_ ULONG CurrentTop,
    _In_ ULONG TopDelta,
    _In_ BOOLEAN Restore)
{

}

VOID
NTAPI
DoScrollUefi(
    _In_ ULONG Scroll)
{

}

VOID
NTAPI
DisplayCharacterUefi(
    _In_ CHAR Character,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG TextColor,
    _In_ ULONG BackColor)
{
}

VOID
NTAPI
VidScreenToBufferBltUefi(
    _Out_writes_bytes_(Delta * Height) PUCHAR Buffer,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG Delta)
{
}

VOID
NTAPI
VidSolidColorFillUefi(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Right,
    _In_ ULONG Bottom,
    _In_ UCHAR Color)
{

}