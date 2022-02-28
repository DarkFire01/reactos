#pragma once

#include "../../include/rosefip.h"

typedef struct BLOCKINFO
{
    unsigned long long*    BaseAddress;
    unsigned long long     BufferSize;
    unsigned int           ScreenWidth;
    unsigned int           ScreenHeight;
    unsigned int           PixelsPerScanLine;
	unsigned long long*    LoaderFileSize;
	EFI_MEMORY_DESCRIPTOR* MMap;
	unsigned long long     MMapSize;
	unsigned long long     MMapDescriptorSize;
} BLOCKINFO;

EFI_STATUS
RefiInitializeUefiFilesystem(
        _In_ EFI_HANDLE ImageHandle,
        _In_ EFI_SYSTEM_TABLE *SystemTable);

EFI_FILE_PROTOCOL* 
RefiGetFile(CHAR16* FileName);


VOID
RefiReadFile(CHAR16* FileName, PVOID Buffer, EFI_SYSTEM_TABLE *SystemTable);

UINT64* 
RefiGetFileSize (EFI_FILE_PROTOCOL* FileName);