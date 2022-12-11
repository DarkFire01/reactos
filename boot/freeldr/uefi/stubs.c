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

BOOLEAN
UefiConsKbHit(VOID)
{
    return 0;
}

int
UefiConsGetCh(void)
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
VOID
UefiPcBeep(VOID)
{

}

VOID
UefiGetExtendedBIOSData(PULONG ExtendedBIOSDataArea,
                        PULONG ExtendedBIOSDataSize)
{

}

TIMEINFO*
UefiGetTime(VOID)
{
    return 0;
}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(VOID)
{
    return 0;
}
VOID
UefiHwIdle(VOID)
{

}


#if 0
UCHAR DriveMapGetBiosDriveNumber(PCSTR DeviceName)
{
    UCHAR BiosDriveNumber = 0;

    //TRACE("DriveMapGetBiosDriveNumber(%s)\n", DeviceName);

    // If they passed in a number string then just
    // convert it to decimal and return it
    if (DeviceName[0] >= '0' && DeviceName[0] <= '9')
    {
        return (UCHAR)strtoul(DeviceName, NULL, 0);
    }

    // Convert the drive number string into a number
    // 'hd1' = 1
    BiosDriveNumber = atoi(&DeviceName[2]);

    // If it's a hard disk then set the high bit
    if ((DeviceName[0] == 'h' || DeviceName[0] == 'H') &&
        (DeviceName[1] == 'd' || DeviceName[1] == 'D'))
    {
        BiosDriveNumber |= 0x80;
    }

    return BiosDriveNumber;
}
#endif