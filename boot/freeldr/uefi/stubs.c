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


#if defined(__i386__) || defined(_M_AMD64)
VOID __cdecl DiskStopFloppyMotor(VOID)
{
    //WRITE_PORT_UCHAR((PUCHAR)0x3F2, 0); // DOR_FDC_ENABLE | DOR_DMA_IO_INTERFACE_ENABLE 0x0C // we changed 0x0C->0 to workaround CORE-16469
}
#endif // defined __i386__ || defined(_M_AMD64)
