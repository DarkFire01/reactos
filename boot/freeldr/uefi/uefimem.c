/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Memory Managment Functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/**** Includes ***********************************************************************************/

#include <uefildr.h>

#include <debug.h>

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
EFI_MEMORY_MAP_OUTPUT
PUEFI_LoadMemoryMap()
{
    UINTN LocMapKey;
    UINTN LocMapSize;
    UINTN LocDescriptorSize;
    UINT32 LocDescriptorVersion;
    EFI_STATUS Status;
	EFI_MEMORY_MAP_OUTPUT MapOutput;

	MapOutput.EfiMemoryMap = NULL;
	MapOutput.MapSize = 0;
	MapOutput.MapKey = 0;
	MapOutput.DescriptorSize = 0;
	MapOutput.DescriptorVersion = 0;

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
                                            EntryCount,
                                            BasePage,
                                            PageCount,
                                            LoaderFree);
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
	for (Index = 0; Index < EntryCount; ++Index)
    {
        switch (MapEntry->Type)
        {
            case EfiConventionalMemory:
            {
                UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart / PAGE_SIZE), (MapEntry->NumberOfPages * MM_PAGE_SIZE), EntryCount, LoaderFree);
            }
            default:
            {
                UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart / PAGE_SIZE), (MapEntry->NumberOfPages * MM_PAGE_SIZE), EntryCount, LoaderReserve);
            }
        }
        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, MapOutput.DescriptorSize);
    }

    return FreeldrMem;
}
