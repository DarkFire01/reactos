/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI stubs
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>

/* TODO: Handle this with custom Disk / partition setup */
UCHAR
DriveMapGetBiosDriveNumber(PCSTR DeviceName)
{
    return 0;
}

VOID
StallExecutionProcessor(ULONG Microseconds)
{

}

VOID
NTAPI
KeStallExecutionProcessor(ULONG Microseconds)
{
    StallExecutionProcessor(Microseconds);
}

USHORT
__cdecl PxeCallApi(USHORT Segment, USHORT Offset, USHORT Service, VOID* Parameter)
{
    return 0;
}

//TODOS

VOID
UefiConsPutChar(int Ch)
{

}


VOID
UefiInitalizeVideo(_In_ EFI_HANDLE ImageHandle,
                   _In_ EFI_SYSTEM_TABLE *SystemTable,
                   _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{

}

VOID
UefiVideoClearScreen(UCHAR Attr)
{

}

VIDEODISPLAYMODE
UefiVideoSetDisplayMode(char *DisplayMode, BOOLEAN Init)
{
    return 0;
}

VOID
UefiVideoGetDisplaySize(PULONG Width, PULONG Height, PULONG Depth)
{

}

ULONG
UefiVideoGetBufferSize(VOID)
{
    return 0;
}

VOID
UefiVideoGetFontsFromFirmware(PULONG RomFontPointers)
{

}

VOID
UefiVideoSetTextCursorPosition(UCHAR X, UCHAR Y)
{

}

VOID
UefiVideoHideShowTextCursor(BOOLEAN Show)
{

}

VOID
UefiVideoOutputChar(UCHAR Char, unsigned X,
                    unsigned Y, ULONG FgColor, ULONG BgColor)
{

}

VOID
UefiVideoPutChar(int Ch, UCHAR Attr,
                 unsigned X, unsigned Y)
{

}

VOID
UefiVideoCopyOffScreenBufferToVRAM(PVOID Buffer)
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

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
    return 0;
}

VOID
UefiGetExtendedBIOSData(PULONG ExtendedBIOSDataArea,
                        PULONG ExtendedBIOSDataSize)
{

}

UCHAR
UefiGetFloppyCount(VOID)
{
    return 0;
}

BOOLEAN
UefiDiskReadLogicalSectors(IN UCHAR DriveNumber,
                           IN ULONGLONG SectorNumber,
                           IN ULONG SectorCount,
                           OUT PVOID Buffer)
{
    return 0;
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber,
                         PGEOMETRY Geometry)
{
    return 0;
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
    return 0;
}

BOOLEAN
UefiInitializeBootDevices(VOID)
{
    return 0;
}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(VOID)
{
    return 0;
}

VOID
UefiPrepareForReactOS(VOID)
{

}

VOID
UefiPcBeep(VOID)
{
    /* Not possible on UEFI, for now */
}

BOOLEAN
UefiConsKbHit(VOID)
{
    return FALSE;
}

int
UefiConsGetCh(void)
{
    return 0;
}

VOID
UefiHwIdle(VOID)
{

}
