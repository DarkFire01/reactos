/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Video output
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

#define CHAR_WIDTH  8
#define CHAR_HEIGHT 16
#define TOP_BOTTOM_LINES 0
#define FB_SIZE_MB 4
#define MAKE_COLOR(Red, Green, Blue) (0xff000000 | (((Red) & 0xff) << 16) | (((Green) & 0xff) << 8) | ((Blue) & 0xff))

/* GLOBALS ********************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

UCHAR MachDefaultTextColor = COLOR_GRAY;
REACTOS_INTERNAL_BGCONTEXT refiFbData;

/* FUNCTIONS ******************************************************************/

VOID
UefiInitalizeVideo(_In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    EFI_STATUS Status = 0;
    Status = GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(refiFbData), (void**)&refiFbData);
    if (Status != EFI_SUCCESS)
        ERR("UefiMachInit: Could not allocated memory for MachVtbl");

    gop->SetMode(gop,1);
    refiFbData.BaseAddress        = (ULONG_PTR)gop->Mode->FrameBufferBase;
    refiFbData.BufferSize         = gop->Mode->FrameBufferSize;
    refiFbData.ScreenWidth        = gop->Mode->Info->HorizontalResolution;
    refiFbData.ScreenHeight       = gop->Mode->Info->VerticalResolution;
    refiFbData.PixelsPerScanLine  = gop->Mode->Info->PixelsPerScanLine;
    refiFbData.PixelFormat        = gop->Mode->Info->PixelFormat;
}

ULONG
UefiVideoAttrToSingleColor(UCHAR Attr)
{
    UCHAR Intensity;
    Intensity = (0 == (Attr & 0x08) ? 127 : 255);

    return 0xff000000 |
         (0 == (Attr & 0x04) ? 0 : (Intensity << 16)) |
         (0 == (Attr & 0x02) ? 0 : (Intensity << 8)) |
         (0 == (Attr & 0x01) ? 0 : Intensity);
}

VOID
UefiVideoAttrToColors(UCHAR Attr, ULONG *FgColor, ULONG *BgColor)
{
    *FgColor = UefiVideoAttrToSingleColor(Attr & 0xf);
    *BgColor = UefiVideoAttrToSingleColor((Attr >> 4) & 0xf);
}

VOID
UefiVideoClearScreen(UCHAR Attr)
{

    ULONG FgColor, BgColor;

    UefiVideoAttrToColors(Attr, &FgColor, &BgColor);
    for(int y = 0; y < refiFbData.ScreenHeight; y++)
    {
        for(int x = 0; x < refiFbData.ScreenWidth; x++)
        {
            *((UINT32*)(refiFbData.BaseAddress + 4 * refiFbData.PixelsPerScanLine * (y) + 4 * (x))) = BgColor;
        }
    }
}

#include "../freeldr/arch/vgafont.c"

VOID
UefiVideoOutputChar(UCHAR Char, unsigned X, unsigned Y, ULONG FgColor, ULONG BgColor)
{
    PUCHAR FontPtr;
    PULONG Pixel;
    UCHAR Mask;
    unsigned Line;
    unsigned Col;
    ULONG Delta;
    Delta = (refiFbData.PixelsPerScanLine * 4 + 3) & ~ 0x3;
    FontPtr = BitmapFont8x16 + Char * 16;
    Pixel = (PULONG) ((char *) refiFbData.BaseAddress +
            (Y * CHAR_HEIGHT + TOP_BOTTOM_LINES) *  Delta + X * CHAR_WIDTH * 4);

    for (Line = 0; Line < CHAR_HEIGHT; Line++)
    {
        Mask = 0x80;
        for (Col = 0; Col < CHAR_WIDTH; Col++)
        {
            Pixel[Col] = (0 != (FontPtr[Line] & Mask) ? FgColor : BgColor);
            Mask = Mask >> 1;
        }
        Pixel = (PULONG) ((char *) Pixel + Delta);
    }
}

VOID
UefiVideoPutChar(int Ch, UCHAR Attr, unsigned X, unsigned Y)
{
    ULONG FgColor = 0;
    ULONG BgColor = 0;
    if (Ch != 0)
    {
        UefiVideoAttrToColors(Attr, &FgColor, &BgColor);
        UefiVideoOutputChar(Ch, X, Y, FgColor, BgColor);
    }
}

VOID
UefiVideoGetDisplaySize(PULONG Width, PULONG Height, PULONG Depth)
{
    *Width =  refiFbData.ScreenWidth / CHAR_WIDTH;
    *Height = (refiFbData.ScreenHeight - 2) / CHAR_HEIGHT;
    *Depth =  0;
}

VIDEODISPLAYMODE
UefiVideoSetDisplayMode(char *DisplayMode, BOOLEAN Init)
{
    /* We only have one mode, semi-text */
    return VideoTextMode;
}
ULONG
UefiVideoGetBufferSize(VOID)
{
    return ((refiFbData.ScreenHeight - 2) / CHAR_HEIGHT * (refiFbData.ScreenWidth / CHAR_WIDTH) * 2);
}

VOID
UefiVideoCopyOffScreenBufferToVRAM(PVOID Buffer)
{
    PUCHAR OffScreenBuffer = (PUCHAR) Buffer;

    ULONG Col, Line;
    for (Line = 0; Line < (refiFbData.ScreenHeight - 2) / CHAR_HEIGHT; Line++)
    {
        for (Col = 0; Col < refiFbData.ScreenWidth / CHAR_WIDTH; Col++)
        {
            UefiVideoPutChar(OffScreenBuffer[0], OffScreenBuffer[1], Col, Line);
            OffScreenBuffer += 2;
        }
    }
}

VOID
UefiVideoScrollUp(VOID)
{
    ULONG BgColor, Dummy;
    ULONG Delta;
    Delta = (refiFbData.PixelsPerScanLine * 4 + 3) & ~ 0x3;
    ULONG PixelCount = refiFbData.ScreenWidth * CHAR_HEIGHT *
                       (((refiFbData.ScreenHeight - 2 * TOP_BOTTOM_LINES) / CHAR_HEIGHT) - 1);
    PULONG Src = (PULONG)((PUCHAR)refiFbData.BaseAddress + (CHAR_HEIGHT + TOP_BOTTOM_LINES) * Delta);
    PULONG Dst = (PULONG)((PUCHAR)refiFbData.BaseAddress + TOP_BOTTOM_LINES * Delta);

    UefiVideoAttrToColors(ATTR(COLOR_WHITE, COLOR_BLACK), &Dummy, &BgColor);

    while (PixelCount--)
        *Dst++ = *Src++;

    for (PixelCount = 0; PixelCount < refiFbData.ScreenWidth * CHAR_HEIGHT; PixelCount++)
        *Dst++ = BgColor;
}