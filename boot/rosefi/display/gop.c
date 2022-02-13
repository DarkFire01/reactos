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

/* Indepth GOP / FULL SPEED */