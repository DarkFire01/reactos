/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Video output
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

UCHAR MachDefaultTextColor = COLOR_GRAY;
PREACTOS_INTERNAL_BGCONTEXT refiFbData;

VOID
UefiInitalizeVideo(_In_ EFI_HANDLE ImageHandle,
                   _In_ EFI_SYSTEM_TABLE *SystemTable,
                   _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
	gop->SetMode(gop,1);
    SystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(refiFbData), (void**)&refiFbData);
    refiFbData->BaseAddress        = (ULONG_PTR)gop->Mode->FrameBufferBase;

    refiFbData->BufferSize         = gop->Mode->FrameBufferSize;
    refiFbData->ScreenWidth        = gop->Mode->Info->HorizontalResolution;
    refiFbData->ScreenHeight       = gop->Mode->Info->VerticalResolution;
    refiFbData->PixelsPerScanLine  = gop->Mode->Info->PixelsPerScanLine;
    refiFbData->PixelFormat        = gop->Mode->Info->PixelFormat;
}

VOID
UefiVideoClearScreen(UCHAR Attr)
{
    for(int y = 0; y < refiFbData->ScreenHeight; y++)
    {
        for(int x = 0; x < refiFbData->ScreenWidth; x++)
        {
            *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Attr << 2;
        }
    }
}

#include "font.c"

VOID
UefiVideoOutputChar(UCHAR Char, unsigned X, unsigned Y, ULONG FgColor, ULONG BgColor)
{
  PUCHAR FontPtr;
  PULONG Pixel;
  UCHAR Mask;
  unsigned Line;
  unsigned Col;
  ULONG Delta;
  Delta = (refiFbData->PixelsPerScanLine * 4 + 3) & ~ 0x3;
  FontPtr = UefiBitmapFont8x16 + Char * 16;
  Pixel = (PULONG) ((char *) refiFbData->BaseAddress + (Y * CHAR_HEIGHT + TOP_BOTTOM_LINES) *  Delta
                  + X * CHAR_WIDTH * 4);
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
    UefiVideoOutputChar(Ch, X, Y, 0xFFFFFF, 0x000000);
}

VOID
UefiVideoGetDisplaySize(PULONG Width, PULONG Height, PULONG Depth)
{
    Width =  (PULONG)&refiFbData->ScreenWidth;
    Height = (PULONG)&refiFbData->ScreenHeight;
    Depth =  (PULONG)&refiFbData->PixelsPerScanLine;
}

VIDEODISPLAYMODE
UefiVideoSetDisplayMode(char *DisplayMode, BOOLEAN Init)
{
    return VideoGraphicsMode;
}

ULONG
UefiVideoGetBufferSize(VOID)
{
    return refiFbData->BufferSize;
}
