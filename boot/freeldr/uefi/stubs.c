#include <uefildr.h>

#include <debug.h>

/* Console */
VOID
UefiConsPutChar(int c)
{

}

BOOLEAN
UefiConsKbHit(VOID)
{
    /* No keyboard support yet */
    return FALSE;
}

int
UefiConsGetCh(void)
{
    /* No keyboard support yet */
    while (1) ;

    return 0;
}

/* Video */

VOID
UefiVideoClearScreen(UCHAR Attr)
{

}

VIDEODISPLAYMODE
UefiVideoSetDisplayMode(char *DisplayMode, BOOLEAN Init)
{
  /* We only have one mode, semi-text */
  return VideoTextMode;
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
  /* We don't have a cursor yet */
}

VOID
UefiVideoHideShowTextCursor(BOOLEAN Show)
{
  /* We don't have a cursor yet */
}

VOID
UefiVideoPutChar(int Ch, UCHAR Attr, unsigned X, unsigned Y)
{

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

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
}

VOID
UefiGetExtendedBIOSData(PULONG ExtendedBIOSDataArea, PULONG ExtendedBIOSDataSize)
{
    /* lol what the fuck */
    *ExtendedBIOSDataArea = 0;
    *ExtendedBIOSDataSize = 0;
}

static
UCHAR
UefiGetFloppyCount(VOID)
{
    /* no floppy for you */
    return 0;
}

BOOLEAN
UefiDiskReadLogicalSectors(
    IN UCHAR DriveNumber,
    IN ULONGLONG SectorNumber,
    IN ULONG SectorCount,
    OUT PVOID Buffer)
{
}

BOOLEAN
UefiDiskGetDriveGeometry(UCHAR DriveNumber, PGEOMETRY Geometry)
{
}

ULONG
UefiDiskGetCacheableBlockCount(UCHAR DriveNumber)
{
}

TIMEINFO*
UefiGetTime(VOID)
{
}

BOOLEAN
UefiInitializeBootDevices(VOID)
{
}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(VOID)
{
}

VOID
UefiPrepareForReactOS(VOID)
{

}

VOID UefiHwIdle(VOID)
{
    /* UNIMPLEMENTED */
}


