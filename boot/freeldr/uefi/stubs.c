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
UefiInitConsole(_In_ EFI_SYSTEM_TABLE *SystemTable)
{

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
    PCONFIGURATION_COMPONENT_DATA SystemKey;
  //ULONG BusNumber = 0;

    /* Create the 'System' key */
    FldrCreateSystemKey(&SystemKey);
    // TODO: Discover and set the other machine types
    FldrSetIdentifier(SystemKey, "AT/AT COMPATIBLE");
#if 0
    /* Detect buses */
    DetectPciBios(SystemKey, &BusNumber);
    DetectApmBios(SystemKey, &BusNumber);
    DetectPnpBios(SystemKey, &BusNumber);
    DetectIsaBios(SystemKey, &BusNumber); // TODO: Detect first EISA or MCA, before ISA
    DetectAcpiBios(SystemKey, &BusNumber);
#endif
    // TODO: Collect the ROM blocks from 0xC0000 to 0xF0000 and append their
    // CM_ROM_BLOCK data into the 'System' key's configuration data.

    return SystemKey;
}
