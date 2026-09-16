/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel stack allocator for amd64
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* Each stack gets this many PTEs, guard page included */
#define MI_STACK_PAGES 32
#define MI_STACK_SIZE (MI_STACK_PAGES * PAGE_SIZE)

/* GLOBALS ********************************************************************/

/* Free list heads and PTE bases, one per level below the PXE */
static PMMPTE MiStackPteBaseByLevel[3];
static PMMPTE MiNextFreeStackPteByLevel[3];
static KSPIN_LOCK MiStackPteLock;

/* FUNCTIONS ******************************************************************/

/**
 * @brief Fills a page table of the stack region with a fresh page.
 */
static
NTSTATUS
MiMapStackPageTable(
    _In_ PMMPTE PointerPte)
{
    PFN_NUMBER PageFrameNumber;
    MMPTE TempPte;
    KIRQL OldIrql;

    ASSERT(PointerPte->u.Hard.Valid == 0);

    OldIrql = MiAcquirePfnLock();

    PageFrameNumber = MiRemoveZeroPage(MI_GET_NEXT_COLOR());
    if (PageFrameNumber == 0)
    {
        MiReleasePfnLock(OldIrql);
        return STATUS_NO_MEMORY;
    }

    MiInitializePfn(PageFrameNumber, PointerPte, TRUE);

    MiReleasePfnLock(OldIrql);

    MI_MAKE_HARDWARE_PTE(&TempPte,
                         PointerPte,
                         MM_EXECUTE_READWRITE,
                         PageFrameNumber);
    MI_WRITE_VALID_PTE(PointerPte, TempPte);

    return STATUS_SUCCESS;
}

/**
 * @brief Chains free PTEs of one level into its free list.
 *
 * @param[in] FirstPte
 * First PTE of the run.
 *
 * @param[in] NumberOfPtes
 * Number of PTEs in the run.
 *
 * @param[in] Level
 * 0 for the PTEs a stack is made of, 1 for PDEs, 2 for PPEs.
 */
static
VOID
MiInsertStackPtesInList(
    _In_ PMMPTE FirstPte,
    _In_ ULONG NumberOfPtes,
    _In_ ULONG Level)
{
    const ULONG PteDelta = (Level == 0) ? MI_STACK_PAGES : 1;
    const PMMPTE LastPte = FirstPte + NumberOfPtes - PteDelta;
    PMMPTE PointerPte;
    ULONG NextEntryOffset;

    ASSERT(Level < 3);
    ASSERT((Level == 2) || (ALIGN_DOWN_POINTER_BY(FirstPte, PAGE_SIZE) == FirstPte));

    LastPte->u.List.NextEntry = MM_EMPTY_PTE_LIST;
    NextEntryOffset = (ULONG)(LastPte - MiStackPteBaseByLevel[Level]);

    for (PointerPte = LastPte - PteDelta;
         PointerPte >= FirstPte;
         PointerPte -= PteDelta)
    {
        ASSERT(PointerPte->u.Long == 0);
        PointerPte->u.List.NextEntry = NextEntryOffset;
        NextEntryOffset -= PteDelta;
    }

    MiNextFreeStackPteByLevel[Level] = FirstPte;
}

/**
 * @brief Prepares the kernel stack region for handing out stacks.
 */
CODE_SEG("INIT")
VOID
NTAPI
MiInitializeStackAllocator(
    VOID)
{
    PVOID BaseAddress = MiSystemVaRegions[AssignedRegionKernelStacks].BaseAddress;
    SIZE_T SizeInBytes = MiSystemVaRegions[AssignedRegionKernelStacks].NumberOfBytes;

    ASSERT(ALIGN_DOWN_POINTER_BY(BaseAddress, PPE_MAPPED_VA) == BaseAddress);
    ASSERT(MiAddressToPxe(BaseAddress)->u.Hard.Valid);

    KeInitializeSpinLock(&MiStackPteLock);

    MiStackPteBaseByLevel[0] = MiAddressToPte(BaseAddress);
    MiStackPteBaseByLevel[1] = MiAddressToPde(BaseAddress);
    MiStackPteBaseByLevel[2] = MiAddressToPpe(BaseAddress);

    /* Only the page directories are known up front, page tables follow on demand */
    MiInsertStackPtesInList(MiAddressToPpe(BaseAddress),
                            (ULONG)(SizeInBytes / PPE_MAPPED_VA),
                            2);
}

/**
 * @brief Takes the next free entry of a level, growing the level above it when needed.
 */
static
PMMPTE
MiAllocateStackPtes(
    _In_ ULONG Level)
{
    PMMPTE PointerPte;
    PMMPDE PointerPde;
    ULONG NextPteOffset;
    NTSTATUS Status;

    /* The region has one page directory pointer table, it cannot grow past it */
    if (Level == 3)
        return NULL;

    PointerPte = MiNextFreeStackPteByLevel[Level];
    if (PointerPte == NULL)
    {
        PointerPde = MiAllocateStackPtes(Level + 1);
        if (PointerPde == NULL)
            return NULL;

        Status = MiMapStackPageTable(PointerPde);
        if (!NT_SUCCESS(Status))
            return NULL;

        PointerPte = MiPdeToPte(PointerPde);
        MiInsertStackPtesInList(PointerPte, PTE_PER_PAGE, Level);
    }

    NextPteOffset = PointerPte->u.List.NextEntry;
    if (NextPteOffset != MM_EMPTY_PTE_LIST)
    {
        MiNextFreeStackPteByLevel[Level] = MiStackPteBaseByLevel[Level] + NextPteOffset;
    }
    else
    {
        MiNextFreeStackPteByLevel[Level] = NULL;
    }

    return PointerPte;
}

/**
 * @brief Reserves the PTEs of one kernel stack.
 *
 * @param[in] NumberOfPtes
 * Number of PTEs the caller needs, guard page included.
 *
 * @return The first PTE of the stack slot, or NULL when the region is full.
 */
PMMPTE
NTAPI
MiReserveKernelStackPtes(
    _In_ ULONG NumberOfPtes)
{
    PMMPTE PointerPte;
    KIRQL OldIrql;

    /* Every stack gets a full slot, whatever it asks for */
    ASSERT(NumberOfPtes <= MI_STACK_PAGES);

    KeAcquireSpinLock(&MiStackPteLock, &OldIrql);
    PointerPte = MiAllocateStackPtes(0);
    KeReleaseSpinLock(&MiStackPteLock, OldIrql);

    return PointerPte;
}

/**
 * @brief Gives the PTEs of a kernel stack back.
 *
 * @param[in] FirstPte
 * First PTE of the slot, as returned by MiReserveKernelStackPtes.
 *
 * @param[in] NumberOfPtes
 * Number of PTEs the caller reserved.
 */
VOID
NTAPI
MiReleaseKernelStackPtes(
    _In_ PMMPTE FirstPte,
    _In_ ULONG NumberOfPtes)
{
    PMMPTE PointerPte;
    KIRQL OldIrql;

    ASSERT(NumberOfPtes <= MI_STACK_PAGES);
    ASSERT(ALIGN_DOWN_POINTER_BY(MiPteToAddress(FirstPte), MI_STACK_SIZE) == MiPteToAddress(FirstPte));

    RtlZeroMemory(FirstPte, MI_STACK_PAGES * sizeof(MMPTE));

    KeAcquireSpinLock(&MiStackPteLock, &OldIrql);

    PointerPte = MiNextFreeStackPteByLevel[0];
    if (PointerPte != NULL)
    {
        FirstPte->u.List.NextEntry = PointerPte - MiStackPteBaseByLevel[0];
    }
    else
    {
        FirstPte->u.List.NextEntry = MM_EMPTY_PTE_LIST;
    }

    MiNextFreeStackPteByLevel[0] = FirstPte;

    KeReleaseSpinLock(&MiStackPteLock, OldIrql);
}
