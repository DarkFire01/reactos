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
EFI_SYSTEM_TABLE *LocSystemTable;

VOID
UefiMemInit(_In_ EFI_HANDLE ImageHandle,
             _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    LocSystemTable = SystemTable;
}
 PFREELDR_MEMORY_DESCRIPTOR FreeldrMem;
VOID
UefiMemConfigure()
{

}
UINT32 MapSizeStatic;
UINT32 DescriptorSizeStatic;
UINT32 MapSize;
UINT32 MapKey;
UINT32 DescriptorSize;
PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
    UefiVideoClearScreen(0);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(FreeldrMem) * 256, (void**)&FreeldrMem);

    /* Use the power of UEFI to get the memory map */
    EFI_MEMORY_DESCRIPTOR* Map = NULL;
    UINT32 EntryCount, count;


	UINT32 DescriptorVersion;
    unsigned short int* buffer = 0;

   // LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(EFI_MEMORY_DESCRIPTOR)  * 80, (void**)Map);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(int), (void**)buffer);
	LocSystemTable->BootServices->GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSize, (void**)&Map);
	LocSystemTable->BootServices->GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    //&DescriptorSizeStatic = (UINT32*)DescriptorSize;

    /* Number of map entries */
    EntryCount = (MapSize / DescriptorSize);
    UefiConsSetCursor(0,0);
    UefiVideoClearScreen(0);
 EFI_MEMORY_DESCRIPTOR* MapOffset;

    /*
     * Now here is where things get fun / they suck
     * Freeldr is built to load memory in a way that's specific
     * to BIOS. But here freeldr is going to be using UEFI, So for now we must make some kind of
     * ... Translation chart i guess. 
     * We *COULD* add in UEFI entries but that isn't.. what i think is the right path for now/
     * EFI_MEMORY_TYPE is the enum that decides what type of memory it is
     */
    for(count = 0; count < 4; count++)
    {
        MapOffset = &Map[count];
           // EFI_MEMORYa_DESCRIPTOR* MapOffset = (EFI_MEMORY_DESCRIPTOR*)(Map + (count * DescriptorSize));
#if 1
        switch(Map->Type)
        {
            case EfiConventionalMemory:
                AddMemoryDescriptor(FreeldrMem,
                                    MAX_BIOS_DESCRIPTORS,
                                    (MapOffset->PhysicalStart / MM_PAGE_SIZE),
                                    MapOffset->NumberOfPages,
                                    LoaderFree);
                break;
            case EfiLoaderCode:
                AddMemoryDescriptor(FreeldrMem,
                                    MAX_BIOS_DESCRIPTORS,
                                    (MapOffset->PhysicalStart / MM_PAGE_SIZE),
                                    MapOffset->NumberOfPages,
                                    LoaderLoadedProgram);
                break;
            default:
                AddMemoryDescriptor(FreeldrMem,
                                MAX_BIOS_DESCRIPTORS,
                                MapOffset->PhysicalStart / MM_PAGE_SIZE,
                                MapOffset->NumberOfPages,
                                LoaderReserve);
                break;
        }
    #endif
      //  UefiVideoClearScreen(0);
    }
    UefiVideoClearScreen(0);
    printf("leaving func");
    return FreeldrMem;
}
