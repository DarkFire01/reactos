/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI stubs
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>

/* TODO: Handle this with custom Disk / partition setup */

VOID
NTAPI
KeStallExecutionProcessor(ULONG Microseconds)
{
    StallExecutionProcessor(Microseconds);
}

static VOID
__StallExecutionProcessor(ULONG Loops)
{
}

VOID StallExecutionProcessor(ULONG Microseconds)
{
}

USHORT
__cdecl PxeCallApi(USHORT Segment, USHORT Offset, USHORT Service, VOID* Parameter)
{
    return 0;
}

VOID
UefiVideoGetFontsFromFirmware(PULONG RomFontPointers)
{

}

VOID
UefiVideoHideShowTextCursor(BOOLEAN Show)
{

}

BOOLEAN
UefiVideoIsPaletteFixed(VOID)
{
    return 0;
}

VOID
UefiVideoSetPaletteColor(UCHAR Color, UCHAR Red,
                         UCHAR Green, UCHAR Blue)
{

}

VOID
UefiVideoGetPaletteColor(UCHAR Color, UCHAR* Red,
                         UCHAR* Green, UCHAR* Blue)
{

}

VOID
UefiVideoSync(VOID)
{

}

VOID
UefiGetExtendedBIOSData(PULONG ExtendedBIOSDataArea,
                        PULONG ExtendedBIOSDataSize)
{

}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(VOID)
{
    return 0;
}
