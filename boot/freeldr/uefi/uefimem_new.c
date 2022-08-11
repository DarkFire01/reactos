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

extern EFI_SYSTEM_TABLE* LocSystemTable;
extern EFI_HANDLE LocImageHandle;
extern PFREELDR_MEMORY_DESCRIPTOR BiosMemoryMap;

ULONG FreeldrEntryCount = 0;

typedef struct _EFI_MEMORY_MAP_OUTPUT
{
	EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
	UINTN MapKey;
	UINTN DescriptorSize;
	UINTN MapSize;
	UINT32 DescriptorVersion;
} EFI_MEMORY_MAP_OUTPUT, *PEFI_MEMORY_MAP_OUTPUT;

/**** Private Functions **************************************************************************/

/*
 *	On a UEFI system we load the memory map several times over in order to get the updated mapkey
 *	This function is suppose to abstraction of this seqence ANY ALLOCATION REQUIRES USING 
 *	BOOTSERVICES REQUIRES THIS TO BE REFRESHED
 */
EFI_MEMORY_MAP_OUTPUT /* STATUS: UNCLEANED */
PUEFI_LoadMemoryMap()
{
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	EFI_STATUS Status;

    Status = LocSystemTable->BootServices->GetMemoryMap(MapOutput.MapSize,
                                                        MapOutput.MemoryMap,
                                                        MapOutput.MapKey,
                                                        MapOutput.DescriptorSize,
                                                        MapOutput.DescriptorVersion);

    /* Reallocate and retrieve again the needed memory map size (since memory
     * allocated by AllocatePool() counts in the map), until it's OK. */
    do
    {
        /* Reallocate the memory map buffer */
        if (MemoryMap)
            LocSystemTable->BootServices->FreePool(MapOutput.MemoryMap);
        LocSystemTable->BootServices->AllocatePool(EfiLoaderData, MapOutput.MapSize, &MapOutput.MemoryMap);
        ASSERT(MemoryMap != NULL); // FIXME Error Handling with graceful exit!

        Status = LocSystemTable->BootServices->GetMemoryMap(MapOutput.MapSize,
                                                        MapOutput.MemoryMap,
                                                        MapOutput.MapKey,
                                                        MapOutput.DescriptorSize,
                                                        MapOutput.DescriptorVersion);
    } while (Status == EFI_BUFFER_TOO_SMALL);
	printf("Mapoutput Type: %X\r\n", MapOutput.MemoryMap.Type);
	return MapOutput;
}

EFI_STATUS /* STATUS: UNIMPLEMENTED */
PUEFI_FinalizeMemoryMap()
{
	return EFI_SUCESS;
}
/**** Public Functions ***************************************************************************/

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
	EFI_MEMORY_MAP_OUTPUT MapOutput;
	
	MapOutput = PUEFI_LoadMemoryMap();
	
	
	return FreeldrMem;
}

VOID
UefiPrepareForReactOS()
{
	EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
	EFI_MEMORY_DESCRIPTOR* MapEntry;
	UINTN DescriptorSize;
	EFI_STATUS Status;
	UINTN MapSize;
	UINTN MapKey;

	Status = PUEFI_LoadMemoryMap(&MapKey, &DescriptorSize,
								 &MapSize, MemoryMap);
	
	if (EFI_ERROR(Status))
    {
        // ERROR HANDLING
    }

	Status = LocSystemTable->BootServices->ExitBootServices(LocImageHandle, MapKey);
	
	if (EFI_ERROR(Status))
    {
        // ERROR HANDLING
    }
	/* Go and activate paging! */
    UefiVideoClearScreen(0);
    UefiConsSetCursor(0,0);
}
