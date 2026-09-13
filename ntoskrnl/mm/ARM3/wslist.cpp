/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD-3-Clause (https://spdx.org/licenses/BSD-3-Clause)
 * FILE:            ntoskrnl/mm/ARM3/wslist.cpp
 * PURPOSE:         Working set list management
 * PROGRAMMERS:     Jérôme Gardou
 *                  Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/
#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "miarm.h"

/* GLOBALS ********************************************************************/
PMMWSL MmWorkingSetList;
KEVENT MmWorkingSetManagerEvent;

/* A free entry keeps the index of the next free one, shifted past the valid bit */
#define MI_WSLE_FREE_END ((ULONG)MAXLONG)

/* Entries not accessed for this many passes get trimmed */
#define MI_WSLE_TRIM_AGE        3
#define MI_WSLE_TRIM_AGE_HARD   1

/* LOCAL FUNCTIONS ************************************************************/

static MMPTE GetPteTemplateForWsList(PMMWSL WsList)
{
    return (WsList == MmSystemCacheWorkingSetList) ? ValidKernelPte : ValidKernelPteLocal;
}

static ULONG GetNextPageColorForWsList(PMMWSL WsList)
{
    return (WsList == MmSystemCacheWorkingSetList) ? MI_GET_NEXT_COLOR() : MI_GET_NEXT_PROCESS_COLOR(PsGetCurrentProcess());
}

static ULONG GetEntriesPerPage()
{
    return PAGE_SIZE / sizeof(MMWSLE);
}

/**
 * @brief Pushes an entry on the free chain.
 */
static void FreeWsleIndex(PMMWSL WsList, ULONG Index)
{
    ASSERT(Index >= WsList->FirstDynamic);
    ASSERT(Index < WsList->LastEntry);

    ULONG Next = (WsList->FirstFree == ULONG_MAX) ? MI_WSLE_FREE_END : WsList->FirstFree;
    WsList->Wsle[Index].u1.Long = (ULONG_PTR)Next << 1;
    WsList->FirstFree = Index;
}

/**
 * @brief Maps one more page of entries at the end of the list.
 * @return FALSE if no page could be had.
 */
static bool GrowWsList(PMMWSL WsList)
{
    PMMPTE PointerPte = MiAddressToPte(&WsList->Wsle[WsList->LastInitializedWsle]);
    ASSERT(PointerPte->u.Hard.Valid == 0);

    /* The list lives in hyperspace, it cannot run into the next region */
    if ((ULONG_PTR)MiPteToAddress(PointerPte) >= HYPER_SPACE_END)
        return false;

    MMPTE TempPte = GetPteTemplateForWsList(WsList);
    {
        ntoskrnl::MiPfnLockGuard PfnLock;

        PFN_NUMBER Page = MiRemoveAnyPage(GetNextPageColorForWsList(WsList));
        if (Page == 0)
            return false;

        TempPte.u.Hard.PageFrameNumber = Page;
        MiInitializePfnAndMakePteValid(Page, PointerPte, TempPte);
    }

    WsList->LastInitializedWsle += GetEntriesPerPage();
    return true;
}

static ULONG GetFreeWsleIndex(PMMWSL WsList)
{
    ULONG Index;

    if (WsList->FirstFree != ULONG_MAX)
    {
        Index = WsList->FirstFree;
        ASSERT(Index < WsList->LastEntry);
        ASSERT(WsList->Wsle[Index].u1.e1.Valid == 0);

        ULONG Next = (ULONG)(WsList->Wsle[Index].u1.Long >> 1);
        WsList->FirstFree = (Next == MI_WSLE_FREE_END) ? ULONG_MAX : Next;
    }
    else
    {
        Index = WsList->LastEntry;
        if ((Index >= WsList->LastInitializedWsle) && !GrowWsList(WsList))
            return ULONG_MAX;

        WsList->LastEntry++;
    }

    WsList->Wsle[Index].u1.Long = 0;
    return Index;
}

/**
 * @brief Gives back the tail of the list and the pages behind it.
 * @remarks The working set lock must be held, the PFN lock must not.
 */
static void ShrinkWsList(PMMWSL WsList)
{
    PMMWSLE Wsle = WsList->Wsle;
    ULONG LastEntry = WsList->LastEntry;

    while ((LastEntry > WsList->FirstDynamic) && (Wsle[LastEntry - 1].u1.e1.Valid == 0))
        LastEntry--;

    if (LastEntry != WsList->LastEntry)
    {
        WsList->LastEntry = LastEntry;

        /* Chain the remaining holes again, lowest index first */
        WsList->FirstFree = ULONG_MAX;
        for (ULONG Index = LastEntry; Index-- > WsList->FirstDynamic;)
        {
            if (Wsle[Index].u1.e1.Valid == 0)
                FreeWsleIndex(WsList, Index);
        }
    }

    /* Keep one spare page of entries */
    while ((WsList->LastInitializedWsle - WsList->LastEntry) > GetEntriesPerPage())
    {
        PMMPTE PointerPte = MiAddressToPte(Wsle + WsList->LastInitializedWsle - 1);

        /* We must not free ourself! */
        ASSERT(MiPteToAddress(PointerPte) != WsList);

        PFN_NUMBER Page = PFN_FROM_PTE(PointerPte);
        {
            ntoskrnl::MiPfnLockGuard PfnLock;

            PMMPFN Pfn = MiGetPfnEntry(Page);
            MI_SET_PFN_DELETED(Pfn);
            MiDecrementShareCount(MiGetPfnEntry(Pfn->u4.PteFrame), Pfn->u4.PteFrame);
            MiDecrementShareCount(Pfn, Page);
        }

        PointerPte->u.Long = 0;
        KeInvalidateTlbEntry(MiPteToAddress(PointerPte));
        WsList->LastInitializedWsle -= GetEntriesPerPage();
    }
}

static
VOID
RemoveFromWsList(PMMWSL WsList, PVOID Address)
{
    /* Make sure that we are holding the right locks. */
    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    PMMPTE PointerPte = MiAddressToPte(Address);

    /* Make sure we are removing a paged-in address */
    ASSERT(PointerPte->u.Hard.Valid == 1);
    PMMPFN Pfn1 = MiGetPfnEntry(PFN_FROM_PTE(PointerPte));
    ASSERT(Pfn1->u3.e1.PageLocation == ActiveAndValid);

    /* Shared pages not supported yet */
    ASSERT(Pfn1->u3.e1.PrototypePte == 0);

    /* Nor are "ROS PFN" */
    ASSERT(MI_IS_ROS_PFN(Pfn1) == FALSE);

    /* And we should have a valid index here */
    ASSERT(Pfn1->u1.WsIndex != 0);
    ASSERT(WsList->Wsle[Pfn1->u1.WsIndex].u1.e1.Valid == 1);
    ASSERT(PAGE_ALIGN(WsList->Wsle[Pfn1->u1.WsIndex].u1.VirtualAddress) == PAGE_ALIGN(Address));

    FreeWsleIndex(WsList, Pfn1->u1.WsIndex);
    Pfn1->u1.WsIndex = 0;
}

/**
 * @brief Moves pages that were not used lately out of a working set.
 *
 * @param[in] Vm
 * Working set to trim, locked exclusively.
 *
 * @param[in] TrimAge
 * Number of passes an entry must have gone unaccessed.
 *
 * @param[in] Target
 * Maximum number of pages to trim.
 *
 * @return Number of pages trimmed.
 */
static
ULONG
TrimWsList(PMMSUPPORT Vm, ULONG TrimAge, ULONG Target)
{
    PMMWSL WsList = Vm->VmWorkingSetList;

    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    ULONG Ret = 0;

    /* Walk the array */
    for (ULONG i = WsList->FirstDynamic; (i < WsList->LastEntry) && (Ret < Target); i++)
    {
        MMWSLE& Entry = WsList->Wsle[i];
        if (!Entry.u1.e1.Valid)
            continue;

        /* Only direct entries for now */
        ASSERT(Entry.u1.e1.Direct == 1);

        PVOID VirtualAddress = PAGE_ALIGN(Entry.u1.VirtualAddress);
        PMMPTE PointerPte = MiAddressToPte(VirtualAddress);

        /* This must be valid */
        ASSERT(PointerPte->u.Hard.Valid);

        /* If the PTE was accessed, simply reset and that's the end of it */
        if (PointerPte->u.Hard.Accessed)
        {
            Entry.u1.e1.Age = 0;
            PointerPte->u.Hard.Accessed = 0;
            KeInvalidateTlbEntry(VirtualAddress);
            continue;
        }

        /* If the entry is not so old, just age it */
        if (Entry.u1.e1.Age < TrimAge)
        {
            Entry.u1.e1.Age++;
            continue;
        }

        /* Page tables are never trimmed */
        if (MI_IS_PAGE_TABLE_ADDRESS(VirtualAddress))
            continue;

        PFN_NUMBER Page = PFN_FROM_PTE(PointerPte);
        PMMPFN Pfn = MiGetPfnEntry(Page);

        /* Not supported yet */
        ASSERT(Pfn->u3.e1.PrototypePte == 0);
        ASSERT(!MI_IS_ROS_PFN(Pfn));

        /* Pages locked by VirtualLock stay */
        if (Pfn->Wsle.u1.e1.LockedInMemory || Pfn->Wsle.u1.e1.LockedInWs)
            continue;

        MiRemoveFromWorkingSetList(Vm, VirtualAddress);

        {
            ntoskrnl::MiPfnLockGuard PfnLock;

            /* The PFN has the current protection, the entry may be stale */
            MMPTE OldPte = *PointerPte;
            MI_MAKE_TRANSITION_PTE(PointerPte, Page, Pfn->OriginalPte.u.Soft.Protection);
            KeInvalidateTlbEntry(VirtualAddress);

            /* Dirtify the page, if needed */
            if (OldPte.u.Hard.Dirty)
                Pfn->u3.e1.Modified = 1;

            /* Drop the share count. This will take care of putting it in the standby or modified list. */
            MiDecrementShareCount(Pfn, Page);
        }

        Ret++;
    }

    return Ret;
}

/**
 * @brief Counts the working sets that can be trimmed.
 * @remarks The expansion lock must be held.
 */
static
ULONG
CountExpansionList()
{
    ULONG Count = 0;

    for (PLIST_ENTRY Entry = MmWorkingSetExpansionHead.Flink;
         Entry != &MmWorkingSetExpansionHead;
         Entry = Entry->Flink)
    {
        Count++;
    }

    return Count;
}

/* GLOBAL FUNCTIONS ***********************************************************/
extern "C"
{

_Use_decl_annotations_
VOID
NTAPI
MiInsertInWorkingSetList(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address,
    _In_ ULONG Protection)
{
    PMMWSL WsList = Vm->VmWorkingSetList;

    /* Make sure that we are holding the WS lock. */
    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    PMMPTE PointerPte = MiAddressToPte(Address);

    /* Make sure we are adding a paged-in address */
    ASSERT(PointerPte->u.Hard.Valid == 1);
    PMMPFN Pfn1 = MiGetPfnEntry(PFN_FROM_PTE(PointerPte));
    ASSERT(Pfn1->u3.e1.PageLocation == ActiveAndValid);

    /* Shared pages not supported yet */
    ASSERT(Pfn1->u1.WsIndex == 0);
    ASSERT(Pfn1->u3.e1.PrototypePte == 0);

    /* Nor are "ROS PFN" */
    ASSERT(MI_IS_ROS_PFN(Pfn1) == FALSE);

    /* Without an entry the page just cannot be trimmed */
    ULONG Index = GetFreeWsleIndex(WsList);
    if (Index == ULONG_MAX)
        return;

    Pfn1->u1.WsIndex = Index;
    MMWSLENTRY& NewWsle = WsList->Wsle[Index].u1.e1;
    NewWsle.VirtualPageNumber = reinterpret_cast<ULONG_PTR>(Address) >> PAGE_SHIFT;
    NewWsle.Protection = Protection;
    NewWsle.Direct = 1;
    NewWsle.Hashed = 0;
    NewWsle.LockedInMemory = 0;
    NewWsle.LockedInWs = 0;
    NewWsle.Age = 0;
    NewWsle.Valid = 1;

    Vm->WorkingSetSize++;
    if (Vm->WorkingSetSize > Vm->PeakWorkingSetSize)
        Vm->PeakWorkingSetSize = Vm->WorkingSetSize;
}

_Use_decl_annotations_
VOID
NTAPI
MiRemoveFromWorkingSetList(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address)
{
    RemoveFromWsList(Vm->VmWorkingSetList, Address);

    ASSERT(Vm->WorkingSetSize != 0);
    Vm->WorkingSetSize--;
}

/**
 * @brief Adds a valid private user page to the current process working set.
 *
 * @param[in] Address
 * Faulting address.
 *
 * @remarks The process working set lock must be held exclusively and the PFN lock must not be.
 */
VOID
NTAPI
MiAddPrivatePageToWorkingSet(
    _In_ PVOID Address)
{
    PEPROCESS Process = PsGetCurrentProcess();

    if (Address > MM_HIGHEST_USER_ADDRESS)
        return;

    /* Hand built processes share a list that is not set up for this */
    if (Process->Vm.WorkingSetExpansionLinks.Flink == NULL)
        return;

    PMMPTE PointerPte = MiAddressToPte(Address);
    if (MiAddressToPde(Address)->u.Hard.Valid == 0 || PointerPte->u.Hard.Valid == 0)
        return;

    PMMPFN Pfn1 = MiGetPfnEntry(PFN_FROM_PTE(PointerPte));
    if ((Pfn1 == NULL) ||
        (Pfn1->u3.e1.PrototypePte == 1) ||
        MI_IS_ROS_PFN(Pfn1) ||
        (Pfn1->u3.e1.PageLocation != ActiveAndValid) ||
        (Pfn1->u1.WsIndex != 0) ||
        ((PMMPTE)((ULONG_PTR)Pfn1->PteAddress & ~1) != PointerPte))
    {
        return;
    }

    MiInsertInWorkingSetList(&Process->Vm, PAGE_ALIGN(Address), (ULONG)Pfn1->OriginalPte.u.Soft.Protection);
}

_Use_decl_annotations_
VOID
NTAPI
MiInitializeWorkingSetList(_Inout_ PMMSUPPORT WorkingSet)
{
    PMMWSL WsList = WorkingSet->VmWorkingSetList;

    /* Initialize some fields */
    WsList->FirstFree = ULONG_MAX;
    WsList->Wsle = reinterpret_cast<PMMWSLE>(WsList + 1);
    WsList->LastEntry = 0;
    WsList->FirstDynamic = 0;
    /* The first page is already allocated */
    WsList->LastInitializedWsle = (PAGE_SIZE - sizeof(*WsList)) / sizeof(MMWSLE);

    /* Insert the address we already know: our PDE base and the Working Set List */
    if (MI_IS_PROCESS_WORKING_SET(WorkingSet))
    {
        ASSERT(WorkingSet->VmWorkingSetList == MmWorkingSetList);
#if _MI_PAGING_LEVELS == 4
        MiInsertInWorkingSetList(WorkingSet, (PVOID)PXE_BASE, 0U);
#elif _MI_PAGING_LEVELS == 3
        MiInsertInWorkingSetList(WorkingSet, (PVOID)PPE_BASE, 0U);
#elif _MI_PAGING_LEVELS == 2
        MiInsertInWorkingSetList(WorkingSet, (PVOID)PDE_BASE, 0U);
#endif
    }

#if _MI_PAGING_LEVELS == 4
    MiInsertInWorkingSetList(WorkingSet, MiAddressToPpe(WorkingSet->VmWorkingSetList), 0UL);
#endif
#if _MI_PAGING_LEVELS >= 3
    MiInsertInWorkingSetList(WorkingSet, MiAddressToPde(WorkingSet->VmWorkingSetList), 0UL);
#endif
    MiInsertInWorkingSetList(WorkingSet, (PVOID)MiAddressToPte(WorkingSet->VmWorkingSetList), 0UL);
    MiInsertInWorkingSetList(WorkingSet, (PVOID)WorkingSet->VmWorkingSetList, 0UL);

    /* From now on, every added page can be trimmed at any time */
    WsList->FirstDynamic = WsList->LastEntry;

    /* We can add this to our list */
    ExInterlockedInsertTailList(&MmWorkingSetExpansionHead, &WorkingSet->WorkingSetExpansionLinks, &MmExpansionLock);
}

/**
 * @brief Gives back the working set list pages that are no longer needed.
 * @remarks The process working set lock must be held exclusively.
 */
VOID
NTAPI
MiShrinkWorkingSetList(
    _Inout_ PMMSUPPORT WorkingSet)
{
    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    if (WorkingSet->VmWorkingSetList != NULL)
        ShrinkWsList(WorkingSet->VmWorkingSetList);
}

/**
 * @brief Trims process working sets when physical memory runs low.
 * @remarks Runs on the balance set manager thread.
 */
VOID
NTAPI
MmWorkingSetManager(VOID)
{
    PETHREAD CurrentThread = PsGetCurrentThread();
    KIRQL OldIrql;

    if (MmAvailablePages >= MmPlentyFreePages)
        return;

    OldIrql = MiAcquireExpansionLock();
    ULONG Count = CountExpansionList();
    MiReleaseExpansionLock(OldIrql);

    while (Count-- && (MmAvailablePages < MmPlentyFreePages))
    {
        BOOLEAN TrimHard = MmAvailablePages < MmMinimumFreePages;

        /* Take the working set off the list while we work on it */
        OldIrql = MiAcquireExpansionLock();
        if (IsListEmpty(&MmWorkingSetExpansionHead))
        {
            MiReleaseExpansionLock(OldIrql);
            break;
        }

        PLIST_ENTRY VmListEntry = RemoveHeadList(&MmWorkingSetExpansionHead);
        PMMSUPPORT Vm = CONTAINING_RECORD(VmListEntry, MMSUPPORT, WorkingSetExpansionLinks);
        PEPROCESS Process = NULL;

        /* FIXME: Session & system space unsupported */
        if (MI_IS_PROCESS_WORKING_SET(Vm) && (Vm != MmGetKernelAddressSpace()))
        {
            Process = CONTAINING_RECORD(Vm, EPROCESS, Vm);

            /* The reference keeps it on our side of the list removal in process deletion */
            if (!ObReferenceObjectSafe(Process))
                Process = NULL;
        }

        if (Process == NULL)
        {
            InsertTailList(&MmWorkingSetExpansionHead, VmListEntry);
            MiReleaseExpansionLock(OldIrql);
            continue;
        }

        MiReleaseExpansionLock(OldIrql);

        if (!Process->VmDeleted && ExAcquireRundownProtection(&Process->RundownProtect))
        {
            ASSERT(!KeIsAttachedProcess());
            KeAttachProcess(&Process->Pcb);
            MiLockProcessWorkingSet(Process, CurrentThread);

            if (!Process->VmDeleted)
            {
                ULONG Target = 0;

                if (TrimHard)
                    Target = (ULONG)Vm->WorkingSetSize;
                else if (Vm->WorkingSetSize > Vm->MinimumWorkingSetSize)
                    Target = (ULONG)(Vm->WorkingSetSize - Vm->MinimumWorkingSetSize);

                if (Target != 0)
                {
                    Vm->Flags.BeingTrimmed = 1;
                    TrimWsList(Vm, TrimHard ? MI_WSLE_TRIM_AGE_HARD : MI_WSLE_TRIM_AGE, Target);
                    ShrinkWsList(Vm->VmWorkingSetList);
                    Vm->Flags.BeingTrimmed = 0;
                }
            }

            MiUnlockProcessWorkingSet(Process, CurrentThread);
            KeDetachProcess();
            ExReleaseRundownProtection(&Process->RundownProtect);
        }

        /* Back at the tail, the others get their turn first next time */
        OldIrql = MiAcquireExpansionLock();
        InsertTailList(&MmWorkingSetExpansionHead, VmListEntry);
        MiReleaseExpansionLock(OldIrql);

        ObDereferenceObject(Process);
    }
}

} // extern "C"
