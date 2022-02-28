
#include "fs.h"


struct EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID       = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
struct EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
struct EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID        = {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
struct EFI_GUID EFI_FILE_INFO_GUID                   = {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

EFI_HANDLE ImageHandle;
EFI_SYSTEM_TABLE* SystemTable;
EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume;
EFI_FILE_PROTOCOL* RootFS;
PVOID Buffer;
UINT64* LoaderFileSize;

BLOCKINFO bi;
EFI_HANDLE LocalImageHandle;
EFI_SYSTEM_TABLE *LocalSystemTable;

EFI_STATUS
RefiInitializeUefiFilesystem(
        _In_ EFI_HANDLE ImageHandle,
        _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
	EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    EFI_STATUS RefiStatus;

    LocalImageHandle = ImageHandle;
    LocalSystemTable = SystemTable;
	
    RefiStatus = SystemTable->BootServices->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (PVOID)&LoadedImage);
    if(RefiStatus != EFI_SUCCESS)
    {
        return RefiStatus;
    }
    RefiStatus = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (PVOID)&DevicePath);
	if(RefiStatus != EFI_SUCCESS)
    {
        return RefiStatus;
    }
    RefiStatus = SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (PVOID)&Volume);
	if(RefiStatus != EFI_SUCCESS)
    {
        return RefiStatus;
    }
    RefiStatus = Volume->OpenVolume(Volume, &RootFS);

    return RefiStatus;
}

UINT64* RefiGetFileSize (EFI_FILE_PROTOCOL* FileName)
{
    UINT64* Size = 0;
    FileName->SetPosition(FileName, 0xFFFFFFFF);
    FileName->GetPosition(FileName, Size);
	FileName->SetPosition(FileName, 0);
    return Size;
}

void 
closeFile(EFI_FILE_PROTOCOL* FileHandle)
{
    if((FileHandle->Close(FileHandle)) != EFI_SUCCESS)
	{
        RefiColSetColor(SystemTable,EFI_BROWN);
        RefiColPrint(SystemTable,L"Closing File Handle FAILED\r\n");
	}
}

EFI_FILE_PROTOCOL* 
RefiGetFile(CHAR16* FileName)
{
    EFI_STATUS refiCheck;
    // This opens a file from the EFI FAT32 file system volume.
    // It loads from root, so you must supply full path if the file is not in the root.
    // Example : "somefolder//myfile"  <--- Notice the double forward slash.
    EFI_FILE_PROTOCOL* FileHandle;
    refiCheck = LocalSystemTable->BootServices->AllocatePool(EfiLoaderData, (1000), (void**)&FileHandle);
    if (refiCheck != EFI_SUCCESS)
    {
        RefiColPrint(LocalSystemTable, L"\r\nRefiGetFile: Allocation failed");
    }
    refiCheck = RootFS->Open(RootFS, &FileHandle, FileName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    //RefiColPrint(SystemTable, CheckStandardEFIError(refiCheck));

    
    return FileHandle;
}

UINT32 ENTRY_POINT;
EFI_STATUS refiCheckTwo;
CHAR16* str;
VOID
RefiReadFile(CHAR16* FileName, PVOID Buffer, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_FILE_PROTOCOL* FileHandle;
    refiCheckTwo = SystemTable->BootServices->AllocatePool(EfiLoaderData, (1000), (void**)&FileHandle);
	// We get the file size, allocate memory for it,
    // read the file into the buffer, then we close the file.
    FileHandle = RefiGetFile(FileName);
    if(FileHandle != NULL)
    {
		UINT64* FileSize = (UINT64*)RefiGetFileSize(FileHandle);
		if((SystemTable->BootServices->AllocatePool(EfiLoaderData, *FileSize, Buffer)) != EFI_SUCCESS)
		{
			RefiColSetColor(LocalSystemTable,EFI_BROWN);
		}
		FileHandle->SetPosition(FileHandle, 0);

        if((FileHandle->Read(FileHandle, (UINTN*)FileSize, Buffer)) != EFI_SUCCESS)
		{
			RefiColSetColor(LocalSystemTable,EFI_BROWN);
			RefiColPrint(LocalSystemTable,L"Reading File FAILED\r\n");
		}
		
		LoaderFileSize = FileSize;

		RefiColSetColor(LocalSystemTable,EFI_LIGHTCYAN);    
		RefiColPrint(LocalSystemTable, L"\r\nDynamic File Signature\r\n");
		RefiColSetColor(LocalSystemTable, EFI_LIGHTRED);    
		UINT8* test1 = (UINT8*)Buffer;
		
		// 86 64    <---- BIN
		// 45 4C 46 <---- ELF
		
		UINT8 p1,p2,p3,p4;
		p1 = *test1;
		test1+=1;
		p2 = *test1;
		test1+=1;
		p3 = *test1;
		test1+=1;
		p4 = *test1;

		if(p1 == 100 && p2 == 134)
		{
			RefiColPrint(LocalSystemTable, L"BINARY - 8664 Signature\r\n");
			RefiColSetColor(LocalSystemTable, EFI_WHITE);
			test1+=37;
			p1 = *test1;
			test1+=1;
			p2 = *test1;
			test1+=1;
			p3 = *test1;
			test1+=1;
			p4 = *test1;

				CHAR16 s[2];
				RefiItoa(p1, (PUSHORT)s, 16);
				RefiColPrint(LocalSystemTable, s);
				RefiColPrint(LocalSystemTable, L"  ");
				
				RefiItoa(p2, (PUSHORT)s, 16);
				RefiColPrint(LocalSystemTable, s);
				RefiColPrint(LocalSystemTable, L"  ");
				
				RefiItoa(p3, (PUSHORT)s, 16);
				RefiColPrint(LocalSystemTable, s);
				RefiColPrint(LocalSystemTable, L"  ");
				
				RefiItoa(p4, (PUSHORT)s, 16);
				RefiColPrint(LocalSystemTable, s);
				RefiColSetColor(LocalSystemTable, EFI_BROWN); 
				RefiColPrint(LocalSystemTable, L"  \r\n\r\nENTRY POINT : ");
				RefiColSetColor(LocalSystemTable, EFI_GREEN); 
				
                ENTRY_POINT = (p4 << 24) | (p3 << 16) | (p2 << 8) | p1 ;
				
				UINT16 s2[5];
				RefiItoa(ENTRY_POINT, (PUSHORT)s2, 10);
				RefiColPrint(LocalSystemTable, s2);
				RefiColPrint(LocalSystemTable, L"  ");
		}
		else if(p2 == 69 && p3 == 76 && p4 == 70)
		{
			RefiColPrint(LocalSystemTable, L"ELF - 45 4C 46 Signature\r\n");
			RefiColPrint(LocalSystemTable, L"Add your own code + the ELF Header file to make this work.");
		}
        else
        {
            RefiColPrint(LocalSystemTable, L"Not a binary file");
        }
		
        closeFile(FileHandle);
    }
}
