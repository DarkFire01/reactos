/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Memory Managment Functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/**** Includes ***********************************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);
/**** Global Variables ***************************************************************************/

#define NEXT_MEMORY_DESCRIPTOR(Descriptor, DescriptorSize) \
    (EFI_MEMORY_DESCRIPTOR*)((char*)(Descriptor) + (DescriptorSize))

ULONG FreeldrEntryCount = 0;

extern PFREELDR_MEMORY_DESCRIPTOR BiosMemoryMap;
extern PREACTOS_INTERNAL_BGCONTEXT refiFbData;
extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;
extern SIZE_T DiskReadBufferSize;

typedef struct _EFI_MEMORY_MAP_OUTPUT
{
	EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
	UINTN MapKey;
	UINT32 DescriptorSize;
	UINTN MapSize;
	UINT32 DescriptorVersion;
} EFI_MEMORY_MAP_OUTPUT, *PEFI_MEMORY_MAP_OUTPUT;


ULONG
AddMemoryDescriptor(
    IN OUT PFREELDR_MEMORY_DESCRIPTOR List,
    IN ULONG MaxCount,
    IN PFN_NUMBER BasePage,
    IN PFN_NUMBER PageCount,
    IN TYPE_OF_MEMORY MemoryType);

/**** Private Functions **************************************************************************/

/*
 *	On a UEFI system we load the memory map several times over in order to get the updated mapkey
 *	This function is suppose to be a abstraction of this seqence ANY ALLOCATION DONE USING
 *	BOOTSERVICES REQUIRES THIS TO BE REFRESHED
 */
    UINTN LocMapKey;
    UINTN LocMapSize;
    UINTN LocDescriptorSize;
    UINT32 LocDescriptorVersion;
EFI_MEMORY_MAP_OUTPUT
PUEFI_LoadMemoryMap()
{
    EFI_STATUS Status;
	EFI_MEMORY_MAP_OUTPUT MapOutput;

	MapOutput.EfiMemoryMap = NULL;
	MapOutput.MapSize = 0;
	MapOutput.MapKey = 0;
	MapOutput.DescriptorSize = 0;
	MapOutput.DescriptorVersion = 0;

    UefiVideoPutChar('C', 0xFF, 32, 32);

    Status = GlobalSystemTable->BootServices->GetMemoryMap(&LocMapSize,
                                                           MapOutput.EfiMemoryMap,
                                                           &LocMapKey,
                                                           &LocDescriptorSize,
                                                           &LocDescriptorVersion);

    /* Reallocate and retrieve again the needed memory map size (since memory
     * allocated by AllocatePool() counts in the map), until it's OK. */
    do
    {
        /* Reallocate the memory map buffer */
        if (MapOutput.EfiMemoryMap)
            GlobalSystemTable->BootServices->FreePool(MapOutput.EfiMemoryMap);

        GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, MapOutput.MapSize, (VOID**)&MapOutput.EfiMemoryMap);
        Status = GlobalSystemTable->BootServices->GetMemoryMap(&LocMapSize,
                                                        MapOutput.EfiMemoryMap,
                                                        &LocMapKey,
                                                        &LocDescriptorSize,
                                                        &LocDescriptorVersion);
    } while (Status == EFI_BUFFER_TOO_SMALL);

	MapOutput.MapSize = LocMapSize;
	MapOutput.MapKey = LocMapKey;
	MapOutput.DescriptorSize = LocDescriptorSize;
	MapOutput.DescriptorVersion = LocDescriptorVersion;

	return MapOutput;
}

VOID
UefiSetMemory(
    PFREELDR_MEMORY_DESCRIPTOR MemoryMap,
    ULONG_PTR BaseAddress,
    SIZE_T Size,
    UINT32 EntryCount,
    TYPE_OF_MEMORY MemoryType)
{
    ULONG_PTR BasePage, PageCount;

    BasePage = BaseAddress / EFI_PAGE_SIZE;
    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(BaseAddress, Size);

    /* Add the memory descriptor */
    FreeldrEntryCount = AddMemoryDescriptor(MemoryMap,
                                            100000,
                                            BasePage,
                                            PageCount,
                                            MemoryType);
}

/**** Public Functions ***************************************************************************/
VOID
UefiReserveMemory(
    PFREELDR_MEMORY_DESCRIPTOR MemoryMap,
    ULONG_PTR BaseAddress,
    SIZE_T Size,
    TYPE_OF_MEMORY MemoryType,
    PCHAR Usage)
{
    ULONG_PTR BasePage, PageCount;
    ULONG i;

    BasePage = BaseAddress / PAGE_SIZE;
    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(BaseAddress, Size);

    for (i = 0; i < FreeldrEntryCount; i++)
    {
        /* Check for conflicting descriptor */
        if ((MemoryMap[i].BasePage < BasePage + PageCount) &&
            (MemoryMap[i].BasePage + MemoryMap[i].PageCount > BasePage))
        {
            /* Check if the memory is free */
            if (MemoryMap[i].MemoryType != LoaderFree)
            {
                FrLdrBugCheckWithMessage(
                    MEMORY_INIT_FAILURE,
                    __FILE__,
                    __LINE__,
                    "Failed to reserve memory in the range 0x%Ix - 0x%Ix for %s",
                    BaseAddress,
                    Size,
                    Usage);
            }
        }
    }

    /* Add the memory descriptor */
    FreeldrEntryCount = AddMemoryDescriptor(MemoryMap,
                                     MAX_BIOS_DESCRIPTORS,
                                     BasePage,
                                     PageCount,
                                     MemoryType);
}

    EFI_MEMORY_DESCRIPTOR* MapEntry;
    EFI_MEMORY_DESCRIPTOR MapData;
	EFI_MEMORY_MAP_OUTPUT MapOutput;
      UINT32 EntryCount = 0;
PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
	PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
	EFI_STATUS Status = 0;
	UINT32 Index = 0;
    UINT32 DescCount = 0;

	MapOutput = PUEFI_LoadMemoryMap();
    if (MapOutput.EfiMemoryMap == NULL)
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
    }

	EntryCount = MapOutput.MapSize / MapOutput.DescriptorSize;
	Status = GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*FreeldrMem) * EntryCount, (void**)&FreeldrMem);
	Status = GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*BiosMemoryMap) * EntryCount, (void**)&BiosMemoryMap);
    if (Status != EFI_SUCCESS)
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
    }

    MapEntry = MapOutput.EfiMemoryMap;
    DescCount = EntryCount;
    UefiSetMemory(FreeldrMem, 0xFFFF, 0x5000 * EFI_PAGE_SIZE, 0, LoaderFree);

	for (Index = 0; Index < EntryCount; ++Index)
    {

        switch (MapEntry->Type)
        {

            case EfiConventionalMemory:
            {
                MapData = *(MapEntry);
                   DescCount += 100;
          //   UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart), (MapEntry->NumberOfPages * EFI_PAGE_SIZE), DescCount, LoaderFree);
#if 0
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                            1000,
                                            (MapData.PhysicalStart / PAGE_SIZE),
                                            MapData.NumberOfPages,
                                            LoaderFree);
                    #endif
            }
            case  EfiUnusableMemory:
            {

               // MapData = *(MapEntry);
                //UefiSetMemory(FreeldrMem, (MapData.PhysicalStart), (MapData.NumberOfPages * EFI_PAGE_SIZE), DescCount, LoaderBad);
                break;
            }
            case  EfiLoaderCode:
            {
              //  UefiReserveMemory(FreeldrMem, FREELDR_BASE, FrLdrImageSize, LoaderLoadedProgram, "FreeLdr image");

               // MapData = *(MapEntry);
                //UefiSetMemory(FreeldrMem, (MapData.PhysicalStart), (MapData.NumberOfPages * EFI_PAGE_SIZE), DescCount, LoaderLoadedProgram);
                break;
            }

            default:
            {
                        //       MapData = *(MapEntry);
               // UefiSetMemory(FreeldrMem, (MapData.PhysicalStart), (MapData.NumberOfPages * EFI_PAGE_SIZE), DescCount, LoaderReserve);
                break;
            }
        }
        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, MapOutput.DescriptorSize);
    }

    //UefiMemFinalizeMemoryMap(FreeldrMem);
    return FreeldrMem;
}

EFI_MEMORY_MAP_OUTPUT MapOutput;
UINTN MapKeyLoc;
EFI_STATUS Status;
VOID
UefiPrepareForReactOS(VOID)
{
    printf("UefiPrepareForReactOS: Exiting BootServices...\r");
	MapOutput = PUEFI_LoadMemoryMap();
	Status = 0;
	MapKeyLoc = MapOutput.MapKey;
	GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle,MapOutput.MapKey);
    /* UEFI spec demands twice! */
	if (Status != EFI_SUCCESS)
	{
		GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle,MapOutput.MapKey);
	}

    	if (Status != EFI_SUCCESS)
	{
	    TRACE("ExitBootServices failed");
        for(;;)
        {

        }
	}
	TRACE("ExitBootServices Sucessful");

}