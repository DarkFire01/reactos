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

#define NEXT_MEMORY_DESCRIPTOR(Descriptor, DescriptorSize) \
    (EFI_MEMORY_DESCRIPTOR*)((char*)(Descriptor) + (DescriptorSize))

ULONG
AddMemoryDescriptor(
    IN OUT PFREELDR_MEMORY_DESCRIPTOR List,
    IN ULONG MaxCount,
    IN PFN_NUMBER BasePage,
    IN PFN_NUMBER PageCount,
    IN TYPE_OF_MEMORY MemoryType);

/**** Global Variables ***************************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

UINT32 FreeldrDescCount;
EFI_MEMORY_DESCRIPTOR* EfiMemoryMap = NULL;
/**** Private Functions **************************************************************************/
VOID
PUEFI_LoadMemoryMap(UINTN* LocMapKey,
                    UINTN* LocMapSize,
                    UINTN* LocDescriptorSize,
                    UINT32* LocDescriptorVersion)
{
    EFI_STATUS Status;
    Status = GlobalSystemTable->BootServices->GetMemoryMap(LocMapSize,
                                                           EfiMemoryMap,
                                                           LocMapKey,
                                                           LocDescriptorSize,
                                                           LocDescriptorVersion);

    /* Reallocate and retrieve again the needed memory map size (since memory
     * allocated by AllocatePool() counts in the map), until it's OK. */
    do
    {
        /* Reallocate the memory map buffer */
        if (EfiMemoryMap)
            GlobalSystemTable->BootServices->FreePool(EfiMemoryMap);

        GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, *LocMapSize, (VOID**)&EfiMemoryMap);
        Status = GlobalSystemTable->BootServices->GetMemoryMap(LocMapSize,
                                                               EfiMemoryMap,
                                                               LocMapKey,
                                                               LocDescriptorSize,
                                                               LocDescriptorVersion);
                                                                   TRACE("EntryCount %d\n", *LocDescriptorSize);
    } while (Status == EFI_BUFFER_TOO_SMALL);

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

    BasePage = BaseAddress;
    PageCount = Size;

    /* Add the memory descriptor */
    FreeldrDescCount = AddMemoryDescriptor(MemoryMap,
                                            100000,
                                            BasePage,
                                            PageCount,
                                            MemoryType);
}

/**** Public Functions ***************************************************************************/
UINT32 DescriptorVersion;
UINTN MapKey;
SIZE_T FreeldrMemMapSize;
UINTN DescriptorSize;
EFI_STATUS Status;
UINT32 Index;
UINTN MapSize;
EFI_MEMORY_DESCRIPTOR* MapEntry = NULL;
PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
    UINT32 EntryCount = 1;
    FreeldrDescCount = 0;

    TRACE("UefiMemGetMemoryMap\n");

    PUEFI_LoadMemoryMap( &MapKey,
                         &MapSize,
                         &DescriptorSize,
                         &DescriptorVersion);
    EntryCount = MapSize / DescriptorSize;
    FreeldrMemMapSize = (sizeof(FREELDR_MEMORY_DESCRIPTOR) * EntryCount * 8);

    /* Create a pool of memory based on FreeldrMap */
	Status = GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, FreeldrMemMapSize, (void**)&FreeldrMem);
    RtlZeroMemory(FreeldrMem, FreeldrMemMapSize);
    if (Status != EFI_SUCCESS)
    {
        UiMessageBoxCritical("Unable to initialize memory manager.");
    }

    MapEntry = EfiMemoryMap;
	for (Index = 0; Index < EntryCount;     ++Index)
    {
        switch (MapEntry->Type)
        {
            case EfiConventionalMemory:
            {
                UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart / EFI_PAGE_SIZE), MapEntry->NumberOfPages, 1000, LoaderFree);
                break;
            }
            case EfiACPIMemoryNVS:
            {
                //UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart / EFI_PAGE_SIZE), (MapEntry->NumberOfPages), 1000, LoaderFirmwareTemporary);
                break;
            }
            case EfiLoaderCode:
             {
                UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart / EFI_PAGE_SIZE), (MapEntry->NumberOfPages), 1000, LoaderLoadedProgram);
                break;
            }
            case EfiLoaderData:
            {
                UefiSetMemory(FreeldrMem, (MapEntry->PhysicalStart / EFI_PAGE_SIZE), (MapEntry->NumberOfPages), 1000, LoaderLoadedProgram);
                break;
            }
            case EfiReservedMemoryType:
            case EfiMemoryMappedIOPortSpace:
            case EfiMemoryMappedIO:
            {
                break;
            }
            case  EfiUnusableMemory:
            case  EfiPalCode:
            default:
            {
                break;
            }
        }
        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescriptorSize);
    }
    *MemoryMapSize = FreeldrDescCount;
    return FreeldrMem;
}

VOID
UefiPrepareForReactOS(VOID)
{

    printf("UefiPrepareForReactOS: Exiting BootServices...\r");
	PUEFI_LoadMemoryMap(&MapKey,
                         &MapSize,
                         &DescriptorSize,
                         &DescriptorVersion);
	Status = 0;
	Status = GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle,MapKey);
    /* UEFI spec demands twice! */
	if (Status != EFI_SUCCESS)
	{
		Status = GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle,MapKey);
	}

    	if (Status != EFI_SUCCESS)
	{
	    TRACE("ExitBootServices failed");
        for(;;)
        {

        }
	}
	TRACE("ExitBootServices Sucessful");

    /* Disable Interrupts */
//    _disable();

    /* Re-initialize EFLAGS */
   // __writeeflags(0);

    /* Set the PDBR */
  //  __writecr3((ULONG_PTR)PDE);

    /* Enable paging by modifying CR0 */
    //__writecr0(__readcr0() | ~CR0_PG);
}
