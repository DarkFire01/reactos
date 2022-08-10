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

extern EFI_SYSTEM_TABLE *LocSystemTable;
extern EFI_HANDLE LocImageHandle;
extern PFREELDR_MEMORY_DESCRIPTOR BiosMemoryMap;
PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
ULONG FreeldrEntryCount = 0;
UINTN MapSize;
UINTN MapKey;
UINTN DescriptorSize;
UINT32 DescriptorVersion;
EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{

    #if 1
    EFI_STATUS Status = 0;
    EFI_MEMORY_DESCRIPTOR* MapEntry;
    UINTN EntryCount, Index; // Replaced "count" by "Index".
 
    /* Retrieve the initial needed memory map size */
    Status = LocSystemTable->BootServices->GetMemoryMap(&MapSize,
                                                        MemoryMap,
                                                        &MapKey,
                                                        &DescriptorSize,
                                                        &DescriptorVersion);
    Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSize, &MemoryMap);
        ASSERT(MemoryMap != NULL); // FIXME Error Handling with graceful exit!
 
    Status = LocSystemTable->BootServices->GetMemoryMap(&MapSize,
                                                        MemoryMap,
                                                        &MapKey,
                                                        &DescriptorSize,
                                                        &DescriptorVersion);
    if (EFI_ERROR(Status))
    {
        printf("FUCK");
        LocSystemTable->BootServices->FreePool(MemoryMap);
        *MemoryMapSize = 0;
        return NULL;
    }
    UINTN Pages = 0;
    EntryCount = MapSize / DescriptorSize;
    Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*FreeldrMem) * EntryCount, &FreeldrMem);
    Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*BiosMemoryMap) * EntryCount, &BiosMemoryMap);
    if (EFI_ERROR(Status))
    {
        LocSystemTable->BootServices->FreePool(MemoryMap);
        *MemoryMapSize = 0;
        return NULL;
    }
    if (EFI_ERROR(Status))
    {
        LocSystemTable->BootServices->FreePool(MemoryMap);
        *MemoryMapSize = 0;
        return NULL;
    }

    EntryCount = MapSize / DescriptorSize;
    MapEntry = MemoryMap;
    Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*FreeldrMem) * EntryCount, &FreeldrMem);
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
                Pages += MapEntry->NumberOfPages;
                //printf("Free Memory: %X\r\n", MapEntry->NumberOfPages);
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount + 256,
                                                        (MapEntry->VirtualStart / EFI_PAGE_SIZE),
                                                        MapEntry->NumberOfPages,
                                                        LoaderFree);
                break;
            }
            case EfiLoaderCode:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount + 256,
                                                        (MapEntry->PhysicalStart / EFI_PAGE_SIZE),
                                                        MapEntry->NumberOfPages,
                                                        LoaderLoadedProgram);
                //printf("EfiLoaderCode: %X\r\n", MapEntry->NumberOfPages);
                //printf("EfiLoaderCode Physical Start: %X\r\n", MapEntry->PhysicalStart);
                  Pages += MapEntry->NumberOfPages;
                break;
            }
 
            default:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount + 256,
                                                        MapEntry->PhysicalStart / EFI_PAGE_SIZE,
                                                        MapEntry->NumberOfPages,
                                                        LoaderReserve);
            }
        }

        #if 0
        switch (MapEntry->Type)
        {
            //
            // Note: The following code is repetitive and can be simplified.
            //
            case EfiConventionalMemory:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount + 256,
                                                        (MapEntry->PhysicalStart / EFI_PAGE_SIZE),
                                                        MapEntry->NumberOfPages,
                                                        LoaderFree);
                //printf("EfiConventionalMemory: %X\r\n", MapEntry->NumberOfPages);
                //printf("EfiConventionalMemory Physical Start: %X\r\n", MapEntry->PhysicalStart);
                Pages += MapEntry->NumberOfPages;
                break;
            }
            case EfiLoaderCode:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount + 256,
                                                        (MapEntry->PhysicalStart / EFI_PAGE_SIZE),
                                                        MapEntry->NumberOfPages,
                                                        LoaderLoadedProgram);
                //printf("EfiLoaderCode: %X\r\n", MapEntry->NumberOfPages);
                //printf("EfiLoaderCode Physical Start: %X\r\n", MapEntry->PhysicalStart);
                  Pages += MapEntry->NumberOfPages;
                break;
            }
 
            default:
            {
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                        EntryCount + 256,
                                                        MapEntry->PhysicalStart / EFI_PAGE_SIZE,
                                                        MapEntry->NumberOfPages,
                                                        LoaderReserve);
                //printf("Other Memory: %X\r\n", MapEntry->NumberOfPages);
               // printf("Other Memory Physical Start: %X\r\n", MapEntry->PhysicalStart);
                  Pages += MapEntry->NumberOfPages;
                break;
            }
        }
        #endif
 
        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescriptorSize);
        //printf("MapEntry Address %X\r\n", MapEntry->PhysicalStart);
    }
    UefiConsSetCursor(0,0);
   //  printf("Pages before jump %X\r\n", Pages);
//
   // LocSystemTable->BootServices->FreePool(MemoryMap);
    *MemoryMapSize = FreeldrEntryCount; // PcMemFinalizeMemoryMap(FreeldrMem);
#endif
    UefiVideoClearScreen(0);
    //printf("Pages before jump %X\r\n", Pages);

    return FreeldrMem;
}

EFI_MEMORY_DESCRIPTOR* MemoryMapExit = NULL;
UINTN MapKeyExit;
VOID
UefiPrepareForReactOS()
{
    UefiVideoClearScreen(0);
    UefiConsSetCursor(0,0);
    EFI_STATUS status = 0;

    status = LocSystemTable->BootServices->GetMemoryMap(&MapSize,
                                                        MemoryMap,
                                                        &MapKeyExit,
                                                        &DescriptorSize,
                                                        &DescriptorVersion);
         printf("FUCK !Status: %X\r\n", status);
     status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapSize, &MemoryMapExit);
       printf("FUCK !Status: %X\r\n", status);
        status = LocSystemTable->BootServices->GetMemoryMap(&MapSize,
                                                        MemoryMap,
                                                        &MapKeyExit,
                                                        &DescriptorSize,
                                                        &DescriptorVersion);
          printf("FUCK !Status: %X\r\n", status);                     
    status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle, MapKeyExit);
       printf("FUCK !Status: %X\r\n", status);  
    status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle, MapKeyExit);
       printf("FUCK !Status: %X\r\n", status);  
    for(;;)
    {
      //  printf("FUCK !Status: %X\r\n", status);
    }
}