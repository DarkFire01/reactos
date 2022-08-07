/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Entry point
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

EFI_SYSTEM_TABLE *LocSystemTable;
EFI_HANDLE LocImageHandle;
VOID
UefiMemInit(_In_ EFI_HANDLE ImageHandle,
             _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    LocSystemTable = SystemTable;
    LocImageHandle = ImageHandle;
}

VOID
UefiMemConfigure()
{

}
PFREELDR_MEMORY_DESCRIPTOR FreeldrMem;
UINTN MapSizeStatic;
UINTN DescriptorSizeStatic;
UINTN *MapSize;
UINTN *MapKey;
UINTN *DescriptorSize;
UINTN *DescriptorVersion;
PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
    EFI_MEMORY_TYPE Type;
    UINT32 EntryCount, count;
    EFI_STATUS Status = 0;
    MapSizeStatic = MapSize[0];
    DescriptorSizeStatic = DescriptorSize[0];
    UefiVideoClearScreen(0);
    //EFI_MEMORY_DESCRIPTOR* ActiveDesc = NULL;
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(UINTN), (void**)MapSize);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(UINTN), (void**)MapKey);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(UINTN), (void**)DescriptorSize);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(UINTN), (void**)DescriptorVersion);
    RtlZeroMemory(MapSize, sizeof(UINTN));
    RtlZeroMemory(MapKey, sizeof(UINTN));
    RtlZeroMemory(DescriptorSize, sizeof(UINTN));
    RtlZeroMemory(DescriptorVersion, sizeof(UINTN));
    
    /*
     * 1) Obtain memory map size but getting a porition of the memory map
     * 2) ALlocate pool big enough to fit memory map detected at runtime
     * 3) Obtain full memory map
     */
    EFI_MEMORY_DESCRIPTOR* Map = NULL;
    Status = LocSystemTable->BootServices->GetMemoryMap(MapSize, Map, MapKey, DescriptorSize, DescriptorVersion);
  // Status = LocSystemTable->BootServices->GetMemoryMap(NULL, Map, NULL, NULL, NULL);
    printf("Memorymap Startus %d\r\n", Status);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSizeStatic, (void**)&Map);
    Status = LocSystemTable->BootServices->GetMemoryMap(MapSize, Map, MapKey, DescriptorSize, DescriptorVersion);
    printf("Memorymap Startus %d\r\n", Status);
    //ActiveDesc = Map;
    EntryCount = (MapSizeStatic / DescriptorSizeStatic);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(FreeldrMem) * (EntryCount), (void**)&FreeldrMem);
    printf("EntryCount:%d", EntryCount);
    for(count = 0; count < EntryCount; count++)
    {
        Type = Map->Type;

        switch(Type)
        {
            case EfiConventionalMemory:
                AddMemoryDescriptor(FreeldrMem,
                                    MAX_BIOS_DESCRIPTORS,
                                    (Map->PhysicalStart / EFI_PAGE_SIZE),
                                    Map->NumberOfPages,
                                    LoaderFree);
                                    printf("hi! Free mem");
                break;
            case EfiLoaderCode:
                AddMemoryDescriptor(FreeldrMem,
                                    MAX_BIOS_DESCRIPTORS,
                                    (Map->PhysicalStart / EFI_PAGE_SIZE),
                                    Map->NumberOfPages,
                                    LoaderLoadedProgram);
                break;
            default:
                AddMemoryDescriptor(FreeldrMem,
                                MAX_BIOS_DESCRIPTORS,
                                Map->PhysicalStart / EFI_PAGE_SIZE,
                                Map->NumberOfPages,
                                LoaderReserve);
                break;
        }

        Map = (EFI_MEMORY_DESCRIPTOR*)((UINTN*)Map + DescriptorSizeStatic);
    }
    for(;;) 
    {

    }
    #if 0
    //LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(FreeldrMem) * 256, (void**)&FreeldrMem);

    /* Use the power of UEFI to get the memory map */
    EFI_MEMORY_DESCRIPTOR* Map = NULL;
    UINT32 EntryCount, count;


    EFI_MEMORY_DESCRIPTOR* buffer = 0;

   // LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(EFI_MEMORY_DESCRIPTOR)  * 80, (void**)Map);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(int), (void**)buffer);
	LocSystemTable->BootServices->GetMemoryMap(&MapSize, buffer, &MapKey, &DescriptorSize, &DescriptorVersion);
    LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSize, (void**)&Map);
    //RtlZeroMemory(&Map, MapSize);
	LocSystemTable->BootServices->GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    //&DescriptorSizeStatic = (UINT32*)DescriptorSize;

    /* Number of map entries */
    EntryCount = (MapSize / DescriptorSize);
    UefiConsSetCursor(0,0);
    UefiVideoClearScreen(0);
    EFI_MEMORY_DESCRIPTOR* MapOffset;
    //printf("EntryCount %d\r\n", EntryCount);
    /*
     * Now here is where things get fun / they suck
     * Freeldr is built to load memory in a way that's specific
     * to BIOS. But here freeldr is going to be using UEFI, So for now we must make some kind of
     * ... Translation chart i guess. 
     * We *COULD* add in UEFI entries but that isn't.. what i think is the right path for now/
     * EFI_MEMORY_TYPE is the enum that decides what type of memory it is
     */
    for(count = 0; count < EntryCount; count++)
    {
       MapOffset = &Map[count];
       //  MapOffset = (EFI_MEMORY_DESCRIPTOR*)(Map + (count * DescriptorSize));

      // printf("MemType: 0x%X", Map[count].Type);
      // printf("Attribute: 0x%X", Map[count].Attribute);
      // printf("NumberOfPages: 0x%X", Map[count].NumberOfPages);
      // printf("PhysicalStart: 0x%X", Map[count].PhysicalStart);
      // printf("VirtualStart: 0x%X\r\n", Map[count].VirtualStart);
 

        // if(MapOffset->Attribute)
#if 1
        switch(MapOffset->Type)
        {
            case EfiConventionalMemory:
                   printf("EfiConventionalMemory: %X\r\n", MapOffset->NumberOfPages);
                AddMemoryDescriptor(FreeldrMem,
                                    MAX_BIOS_DESCRIPTORS,
                                    (MapOffset->PhysicalStart / MM_PAGE_SIZE),
                                    MapOffset->NumberOfPages,
                                    LoaderFree);
                printf("EfiConventionalMemory: %X\r\n", MapOffset->NumberOfPages);
                break;
            case EfiLoaderCode:
                AddMemoryDescriptor(FreeldrMem,
                                    MAX_BIOS_DESCRIPTORS,
                                    (MapOffset->PhysicalStart / MM_PAGE_SIZE),
                                    MapOffset->NumberOfPages,
                                    LoaderLoadedProgram);
                printf("EfiLoaderCode: %X\r\n", MapOffset->NumberOfPages);
                break;
            default:
                AddMemoryDescriptor(FreeldrMem,
                                MAX_BIOS_DESCRIPTORS,
                                MapOffset->PhysicalStart / MM_PAGE_SIZE,
                                MapOffset->NumberOfPages,
                                LoaderReserve);
                printf("Other Memory: %X\r\n", MapOffset->NumberOfPages);
                break;
        }
    #endif
      //  UefiVideoClearScreen(0);
    }
#endif
  //  UefiVideoClearScreen(0);
    UefiVideoClearScreen(0);
    return FreeldrMem;
}


VOID
UefiPrepareForReactOS(VOID)
{
    #if 0

     EFI_MEMORY_DESCRIPTOR* Map = NULL;



	UINT32 DescriptorVersion;
   // unsigned short int* buffer = 0;
 //   LocSystemTable->BootServices->GetMemoryMap(MapSize, Map, MapKey, DescriptorSize, DescriptorVersion);

    EFI_STATUS status;
    printf("TIME TOO BOOT MOTHER FRUCKLERS");
   // status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle, MapKey);
    if (status != EFI_SUCCESS)
    {
        printf("Dead\r\n");
        printf("status is: %d", status);

    } 
#endif
}