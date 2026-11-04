/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Memory Management Functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

#define NEXT_MEMORY_DESCRIPTOR(Descriptor, DescriptorSize) \
    (EFI_MEMORY_DESCRIPTOR*)((char*)(Descriptor) + (DescriptorSize))
#define EXIT_STACK_SIZE 0x1000

/*
 * Room on top of the firmware's own entries for the ranges set aside by hand
 * below, the terminator, and the one descriptor each of them can split an
 * existing range into.
 */
#define SPARE_DESCRIPTOR_COUNT 8

ULONG
AddMemoryDescriptor(
    _Inout_ PFREELDR_MEMORY_DESCRIPTOR List,
    _In_ ULONG MaxCount,
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount,
    _In_ TYPE_OF_MEMORY MemoryType);

/* GLOBALS *******************************************************************/

extern EFI_SYSTEM_TABLE* GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

EFI_MEMORY_DESCRIPTOR* EfiMemoryMap = NULL;
UINT32 FreeldrDescCount;
static ULONG FreeldrDescMax;
PVOID OsLoaderBase;
SIZE_T OsLoaderSize;
EFI_HANDLE PublicBootHandle;
PVOID ExitStack;
PVOID EndofExitStack;

/* The memory map ExitBootServices took, SetVirtualAddressMap wants that one */
static UINTN UefiFinalMapSize;
static UINTN UefiFinalDescriptorSize;
static UINT32 UefiFinalDescriptorVersion;

void _exituefi(VOID);

/* FUNCTIONS *****************************************************************/

static
VOID
PUEFI_LoadMemoryMap(
    _Out_ UINTN*  LocMapKey,
    _Out_ UINTN*  LocMapSize,
    _Out_ UINTN*  LocDescriptorSize,
    _Out_ UINT32* LocDescriptorVersion)
{
    EFI_STATUS Status;
    UINTN AllocationSize = 0;
    ULONG Count = 0;

    Status = GlobalSystemTable->BootServices->GetMemoryMap(LocMapSize,
                                                           EfiMemoryMap,
                                                           LocMapKey,
                                                           LocDescriptorSize,
                                                           LocDescriptorVersion);

    /* Reallocate and retrieve again the needed memory map size (since memory
     * allocated by AllocatePool() counts in the map), until it's OK. */
    while (Status != EFI_SUCCESS)
    {
        /* Reallocate the memory map buffer */
        if (EfiMemoryMap)
            GlobalSystemTable->BootServices->FreePool(EfiMemoryMap);

        /* If MapSize never reports the correct size after the first time, increment */
        AllocationSize = *LocMapSize + (*LocDescriptorSize * Count);
        GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, AllocationSize,
                                                      (VOID**)&EfiMemoryMap);
        Status = GlobalSystemTable->BootServices->GetMemoryMap(LocMapSize,
                                                               EfiMemoryMap,
                                                               LocMapKey,
                                                               LocDescriptorSize,
                                                               LocDescriptorVersion);
        Count++;
    }
}

static
VOID
UefiSetMemory(
    _Inout_ PFREELDR_MEMORY_DESCRIPTOR MemoryMap,
    _In_ ULONG_PTR BaseAddress,
    _In_ PFN_COUNT SizeInPages,
    _In_ TYPE_OF_MEMORY MemoryType)
{
    ULONG_PTR BasePage, PageCount;

    BasePage = BaseAddress / EFI_PAGE_SIZE;
    PageCount = SizeInPages;

    /* Add the memory descriptor */
    FreeldrDescCount = AddMemoryDescriptor(MemoryMap,
                                           FreeldrDescMax,
                                           BasePage,
                                           PageCount,
                                           MemoryType);
}

/**
 * @brief
 * Sets the pages holding the SMBIOS structures aside.
 *
 * @param[in,out] MemoryMap
 * The map to set them aside in.
 *
 * @remarks
 * The firmware is free to keep the structures in memory it gives up along
 * with boot services, and the address the entry point carries is the only
 * way the kernel has of reaching them, so those pages must outlive us.
 */
static
VOID
UefiReserveSmbiosTable(
    _Inout_ PFREELDR_MEMORY_DESCRIPTOR MemoryMap)
{
    ULONGLONG TableAddress, LastPage, BasePage;
    PVOID EntryPoint;
    ULONG TableLength;

    EntryPoint = UefiGetSmbiosEntryPoint(NULL);
    if (EntryPoint == NULL)
        return;

    if (!UefiGetSmbiosTableRange(EntryPoint, &TableAddress, &TableLength))
        return;

    /* A table this loader cannot address is one it cannot hand over either */
    if ((TableAddress + TableLength - 1) > (ULONGLONG)(ULONG_PTR)-1)
    {
        WARN("SMBIOS structures at %I64x are out of reach\n", TableAddress);
        return;
    }

    BasePage = TableAddress / EFI_PAGE_SIZE;
    LastPage = (TableAddress + TableLength - 1) / EFI_PAGE_SIZE;

    TRACE("Reserving SMBIOS structures at %I64x, %lu bytes\n", TableAddress, TableLength);
    UefiSetMemory(MemoryMap,
                  (ULONG_PTR)(BasePage * EFI_PAGE_SIZE),
                  (PFN_COUNT)(LastPage - BasePage + 1),
                  LoaderSpecialMemory);
}

static
TYPE_OF_MEMORY
UefiConvertToFreeldrDesc(EFI_MEMORY_TYPE EfiMemoryType)
{
    switch (EfiMemoryType)
    {
        case EfiReservedMemoryType:
            return LoaderReserve;
        case EfiLoaderCode:
            return LoaderLoadedProgram;
        case EfiLoaderData:
            return LoaderLoadedProgram;
        case EfiBootServicesCode:
            return LoaderFirmwareTemporary;
        case EfiBootServicesData:
            return LoaderFirmwareTemporary;
        case EfiRuntimeServicesCode:
            return LoaderFirmwarePermanent;
        case EfiRuntimeServicesData:
            return LoaderFirmwarePermanent;
        case EfiConventionalMemory:
            return LoaderFree;
        case EfiUnusableMemory:
            return LoaderBad;
        case EfiACPIReclaimMemory:
            return LoaderSpecialMemory;
        case EfiACPIMemoryNVS:
            return LoaderSpecialMemory;
        case EfiMemoryMappedIO:
            return LoaderReserve;
        case EfiMemoryMappedIOPortSpace:
            return LoaderReserve;
        default:
            break;
    }
    return LoaderReserve;
}

PFREELDR_MEMORY_DESCRIPTOR
UefiMemGetMemoryMap(ULONG *MemoryMapSize)
{
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    UINT32 DescriptorVersion;
    SIZE_T FreeldrMemMapSize;
    UINTN DescriptorSize;
    EFI_STATUS Status;
    UINTN MapSize;
    UINTN MapKey;
    UINT32 Index;

    EFI_GUID EfiLoadedImageProtocol = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    PFREELDR_MEMORY_DESCRIPTOR FreeldrMem = NULL;
    EFI_MEMORY_DESCRIPTOR* MapEntry = NULL;
    UINT32 EntryCount = 0;
    FreeldrDescCount = 0;

    Status = GlobalSystemTable->BootServices->HandleProtocol(GlobalImageHandle,
                                                             &EfiLoadedImageProtocol,
                                                             (VOID**)&LoadedImage);
    if (Status != EFI_SUCCESS)
    {
        TRACE("Failed to find LoadedImageHandle with status: %d\n", Status);
        UiMessageBoxCritical("Unable to initialize memory manager.");
        return NULL;
    }
    OsLoaderBase = LoadedImage->ImageBase;
    OsLoaderSize = LoadedImage->ImageSize;
    PublicBootHandle = LoadedImage->DeviceHandle;
    EfiMemoryMap = NULL;

    TRACE("UefiMemGetMemoryMap: Gather memory map\n");
    PUEFI_LoadMemoryMap(&MapKey,
                        &MapSize,
                        &DescriptorSize,
                        &DescriptorVersion);

    TRACE("Value of MapKey: %d\n", MapKey);
    TRACE("Value of MapSize: %d\n", MapSize);
    TRACE("Value of DescriptorSize: %d\n", DescriptorSize);
    TRACE("Value of DescriptorVersion: %d\n", DescriptorVersion);

    EntryCount = (MapSize / DescriptorSize);

    /* One entry past the most we allow, so the terminator always has a home */
    FreeldrDescMax = EntryCount + SPARE_DESCRIPTOR_COUNT;
    FreeldrMemMapSize = (sizeof(FREELDR_MEMORY_DESCRIPTOR) * (FreeldrDescMax + 1));
    Status = GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData,
                                                           FreeldrMemMapSize,
                                                           (void**)&FreeldrMem);
    if (Status != EFI_SUCCESS)
    {
        TRACE("Failed to allocate pool with status %d\n", Status);
        UiMessageBoxCritical("Unable to initialize memory manager.");
        return NULL;
    }

    RtlZeroMemory(FreeldrMem, FreeldrMemMapSize);
    MapEntry = EfiMemoryMap;
	for (Index = 0; Index < EntryCount; ++Index)
    {
        TYPE_OF_MEMORY MemoryType = UefiConvertToFreeldrDesc(MapEntry->Type);
        if (MemoryType == LoaderFree)
        {
            Status = GlobalSystemTable->BootServices->AllocatePages(AllocateAddress,
                                                                    EfiLoaderData,
                                                                    MapEntry->NumberOfPages,
                                                                    &MapEntry->PhysicalStart);
            if (Status != EFI_SUCCESS)
            {
                /* We failed to reserve the page, so change its type */
                MemoryType = LoaderFirmwareTemporary;
            }
        }

        /* We really don't want to touch these reserved spots at all */
        if (MemoryType != LoaderReserve)
        {
            UefiSetMemory(FreeldrMem,
                          MapEntry->PhysicalStart,
                          MapEntry->NumberOfPages,
                          MemoryType);
        }

        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescriptorSize);
    }

    /* Windows expects the first page to be reserved, otherwise it asserts.
     * However it can be just a free page on some UEFI systems. */
    UefiSetMemory(FreeldrMem, 0x000000, 1, LoaderFirmwarePermanent);

    /* Keep the firmware tables the kernel still needs */
    UefiReserveSmbiosTable(FreeldrMem);

    *MemoryMapSize = FreeldrDescCount;
    return FreeldrMem;
}

VOID
UefiExitBootServices(VOID)
{
    UINTN MapKey;
    UINTN MapSize;
    EFI_STATUS Status;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;

    TRACE("Attempting to exit bootsevices\n");
    PUEFI_LoadMemoryMap(&MapKey,
                        &MapSize,
                        &DescriptorSize,
                        &DescriptorVersion);

    Status = GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle, MapKey);
    /* UEFI spec demands twice! */
    if (Status != EFI_SUCCESS)
        Status = GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle, MapKey);

    UefiFinalMapSize = MapSize;
    UefiFinalDescriptorSize = DescriptorSize;
    UefiFinalDescriptorVersion = DescriptorVersion;

    if (Status != EFI_SUCCESS)
    {
        TRACE("Failed to exit boot services with status: %d\n", Status);
        FrLdrBugCheckWithMessage(EXIT_BOOTSERVICES_FAILURE,
                                 __FILE__,
                                 __LINE__,
                                 "Failed to exit boot services with status: %d",
                                 Status);
    }
    else
    {
        TRACE("Exited bootservices\n");
    }
}

/**
 * @brief Moves the UEFI runtime services to the addresses the kernel calls them at.
 *
 * Runs after ExitBootServices, still on the firmware's identity mapping.
 * The runtime regions are packed one after another from VirtualBase.
 *
 * @param[in] VirtualBase
 * Start of the window the runtime regions are placed in.
 *
 * @param[in] MapPage
 * Maps one page in the kernel tables, Cached is FALSE for device memory.
 *
 * @param[out] Services
 * Receives the virtual entry points of the runtime services.
 *
 * @return
 * The SetVirtualAddressMap status, the services are unusable unless it is 0.
 */
ULONG_PTR
UefiSetRuntimeVirtualMap(
    _In_ ULONG64 VirtualBase,
    _In_ BOOLEAN (*MapPage)(_In_ ULONG64 VirtualAddress, _In_ ULONG64 PhysicalAddress, _In_ BOOLEAN Cached),
    _Out_ PVIRTUAL_EFI_RUNTIME_SERVICES Services)
{
    EFI_RUNTIME_SERVICES *Runtime = GlobalSystemTable->RuntimeServices;
    EFI_MEMORY_DESCRIPTOR *Descriptor;
    ULONG64 Virtual = VirtualBase;
    UINT64 Page;
    UINTN Offset;
    EFI_STATUS Status;

    RtlZeroMemory(Services, sizeof(*Services));
    if (!Runtime || !EfiMemoryMap || !UefiFinalDescriptorSize)
        return EFI_UNSUPPORTED;

    for (Offset = 0; Offset < UefiFinalMapSize; Offset += UefiFinalDescriptorSize)
    {
        Descriptor = (EFI_MEMORY_DESCRIPTOR *)((PUCHAR)EfiMemoryMap + Offset);
        if (!(Descriptor->Attribute & EFI_MEMORY_RUNTIME))
            continue;

        Descriptor->VirtualStart = Virtual;
        for (Page = 0; Page < Descriptor->NumberOfPages; Page++)
        {
            if (!MapPage(Virtual + Page * EFI_PAGE_SIZE,
                         Descriptor->PhysicalStart + Page * EFI_PAGE_SIZE,
                         (Descriptor->Attribute & EFI_MEMORY_WB) != 0))
            {
                return EFI_OUT_OF_RESOURCES;
            }
        }
        Virtual += Descriptor->NumberOfPages * EFI_PAGE_SIZE;
    }

    Status = Runtime->SetVirtualAddressMap(UefiFinalMapSize,
                                           UefiFinalDescriptorSize,
                                           UefiFinalDescriptorVersion,
                                           EfiMemoryMap);
    if (EFI_ERROR(Status))
        return Status;

    /* The firmware rewrote the table entries, they are virtual from here on */
    Services->GetTime = (ULONG_PTR)Runtime->GetTime;
    Services->SetTime = (ULONG_PTR)Runtime->SetTime;
    Services->GetWakeupTime = (ULONG_PTR)Runtime->GetWakeupTime;
    Services->SetWakeupTime = (ULONG_PTR)Runtime->SetWakeupTime;
    Services->SetVirtualAddressMap = (ULONG_PTR)Runtime->SetVirtualAddressMap;
    Services->ConvertPointer = (ULONG_PTR)Runtime->ConvertPointer;
    Services->GetVariable = (ULONG_PTR)Runtime->GetVariable;
    Services->GetNextVariableName = (ULONG_PTR)Runtime->GetNextVariableName;
    Services->SetVariable = (ULONG_PTR)Runtime->SetVariable;
    Services->GetNextHighMonotonicCount = (ULONG_PTR)Runtime->GetNextHighMonotonicCount;
    Services->ResetSystem = (ULONG_PTR)Runtime->ResetSystem;
    Services->UpdateCapsule = (ULONG_PTR)Runtime->UpdateCapsule;
    Services->QueryCapsuleCapabilities = (ULONG_PTR)Runtime->QueryCapsuleCapabilities;
    Services->QueryVariableInfo = (ULONG_PTR)Runtime->QueryVariableInfo;
    return EFI_SUCCESS;
}

VOID
UefiPrepareForReactOS(VOID)
{
    _exituefi();
}
