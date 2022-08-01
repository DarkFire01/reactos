/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Entry point
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

FREELDR_MEMORY_DESCRIPTOR PcMemoryMap[80 + 1];

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
#if 0
    /* Use the power of UEFI to get the memory map */
    EFI_MEMORY_DESCRIPTOR* Map = NULL;
	UINTN MapSize, MapKey;
	UINTN DescriptorSize;
	UINT32 DescriptorVersion;
    unsigned short int* buffer = 0;
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(int), (void**)buffer);
	{
		
		LocSystemTable->BootServices->GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
		LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSize, (void**)&Map);
		LocSystemTable->BootServices->GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);

	}

    UefiVideoClearScreen(0);
    UefiPrintF("MemoryManager SetupComplete", 1, 1, 0xFFFFFF, 0x000000);

    //UINT32 offset = Map->NumberOfPages * EFI_PAGE_SIZE;
    //EFI_MEMORY_TYPE
    return 0;
#endif
return 0;
}
