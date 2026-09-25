/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel virtual address layout for amd64
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

MI_SYSTEM_VA_ASSIGNMENT MiSystemVaRegions[AssignedRegionMaximum];

/* The system PTE lists in syspte.c store their offsets from here */
PVOID MiSystemPteBaseAddress;

static ULONG MiRandomSeed = 'MRnd';

/* One bit per PXE of the kernel half of the address space */
static ULONG MiSystemVaAssignment[8];
static RTL_BITMAP MiSystemVaAssignmentBitmap = { 256, MiSystemVaAssignment };

/* FUNCTIONS ******************************************************************/

CODE_SEG("INIT")
static
VOID
MiReserveVaRange(
    _In_ ULONG64 BaseAddress,
    _In_ ULONG64 NumberOfBytes)
{
    ULONG64 EndingAddress = BaseAddress + NumberOfBytes - 1;
    ULONG Pxi;

    for (Pxi = MiAddressToPxi((PVOID)BaseAddress);
         Pxi <= MiAddressToPxi((PVOID)EndingAddress);
         Pxi++)
    {
        ASSERT((Pxi - 256) < 256);
        RtlSetBit(&MiSystemVaAssignmentBitmap, Pxi - 256);
    }
}

CODE_SEG("INIT")
static
VOID
MiReserveVaRegion(
    _In_ MI_ASSIGNED_REGION_TYPES Region,
    _In_ ULONG64 BaseAddress,
    _In_ ULONG64 NumberOfBytes)
{
    MiReserveVaRange(BaseAddress, NumberOfBytes);
    MiSystemVaRegions[Region].BaseAddress = (PVOID)BaseAddress;
    MiSystemVaRegions[Region].NumberOfBytes = NumberOfBytes;
}

/**
 * @brief Takes a free run of PXEs, chosen at random.
 *
 * @param[in] NumberOfPxis
 * Number of consecutive PXEs needed.
 *
 * @return The first PXE index of the run.
 */
CODE_SEG("INIT")
static
ULONG
MiAcquireRandomPxiRange(
    _In_ ULONG NumberOfPxis)
{
    ULONG AvailableSlots = 0;
    ULONG SkipCount;
    ULONG Index = 0;
    ULONG i;

    for (i = 0; i < (256 - NumberOfPxis); i++)
    {
        if (RtlAreBitsClear(&MiSystemVaAssignmentBitmap, i, NumberOfPxis))
            AvailableSlots++;
    }

    /* The address space is far too small when this happens */
    if (AvailableSlots < 100)
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    /* Skip over unavailable slots and over SkipCount available ones */
    SkipCount = RtlRandomEx(&MiRandomSeed) % AvailableSlots;
    while (!RtlAreBitsClear(&MiSystemVaAssignmentBitmap, Index, NumberOfPxis) ||
           (SkipCount-- != 0))
    {
        Index++;
    }

    RtlSetBits(&MiSystemVaAssignmentBitmap, Index, NumberOfPxis);

    return Index + 256;
}

/**
 * @brief Gives a region a random address, using as few PXEs as it needs.
 *
 * @param[in] Region
 * Region to place.
 *
 * @param[in] NumberOfBytes
 * Size of the region.
 *
 * @param[in] Alignment
 * Alignment of the base address within the PXE range.
 */
CODE_SEG("INIT")
static
VOID
MiRandomizeVaRegion(
    _In_ MI_ASSIGNED_REGION_TYPES Region,
    _In_ ULONG64 NumberOfBytes,
    _In_ ULONG64 Alignment)
{
    ULONG64 FullSize = ALIGN_UP_BY(NumberOfBytes, PXE_MAPPED_VA);
    ULONG NumberOfPxis = (ULONG)(FullSize / PXE_MAPPED_VA);
    ULONG64 MaxOffset;
    ULONG AvailableSlots, SlotIndex, Pxi;

    ASSERT(NumberOfPxis != 0);
    ASSERT(Alignment >= PDE_MAPPED_VA);
    ASSERT(Alignment <= PXE_MAPPED_VA);
    ASSERT((Alignment & (Alignment - 1)) == 0);

    Pxi = MiAcquireRandomPxiRange(NumberOfPxis);

    /* Pick one of the aligned slots the region can sit at */
    MaxOffset = FullSize - NumberOfBytes;
    AvailableSlots = (ULONG)(1 + (MaxOffset / Alignment));
    SlotIndex = RtlRandomEx(&MiRandomSeed) % AvailableSlots;

    MiSystemVaRegions[Region].BaseAddress = Add2Ptr(MiPxiToAddress(Pxi), SlotIndex * Alignment);
    MiSystemVaRegions[Region].NumberOfBytes = NumberOfBytes;
}

CODE_SEG("INIT")
static
PFN_NUMBER
MiFindHighestPfnNumber(
    _In_ const LOADER_PARAMETER_BLOCK *LoaderBlock)
{
    PMEMORY_ALLOCATION_DESCRIPTOR Descriptor;
    PLIST_ENTRY ListEntry;
    PFN_NUMBER HighestPfn = 0;

    for (ListEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &LoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        Descriptor = CONTAINING_RECORD(ListEntry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        HighestPfn = max(HighestPfn, Descriptor->BasePage + Descriptor->PageCount);
    }

    return HighestPfn;
}

/**
 * @brief Assigns an address to every region of the kernel address space.
 *
 * @param[in] LoaderBlock
 * Loader block, it tells how much memory the machine has.
 *
 * @remarks Runs before the memory manager is initialized, on the boot processor only.
 */
CODE_SEG("INIT")
VOID
NTAPI
MiInitializeKernelVaLayout(
    _In_ const LOADER_PARAMETER_BLOCK *LoaderBlock)
{
    PFN_NUMBER HighestPfn;
    SIZE_T BootImageSize;

    /* Seed the generator with what the boot took, it differs between boots */
    if (LoaderBlock->Extension->LoaderPerformanceData != NULL)
    {
        MiRandomSeed ^= (ULONG)LoaderBlock->Extension->LoaderPerformanceData->StartTime;
        MiRandomSeed ^= _rotl((ULONG)LoaderBlock->Extension->LoaderPerformanceData->EndTime, 16);
    }
    MiRandomSeed ^= _rotl((ULONG)__rdtsc(), MiRandomSeed & 0x1F);

    /* These are at a fixed address, the page tables and hyperspace are hardcoded */
    MiReserveVaRange(KI_USER_SHARED_DATA, PAGE_SIZE);
    MiReserveVaRange(MM_HAL_VA_START, 4 * _1MB);
    MiReserveVaRange(MI_SYSTEM_CACHE_WS_START, PAGE_SIZE);
    MiReserveVaRegion(AssignedRegionPageTables, PTE_BASE, 512 * _1GB);
    MiReserveVaRegion(AssignedRegionHyperSpace, HYPER_SPACE, 512 * _1GB);
    MiReserveVaRegion(AssignedRegionSession, MI_SESSION_SPACE_START, 512 * _1GB);

    /* The loader mapped the boot images where it pleased */
    BootImageSize = LoaderBlock->Extension->LoaderPagesSpanned * PAGE_SIZE;
    MiReserveVaRegion(AssignedRegionSystemImages,
                      MI_LOADER_MAPPINGS,
                      BootImageSize + PAGE_SIZE);

    /* The PFN database only needs to describe the memory that is there */
    HighestPfn = MiFindHighestPfnNumber(LoaderBlock);
    MiRandomizeVaRegion(AssignedRegionPfnDatabase,
                        HighestPfn * sizeof(MMPFN) + _1MB,
                        PDE_MAPPED_VA);

    MiRandomizeVaRegion(AssignedRegionSystemCache, 2 * _1TB, 512 * _1GB);

    /* System view space is global, so it takes the top of the cache region instead of session space */
    MiSystemVaRegions[AssignedRegionSystemCache].NumberOfBytes -= MI_SYSTEM_VIEW_SIZE;

    MiRandomizeVaRegion(AssignedRegionPagedPool, 128 * _1GB, PDE_MAPPED_VA);

    /*
     * A system PTE holds the offset of the next free one as 32 bits of PTE index,
     * so the pools these lists describe have to share one window of 16 TB at most.
     * Non paged pool comes first, its expansion area is described by such a list.
     */
    MiRandomizeVaRegion(AssignedRegionNonPagedPool, 256 * _1GB, PDE_MAPPED_VA);
    MiSystemPteBaseAddress = MiSystemVaRegions[AssignedRegionNonPagedPool].BaseAddress;
    MiSystemVaRegions[AssignedRegionNonPagedPool].NumberOfBytes = 128 * _1GB;
    MiSystemVaRegions[AssignedRegionSystemPtes].BaseAddress =
        Add2Ptr(MiSystemPteBaseAddress, 128 * _1GB);
    MiSystemVaRegions[AssignedRegionSystemPtes].NumberOfBytes = 128 * _1GB;

    /* Stacks get whole page directories, the allocator hands out slots of them */
    MiRandomizeVaRegion(AssignedRegionKernelStacks, 128 * _1GB, PPE_MAPPED_VA);
}
