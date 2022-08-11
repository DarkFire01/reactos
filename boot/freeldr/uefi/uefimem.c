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
/**** Public Functions ***************************************************************************/

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
	PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
	EFI_MEMORY_DESCRIPTOR* MapEntry;
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	TYPE_OF_MEMORY FrldrMemoryType;
	EFI_STATUS Status = 0;
    UINT32 EntryCount = 0;
	UINT32 Index = 0;

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
                FrldrMemoryType = LoaderFree;
            }
            case EfiLoaderCode:
            {
               // FrldrMemoryType = LoaderLoadedProgram;
            }
            default:
            {										
				//FrldrMemoryType = LoaderReserve;
            }
        }
		
		FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                EntryCount,
                                                (MapOutput.EfiMemoryMap->VirtualStart / EFI_PAGE_SIZE),
                                                MapOutput.EfiMemoryMap->NumberOfPages,
                                                FrldrMemoryType);

		MapOutput.EfiMemoryMap = (EFI_MEMORY_DESCRIPTOR*)((char*)MapOutput.EfiMemoryMap + MapOutput.DescriptorSize);
	}

	printf("Exiting\r\n");
	return FreeldrMem;
}

VOID
UefiPrepareForReactOS()
{
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	EFI_STATUS Status = 0;
	MapOutput = PUEFI_LoadMemoryMap();
	UINTN MapKeyLoc = MapOutput.MapKey;
	Status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle,MapKeyLoc);

	if (EFI_ERROR(Status))
    {
        Status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle,MapKeyLoc);
    }
	if (Status != EFI_SUCCESS)
	{
		printf("Boot Services failed");
	}
}
