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


