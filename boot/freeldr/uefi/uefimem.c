/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Entry point
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);
#define NEXT_MEMORY_DESCRIPTOR(Descriptor, DescriptorSize) \
    (EFI_MEMORY_DESCRIPTOR*)((UINTN*)(Descriptor) + (DescriptorSize))
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
PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
ULONG FreeldrEntryCount = 0;
PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
    EFI_STATUS Status;
    UINTN MapSize = 0;
    EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
    EFI_MEMORY_DESCRIPTOR* MapEntry;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;
    UINT32 EntryCount, Index; // Replaced "count" by "Index".
 
    /* Retrieve the initial needed memory map size */
    Status = LocSystemTable->BootServices->GetMemoryMap(&MapSize,
                                                        MemoryMap,
                                                        &MapKey,
                                                        &DescriptorSize,
                                                        &DescriptorVersion);

    /* Reallocate and retrieve again the needed memory map size (since memory
     * allocated by AllocatePool() counts in the map), until it's OK. */
    do
    {
        /* Reallocate the memory map buffer */
        if (MemoryMap)
            LocSystemTable->BootServices->FreePool(MemoryMap);
        LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSize, &MemoryMap);
        ASSERT(MemoryMap != NULL); // FIXME Error Handling with graceful exit!
 
        Status = LocSystemTable->BootServices->GetMemoryMap(&MapSize,
                                                            MemoryMap,
                                                            &MapKey,
                                                            &DescriptorSize,
                                                            &DescriptorVersion);
    } while (Status == EFI_BUFFER_TOO_SMALL);

    if (EFI_ERROR(Status))
    {
        LocSystemTable->BootServices->FreePool(MemoryMap);
        *MemoryMapSize = 0;
        return NULL;
    }

    EntryCount = MapSize / DescriptorSize;
    MapEntry = MemoryMap;

    Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(FreeldrMem) * EntryCount, &FreeldrMem);
    if (EFI_ERROR(Status))
    {
        LocSystemTable->BootServices->FreePool(MemoryMap);
        *MemoryMapSize = 0;
        return NULL;
    }

    for (Index = 0; Index < EntryCount; ++Index)
    {
        switch (MapEntry->Type)
        {
            //
            // Note: The following code is repetitive and can be simplified.
            //
            case EfiConventionalMemory:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount,
                                                        (MapEntry->PhysicalStart / EFI_PAGE_SIZE),
                                                        MapEntry->NumberOfPages,
                                                        LoaderFree);
                //printf("EfiConventionalMemory: %X\r\n", MapEntry->NumberOfPages);
                //printf("EfiConventionalMemory Physical Start: %X\r\n", MapEntry->PhysicalStart);
                break;
            }
 
            case EfiLoaderCode:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount,
                                                        (MapEntry->PhysicalStart / EFI_PAGE_SIZE),
                                                        MapEntry->NumberOfPages,
                                                        LoaderLoadedProgram);
                //printf("EfiLoaderCode: %X\r\n", MapEntry->NumberOfPages);
                //printf("EfiLoaderCode Physical Start: %X\r\n", MapEntry->PhysicalStart);
                break;
            }
 
            default:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount,
                                                        MapEntry->PhysicalStart / EFI_PAGE_SIZE,
                                                        MapEntry->NumberOfPages,
                                                        LoaderReserve);
                //printf("Other Memory: %X\r\n", MapEntry->NumberOfPages);
                //printf("Other Memory Physical Start: %X\r\n", MapEntry->PhysicalStart);
                break;
            }
        }
 
        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescriptorSize);
    }

    printf("Exiting func");
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