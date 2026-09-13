/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Modified page writer
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

#define MI_WRITE_CLUSTER_PAGES 16

/* Size of the modified list at which the writer gets woken up */
PFN_NUMBER MmModifiedPageMaximum = 800;

/* Pages written to paging files since boot */
ULONG MmModifiedPagesWritten;

static KEVENT MiModifiedPageWriterEvent;
static KEVENT MiModifiedPageWriterIdleEvent;

/* Signaled while at least MmMinimumFreePages pages are available */
static KEVENT MiAvailablePagesEvent;
static BOOLEAN MiModifiedPageWriterStarted;
static BOOLEAN MiModifiedPageWriterStopping;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Tells which parts of the modified list should be written out now.
 *
 * @param[out] PagingFile
 * Receives TRUE when pages have to go to the paging files.
 *
 * @param[out] Mapped
 * Receives TRUE when pages have to go back to their files.
 *
 * @return TRUE if there is anything to write.
 *
 * @remarks The PFN lock must be held.
 */
static
BOOLEAN
MiShouldWriteModifiedPages(
    _Out_ PBOOLEAN PagingFile,
    _Out_ PBOOLEAN Mapped)
{
    BOOLEAN Needed;

    MI_ASSERT_PFN_LOCK_HELD();

    /* Under pressure everything goes, otherwise trim the list down to half */
    Needed = (BOOLEAN)(!MiModifiedPageWriterStopping &&
                       ((MmAvailablePages < MmPlentyFreePages) ||
                        (MmModifiedPageListHead.Total > (MmModifiedPageMaximum / 2))));

    *PagingFile = (BOOLEAN)(Needed &&
                            (MmNumberOfPagingFiles != 0) &&
                            (MmModifiedPageListByColor[0].Total != 0));

    *Mapped = (BOOLEAN)(Needed && (MmModifiedMappedPageListHead.Total != 0));

    return (BOOLEAN)(*PagingFile || *Mapped);
}

/**
 * @brief Pulls a cluster of pages off the modified list for one paging file write.
 *
 * @param[out] Pages
 * Receives the pages, in paging file order.
 *
 * @param[out] PageFileIndex
 * Receives the paging file the cluster goes to.
 *
 * @param[out] PageFileOffset
 * Receives the paging file offset of the first page.
 *
 * @return Number of pages gathered, 0 if no paging file space is left.
 *
 * @remarks The PFN lock must be held. Every page is referenced and marked
 * as being written, so it stays in transition until the write completes.
 */
static
ULONG
MiGatherPagefilePages(
    _Out_writes_(MI_WRITE_CLUSTER_PAGES) PPFN_NUMBER Pages,
    _Out_ PULONG PageFileIndex,
    _Out_ PULONG_PTR PageFileOffset)
{
    PFN_NUMBER PageFrameIndex;
    PMMPFN Pfn1;
    ULONG Count, i;

    MI_ASSERT_PFN_LOCK_HELD();

    Count = (ULONG)min(MI_WRITE_CLUSTER_PAGES, MmModifiedPageListByColor[0].Total);
    ASSERT(Count != 0);

    /* Copies left by earlier writes are stale, give their space back before reserving */
    PageFrameIndex = MmModifiedPageListByColor[0].Flink;
    for (i = 0; i < Count; i++)
    {
        ASSERT(PageFrameIndex != LIST_HEAD);
        Pfn1 = MI_PFN_ELEMENT(PageFrameIndex);

        if (MiReleasePageFileSpace(Pfn1->OriginalPte))
        {
            Pfn1->OriginalPte.u.Soft.PageFileLow = 0;
            Pfn1->OriginalPte.u.Soft.PageFileHigh = 0;
        }

        PageFrameIndex = Pfn1->u1.Flink;
    }

    if (!MiReservePageFileSpace(&Count, PageFileIndex, PageFileOffset))
        return 0;

    for (i = 0; i < Count; i++)
    {
        PageFrameIndex = MmModifiedPageListByColor[0].Flink;
        ASSERT(PageFrameIndex != LIST_HEAD);

        Pfn1 = MI_PFN_ELEMENT(PageFrameIndex);
        ASSERT(Pfn1->u3.e1.PageLocation == ModifiedPageList);
        ASSERT(Pfn1->u3.e1.Modified == 1);

        MiUnlinkPageFromList(Pfn1);

        Pfn1->OriginalPte.u.Soft.PageFileLow = *PageFileIndex;
        Pfn1->OriginalPte.u.Soft.PageFileHigh = *PageFileOffset + i;

        /* The writer owns the page until the I/O is done */
        Pfn1->u3.e2.ReferenceCount = 1;
        Pfn1->u3.e1.PageLocation = TransitionPage;
        Pfn1->u3.e1.WriteInProgress = 1;
        Pfn1->u3.e1.Modified = 0;

        Pages[i] = PageFrameIndex;
    }

    return Count;
}

/**
 * @brief Finishes a paging file write and puts the pages where they belong.
 *
 * @param[in] Pages
 * Pages that were written.
 *
 * @param[in] Count
 * Number of pages.
 *
 * @param[in] Status
 * Result of the paging I/O.
 */
static
VOID
MiCompletePagefileWrite(
    _In_reads_(Count) PPFN_NUMBER Pages,
    _In_ ULONG Count,
    _In_ NTSTATUS Status)
{
    PMMPFN Pfn1;
    KIRQL OldIrql;
    ULONG i;

    OldIrql = MiAcquirePfnLock();

    for (i = 0; i < Count; i++)
    {
        Pfn1 = MI_PFN_ELEMENT(Pages[i]);
        ASSERT(Pfn1->u3.e1.WriteInProgress == 1);
        ASSERT(Pfn1->u3.e2.ReferenceCount != 0);

        Pfn1->u3.e1.WriteInProgress = 0;

        if (!NT_SUCCESS(Status))
        {
            /* The paged out copy is useless, keep the page dirty */
            MiReleasePageFileSpace(Pfn1->OriginalPte);
            Pfn1->OriginalPte.u.Soft.PageFileLow = 0;
            Pfn1->OriginalPte.u.Soft.PageFileHigh = 0;
            Pfn1->u3.e1.Modified = 1;
        }

        /* Standby when clean, modified again when written meanwhile, freed when deleted */
        MiDecrementReferenceCount(Pfn1, Pages[i]);
    }

    if (NT_SUCCESS(Status))
        MmModifiedPagesWritten += Count;

    MiReleasePfnLock(OldIrql);
}

/**
 * @brief Writes modified pages to the paging files whenever asked to.
 */
static
VOID
NTAPI
MiModifiedPageWriter(
    _In_ PVOID Context)
{
    UCHAR MdlBuffer[sizeof(MDL) + MI_WRITE_CLUSTER_PAGES * sizeof(PFN_NUMBER)];
    PFN_NUMBER Pages[MI_WRITE_CLUSTER_PAGES];
    LARGE_INTEGER RetryDelay, BusyDelay;
    BOOLEAN PagingFile, Mapped, Wrote;
    ULONG PageFileIndex, Count;
    ULONG_PTR PageFileOffset;
    NTSTATUS Status;
    KIRQL OldIrql;
    PMDL Mdl;

    UNREFERENCED_PARAMETER(Context);

    KeSetPriorityThread(&PsGetCurrentThread()->Tcb, LOW_REALTIME_PRIORITY + 1);
    RetryDelay.QuadPart = -10 * 1000 * 1000;
    BusyDelay.QuadPart = -100 * 10000LL;

    for (;;)
    {
        KeWaitForSingleObject(&MiModifiedPageWriterEvent,
                              WrFreePage,
                              KernelMode,
                              FALSE,
                              NULL);

        for (;;)
        {
            OldIrql = MiAcquirePfnLock();

            if (!MiShouldWriteModifiedPages(&PagingFile, &Mapped))
            {
                MiReleasePfnLock(OldIrql);
                break;
            }

            KeClearEvent(&MiModifiedPageWriterIdleEvent);
            Wrote = FALSE;

            Count = PagingFile ? MiGatherPagefilePages(Pages, &PageFileIndex, &PageFileOffset) : 0;
            MiReleasePfnLock(OldIrql);

            if (Count != 0)
            {
                Mdl = (PMDL)MdlBuffer;
                MmInitializeMdl(Mdl, NULL, (SIZE_T)Count << PAGE_SHIFT);
                Mdl->MdlFlags |= MDL_PAGES_LOCKED;
                RtlCopyMemory(MmGetMdlPfnArray(Mdl), Pages, Count * sizeof(PFN_NUMBER));

                Status = MiWritePageFile(Mdl, PageFileIndex, PageFileOffset);
                MiCompletePagefileWrite(Pages, Count, Status);

                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("Paging file write failed: 0x%lx\n", Status);
                    KeSetEvent(&MiModifiedPageWriterIdleEvent, IO_NO_INCREMENT, FALSE);
                    KeDelayExecutionThread(KernelMode, FALSE, &RetryDelay);
                    break;
                }

                Wrote = TRUE;
            }

            if (Mapped)
            {
                Status = MiWriteModifiedMappedPages();
                if (NT_SUCCESS(Status))
                {
                    Wrote = TRUE;
                }
                else if (Status != STATUS_NO_MORE_ENTRIES)
                {
                    /* The file is busy or gone, its pages went back on the list */
                    if (Status != STATUS_CANT_WAIT)
                        DPRINT1("Mapped file write failed: 0x%lx\n", Status);

                    KeSetEvent(&MiModifiedPageWriterIdleEvent, IO_NO_INCREMENT, FALSE);
                    KeDelayExecutionThread(KernelMode, FALSE, &BusyDelay);
                    break;
                }
            }

            KeSetEvent(&MiModifiedPageWriterIdleEvent, IO_NO_INCREMENT, FALSE);

            if (!Wrote)
                break;
        }
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Starts the modified page writer thread.
 */
CODE_SEG("INIT")
VOID
NTAPI
MiInitializeModifiedPageWriter(VOID)
{
    HANDLE ThreadHandle;
    NTSTATUS Status;

    switch (MmQuerySystemSize())
    {
        case MmSmallSystem:
            MmModifiedPageMaximum = 100;
            break;

        case MmMediumSystem:
            MmModifiedPageMaximum = 400;
            break;

        default:
            MmModifiedPageMaximum = 800;
            break;
    }

    KeInitializeEvent(&MiModifiedPageWriterEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&MiModifiedPageWriterIdleEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&MiAvailablePagesEvent,
                      NotificationEvent,
                      MmAvailablePages >= MmMinimumFreePages);

    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  MiModifiedPageWriter,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x7010, Status, 0, 0);
    }

    ZwClose(ThreadHandle);
    MiModifiedPageWriterStarted = TRUE;
}

/**
 * @brief Wakes up the modified page writer.
 * @remarks Callable up to DISPATCH_LEVEL.
 */
VOID
NTAPI
MiWakeModifiedPageWriter(VOID)
{
    if (MiModifiedPageWriterStarted)
    {
        KeSetEvent(&MiModifiedPageWriterEvent, IO_NO_INCREMENT, FALSE);
    }
}

/**
 * @brief Asks the balance set manager for a working set trimming pass.
 * @remarks Callable up to DISPATCH_LEVEL.
 */
VOID
NTAPI
MiWakeWorkingSetManager(VOID)
{
    if (MiModifiedPageWriterStarted)
    {
        KeSetEvent(&MmWorkingSetManagerEvent, IO_NO_INCREMENT, FALSE);
    }
}

/**
 * @brief Tracks whether faults can get pages again.
 *
 * @param[in] Available
 * TRUE when the available count just reached MmMinimumFreePages, FALSE when it dropped below.
 *
 * @remarks The PFN lock must be held.
 */
VOID
NTAPI
MiNotifyAvailablePages(
    _In_ BOOLEAN Available)
{
    if (!MiModifiedPageWriterStarted)
        return;

    if (Available)
        KeSetEvent(&MiAvailablePagesEvent, IO_NO_INCREMENT, FALSE);
    else
        KeClearEvent(&MiAvailablePagesEvent);
}

/**
 * @brief Waits for pages to become available after an allocation failed.
 *
 * @remarks Must be called without any working set lock held, the fault is retried afterwards.
 */
VOID
NTAPI
MiWaitForFreePage(VOID)
{
    LARGE_INTEGER Timeout;

    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);
    ASSERT(!MM_ANY_WS_LOCK_HELD(PsGetCurrentThread()));

    /* Get modified pages written and legacy pages trimmed */
    MiWakeModifiedPageWriter();
    MmRebalanceMemoryConsumers();

    if (!MiModifiedPageWriterStarted)
        return;

    /* And working sets trimmed */
    KeSetEvent(&MmWorkingSetManagerEvent, IO_NO_INCREMENT, FALSE);

    /* The legacy balancer frees pages without telling anyone, so do not wait forever */
    Timeout.QuadPart = -100 * 10000LL;
    KeWaitForSingleObject(&MiAvailablePagesEvent, WrFreePage, KernelMode, FALSE, &Timeout);
}

/**
 * @brief Stops writing and waits for a write in progress to finish.
 * @remarks Used at shutdown before the paging files go away.
 */
VOID
NTAPI
MiStopModifiedPageWriter(VOID)
{
    KIRQL OldIrql;

    if (!MiModifiedPageWriterStarted)
        return;

    OldIrql = MiAcquirePfnLock();
    MiModifiedPageWriterStopping = TRUE;
    MiReleasePfnLock(OldIrql);

    KeWaitForSingleObject(&MiModifiedPageWriterIdleEvent,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
}

/* EOF */
