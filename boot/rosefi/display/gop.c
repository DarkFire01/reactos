/*
 * PROJECT:     ReactOS UEFI Bootloader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI GOP core file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include <rosefip.h>

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

