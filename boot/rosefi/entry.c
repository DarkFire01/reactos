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
void* OSBuffer_Handle;

EFI_STATUS
RefiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    EFI_STATUS refiCheck;

    //RefiDebugInit(0);
    RefiColPrint(SystemTable, L"RefiEntry: Starting ROSEFI\r\n");

    /* Grab GOP pointer and initalize UI */
    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    RefiInitUI(SystemTable, gop);

    RefiBaseClearScreen(0x000000);
    RefiBaseDrawBox(32, 32, 64, 64, 0x423E41);

    InitializeFILESYSTEM(ImageHandle, SystemTable);
    RefiBaseDrawBox(32, 32, 128, 64, 0xF03F00);
    //PSF1_FONT baseFont = LoadPSF1Font();
   // EFI_FILE_PROTOCOL* Font = openFile(SystemTable, (CHAR16*)"font.psf");
    RefiBaseClearScreen(0x000000);
    /* BOT */
    //RefiStartFileSystem(ImageHandle, SystemTable, &Volume);
    /* Start FS support */
    for(;;)
    {

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


int memcmp(const void* aptr, const void* bptr, size_t n){
	const unsigned char* a = aptr, *b = bptr;
	for (size_t i = 0; i < n; i++){
		if (a[i] < b[i]) return -1;
		else if (a[i] > b[i]) return 1;
	}
	return 0;
}



VOID
RefiStartLoader()
{
    /* Here we begin real loader setup */
}

unsigned short int* CheckStandardEFIError(unsigned long long s)
{
    switch(s)
    {
        case EFI_LOAD_ERROR:
        {
            return (unsigned short int*)L" Load Error\r\n";
        }
        case EFI_INVALID_PARAMETER:
        {
            return (unsigned short int*)L" Invalid Parameter\r\n";
        }
        case EFI_UNSUPPORTED:
        {
            return (unsigned short int*)L" Unsupported\r\n";
        }
        case EFI_BAD_BUFFER_SIZE:
        {
            return (unsigned short int*)L" Bad Buffer Size\r\n";
        }
        case EFI_BUFFER_TOO_SMALL:
        {
            return (unsigned short int*)L" Buffer Too Small\r\n";
        }
        case EFI_NOT_READY:
        {
            return (unsigned short int*)L" Not Ready\r\n";
        }
        case EFI_DEVICE_ERROR:
        {
            return (unsigned short int*)L" Device Error\r\n";
        }
        case EFI_WRITE_PROTECTED:
        {
            return (unsigned short int*)L" Write Protected\r\n";
        }
        case EFI_OUT_OF_RESOURCES:
        {
            return (unsigned short int*)L" Out Of Resources\r\n";
        }
        case EFI_VOLUME_CORRUPTED:
        {
            return (unsigned short int*)L" Volume Corrupted\r\n";
        }
        case EFI_VOLUME_FULL:
        {
            return (unsigned short int*)L" Volume Full\r\n";
        }
        case EFI_NO_MEDIA:
        {
            return (unsigned short int*)L" No Media\r\n";
        }
        case EFI_MEDIA_CHANGED:
        {
            return (unsigned short int*)L" Media Changed\r\n";
        }
        case EFI_NOT_FOUND:
        {
            return (unsigned short int*)L" File Not Found\r\n";
        }
        case EFI_ACCESS_DENIED:
        {
            return (unsigned short int*)L" Access Denied\r\n";
        }
        case EFI_NO_RESPONSE:
        {
            return (unsigned short int*)L" No Response\r\n";
        }
        case EFI_NO_MAPPING:
        {
            return (unsigned short int*)L" No Mapping\r\n";
        }
        case EFI_TIMEOUT:
        {
            return (unsigned short int*)L" Timeout\r\n";
        }
        case EFI_NOT_STARTED:
        {
            return (unsigned short int*)L" Not Started\r\n";
        }
        case EFI_ALREADY_STARTED:
        {
            return (unsigned short int*)L" Already Started\r\n";
        }
        case EFI_ABORTED:
        {
            return (unsigned short int*)L" Aborted\r\n";
        }
        case EFI_ICMP_ERROR:
        {
            return (unsigned short int*)L" ICMP Error\r\n";
        }
        case EFI_TFTP_ERROR:
        {
            return (unsigned short int*)L" TFTP Error\r\n";
        }
        case EFI_PROTOCOL_ERROR:
        {
            return (unsigned short int*)L" Protocol Error\r\n";
        }
        case EFI_INCOMPATIBLE_VERSION:
        {
            return (unsigned short int*)L" Incompatible Version\r\n";
        }
        case EFI_SECURITY_VIOLATION:
        {
            return (unsigned short int*)L" Security Violation\r\n";
        }
        case EFI_CRC_ERROR:
        {
            return (unsigned short int*)L" CRC Error\r\n";
        }
        case EFI_END_OF_MEDIA:
        {
            return (unsigned short int*)L" End Of Media\r\n";
        }
        case EFI_END_OF_FILE:
        {
            return (unsigned short int*)L" End Of File\r\n";
        }
        case EFI_INVALID_LANGUAGE:
        {
            return (unsigned short int*)L" Invalid Language\r\n";
        }
        case EFI_COMPROMISED_DATA:
        {
            return (unsigned short int*)L" Compromised Data\r\n";
        }
        case EFI_IP_ADDRESS_CONFLICT:
        {
            return (unsigned short int*)L" IP Address Conflict\r\n";
        }
        case EFI_HTTP_ERROR:
        {
            return (unsigned short int*)L" End Of File\r\n";
        }
        case EFI_WARN_UNKNOWN_GLYPH:
        {
            return (unsigned short int*)L" WARNING - Unknown Glyph\r\n";
        }
        case EFI_WARN_DELETE_FAILURE:
        {
            return (unsigned short int*)L" WARNING - Delete Failure\r\n";
        }
        case EFI_WARN_WRITE_FAILURE:
        {
            return (unsigned short int*)L" WARNING - Write Failure\r\n";
        }
        case EFI_WARN_BUFFER_TOO_SMALL:
        {
            return (unsigned short int*)L" WARNING - Buffer Too Small\r\n";
        }
        case EFI_WARN_STALE_DATA:
        {
            return (unsigned short int*)L" WARNING - Stale Data\r\n";
        }
        case EFI_WARN_FILE_SYSTEM:
        {
            return (unsigned short int*)L" WARNING - File System\r\n";
        }
        case EFI_WARN_RESET_REQUIRED:
        {
            return (unsigned short int*)L" WARNING - Reset Required\r\n";
        }
        case EFI_SUCCESS:
        {
            return (unsigned short int*)L" Successful\r\n";
        }
    }
    return (unsigned short int*)L" ERROR\r\n";
}

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
    if (Status == EFI_LOAD_ERROR)
    {
        RefiColPrint(SystemTable, L"Failed to load font");
    }
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
        EFI_STATUS Status = SystemTable->BootServices->AllocatePool(EfiLoaderData, 0x00100000, (void**)&OSBuffer_Handle);
         RefiColPrint(SystemTable, CheckStandardEFIError(Status));
    
        UINT64 fileSize = 0x00100000;
        
        RefiColPrint(SystemTable, L"Reading File ... ");
        Status = mytextfile->Read(mytextfile, (UINTN*)&fileSize, OSBuffer_Handle);
         RefiColPrint(SystemTable, CheckStandardEFIError(Status));
        closeFile(SystemTable, mytextfile);
    }
}