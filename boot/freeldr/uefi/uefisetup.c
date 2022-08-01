#include <uefildr.h>

#include <debug.h>

EFI_GUID EfiGraphicsOutputProtocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

EFI_STATUS
UefiMachInit(_In_ EFI_HANDLE ImageHandle,
             _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"UEFI EntryPoint: Starting freeldr from UEFI");
    Status = SystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(MachVtbl), (void**)&MachVtbl);

    /* Pass SystemTable ptr to console */
    UefiInitConsole(SystemTable);

    /* Setup GOP mode */
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    UefiInitalizeVideo(ImageHandle, SystemTable, gop);
    UefiVideoClearScreen(0);
    UefiPrintF("GOP setup sucessful", 0, 0, 0xFFFFFF, 0x000000);
  
    /* Temporary code */
    UefiVideoClearScreen(0);
    UefiInitializeInputSupport(ImageHandle, SystemTable);
    UefiPrintF("Press any key to continue", 0, 0, 0xFFFFFF, 0x000000);
    while(UefiConsKbHit() ==  FALSE)
    {

    }
    UefiVideoClearScreen(0);
    UefiPollAndDrawKeyboardInput();
    /* Now wait until a key becomes available.  This is a simple
       polling implementation.  You could try and use the WaitForKey
       event instead if you like */
    UefiPrintF("Keypressed!", 0, 2, 0xFFFFFF, 0x000000);
  
    /* Setup vtbl */
    RtlZeroMemory(&MachVtbl, sizeof(MachVtbl));
    /* Universal */
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

    /* hmm */
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

  //  HalpCalibrateStallExecution();
  return EFI_SUCCESS;
}