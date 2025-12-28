/*
 * PROJECT:     ReactOS Boot Video Driver for ARM devices
 * LICENSE:     BSD - See COPYING.ARM in root directory
 * PURPOSE:     Main file
 * COPYRIGHT:   Copyright 2008 ReactOS Portable Systems Group <ros.arm@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define LCDTIMING0_PPL(x)       ((((x) / 16 - 1) & 0x3f) << 2)
#define LCDTIMING1_LPP(x)       (((x) & 0x3ff) - 1)
#define LCDCONTROL_LCDPWR       (1 << 11)
#define LCDCONTROL_LCDEN        (1)
#define LCDCONTROL_LCDBPP(x)    (((x) & 7) << 1)
#define LCDCONTROL_LCDTFT       (1 << 5)

#define PL110_LCDTIMING0    (PVOID)0xE0020000
#define PL110_LCDTIMING1    (PVOID)0xE0020004
#define PL110_LCDTIMING2    (PVOID)0xE0020008
#define PL110_LCDUPBASE     (PVOID)0xE0020010
#define PL110_LCDLPBASE     (PVOID)0xE0020014
#define PL110_LCDCONTROL    (PVOID)0xE0020018

static PUSHORT VgaArmBase;
static PHYSICAL_ADDRESS VgaPhysical;

/* PRIVATE FUNCTIONS *********************************************************/

FORCEINLINE
USHORT
VidpBuildColor(
    _In_ UCHAR Color)
{
    UCHAR Red, Green, Blue;

    /* Extract color components */
    Red   = GetRValue(VidpDefaultPalette[Color]) >> 3;
    Green = GetGValue(VidpDefaultPalette[Color]) >> 3;
    Blue  = GetBValue(VidpDefaultPalette[Color]) >> 3;

    /* Build the 16-bit color mask */
    return ((Red & 0x1F) << 11) | ((Green & 0x1F) << 6) | ((Blue & 0x1F));
}

FORCEINLINE
VOID
SetPixel(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ UCHAR Color)
{
    /* Calculate the pixel position and set the color */
    PUSHORT PixelPosition = &VgaArmBase[Left + (Top * SCREEN_WIDTH)];
    WRITE_REGISTER_USHORT(PixelPosition, VidpBuildColor(Color));
}

FORCEINLINE
VOID
SetPixels(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ UCHAR Color,
    _In_ ULONG Count)
{
    /* Calculate the initial pixel position and copy the color */
    PUSHORT PixelPosition = &VgaArmBase[Left + (Top * SCREEN_WIDTH)];
    USHORT vidColor = VidpBuildColor(Color);
    while (Count--)
        WRITE_REGISTER_USHORT(PixelPosition++, vidColor);
}

VOID
DisplayCharacter(
    _In_ CHAR Character,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG TextColor,
    _In_ ULONG BackColor)
{
    /* Get the font line for this character */
    const UCHAR* FontChar = GetFontPtr(Character);
    ULONG i, j, XOffset;

    /* Loop each pixel height */
    for (i = BOOTCHAR_HEIGHT; i > 0; --i, Top++)
    {
        /* Loop each pixel width */
        XOffset = Left;
        for (j = 1 << (BOOTCHAR_WIDTH - 1); j > 0; j >>= 1, XOffset++)
        {
            /* If we should draw this pixel, use the text color. Otherwise
             * this is a background pixel, draw it unless it's transparent. */
            if (FontChar[BOOTCHAR_HEIGHT - i] & (UCHAR)j)
                SetPixel(XOffset, Top, (UCHAR)TextColor);
            else if (BackColor < BV_COLOR_NONE)
                SetPixel(XOffset, Top, (UCHAR)BackColor);
        }
    }
}

VOID
DoScroll(
    _In_ ULONG Scroll)
{
    ULONG Top, Offset;
    PUSHORT SourceOffset, DestOffset;
    PUSHORT i, j;

    /* Set memory positions of the scroll */
    SourceOffset = &VgaArmBase[(VidpScrollRegion.Top * (SCREEN_WIDTH / 8)) + (VidpScrollRegion.Left >> 3)];
    DestOffset = &SourceOffset[Scroll * (SCREEN_WIDTH / 8)];

    /* Start loop */
    for (Top = VidpScrollRegion.Top; Top <= VidpScrollRegion.Bottom; ++Top)
    {
        /* Set number of bytes to loop and start offset */
        Offset = VidpScrollRegion.Left >> 3;
        j = SourceOffset;

        /* Check if this is part of the scroll region */
        if (Offset <= (VidpScrollRegion.Right >> 3))
        {
            /* Update position */
            i = (PUSHORT)(DestOffset - SourceOffset);

            /* Loop the X axis */
            do
            {
                /* Write value in the new position so that we can do the scroll */
                WRITE_REGISTER_USHORT(j, READ_REGISTER_USHORT(j + (ULONG_PTR)i));

                /* Move to the next memory location to write to */
                j++;

                /* Move to the next byte in the region */
                Offset++;

                /* Make sure we don't go past the scroll region */
            } while (Offset <= (VidpScrollRegion.Right >> 3));
        }

        /* Move to the next line */
        SourceOffset += (SCREEN_WIDTH / 8);
        DestOffset += (SCREEN_WIDTH / 8);
    }
}

VOID
PreserveRow(
    _In_ ULONG CurrentTop,
    _In_ ULONG TopDelta,
    _In_ BOOLEAN Restore)
{
    PUSHORT NewPosition, OldPosition;
    ULONG Count;

    /* Calculate the position in memory for the row */
    if (Restore)
    {
        /* Restore the row by copying back the contents saved off-screen */
        NewPosition = &VgaArmBase[CurrentTop * (SCREEN_WIDTH / 8)];
        OldPosition = &VgaArmBase[SCREEN_HEIGHT * (SCREEN_WIDTH / 8)];
    }
    else
    {
        /* Preserve the row by saving its contents off-screen */
        NewPosition = &VgaArmBase[SCREEN_HEIGHT * (SCREEN_WIDTH / 8)];
        OldPosition = &VgaArmBase[CurrentTop * (SCREEN_WIDTH / 8)];
    }

    /* Set the count and copy the pixel data back to the other position */
    Count = TopDelta * (SCREEN_WIDTH / 8);
    for (; Count--; NewPosition++, OldPosition++)
        WRITE_REGISTER_USHORT(NewPosition, READ_REGISTER_USHORT(OldPosition));
}

VOID
VidpInitializeDisplay(VOID)
{
    /* Set framebuffer address */
    WRITE_REGISTER_ULONG(PL110_LCDUPBASE, VgaPhysical.LowPart);
    WRITE_REGISTER_ULONG(PL110_LCDLPBASE, VgaPhysical.LowPart);

    /* Initialize timings to 640x480 */
    WRITE_REGISTER_ULONG(PL110_LCDTIMING0, LCDTIMING0_PPL(SCREEN_WIDTH));
    WRITE_REGISTER_ULONG(PL110_LCDTIMING1, LCDTIMING1_LPP(SCREEN_HEIGHT));

    /* Enable the LCD display */
    WRITE_REGISTER_ULONG(PL110_LCDCONTROL,
                         LCDCONTROL_LCDEN |
                         LCDCONTROL_LCDTFT |
                         LCDCONTROL_LCDPWR |
                         LCDCONTROL_LCDBPP(4));
}

VOID
InitPaletteWithTable(
    _In_reads_(Count) const ULONG* Table,
    _In_ ULONG Count)
{
    UNIMPLEMENTED;
}

/* PUBLIC FUNCTIONS **********************************************************/

BOOLEAN
NTAPI
VidInitialize(
    _In_ BOOLEAN SetMode)
{
    DPRINT1("bv-arm v0.1\n");

    /* Allocate the framebuffer. 600kb works out to 640x480@16bpp. */
    VgaPhysical.QuadPart = -1;
    VgaArmBase = MmAllocateContiguousMemory(600 * 1024, VgaPhysical);
    if (!VgaArmBase) return FALSE;

    /* Get physical address */
    VgaPhysical = MmGetPhysicalAddress(VgaArmBase);
    if (!VgaPhysical.QuadPart) return FALSE;
    DPRINT1("[BV-ARM] Frame Buffer @ 0x%p 0x%p\n", VgaArmBase, VgaPhysical.LowPart);

    /* Setup the display */
    VidpInitializeDisplay();
    return TRUE;
}

VOID
ResetDisplay(
    _In_ BOOLEAN SetMode)
{
    /* Re-initialize the display */
    VidpInitializeDisplay();

    /* Re-initialize the palette and fill the screen black */
    InitializePalette();
    VidSolidColorFill(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, BV_COLOR_BLACK);
}

VOID
NTAPI
VidCleanUp(VOID)
{
    /* Just fill the screen black */
    VidSolidColorFill(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, BV_COLOR_BLACK);
}

VOID
NTAPI
VidScreenToBufferBlt(
    _Out_writes_bytes_all_(Delta * Height) PUCHAR Buffer,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG Delta)
{
    UNIMPLEMENTED;
    while (TRUE);
}

VOID
NTAPI
VidSolidColorFill(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Right,
    _In_ ULONG Bottom,
    _In_ UCHAR Color)
{
    ULONG y;
    for (y = Top; y <= Bottom; ++y)
    {
        SetPixels(Left, y, Color, Right - Left + 1);
    }
}
