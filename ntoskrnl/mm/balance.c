/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/mm/balance.c
 * PURPOSE:         kernel memory managment functions
 *
 * PROGRAMMERS:     David Welch (welch@cwcom.net)
 *                  Cameron Gutman (cameron.gutman@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include "../cache/section/newmm.h"

#include "ARM3/miarm.h"

extern
VOID
MiIncrementAvailablePages(
    VOID);

extern
VOID
MiDecrementAvailablePages(
    VOID);

/* TYPES ********************************************************************/
typedef struct _MM_ALLOCATION_REQUEST
{
    PFN_NUMBER Page;
    LIST_ENTRY ListEntry;
    KEVENT Event;
}
MM_ALLOCATION_REQUEST, *PMM_ALLOCATION_REQUEST;
/* GLOBALS ******************************************************************/

MM_MEMORY_CONSUMER MiMemoryConsumers[MC_MAXIMUM];
static KEVENT MiWritePageEvent;
static KTIMER MiWritePageTimer;
static KEVENT MiPageoutEvent;
static HANDLE MiWriterThread;
static BOOLEAN MiInShutdown;

/* Linked list of used PFNs */
static LIST_ENTRY UserLRUPfnListHead;

/* Linked list of Modified PFNs */
static LIST_ENTRY ModifiedLRUPfnListHead;
PFN_NUMBER MmModifiedPageCount;

/* Linked list of Standby PFNs */
static LIST_ENTRY StandbyLRUPfnListHead;
PFN_NUMBER MmStandbyPageCount;

/* List of RMAPs that we have to free from popping standby pages. */
static PMM_RMAP_ENTRY RmapsToFree;

/* HELPER FUNCTIONS ******************************************************/
_IRQL_requires_max_(DISPATCH_LEVEL)
static
PFN_NUMBER
MiGetFirstLRUPage(
    _Inout_ PLIST_ENTRY ListHead)
{
    PFN_NUMBER Page = 0;
    KIRQL OldIrql = MiAcquirePfnLock();

    if (!IsListEmpty(ListHead))
    {
        PMMPFN Pfn = CONTAINING_RECORD(ListHead->Flink, MMPFN, LruEntry);
        Page = MiGetPfnEntryIndex(Pfn);

        ASSERT(MI_IS_ROS_PFN(Pfn));
    }

    MiReleasePfnLock(OldIrql);

    return Page;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static
VOID
MiInsertLastLRUPage(
    _Inout_ PMMPFN Pfn,
    _Inout_ PLIST_ENTRY ListHead)
{
    KIRQL OldIrql = MiAcquirePfnLock();

    ASSERT(MI_IS_ROS_PFN(Pfn));

    ASSERT(Pfn->LruEntry.Flink == NULL);

    InsertTailList(ListHead, &Pfn->LruEntry);

    /* Must be in no list yet */
    ASSERT(!Pfn->u4.MustBeCached);
    ASSERT(!Pfn->u3.e1.Modified);

    /* HACK for this special case */
    if (ListHead == &UserLRUPfnListHead)
    {
        Pfn->u4.MustBeCached = 1;
    }
    else if (ListHead == &ModifiedLRUPfnListHead)
    {
        Pfn->u3.e1.Modified = 1;
        /* Keep modified page count low */
        if (++MmModifiedPageCount > 200)
        {
            KeSetEvent(&MiWritePageEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    else
    {
        MiIncrementAvailablePages();
        MmStandbyPageCount++;
    }


    MiReleasePfnLock(OldIrql);
}


_IRQL_requires_max_(DISPATCH_LEVEL)
static
VOID
MiRemoveLRUPage(
    _Inout_ PMMPFN Pfn,
    _Inout_ PLIST_ENTRY ListHead)
{
    KIRQL OldIrql = MiAcquirePfnLock();

    ASSERT(MI_IS_ROS_PFN(Pfn));

    RemoveEntryList(&Pfn->LruEntry);
    RtlZeroMemory(&Pfn->LruEntry, sizeof(Pfn->LruEntry));

    /* HACKs & bookkeeping for each case */
    if (ListHead == &UserLRUPfnListHead)
    {
        ASSERT(Pfn->u4.MustBeCached);
        ASSERT(!Pfn->u3.e1.Modified);
        Pfn->u4.MustBeCached = 0;
    }
    else if (ListHead == &ModifiedLRUPfnListHead)
    {
        ASSERT(!Pfn->u4.MustBeCached);
        ASSERT(Pfn->u3.e1.Modified);
        Pfn->u3.e1.Modified = 0;
        MmModifiedPageCount--;
    }
    else
    {
        ASSERT(ListHead == &StandbyLRUPfnListHead);
        ASSERT(!Pfn->u4.MustBeCached);
        ASSERT(!Pfn->u3.e1.Modified);
        /* This one is not available anymore */
        MiDecrementAvailablePages();
        MmStandbyPageCount--;
    }

    MiReleasePfnLock(OldIrql);
}

/* FUNCTIONS *************************************************************/

_IRQL_requires_max_(DISPATCH_LEVEL)
PFN_NUMBER
MmGetLRUFirstUserPage(VOID)
{
    return MiGetFirstLRUPage(&UserLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MmInsertLRULastUserPage(PFN_NUMBER Page)
{
    MiInsertLastLRUPage(MiGetPfnEntry(Page), &UserLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MmRemoveLRUUserPage(PFN_NUMBER Page)
{
    MiRemoveLRUPage(MiGetPfnEntry(Page), &UserLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PFN_NUMBER
MmGetLRUNextUserPage(PFN_NUMBER Page)
{
    KIRQL OldIrql = MiAcquirePfnLock();
    PMMPFN Pfn = MiGetPfnEntry(Page);
    PLIST_ENTRY Entry = NULL;

    Page = 0;

    if (Pfn && MI_IS_ROS_PFN(Pfn) && (Pfn->u4.MustBeCached == 1))
        Entry = Pfn->LruEntry.Flink;

    if (!Entry)
        Entry = UserLRUPfnListHead.Flink;

    if (Entry != &UserLRUPfnListHead)
    {
        Pfn = CONTAINING_RECORD(Entry, MMPFN, LruEntry);
        Page = MiGetPfnEntryIndex(Pfn);
    }

    MiReleasePfnLock(OldIrql);

    return Page;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PFN_NUMBER
MmGetLRUFirstModifiedPage(VOID)
{
    return MiGetFirstLRUPage(&ModifiedLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MmInsertLRULastModifiedPage(PFN_NUMBER Page)
{
    MiInsertLastLRUPage(MiGetPfnEntry(Page), &ModifiedLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MmRemoveLRUModifiedPage(PFN_NUMBER Page)
{
    MiRemoveLRUPage(MiGetPfnEntry(Page), &ModifiedLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PFN_NUMBER
MmGetLRUFirstStandbyPage(VOID)
{
    return MiGetFirstLRUPage(&StandbyLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MmInsertLRULastStandbyPage(PFN_NUMBER Page)
{
    MiInsertLastLRUPage(MiGetPfnEntry(Page), &StandbyLRUPfnListHead);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MmRemoveLRUStandbyPage(PFN_NUMBER Page)
{
    MiRemoveLRUPage(MiGetPfnEntry(Page), &StandbyLRUPfnListHead);
}

CODE_SEG("INIT")
VOID
NTAPI
MmInitializeBalancer(ULONG NrAvailablePages, ULONG NrSystemPages)
{
    memset(MiMemoryConsumers, 0, sizeof(MiMemoryConsumers));

    InitializeListHead(&UserLRUPfnListHead);
    InitializeListHead(&ModifiedLRUPfnListHead);
    InitializeListHead(&StandbyLRUPfnListHead);
}

VOID
NTAPI
MiZeroPhysicalPage(
    IN PFN_NUMBER PageFrameIndex
);

NTSTATUS
NTAPI
MmReleasePageMemoryConsumer(ULONG Consumer, PFN_NUMBER Page)
{
    KIRQL OldIrql;

    if (Page == 0)
    {
        DPRINT1("Tried to release page zero.\n");
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    (void)InterlockedDecrementUL(&MiMemoryConsumers[Consumer].PagesUsed);

    OldIrql = MiAcquirePfnLock();

    MmDereferencePage(Page);

    MiReleasePfnLock(OldIrql);

    return(STATUS_SUCCESS);
}

#if 0
static
BOOLEAN
MmPruneUserPage(PFN_NUMBER Page)
{
    PFN_NUMBER CurrentPage;
    NTSTATUS Status;

    (*NrFreedPages) = 0;

    DPRINT1("MM BALANCER: %s\n", Priority ? "Paging out!" : "Removing access bit!");

    CurrentPage = MmGetLRUFirstUserPage();
    while (CurrentPage != 0 && Target > 0)
    {
        if (Priority)
        {
            Status = MmPageOutPhysicalAddress(CurrentPage);
            if (NT_SUCCESS(Status))
            {
                DPRINT("Succeeded\n");
                Target--;
                (*NrFreedPages)++;
            }
        }
        else
        {
            /* When not paging-out agressively, just reset the accessed bit */
            PEPROCESS Process = NULL;
            PVOID Address = NULL;
            BOOLEAN Accessed = FALSE;

            /*
             * We have a lock-ordering problem here. We cant lock the PFN DB before the Process address space.
             * So we must use circonvoluted loops.
             * Well...
             */
            while (TRUE)
            {
                KAPC_STATE ApcState;
                KIRQL OldIrql = MiAcquirePfnLock();
                PMM_RMAP_ENTRY Entry = MmGetRmapListHeadPage(CurrentPage);
                while (Entry)
                {
                    if (RMAP_IS_SEGMENT(Entry->Address))
                    {
                        Entry = Entry->Next;
                        continue;
                    }

                    /* Check that we didn't treat this entry before */
                    if (Entry->Address < Address)
                    {
                        Entry = Entry->Next;
                        continue;
                    }

                    if ((Entry->Address == Address) && (Entry->Process <= Process))
                    {
                        Entry = Entry->Next;
                        continue;
                    }

                    break;
                }

                if (!Entry)
                {
                    MiReleasePfnLock(OldIrql);
                    break;
                }

                Process = Entry->Process;
                Address = Entry->Address;

                ObReferenceObject(Process);

                if (!ExAcquireRundownProtection(&Process->RundownProtect))
                {
                    ObDereferenceObject(Process);
                    MiReleasePfnLock(OldIrql);
                    continue;
                }

                MiReleasePfnLock(OldIrql);

                KeStackAttachProcess(&Process->Pcb, &ApcState);
                MiLockProcessWorkingSet(Process, PsGetCurrentThread());

                /* Be sure this is still valid. */
                if (MmIsAddressValid(Address))
                {
                    PMMPTE Pte = MiAddressToPte(Address);
                    Accessed = Accessed || Pte->u.Hard.Accessed;
                    Pte->u.Hard.Accessed = 0;

                    /* There is no need to invalidate, the balancer thread is never on a user process */
                    //KeInvalidateTlbEntry(Address);
                }

                MiUnlockProcessWorkingSet(Process, PsGetCurrentThread());

                KeUnstackDetachProcess(&ApcState);
                ExReleaseRundownProtection(&Process->RundownProtect);
                ObDereferenceObject(Process);
            }

            if (!Accessed)
            {
                /* Nobody accessed this page since the last time we check. Time to clean up */

                Status = MmPageOutPhysicalAddress(CurrentPage);
                // DPRINT1("Paged-out one page: %s\n", NT_SUCCESS(Status) ? "Yes" : "No");
                (void)Status;
            }

            /* Done for this page. */
            Target--;
        }

        CurrentPage = MmGetLRUNextUserPage(CurrentPage, TRUE);
    }

    if (CurrentPage)
    {
        KIRQL OldIrql = MiAcquirePfnLock();
        MmDereferencePage(CurrentPage);
        MiReleasePfnLock(OldIrql);
    }

    return STATUS_SUCCESS;
}
#endif

VOID
MmRebalanceMemoryConsumers(VOID)
{
    /* We're in a low memory situation. Try to give pages back. */
    KeSetEvent(&MiWritePageEvent, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&MiPageoutEvent, IO_NO_INCREMENT, FALSE);
}

PFN_NUMBER
MmPopStandbyPage(VOID)
{
    /* We must be under PFN lock */
    MI_ASSERT_PFN_LOCK_HELD();

    PLIST_ENTRY ListEntry = StandbyLRUPfnListHead.Flink;
    while (ListEntry != &StandbyLRUPfnListHead)
    {
        PMMPFN Pfn = CONTAINING_RECORD(ListEntry, MMPFN, LruEntry);

        PMM_RMAP_ENTRY Rmap = Pfn->RmapListHead;

        /* The only mapping must be on a segment */
        NT_ASSERT(Rmap != NULL);
        NT_ASSERT(Rmap->Next == NULL);
        NT_ASSERT(RMAP_IS_SEGMENT(Rmap->Address));

        PCACHE_SECTION_PAGE_TABLE PageTable = (PCACHE_SECTION_PAGE_TABLE)Rmap->Process;

        /* See if we can really free it */
        if (ExTryToAcquirePushLockExclusive(&PageTable->Segment->Lock))
        {
            /* Nice, we will be able to remove it */
            PFN_NUMBER Page = MiGetPfnEntryIndex(Pfn);
            ULONG Offset = (ULONG_PTR)Rmap->Address & ~RMAP_SEGMENT_MASK;
            ULONG_PTR Entry = PageTable->PageEntries[Offset];

            NT_ASSERT(Entry != 0);
            NT_ASSERT(PFN_FROM_SSE(Entry) == Page);
            NT_ASSERT(SHARE_COUNT_FROM_SSE(Entry) == 0);
            NT_ASSERT(!IS_DIRTY_SSE(Entry));
            NT_ASSERT(!IS_SWAP_FROM_SSE(Entry));

            /* Restore value */
            SWAPENTRY SwapEntry = Pfn->u1.SwapEntry;
            if (SwapEntry)
                PageTable->PageEntries[Offset] = MAKE_SWAP_SSE(SwapEntry);
            else
                PageTable->PageEntries[Offset] = 0;

            ExReleasePushLockExclusive(&PageTable->Segment->Lock);

            /* Dereferencing it will bump the number of free pages, but this one was already free */
            MmAvailablePages--;

            /* Unlink it */
            RemoveEntryList(&Pfn->LruEntry);
            RtlZeroMemory(&Pfn->LruEntry, sizeof(Pfn->LruEntry));
            Pfn->RmapListHead = NULL;

            /* Defer freeing this. We can't do it right now because we're holding the PFN lock */
            Rmap->Next = RmapsToFree;
            RmapsToFree = Rmap;

            /* Make that a pristine PFN entry */
            ASSERT(Pfn->u3.ReferenceCount == 1);
            MmDereferencePage(Page);

            return Page;
        }

        ListEntry = ListEntry->Flink;
    }

    /* If we get here, we're un trouble */
    KeBugCheck(NO_PAGES_AVAILABLE);
}

NTSTATUS
NTAPI
MmRequestPageMemoryConsumer(ULONG Consumer, BOOLEAN CanWait,
                            PPFN_NUMBER AllocatedPage)
{
    PFN_NUMBER Page;

    /* Update the target */
    InterlockedIncrementUL(&MiMemoryConsumers[Consumer].PagesUsed);

    /*
     * Actually allocate the page.
     */
    Page = MmAllocPage(Consumer);
    if (Page == 0)
    {
        KeBugCheck(NO_PAGES_AVAILABLE);
    }
    *AllocatedPage = Page;

    return(STATUS_SUCCESS);
}

static
VOID
MiPurgeStaleRmaps(VOID)
{
    while (TRUE)
    {
        PMM_RMAP_ENTRY ToFree;
        PCACHE_SECTION_PAGE_TABLE PageTable;
        KIRQL OldIrql = MiAcquirePfnLock();

        ToFree = RmapsToFree;
        if (ToFree)
        {
            RmapsToFree = ToFree->Next;
        }
        MiReleasePfnLock(OldIrql);

        if (ToFree == NULL)
        {
            break;
        }

        /* Each page map references the segment. Free it here. */
        PageTable = (PCACHE_SECTION_PAGE_TABLE)ToFree->Process;
        MmDereferenceSegment(PageTable->Segment);

        MmFreeRmap(ToFree);
    }
}

static
VOID
NTAPI
MiPageWriterThread(PVOID Unused)
{
    PVOID WaitObjects[2];
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Unused);

    WaitObjects[0] = &MiWritePageEvent;
    WaitObjects[1] = &MiWritePageTimer;

    while (1)
    {
        Status = KeWaitForMultipleObjects(2,
                                          WaitObjects,
                                          WaitAny,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          NULL,
                                          NULL);

        if (Status == STATUS_WAIT_0 || Status == STATUS_WAIT_1)
        {
            PFN_NUMBER Page, PageCount = MmModifiedPageCount;

            DPRINT("Balancer running. %d available pages\n", MmAvailablePages);

            /*
             * Now write modified pages back to disk.
             * Make sure we don't loop forever because of one failed write.
             */
            while ((PageCount != 0) && (Page = MmGetLRUFirstModifiedPage()))
            {
                /* Is it part of file map ? */
                LARGE_INTEGER SegmentOffset;
                PMM_SECTION_SEGMENT Segment = MmGetSectionAssociation(Page, &SegmentOffset);
                if (Segment)
                {
                    /* We're likely modifying FO's ValidDataLength. Lock it */
                    FsRtlAcquireFileExclusive(Segment->FileObject);

                    /* It is. Flush it to disk. */
                    MmLockSectionSegment(Segment);

                    ULONG_PTR Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);

                    /* Now that we own the segment, make sure that it's still in modified state. */
                    if (!IS_SWAP_FROM_SSE(Entry) && (PFN_FROM_SSE(Entry) == Page)
                        && IS_DIRTY_SSE(Entry) && (SHARE_COUNT_FROM_SSE(Entry) == 0))
                    {
                        /* This will write the data to disk and put the page in standby list */
                        DPRINT("Flushing page 0x%lx to disk\n", Page);
                        MmCheckDirtySegment(Segment, &SegmentOffset, TRUE);
                    }

                    MmUnlockSectionSegment(Segment);

                    FsRtlReleaseFile(Segment->FileObject);

                    MmDereferenceSegment(Segment);
                }

                PageCount--;
            }

            /* Handle shutdown */
            if (MiInShutdown)
            {
                return;
            }

            if (Status == STATUS_WAIT_1)
            {
                /* We're not in a hurry. Take this as an occasion to free those */
                MiPurgeStaleRmaps();
            }
        }
        else
        {
            DPRINT1("KeWaitForMultipleObjects failed, status = %x\n", Status);
            KeBugCheck(MEMORY_MANAGEMENT);
        }
    }
}

static
VOID
NTAPI
MiPageoutThread(PVOID Unused)
{
    UNREFERENCED_PARAMETER(Unused);

    while (TRUE)
    {
        /* Wake up when in dire need of free pages */
        KeWaitForSingleObject(&MiPageoutEvent, WrPageOut, KernelMode, FALSE, NULL);

        PFN_NUMBER Page = 0;

        /* Keep the number of standby pages at an acceptable level */
        while ((MmAvailablePages < MmMinimumFreePages) &&
            (MmStandbyPageCount < MmMinimumFreePages) &&
            (Page = MmGetLRUNextUserPage(Page)))
        {
            DPRINT1("Running low on page despite our best efforts. Pruning page 0x%lx from working sets.\n", Page);

            MmPageOutPhysicalAddress(Page);
        }
    }
}


CODE_SEG("INIT")
VOID
NTAPI
MiInitBalancerThread(VOID)
{
    KPRIORITY Priority;
    NTSTATUS Status;
    LARGE_INTEGER Timeout;
    HANDLE ThreadHandle;

    KeInitializeEvent(&MiWritePageEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&MiPageoutEvent, SynchronizationEvent, FALSE);

    KeInitializeTimerEx(&MiWritePageTimer, SynchronizationTimer);

    Timeout.QuadPart = -20000000; /* 2 sec */
    KeSetTimerEx(&MiWritePageTimer,
                 Timeout,
                 2000,         /* 2 sec */
                 NULL);

    /* Modified page writer */
    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  MiPageWriterThread,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    Priority = LOW_REALTIME_PRIORITY + 1;
    NtSetInformationThread(ThreadHandle,
                           ThreadPriority,
                           &Priority,
                           sizeof(Priority));
    MiWriterThread = ThreadHandle;

    /* Working set manager (sort of) */
    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  MiPageoutThread,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    /* This one is important */
    Priority = LOW_REALTIME_PRIORITY + 1;
    NtSetInformationThread(ThreadHandle,
                           ThreadPriority,
                           &Priority,
                           sizeof(Priority));
    ObCloseHandle(ThreadHandle, KernelMode);
}

VOID
MmShutdownBalancer(VOID)
{
    /* Wake up the writer thread. Tell it to finish */
    MiInShutdown = TRUE;
    KeSetEvent(&MiWritePageEvent, IO_NO_INCREMENT, FALSE);
    NtWaitForSingleObject(MiWriterThread, FALSE, NULL);
    ObCloseHandle(MiWriterThread, KernelMode);

    /* Ensure we're freeing this and that all segments are properly unreferenced */
    MiPurgeStaleRmaps();
}


/* EOF */
