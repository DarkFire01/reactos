/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Mappings inside the HAL virtual address range
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* Implemented in hal/halarm64/generic/cache.s */
VOID
NTAPI
HalpFlushTbEntry(
    _In_ PVOID VirtualAddress);

VOID
NTAPI
HalpPublishPteWrites(VOID);

/* GLOBALS *******************************************************************/

/* One allocation can split a loader block in three */
#define HALP_MAX_ALLOC_DESCRIPTORS  64

static MEMORY_ALLOCATION_DESCRIPTOR HalpAllocDescriptors[HALP_MAX_ALLOC_DESCRIPTORS];
static ULONG HalpAllocDescriptorCount;

/* FUNCTIONS *****************************************************************/

/**
 * @brief Takes physical pages out of the loader's free memory for good.
 *
 * The pages are described as HAL memory in the loader block, so the memory
 * manager never hands them out.
 *
 * @param[in] MaxAddress
 * The pages must end below this address.
 *
 * @param[in] Aligned
 * TRUE to start the pages on a 64 KB boundary.
 *
 * @return The physical address of the first page, or 0 when nothing fits.
 */
CODE_SEG("INIT")
ULONG64
NTAPI
HalpAllocPhysicalMemory(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ ULONG64 MaxAddress,
    _In_ PFN_NUMBER PageCount,
    _In_ BOOLEAN Aligned)
{
    PMEMORY_ALLOCATION_DESCRIPTOR Block = NULL, Taken, Rest;
    PFN_NUMBER MaxPage = (PFN_NUMBER)(MaxAddress >> PAGE_SHIFT);
    PFN_NUMBER Skip = 0;
    PLIST_ENTRY Entry;

    if ((HalpAllocDescriptorCount + 2) > HALP_MAX_ALLOC_DESCRIPTORS)
        return 0;

    for (Entry = LoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &LoaderBlock->MemoryDescriptorListHead;
         Entry = Entry->Flink)
    {
        Block = CONTAINING_RECORD(Entry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);

        if ((Block->MemoryType != LoaderFree) && (Block->MemoryType != LoaderFirmwareTemporary))
            continue;

        /* Page 0 is never handed out */
        if (Block->BasePage == 0)
            continue;

        Skip = Aligned ? (ALIGN_UP_BY(Block->BasePage, 16) - Block->BasePage) : 0;
        if ((Block->PageCount >= PageCount + Skip) &&
            (Block->BasePage + Skip + PageCount < MaxPage))
        {
            break;
        }
    }

    if (Entry == &LoaderBlock->MemoryDescriptorListHead)
        return 0;

    Taken = &HalpAllocDescriptors[HalpAllocDescriptorCount++];
    Taken->MemoryType = LoaderHALCachedMemory;
    Taken->BasePage = Block->BasePage + Skip;
    Taken->PageCount = (PFN_COUNT)PageCount;

    if (Skip)
    {
        /* The pages after the allocation stay what the block was */
        if (Block->PageCount > PageCount + Skip)
        {
            Rest = &HalpAllocDescriptors[HalpAllocDescriptorCount++];
            Rest->MemoryType = Block->MemoryType;
            Rest->BasePage = Taken->BasePage + PageCount;
            Rest->PageCount = Block->PageCount - (PFN_COUNT)(Skip + PageCount);
            InsertHeadList(&Block->ListEntry, &Rest->ListEntry);
        }

        Block->PageCount = (PFN_COUNT)Skip;
        InsertHeadList(&Block->ListEntry, &Taken->ListEntry);
    }
    else
    {
        Block->BasePage += PageCount;
        Block->PageCount -= (PFN_COUNT)PageCount;

        /* The list stays sorted, the allocation goes in front of what is left */
        InsertTailList(&Block->ListEntry, &Taken->ListEntry);
        if (Block->PageCount == 0)
            RemoveEntryList(&Block->ListEntry);
    }

    return (ULONG64)Taken->BasePage << PAGE_SHIFT;
}

static
BOOLEAN
HalpIsRunFree(
    _In_ PMMPTE PointerPte,
    _In_ PFN_COUNT PageCount)
{
    PFN_COUNT i;

    for (i = 0; i < PageCount; i++)
    {
        if (PointerPte[i].u.Long != 0)
            return FALSE;
    }

    return TRUE;
}

/**
 * @brief Maps physical pages into the HAL address range.
 *
 * The loader already created the page tables for this range, so this works
 * before the memory manager is up.
 *
 * @param[in] CacheType
 * One of the MI_CACHE_TYPE_* slots the loader programmed into MAIR_EL1.
 */
static
PVOID
NTAPI
HalpMapPhysicalRange(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount,
    _In_ ULONG CacheType)
{
    PMMPTE FirstPte, LastPte, PointerPte;
    MMPTE TempPte;
    PFN_NUMBER Pfn;
    PFN_COUNT i;

    /* The offset inside the first page may need one more page */
    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(PhysicalAddress.LowPart,
                                               PageCount * PAGE_SIZE);

    FirstPte = MiAddressToPte(MM_HAL_VA_START);
    LastPte = MiAddressToPte(MM_HAL_VA_END);

    for (PointerPte = FirstPte; PointerPte + PageCount <= LastPte + 1; PointerPte++)
    {
        if (HalpIsRunFree(PointerPte, PageCount))
            break;
    }

    if (PointerPte + PageCount > LastPte + 1)
        return NULL;

    TempPte.u.Long = 0;
    TempPte.u.Hard.Valid = 1;
    TempPte.u.Hard.NotLargePage = 1;
    TempPte.u.Hard.Accessed = 1;
    TempPte.u.Hard.PrivilegedNoExecute = 1;
    TempPte.u.Hard.UserNoExecute = 1;
    TempPte.u.Hard.Writable = 1;
    TempPte.u.Hard.CacheType = CacheType;

    /* Normal memory is inner shareable, device memory carries no shareability */
    if (CacheType != MI_CACHE_TYPE_DEVICE)
        TempPte.u.Hard.Shareability = 3;

    Pfn = (PFN_NUMBER)(PhysicalAddress.QuadPart >> PAGE_SHIFT);
    for (i = 0; i < PageCount; i++)
    {
        TempPte.u.Hard.PageFrameNumber = Pfn + i;
        PointerPte[i] = TempPte;
    }

    /* New entries are never cached, the walker only has to see the stores */
    HalpPublishPteWrites();

    return (PVOID)((ULONG_PTR)MiPteToAddress(PointerPte) + BYTE_OFFSET(PhysicalAddress.LowPart));
}

PVOID
NTAPI
HalpMapPhysicalMemory(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount,
    _In_ BOOLEAN Device)
{
    return HalpMapPhysicalRange(PhysicalAddress,
                                PageCount,
                                Device ? MI_CACHE_TYPE_DEVICE : MI_CACHE_TYPE_CACHED);
}

/* Register windows of the debug device */
PVOID
NTAPI
HalpMapPhysicalMemory64(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount)
{
    return HalpMapPhysicalRange(PhysicalAddress, PageCount, MI_CACHE_TYPE_DEVICE);
}

/**
 * @brief Maps the block the debug device DMAs into.
 *
 * SoC controllers often do not snoop the caches, so the block is normal
 * non-cacheable memory. Device memory would fault on the unaligned copies the
 * debugger transports make into it.
 */
PVOID
NTAPI
HalpMapDebugMemory(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ PFN_COUNT PageCount)
{
    return HalpMapPhysicalRange(PhysicalAddress, PageCount, MI_CACHE_TYPE_WRITE_COMBINED);
}

VOID
NTAPI
HalpUnmapVirtualAddress(
    _In_ PVOID VirtualAddress,
    _In_ PFN_COUNT PageCount)
{
    PMMPTE PointerPte;
    ULONG_PTR Address;
    PFN_COUNT i;

    if ((ULONG_PTR)VirtualAddress < MM_HAL_VA_START)
        return;

    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(VirtualAddress, PageCount * PAGE_SIZE);
    Address = (ULONG_PTR)PAGE_ALIGN(VirtualAddress);
    PointerPte = MiAddressToPte(Address);

    for (i = 0; i < PageCount; i++)
    {
        PointerPte[i].u.Long = 0;
        HalpFlushTbEntry((PVOID)(Address + i * PAGE_SIZE));
    }
}
