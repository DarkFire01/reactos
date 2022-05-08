#include <uefildr.h>

#include <debug.h>

/* Video */

VOID
UefiVideoGetFontsFromFirmware(PULONG RomFontPointers)
{

}

VOID
UefiVideoSetTextCursorPosition(UCHAR X, UCHAR Y)
{
  /* We don't have a cursor yet */
}

VOID
UefiVideoHideShowTextCursor(BOOLEAN Show)
{
  /* We don't have a cursor yet */
}

VOID
UefiVideoCopyOffScreenBufferToVRAM(PVOID Buffer)
{

}

BOOLEAN
UefiVideoIsPaletteFixed(VOID)
{
  return FALSE;
}

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

/* Arch specific / Other */

VOID UefiPcBeep(VOID)
{
    /* uefi sound support wen */
}

VOID
UefiGetExtendedBIOSData(PULONG ExtendedBIOSDataArea, PULONG ExtendedBIOSDataSize)
{
    /* lol what the fuck */
    *ExtendedBIOSDataArea = 0;
    *ExtendedBIOSDataSize = 0;
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
UefiPrepareForReactOS(VOID)
{

}

VOID UefiHwIdle(VOID)
{
    /* UNIMPLEMENTED */
}


