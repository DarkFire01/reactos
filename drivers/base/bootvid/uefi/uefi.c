
/* PROJECT:     ReactOS Boot Video Driver for UEFI/Framebuffer devices
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI/Framebuffer-specific source file
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* GLOBALS ********************************************************************/

#include <debug.h>

static ULONG_PTR FrameBufferStart = 0;
static ULONG FrameBufferWidth, FrameBufferHeight, PanH, PanV;
static UCHAR BytesPerPixel;
static RGBQUAD CachedPalette[BV_MAX_COLORS];
static PUCHAR BackBuffer = NULL;

PUCHAR BackBuffer;
PROSEFI_FRAMEBUFFER_DATA refiFbData;

#define BB_OFFSET(x, y)    ((y) * SCREEN_WIDTH + (x))
#define FB_OFFSET(x, y)    (((PanV + (y)) * FrameBufferWidth + PanH + (x)) * 4)

static VOID
UEFIClearScreeen(VOID)
{
    /* Just fill the screen black */
    for(int y = 0; y < refiFbData->ScreenHeight; y++)
    {
        for(int x = 0; x < refiFbData->ScreenWidth; x++)
        {
            *((UINT32*)(FrameBufferStart + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = 0x000000;
        }
    }
}
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
    for (y = 0; y < SCREEN_HEIGHT; y++)
    {
        Frame = (PULONG)(FrameBufferStart + FB_OFFSET(-PanH, y));

        for (x = 0; x < PanH; x++)
        {
            *Frame++ = CachedPalette[0];
        }
    }

    /* Screen redraw */
    PUCHAR Back = BackBuffer;
    for (y = 0; y < SCREEN_HEIGHT; y++)
    {
        Frame = (PULONG)(FrameBufferStart + FB_OFFSET(0, y));

        for (x = 0; x < SCREEN_WIDTH; x++)
        {
            *Frame++ = CachedPalette[*Back++];
        }
    }

    /* Right panning */
    for (y = 0; y < SCREEN_HEIGHT; y++)
    {
        Frame = (PULONG)(FrameBufferStart + FB_OFFSET(SCREEN_WIDTH, y));

        for (x = 0; x < PanH; x++)
        {
            *Frame++ = CachedPalette[0];
        }
    }

    /* Bottom panning */
    Frame = (PULONG)(FrameBufferStart + FB_OFFSET(-PanH, SCREEN_HEIGHT));
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
    refiFbData->BaseAddress        = 0xd0000000;
    refiFbData->BufferSize         = 0x21c0000;
    refiFbData->ScreenWidth        = 3840;
    refiFbData->ScreenHeight       = 2160;
    refiFbData->PixelsPerScanLine  = 4096;
    refiFbData->PixelFormat        = 1;

    /* MAP FRAMEBUFFER */
    FrameBufferPhyLoc.QuadPart = refiFbData->BaseAddress;
    FrameBufferStart = (ULONG_PTR)MmMapIoSpace(FrameBufferPhyLoc, refiFbData->BufferSize, MmNonCached);
    if (!FrameBufferStart)
    {
        DPRINT1("Out of memory!\n");
    }

    /* Setup framebuffer */
    FrameBufferWidth = refiFbData->PixelsPerScanLine;
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
    UEFIClearScreeen();
    InitializePalette();

    return TRUE;
}

VOID
NTAPI
VidResetDisplayUefi(
    _In_ BOOLEAN HalReset)
{

    /* Clear the current position */
    VidpCurrentX = 0;
    VidpCurrentY = 0;
    UEFIClearScreeen();
    /* Re-initialize the palette and fill the screen black */
    RtlZeroMemory((PULONG)FrameBufferStart, refiFbData->BufferSize);
    InitializePalette();
    VidSolidColorFill(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, BV_COLOR_BLACK);
}

VOID
NTAPI
InitPaletteWithTableUefi(
    _In_ PULONG Table,
    _In_ ULONG Count)
{
    PULONG Entry = Table;

    for (ULONG i = 0; i < Count; i++, Entry++)
    {
        CachedPalette[i] = *Entry | 0xFF000000;
    }
    ApplyPalette();
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
    PUCHAR Back = BackBuffer + BB_OFFSET(Left, Top);
    PULONG Frame = (PULONG)(FrameBufferStart + FB_OFFSET(Left, Top));

    *Back = Color;
    *Frame = CachedPalette[Color];
}

VOID
NTAPI
PreserveRowUefi(
    _In_ ULONG CurrentTop,
    _In_ ULONG TopDelta,
    _In_ BOOLEAN Restore)
{
    PUCHAR NewPosition, OldPosition;

    /* Calculate the position in memory for the row */
    if (Restore)
    {
        /* Restore the row by copying back the contents saved off-screen */
        NewPosition = BackBuffer + BB_OFFSET(0, CurrentTop);
        OldPosition = BackBuffer + BB_OFFSET(0, SCREEN_HEIGHT);
    }
    else
    {
        /* Preserve the row by saving its contents off-screen */
        NewPosition = BackBuffer + BB_OFFSET(0, SCREEN_HEIGHT);
        OldPosition = BackBuffer + BB_OFFSET(0, CurrentTop);
    }

    /* Set the count and loop every pixel of backbuffer */
    ULONG Count = TopDelta * SCREEN_WIDTH;

    RtlCopyMemory(NewPosition, OldPosition, Count);

    if (Restore)
    {
        NewPosition = BackBuffer + BB_OFFSET(0, CurrentTop);

        /* Set the count and loop every pixel of framebuffer */
        for (ULONG y = 0; y < TopDelta; y++)
        {
            PULONG Frame = (PULONG)(FrameBufferStart + FB_OFFSET(0, CurrentTop + y));

            Count = SCREEN_WIDTH;
            while (Count--)
            {
                *Frame++ = CachedPalette[*NewPosition++];
            }
        }
    }
}

VOID
NTAPI
DoScrollUefi(
    _In_ ULONG Scroll)
{
  ULONG RowSize = VidpScrollRegion[2] - VidpScrollRegion[0] + 1;

    /* Calculate the position in memory for the row */
    PUCHAR OldPosition = BackBuffer + BB_OFFSET(VidpScrollRegion[0], VidpScrollRegion[1] + Scroll);
    PUCHAR NewPosition = BackBuffer + BB_OFFSET(VidpScrollRegion[0], VidpScrollRegion[1]);

    /* Start loop */
    for (ULONG Top = VidpScrollRegion[1]; Top <= VidpScrollRegion[3]; ++Top)
    {
        ULONG i;

        /* Scroll the row */
        RtlCopyMemory(NewPosition, OldPosition, RowSize);

        PULONG Frame = (PULONG)(FrameBufferStart + FB_OFFSET(VidpScrollRegion[0], Top));

        for (i = 0; i < RowSize; ++i)
            Frame[i] = CachedPalette[NewPosition[i]];

        OldPosition += SCREEN_WIDTH;
        NewPosition += SCREEN_WIDTH;
    }
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
     /* Get the font and pixel pointer */
    PUCHAR FontChar = GetFontPtr(Character);

    /* Loop each pixel height */
    for (ULONG y = Top; y < Top + BOOTCHAR_HEIGHT; y++, FontChar += FONT_PTR_DELTA)
    {
        /* Loop each pixel width */
        ULONG x = Left;

        for (UCHAR bit = 1 << (BOOTCHAR_WIDTH - 1); bit > 0; bit >>= 1, x++)
        {
            /* Check if we should draw this pixel */
            if (*FontChar & bit)
            {
                /* We do, use the given Text Color */
                SetPixel(x, y, (UCHAR)TextColor);
            }
            else if (BackColor < BV_COLOR_NONE)
            {
                /*
                 * This is a background pixel. We're drawing it
                 * unless it's transparent.
                 */
                SetPixel(x, y, (UCHAR)BackColor);
            }
        }
    }
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
 /* Clear the destination buffer */
    RtlZeroMemory(Buffer, Delta * Height);

    /* Start the outer Y height loop */
    for (ULONG y = 0; y < Height; y++)
    {
        /* Set current scanline */
        PUCHAR Back = BackBuffer + BB_OFFSET(Left, Top + y);
        PUCHAR Buf = Buffer + y * Delta;

        /* Start the X inner loop */
        for (ULONG x = 0; x < Width; x += sizeof(USHORT))
        {
            /* Read the current value */
            *Buf = (*Back++ & 0xF) << 4;
            *Buf |= *Back++ & 0xF;
            Buf++;
        }
    }
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
    while (Top <= Bottom)
    {
        PUCHAR Back = BackBuffer + BB_OFFSET(Left, Top);
        PULONG Frame = (PULONG)(FrameBufferStart + FB_OFFSET(Left, Top));
        ULONG L = Left;

        while (L++ <= Right)
        {
            *Back++ = Color;
            *Frame++ = CachedPalette[Color];
        }
        Top++;
    }
}