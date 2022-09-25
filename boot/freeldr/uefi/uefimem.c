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
    (EFI_MEMORY_DESCRIPTOR*)((UINTN*)(Descriptor) + (DescriptorSize)

ULONG FreeldrEntryCount = 0;

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;
extern PFREELDR_MEMORY_DESCRIPTOR BiosMemoryMap;
extern SIZE_T DiskReadBufferSize;
extern PREACTOS_INTERNAL_BGCONTEXT refiFbData;

typedef struct _EFI_MEMORY_MAP_OUTPUT
{
	EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
	UINTN MapKey;
	UINT32 DescriptorSize;
	UINTN MapSize;
	UINT32 DescriptorVersion;
} EFI_MEMORY_MAP_OUTPUT, *PEFI_MEMORY_MAP_OUTPUT;

/**** Private Functions **************************************************************************/

ULONG
AddMemoryDescriptor(
    IN OUT PFREELDR_MEMORY_DESCRIPTOR List,
    IN ULONG MaxCount,
    IN PFN_NUMBER BasePage,
    IN PFN_NUMBER PageCount,
    IN TYPE_OF_MEMORY MemoryType);

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
    EFI_STATUS Status;
	UINTN LocDescriptorSize;
	UINT32 LocDescriptorVersion;
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

	for (Index = 0; Index < EntryCount; ++Index)
    {

		/* Let's translate a UEFI memory type to a NT memory type! */
        switch (MapOutput.EfiMemoryMap->Type)
        {
            case EfiConventionalMemory:
            {
               // UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderFree);
                FreeldrEntryCount = AddMemoryDescriptor(FreeldrMem,
                                                EntryCount * 80,
                                                (MapOutput.EfiMemoryMap->PhysicalStart / PAGE_SIZE),
                                                MapOutput.EfiMemoryMap->NumberOfPages,
                                                LoaderFree);
            }
            case EfiLoaderCode:
            {
                UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderLoadedProgram);
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
                UefiSetMemory(FreeldrMem, MapOutput.EfiMemoryMap->PhysicalStart, MapOutput.EfiMemoryMap->NumberOfPages, LoaderSpecialMemory);
            }
        }
        
		MapOutput.EfiMemoryMap = (EFI_MEMORY_DESCRIPTOR*)((char*)MapOutput.EfiMemoryMap + MapOutput.DescriptorSize);
	}

    UefiSetMemory(FreeldrMem, refiFbData->BaseAddress, refiFbData->BufferSize, LoaderSpecialMemory);
	UefiSetMemory(FreeldrMem, (ULONG_PTR)&refiFbData, sizeof(&refiFbData), LoaderFirmwarePermanent);

    return FreeldrMem;
}
