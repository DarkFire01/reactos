
#include <rosefi.h>

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
    EfiCheckSucess(Status, SystemTable);
    EfiColPrint(L"File System is ready",SystemTable);
}

EFI_FILE_PROTOCOL*
RefiOpenFile(CHAR16* FileName, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL* RootFS;
    EFI_FILE_PROTOCOL* FileHandle = NULL;
    EfiSetColor(EFI_MAGENTA, SystemTable);
    Status = Volume->OpenVolume(Volume, &RootFS);
    EfiCheckSucess(Status, SystemTable);
    Status = RootFS->Open(RootFS, &FileHandle, FileName, 0x0000000000000001, 0);
    EfiCheckSucess(Status, SystemTable);
    return FileHandle;
}
VOID
RefiCheckSucess(EFI_STATUS Status, EFI_SYSTEM_TABLE *SystemTable)
{
    if (Status == 0)
    {
        EfiColPrint(L":D yay",SystemTable);
    }
    else
    {
        EfiColPrint(L"D: nuuuu",SystemTable);
    }
}
