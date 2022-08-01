/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Misc/Unsupported Mach funcs
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>
#include <debug.h>

VOID
UefiVideoSetPaletteColor(UCHAR Color, UCHAR Red, UCHAR Green, UCHAR Blue)
{
    /* Not supported */
}

VOID
UefiVideoGetPaletteColor(UCHAR Color, UCHAR* Red, UCHAR* Green, UCHAR* Blue)
{
    /* Not supported */
}

VOID
UefiVideoSync(VOID)
{
    /* Not supported */
}

VOID UefiPcBeep(VOID)
{
    /* UEFI sound support wen */
}

VOID
UefiGetExtendedBIOSData(PULONG ExtendedBIOSDataArea, PULONG ExtendedBIOSDataSize)
{
    /* Not supported */
    *ExtendedBIOSDataArea = 0;
    *ExtendedBIOSDataSize = 0;
}




#ifdef _M_ARM

UCHAR MachDefaultTextColor = COLOR_GRAY;

static unsigned int delay_count = 1;

static
VOID
__StallExecutionProcessor(ULONG Loops)
{
    register volatile unsigned int i;
    for (i = 0; i < Loops; i++);
}

VOID StallExecutionProcessor(ULONG Microseconds)
{
    ULONGLONG LoopCount = ((ULONGLONG)delay_count * (ULONGLONG)Microseconds) / 1000ULL;
    __StallExecutionProcessor((ULONG)LoopCount);
}

VOID
HalpCalibrateStallExecution(VOID)
{
}

#endif

#ifdef _M_ARM64


VOID
FrLdrBugCheckWithMessage(
    ULONG BugCode,
    PCHAR File,
    ULONG Line,
    PSTR Format,
    ...)
{

}


VOID
DbgBreakPoint(VOID)
{

}
VOID
HalpCalibrateStallExecution(VOID)
{
}

#endif