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
    (EFI_MEMORY_DESCRIPTOR*)((UINTN*)(Descriptor) + (DescriptorSize))

extern EFI_SYSTEM_TABLE* LocSystemTable;
extern EFI_HANDLE LocImageHandle;
extern PFREELDR_MEMORY_DESCRIPTOR BiosMemoryMap;
extern PVOID DiskReadBuffer;
extern SIZE_T DiskReadBufferSize;
extern PREACTOS_INTERNAL_BGCONTEXT refiFbData;
extern EFI_GRAPHICS_OUTPUT_PROTOCOL* Locgop;
ULONG FreeldrEntryCount = 0;

typedef struct _EFI_MEMORY_MAP_OUTPUT
{
	EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
	UINTN MapKey;
	UINT32 DescriptorSize;
	UINTN MapSize;
	UINT32 DescriptorVersion;
} EFI_MEMORY_MAP_OUTPUT, *PEFI_MEMORY_MAP_OUTPUT;


/**** Private Functions **************************************************************************/

/*
 *	On a UEFI system we load the memory map several times over in order to get the updated mapkey
 *	This function is suppose to abstraction of this seqence ANY ALLOCATION REQUIRES USING 
 *	BOOTSERVICES REQUIRES THIS TO BE REFRESHED
 */
 	UINTN LocMapSize;
	UINTN LocMapKey;
	UINTN LocDescriptorSize;
	UINT32 LocDescriptorVersion;
EFI_MEMORY_MAP_OUTPUT /* STATUS: UNCLEANED */
PUEFI_LoadMemoryMap()
{
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	EFI_STATUS Status;
	
	MapOutput.EfiMemoryMap = NULL;
	MapOutput.MapSize = 0;
	MapOutput.MapKey = 0;
	MapOutput.DescriptorSize = 0;
	MapOutput.DescriptorVersion = 0;

    Status = LocSystemTable->BootServices->GetMemoryMap(&LocMapSize,
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
            LocSystemTable->BootServices->FreePool(MapOutput.EfiMemoryMap);
        LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapOutput.MapSize, &MapOutput.EfiMemoryMap);
        ASSERT(MapOutput.EfiMemoryMap != NULL); // FIXME Error Handling with graceful exit!
        Status = LocSystemTable->BootServices->GetMemoryMap(&LocMapSize,
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

EFI_STATUS /* STATUS: UNIMPLEMENTED */
PUEFI_FinalizeMemoryMap()
{
	return 0;
}

VOID
UefiSetMemory(
    PFREELDR_MEMORY_DESCRIPTOR MemoryMap,
    ULONG_PTR BaseAddress,
    SIZE_T Size,
    TYPE_OF_MEMORY MemoryType)
{
    ULONG_PTR BasePage, PageCount;

    BasePage = BaseAddress / PAGE_SIZE;
    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(BaseAddress, Size);

    /* Add the memory descriptor */
    FreeldrEntryCount = AddMemoryDescriptor(MemoryMap,
                                     MAX_BIOS_DESCRIPTORS * 21,
                                     BasePage,
                                     PageCount,
                                     MemoryType);
}

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
               // printf("Fuck");
            }
        }
    }

        /* Add the memory descriptor */
    FreeldrEntryCount = AddMemoryDescriptor(MemoryMap,
                                     MAX_BIOS_DESCRIPTORS * 20,
                                     BasePage,
                                     PageCount,
                                     MemoryType);
}

/**** Public Functions ***************************************************************************/

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
	PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
	EFI_MEMORY_DESCRIPTOR* MapEntry;
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	EFI_STATUS Status = 0;
    UINT32 EntryCount = 0;
	UINT32 Index = 0;
    //ULONG_PTR FreeldrPtr;
	MapOutput = PUEFI_LoadMemoryMap();
	MapEntry = MapOutput.EfiMemoryMap;
	EntryCount = MapOutput.MapSize / MapOutput.DescriptorSize;
	
	Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*FreeldrMem) * EntryCount, &FreeldrMem);
	Status = LocSystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(*BiosMemoryMap) * EntryCount, &BiosMemoryMap);

	for (Index = 0; Index < EntryCount; ++Index)
    {

		/* Let's translate a UEFI memory type to a NT memory type! */
        switch (MapOutput.EfiMemoryMap->Type)
        {
            case EfiConventionalMemory:
            {
               // UefiReserveMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderFree, "FREE");
                #if 1
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                EntryCount * 80,
                                                (MapOutput.EfiMemoryMap->PhysicalStart / PAGE_SIZE),
                                                MapOutput.EfiMemoryMap->NumberOfPages,
                                                LoaderFree);
                #endif
            }
			case EfiUnusableMemory:
            {                
               // UefiReserveMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages * EFI_PAGE_SIZE, LoaderBad, "Dead");

            }
            case EfiLoaderCode:
            {
                UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderLoadedProgram);
              //  FreeldrPtr = MapOutput.EfiMemoryMap->PhysicalStart;
            }
            case EfiACPIReclaimMemory:
            {    
                UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderFirmwareTemporary);

            }
            case EfiACPIMemoryNVS:
            {
                UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderFirmwarePermanent);

            }
            case  EfiMemoryMappedIOPortSpace:
            {
               UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderFirmwarePermanent);

            }
            case EfiMemoryMappedIO:
            {
               UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderSpecialMemory);

            }
			case EfiPalCode:
            {
              // UefiReserveMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderSpecialMemory, "FreeLdr image");
            }
            default:
            {										
			   // UefiReserveMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderReserve, "Misc");

            }
        }
		MapOutput.EfiMemoryMap = (EFI_MEMORY_DESCRIPTOR*)((char*)MapOutput.EfiMemoryMap + MapOutput.DescriptorSize);
	}

     UefiSetMemory(FreeldrMem, refiFbData->BaseAddress, refiFbData->BufferSize, LoaderSpecialMemory);
	 UefiSetMemory(FreeldrMem, (ULONG_PTR)&refiFbData, sizeof(&refiFbData), LoaderFirmwarePermanent);
	UefiReserveMemory(FreeldrMem, STACKLOW, STACKADDR - STACKLOW, LoaderOsloaderStack, "FreeLdr stack");
   // UefiReserveMemory(FreeldrMem, STACKLOW, STACKADDR - STACKLOW, LoaderOsloaderStack, "FreeLdr stack");
//   UefiPrepareForReactOS();

	return FreeldrMem;
}

VOID
UefiPrepareForReactOS()
{
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	EFI_STATUS Status = 0;
	MapOutput = PUEFI_LoadMemoryMap();
	printf("Finished");
	UINTN MapKeyLoc = MapOutput.MapKey;
	Status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle,MapKeyLoc);
    //UefiConsSetCursor(0,0);
   // UefiVideoClearScreen(0);
      Status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle,MapKeyLoc);

	if (Status != EFI_SUCCESS)
	{
		printf("Failed: %X\r\n", Status);

	}
}
