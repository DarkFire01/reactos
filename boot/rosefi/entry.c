/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI entry point
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

struct EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID       = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
struct EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
struct EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID        = {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
struct EFI_GUID EFI_FILE_INFO_GUID                   = {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};


EFI_GUID EfiGraphicsOutputProtocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume;
EFI_FILE_PROTOCOL* Font;
PPSF1_FONT OSBuffer_Handle;
PPSF1_FONT CoreFont;
PVOID GlyphBufferHolder;
PUCHAR Test;
EFI_STATUS
RefiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    EFI_STATUS refiCheck;

    RefiColPrint(SystemTable, L"RefiEntry: Starting ROSEFI\r\n");

    /* Grab GOP pointer and initalize UI */
    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    RefiInitUI(SystemTable, gop);
    RefiDrawUIBackground();
#if 0
    InitializeFILESYSTEM(ImageHandle, SystemTable);
    RefiBaseDrawBox(32, 32, 128, 64, 0xF03F00);
    //PSF1_FONT baseFont = LoadPSF1Font();
    //RefiBaseClearScreen(0x0000FF);
    RefiColPrint(SystemTable, L"Attempting to Load font...");
    CoreFont->psf1_header = LoadPSF1Font(SystemTable);
    if (CoreFont->psf1_header.magic[0] == 0x36)
    {
        RefiColPrint(SystemTable, L"Font header sucessfully identified");
    }
    if (CoreFont->psf1_header.magic[1] == 0x04)
    {
        RefiColPrint(SystemTable, L"Font header sucessfully identified");
    }
    if (CoreFont->psf1_header.magic[0] == 0x32)
    {
        RefiColPrint(SystemTable, L"Font header failed");
    }
    if (CoreFont->glyphBuffer == 0)
    {
        RefiColPrint(SystemTable, L"Oh shit its dead");
    }

    refiCheck = SystemTable->BootServices->AllocatePool(EfiLoaderData, 0x00020000, (void**)&GlyphBufferHolder);
    //GlyphBufferHolder = (PVOID)(OSBuffer_Handle + sizeof(PSF1_HEADER));
    CoreFont->glyphBuffer = 0;
    CoreFont->glyphBuffer = OSBuffer_Handle->glyphBuffer;
    //RefiBaseClearScreen(0x0000FF);
    RosEFIAdvPutChar(CoreFont, 0xFF00FF, 'H', 32, 32);
    RosEFIAdvPutChar(CoreFont, 0xFF00FF, 'E', 32 + 32 + 8, 32);
    RosEFIAdvPutChar(CoreFont, 0xFF00FF, 'L', 32 + 64 + 8, 32);
    RosEFIAdvPutChar(CoreFont, 0xFF00FF, 'L', 32 + 96 + 8, 32);
    RosEFIAdvPutChar(CoreFont, 0xFF00FF, 'O', 32 + 128 + 8, 32);
    //RefiBaseDrawBox(0,0,32,32,0x00FF00);
    //RosEFIAdvPutChar(CoreFont, 0xFF00FF, 'G', 64 + 8, 64 + 8);
    /* BOT */
    //RefiStartFileSystem(ImageHandle, SystemTable, &Volume);
    /* Start FS support */
#endif
    for(;;)
    {
        RefiStallProcessor(SystemTable, 1000);
    }
    refiCheck = RefiInitMemoryManager(SystemTable);
    if (refiCheck != EFI_SUCCESS)
    {
        RefiColPrint(SystemTable, L"RefiEntry: Initalizing MemoryManager has failed\r\n");
        RefiColPrint(SystemTable, L"RefiEntry: Rebooting...\r\n");
        RefiStallProcessor(SystemTable, 2000);
        SystemTable->RuntimeServices->ResetSystem(EfiResetCold, refiCheck, 0, 0);
    }

    return refiCheck;
}

VOID
RefiStartLoader()
{
    /* ROSEFI is no longer doing UEFI setup shiz */
    /* Let's load some NT */
}

/* IM DOING THIS TEMPORARILY TO CREATE FILE SYSTEM DATA */
#if 0
PSF1_HEADER LoadPSF1Font(EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    PSF1_HEADER Result;
    readFile(SystemTable, L"\\Font.psf");
    Status = SystemTable->BootServices->AllocatePool(EfiLoaderData, (0x00010000), (void**)&CoreFont);
    RefiColPrint(SystemTable,CheckStandardEFIError(Status));
    Result = OSBuffer_Handle->psf1_header;
    return Result;
}
#endif
VOID
InitializeFILESYSTEM(_In_ EFI_HANDLE ImageHandle,
                     _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    // To load a file, you must have a file system. EFI takes advantage of the FAT32 file system.
    RefiColPrint(SystemTable, L"LoadedImage ... ");
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    Status = SystemTable->BootServices->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void**)&LoadedImage);
    RefiColPrint(SystemTable, CheckStandardEFIError(Status));
    if (Status == EFI_SUCCESS)
    {
        RefiColPrint(SystemTable, L"Success");
    }
    
    RefiColPrint(SystemTable, L"DevicePath ... ");
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    Status = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void**)&DevicePath);
    RefiColPrint(SystemTable, CheckStandardEFIError(Status));
    
    RefiColPrint(SystemTable, L"Volume ... ");
    Status = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&Volume);
    RefiColPrint(SystemTable,CheckStandardEFIError(Status));
}

EFI_FILE_PROTOCOL* openFile(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName)
{
    // This opens a file from the EFI FAT32 file system volume.
    // It loads from root, so you must supply full path if the file is not in the root.
    // Example : "somefolder//myfile"  <--- Notice the double forward slash.
    EFI_STATUS Status;
    RefiColPrint(SystemTable, L"RootFS ... ");
    EFI_FILE_PROTOCOL* RootFS;
    Status = Volume->OpenVolume(Volume, &RootFS);
     RefiColPrint(SystemTable, CheckStandardEFIError(Status));

    RefiColPrint(SystemTable, L"Opening File ... ");
    EFI_FILE_PROTOCOL* FileHandle = NULL;
    Status = RootFS->Open(RootFS, &FileHandle, FileName, 0x0000000000000001, 0);
    RefiColPrint(SystemTable, CheckStandardEFIError(Status));
    return FileHandle;
}

void closeFile(EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_PROTOCOL* FileHandle)
{
    // This closes the file.
    EFI_STATUS Status;
    RefiColPrint(SystemTable, L"Closing File ... ");
    Status = FileHandle->Close(FileHandle);
     RefiColPrint(SystemTable, CheckStandardEFIError(Status));
}

void readFile(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName)
{
    // We create the buffer, allocate memory for it, then read
    // the rile into the buffer. After which, we close the file.
	// Currently we are using a fixed size. Eventually we will fix that.
	// Currently we have a fixed Buffer Handle as well. Eventually we will fixe that.
    EFI_FILE_PROTOCOL* mytextfile = openFile(SystemTable, FileName);
    if(mytextfile != NULL)
    {
        RefiColPrint(SystemTable, L"AllocatingPool ... ");
        EFI_STATUS Status = SystemTable->BootServices->AllocatePool(EfiLoaderData, 0x00010000, (void**)&OSBuffer_Handle);
         RefiColPrint(SystemTable, CheckStandardEFIError(Status));
    
        UINT64 fileSize = 0x0010000;
        
        RefiColPrint(SystemTable, L"Reading File ... ");
        Status = mytextfile->Read(mytextfile, (UINTN*)&fileSize, OSBuffer_Handle);
         RefiColPrint(SystemTable, CheckStandardEFIError(Status));
        closeFile(SystemTable, mytextfile);
    }
}