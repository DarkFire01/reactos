
#include "fs.h"

VOID
RefiStartFileSystem(EFI_HANDLE ImageHandle,
                   EFI_SYSTEM_TABLE *SystemTable,
                   EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume)
{
    EFI_STATUS Status; 
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    Status = SystemTable->BootServices->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void**)&LoadedImage);
    Status = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void**)&DevicePath);
    Status = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&Volume);
    //EfiCheckSucess(Status, SystemTable);
    //EfiColPrint(L"File System is ready",SystemTable);
}
#if 0
EFI_FILE_PROTOCOL*
RefiOpenFile(CHAR16* FileName, EFI_SYSTEM_TABLE *SystemTable)
{

    EFI_STATUS Status;
    EFI_FILE_PROTOCOL* RootFS;
    EFI_FILE_PROTOCOL* FileHandle = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume
    //EfiSetColor(EFI_MAGENTA, SystemTable);
    Status = Volume->OpenVolume(Volume, &RootFS);
   // EfiCheckSucess(Status, SystemTable);
    Status = RootFS->Open(RootFS, &FileHandle, FileName, 0x0000000000000001, 0);
   // EfiCheckSucess(Status, SystemTable);
    return FileHandle;
}
#endif
