/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Mach Setup
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS ********************************************************************/

EFI_GUID EfiGraphicsOutputProtocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
extern EFI_SYSTEM_TABLE *GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;
EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
BOOLEAN AcpiPresent = FALSE;

/* FUNCTIONS ******************************************************************/
VOID
MachInit(const char *CmdLine)
{
     EFI_STATUS Status = 0;

    /* Setup GOP */
    Status = GlobalSystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    UefiInitalizeVideo(gop);

  MachVtbl.ConsPutChar = UefiConsPutChar;
    MachVtbl.ConsKbHit = UefiConsKbHit;
    MachVtbl.ConsGetCh = UefiConsGetCh;
    MachVtbl.VideoClearScreen = UefiVideoClearScreen;
    MachVtbl.VideoSetDisplayMode = UefiVideoSetDisplayMode;
    MachVtbl.VideoGetDisplaySize = UefiVideoGetDisplaySize;
    MachVtbl.VideoGetBufferSize = UefiVideoGetBufferSize;
    MachVtbl.VideoGetFontsFromFirmware = UefiVideoGetFontsFromFirmware;
    MachVtbl.VideoSetTextCursorPosition = UefiVideoSetTextCursorPosition;
    MachVtbl.VideoHideShowTextCursor = UefiVideoHideShowTextCursor;
    MachVtbl.VideoPutChar = UefiVideoPutChar;
    MachVtbl.VideoCopyOffScreenBufferToVRAM = UefiVideoCopyOffScreenBufferToVRAM;
    MachVtbl.VideoIsPaletteFixed = UefiVideoIsPaletteFixed;
    MachVtbl.VideoSetPaletteColor = UefiVideoSetPaletteColor;
    MachVtbl.VideoGetPaletteColor = UefiVideoGetPaletteColor;
    MachVtbl.VideoSync = UefiVideoSync;
    MachVtbl.Beep = UefiPcBeep;
    MachVtbl.PrepareForReactOS = UefiPrepareForReactOS;
    MachVtbl.GetMemoryMap = UefiMemGetMemoryMap;
    MachVtbl.GetExtendedBIOSData = UefiGetExtendedBIOSData;
    MachVtbl.GetFloppyCount = UefiGetFloppyCount;
    MachVtbl.DiskReadLogicalSectors = UefiDiskReadLogicalSectors;
    MachVtbl.DiskGetDriveGeometry = UefiDiskGetDriveGeometry;
    MachVtbl.DiskGetCacheableBlockCount = UefiDiskGetCacheableBlockCount;
    MachVtbl.GetTime = UefiGetTime;
    MachVtbl.InitializeBootDevices = UefiInitializeBootDevices;
    MachVtbl.HwDetect = UefiHwDetect;
    MachVtbl.HwIdle = UefiHwIdle;
}
