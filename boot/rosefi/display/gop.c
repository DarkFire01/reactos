/*
 * PROJECT:     ReactOS UEFI Bootloader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI GOP core file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include <rosefip.h>

/* Quick hand GOP functions */

EFI_GRAPHICS_OUTPUT_BLT_PIXEL
SetGraphicsColor(UINT32 color)
{
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL GColor;
    GColor.Reserved = color >> 24;
    GColor.Red      = color >> 16;
    GColor.Green    = color >> 8;
    GColor.Blue     = color;
    return GColor;
}

VOID
SetPixel(UINT32 xPos, UINT32 yPos, EFI_GRAPHICS_OUTPUT_BLT_PIXEL GraphicsColor, EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    // TODO : Add in a choice instead of defaulting to EfiBltVideoFill.
    gop->Blt(gop, &GraphicsColor, EfiBltVideoFill, 0, 0, xPos, yPos, 1, 1, 0);
}

VOID
CreateFilledBox(UINT32 xPos, UINT32 yPos, UINT32 w, UINT32 h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL GraphicsColor, EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    // TODO : Add in a choice instead of defaulting to EfiBltVideoFill.
    gop->Blt(gop, &GraphicsColor, EfiBltVideoFill, 0, 0, xPos, yPos, w, h, 0);
}

/* Indepth GOP / FULL SPEED */

VOID
RefiInitGOP(_In_ EFI_SYSTEM_TABLE *SystemTable,
            _In_ PROSEFI_FRAMEBUFFER_DATA refiData,
            _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    refiData->BaseAddress        = (ULONG_PTR)gop->Mode->FrameBufferBase;
    refiData->BufferSize         = gop->Mode->FrameBufferSize;
    refiData->ScreenWidth        = gop->Mode->Info->HorizontalResolution;
    refiData->ScreenHeight       = gop->Mode->Info->VerticalResolution;
    refiData->PixelsPerScanLine  = gop->Mode->Info->PixelsPerScanLine;
}

VOID
RefiAssignPixel(ULONG* screen, UINT32 x, UINT32 y, UINT64 color)
{
    ULONG Loc = x + y;
    screen[Loc] = color & 255;
    screen[Loc + 1] = (color >> 8) & 255;
    screen[Loc + 2] = (color >> 16) & 255;
}
