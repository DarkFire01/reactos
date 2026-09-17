/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File backed data sections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

/* Prototype PTEs allocated at most for one subsection */
#define MI_SUBSECTION_PTES          ((ULONG)(_64K / sizeof(MMPTE)))

/* Largest run of pages going through one paging I/O */
/* Cleanup rounds before modified data that cannot be written is given up */
#define MI_CLEANUP_ATTEMPTS         50

#define TAG_MAPPED_CONTROL_AREA     'aCmM'
#define TAG_MAPPED_SEGMENT          'mSmM'
#define TAG_MAPPED_SUBSECTION       'cSmM'
#define TAG_MAPPED_PROTOTYPES       'tPmM'

/* Data control areas nobody references anymore, linked through DereferenceList */
static LIST_ENTRY MiDataFileCleanupList;
static KEVENT MiDataFileCleanupEvent;

/* Serializes the growth of data file segments */
static KGUARDED_MUTEX MiDataFileExtendLock;

/* PRIVATE FUNCTIONS **********************************************************/


/**
 * @brief Decodes the subsection a subsection PTE points to.
 */
static
PMSUBSECTION
MiSubsectionFromPte(
    _In_ PMMPTE PointerPte)
{
    return (PMSUBSECTION)(MiSubsectionPteToSubsection(PointerPte));
}

/**
 * @brief Gets the page a prototype PTE holds in memory, if any.
 */
static
PMMPFN
MiGetResidentPfn(
    _In_ PMMPTE PointerPte)
{
    if (PointerPte->u.Hard.Valid == 1)
        return MI_PFN_ELEMENT(PFN_FROM_PTE(PointerPte));

    if ((PointerPte->u.Soft.Prototype == 0) && (PointerPte->u.Soft.Transition == 1))
        return MI_PFN_ELEMENT(PointerPte->u.Trans.PageFrameNumber);

    return NULL;
}

/**
 * @brief Tells whether anything still holds a data control area.
 * @remarks The PFN lock must be held.
 */
static
BOOLEAN
MiIsDataFileMapReferenced(
    _In_ PCONTROL_AREA ControlArea)
{
    MI_ASSERT_PFN_LOCK_HELD();

    return (BOOLEAN)((ControlArea->NumberOfSectionReferences != 0) ||
                     (ControlArea->NumberOfMappedViews != 0) ||
                     (ControlArea->FlushInProgressCount != 0));
}

/**
 * @brief Fills prototype PTEs so that they point back to their subsection.
 *
 * @param[in] Subsection
 * Subsection owning the prototype PTEs.
 *
 * @param[in] FirstIndex
 * Index of the first prototype PTE to fill.
 *
 * @param[in] Count
 * Number of prototype PTEs to fill.
 */
static
VOID
MiInitializeSubsectionPtes(
    _In_ PMSUBSECTION Subsection,
    _In_ ULONG FirstIndex,
    _In_ ULONG Count)
{
    MMPTE TempPte;

    MI_MAKE_SUBSECTION_PTE(&TempPte, Subsection);
    TempPte.u.Subsect.Protection = Subsection->u.SubsectionFlags.Protection;
    ASSERT(MiSubsectionFromPte(&TempPte) == Subsection);

#ifdef _WIN64
    RtlFillMemoryUlonglong(&Subsection->SubsectionBase[FirstIndex],
                           Count * sizeof(MMPTE),
                           TempPte.u.Long);
#else
    RtlFillMemoryUlong(&Subsection->SubsectionBase[FirstIndex],
                       Count * sizeof(MMPTE),
                       TempPte.u.Long);
#endif
}

/**
 * @brief Gives a subsection the prototype PTEs for a range of file pages.
 *
 * @param[in,out] Subsection
 * Subsection without prototype PTEs yet.
 *
 * @param[in] StartingPage
 * File page described by the first prototype PTE.
 *
 * @param[in] Count
 * Number of prototype PTEs in use.
 *
 * @param[in] Capacity
 * Number of prototype PTEs to allocate, the rest is kept for growth.
 *
 * @return FALSE if paged pool is exhausted.
 */
static
BOOLEAN
MiAllocateSubsectionPtes(
    _Inout_ PMSUBSECTION Subsection,
    _In_ ULONG StartingPage,
    _In_ ULONG Count,
    _In_ ULONG Capacity)
{
    ASSERT((Count != 0) && (Count <= Capacity) && (Capacity <= MI_SUBSECTION_PTES));

    Subsection->SubsectionBase = ExAllocatePoolWithTag(PagedPool,
                                                       Capacity * sizeof(MMPTE),
                                                       TAG_MAPPED_PROTOTYPES);
    if (!Subsection->SubsectionBase)
        return FALSE;

    Subsection->StartingSector = StartingPage;
    Subsection->PtesInSubsection = Count;
    Subsection->UnusedPtes = Capacity - Count;
    MiInitializeSubsectionPtes(Subsection, 0, Count);
    return TRUE;
}

/**
 * @brief Frees a data control area and everything hanging off it.
 * @remarks The file object reference is left alone.
 */
static
VOID
MiFreeDataFileMap(
    _In_ PCONTROL_AREA ControlArea)
{
    PMSUBSECTION Subsection, Next;

    Subsection = (PMSUBSECTION)(ControlArea + 1);
    do
    {
        Next = (PMSUBSECTION)Subsection->NextSubsection;

        if (Subsection->SubsectionBase)
            ExFreePoolWithTag(Subsection->SubsectionBase, TAG_MAPPED_PROTOTYPES);

        if (Subsection != (PMSUBSECTION)(ControlArea + 1))
            ExFreePoolWithTag(Subsection, TAG_MAPPED_SUBSECTION);

        Subsection = Next;
    } while (Subsection);

    if (ControlArea->Segment)
        ExFreePoolWithTag(ControlArea->Segment, TAG_MAPPED_SEGMENT);

    ExFreePoolWithTag(ControlArea, TAG_MAPPED_CONTROL_AREA);
}

/**
 * @brief Builds a data control area describing a file.
 *
 * @param[in] FileObject
 * File the control area describes.
 *
 * @param[in] Size
 * Size of the segment in bytes.
 *
 * @param[out] OutControlArea
 * Receives the control area, not yet known to the file.
 */
static
NTSTATUS
MiAllocateDataFileMap(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG64 Size,
    _Out_ PCONTROL_AREA *OutControlArea)
{
    PCONTROL_AREA ControlArea;
    PMSUBSECTION Subsection, Previous;
    PSEGMENT Segment;
    ULONG PteCount, Page, Count;

    PteCount = (ULONG)((Size + PAGE_SIZE - 1) >> PAGE_SHIFT);

    ControlArea = ExAllocatePoolWithTag(NonPagedPool,
                                        sizeof(CONTROL_AREA) + sizeof(MSUBSECTION),
                                        TAG_MAPPED_CONTROL_AREA);
    if (!ControlArea)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(ControlArea, sizeof(CONTROL_AREA) + sizeof(MSUBSECTION));

    Segment = ExAllocatePoolWithTag(NonPagedPool, sizeof(SEGMENT), TAG_MAPPED_SEGMENT);
    if (!Segment)
    {
        ExFreePoolWithTag(ControlArea, TAG_MAPPED_CONTROL_AREA);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Segment, sizeof(SEGMENT));
    Segment->ControlArea = ControlArea;
    Segment->TotalNumberOfPtes = PteCount;
    Segment->NonExtendedPtes = PteCount;
    Segment->SizeOfSegment = Size;
    Segment->SegmentPteTemplate.u.Soft.Protection = MM_READWRITE;
    Segment->u1.CreatingProcess = PsGetCurrentProcess();

    ControlArea->Segment = Segment;
    ControlArea->FilePointer = FileObject;
    ControlArea->u.Flags.File = 1;

    /* Views pick their own protection, the file data itself is always writable */
    Subsection = (PMSUBSECTION)(ControlArea + 1);
    Page = 0;
    for (;;)
    {
        Subsection->ControlArea = ControlArea;
        Subsection->u.SubsectionFlags.Protection = MM_READWRITE;
        Subsection->StartingSector = Page;

        Count = min(PteCount - Page, MI_SUBSECTION_PTES);
        if ((Count != 0) && !MiAllocateSubsectionPtes(Subsection, Page, Count, Count))
            goto Failed;

        Page += Count;
        if (Page == PteCount)
            break;

        Previous = Subsection;
        Subsection = ExAllocatePoolWithTag(NonPagedPool, sizeof(MSUBSECTION), TAG_MAPPED_SUBSECTION);
        if (!Subsection)
            goto Failed;

        RtlZeroMemory(Subsection, sizeof(MSUBSECTION));
        Previous->NextSubsection = (PSUBSECTION)Subsection;
    }

    Segment->PrototypePte = ((PMSUBSECTION)(ControlArea + 1))->SubsectionBase;
    *OutControlArea = ControlArea;
    return STATUS_SUCCESS;

Failed:
    MiFreeDataFileMap(ControlArea);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/**
 * @brief Makes a data control area describe at least a given size.
 *
 * @param[in] ControlArea
 * Referenced data control area.
 *
 * @param[in] NewSize
 * Size in bytes the segment must reach.
 */
static
NTSTATUS
MiExtendDataFileMap(
    _In_ PCONTROL_AREA ControlArea,
    _In_ ULONG64 NewSize)
{
    PSEGMENT Segment = ControlArea->Segment;
    PMSUBSECTION Last, Subsection;
    ULONG OldPtes, Needed, Count, Capacity;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG64 NewPtes;
    KIRQL OldIrql;

    NewPtes = (NewSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (NewPtes > MAXULONG)
        return STATUS_SECTION_TOO_BIG;

    KeAcquireGuardedMutex(&MiDataFileExtendLock);

    OldPtes = Segment->TotalNumberOfPtes;
    if (NewSize <= Segment->SizeOfSegment)
    {
        KeReleaseGuardedMutex(&MiDataFileExtendLock);
        return STATUS_SUCCESS;
    }

    Last = (PMSUBSECTION)(ControlArea + 1);
    while (Last->NextSubsection)
        Last = (PMSUBSECTION)Last->NextSubsection;

    Needed = (ULONG)(NewPtes - OldPtes);

    /* Room left in the last subsection comes first */
    Count = min(Needed, Last->UnusedPtes);
    if (Count != 0)
    {
        MiInitializeSubsectionPtes(Last, Last->PtesInSubsection, Count);

        OldIrql = MiAcquirePfnLock();
        Last->PtesInSubsection += Count;
        Last->UnusedPtes -= Count;
        Segment->TotalNumberOfPtes += Count;
        Segment->SizeOfSegment = min(NewSize, (ULONG64)Segment->TotalNumberOfPtes << PAGE_SHIFT);
        MiReleasePfnLock(OldIrql);

        Needed -= Count;
    }

    while (Needed != 0)
    {
        /* A file that keeps growing gets more room each time */
        Count = min(Needed, MI_SUBSECTION_PTES);
        Capacity = max(Count, min(Segment->TotalNumberOfPtes, MI_SUBSECTION_PTES));

        Subsection = ExAllocatePoolWithTag(NonPagedPool, sizeof(MSUBSECTION), TAG_MAPPED_SUBSECTION);
        if (!Subsection)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(Subsection, sizeof(MSUBSECTION));
        Subsection->ControlArea = ControlArea;
        Subsection->u.SubsectionFlags.Protection = Last->u.SubsectionFlags.Protection;

        if (!MiAllocateSubsectionPtes(Subsection, Segment->TotalNumberOfPtes, Count, Capacity))
        {
            ExFreePoolWithTag(Subsection, TAG_MAPPED_SUBSECTION);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        OldIrql = MiAcquirePfnLock();
        Last->NextSubsection = (PSUBSECTION)Subsection;
        Segment->TotalNumberOfPtes += Count;
        Segment->SizeOfSegment = min(NewSize, (ULONG64)Segment->TotalNumberOfPtes << PAGE_SHIFT);
        MiReleasePfnLock(OldIrql);

        Last = Subsection;
        Needed -= Count;
    }

    /* Growth inside the last page adds no prototype PTE */
    OldIrql = MiAcquirePfnLock();
    Segment->SizeOfSegment = min(NewSize, (ULONG64)Segment->TotalNumberOfPtes << PAGE_SHIFT);
    MiReleasePfnLock(OldIrql);

    KeReleaseGuardedMutex(&MiDataFileExtendLock);

    /* System views reaching past the old end can use the new pages now */
    if (Segment->TotalNumberOfPtes != OldPtes)
        MiExtendSystemViews(ControlArea, OldPtes);

    return Status;
}

/**
 * @brief Gets how many bytes of a run of file pages lie before the end of the segment.
 *
 * @param[in] Segment
 * Segment of a data control area.
 *
 * @param[in] FileOffset
 * File offset of the first page, inside the segment.
 *
 * @param[in] PageCount
 * Number of pages in the run.
 *
 * @remarks The PFN lock must be held.
 */
static
ULONG
MiGetDataFileReadLength(
    _In_ PSEGMENT Segment,
    _In_ LONGLONG FileOffset,
    _In_ ULONG PageCount)
{
    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT((ULONG64)FileOffset < Segment->SizeOfSegment);

    return (ULONG)min((ULONG64)PageCount << PAGE_SHIFT,
                      Segment->SizeOfSegment - (ULONG64)FileOffset);
}

/**
 * @brief Sets up a free page to receive file data for a prototype PTE.
 *
 * @param[in] PageFrameIndex
 * Page taken off a free, zeroed or standby list.
 *
 * @param[in] PointerProtoPte
 * Prototype PTE holding a subsection PTE.
 *
 * @remarks The PFN lock must be held. The page stays in transition, referenced by
 * the read, until MiCompleteMappedRead runs.
 */
static
VOID
MiInitializeReadPage(
    _In_ PFN_NUMBER PageFrameIndex,
    _In_ PMMPTE PointerProtoPte)
{
    MMPTE TempPte;
    PMMPFN Pfn1;

    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(PointerProtoPte->u.Hard.Valid == 0);
    ASSERT(PointerProtoPte->u.Soft.Prototype == 1);

    MiInitializePfn(PageFrameIndex, PointerProtoPte, FALSE);

    Pfn1 = MI_PFN_ELEMENT(PageFrameIndex);
    Pfn1->u2.ShareCount = 0;
    Pfn1->u3.e1.PageLocation = TransitionPage;
    Pfn1->u3.e1.PrototypePte = 1;
    Pfn1->u3.e1.ReadInProgress = 1;
    Pfn1->u1.Event = NULL;

    MI_MAKE_TRANSITION_PTE(&TempPte, PageFrameIndex, PointerProtoPte->u.Subsect.Protection);
    MI_WRITE_INVALID_PTE(PointerProtoPte, TempPte);
}

/**
 * @brief Finishes a read started with MiInitializeReadPage.
 *
 * @param[in] Pages
 * Pages that were read.
 *
 * @param[in] PageCount
 * Number of pages.
 *
 * @param[in] Status
 * Result of the read.
 *
 * @remarks Pages go to standby when the read worked. Otherwise their prototype PTEs
 * get the file backing back and the pages are freed.
 */
static
VOID
MiCompleteMappedRead(
    _In_reads_(PageCount) PPFN_NUMBER Pages,
    _In_ ULONG PageCount,
    _In_ NTSTATUS Status)
{
    PMMPFN Pfn1;
    KIRQL OldIrql;
    ULONG i;

    OldIrql = MiAcquirePfnLock();

    for (i = 0; i < PageCount; i++)
    {
        Pfn1 = MI_PFN_ELEMENT(Pages[i]);
        ASSERT(Pfn1->u3.e1.ReadInProgress == 1);
        ASSERT(Pfn1->u3.e1.PrototypePte == 1);

        Pfn1->u3.e1.ReadInProgress = 0;
        if (Pfn1->u1.Event)
        {
            KeSetEvent(Pfn1->u1.Event, IO_NO_INCREMENT, FALSE);
            Pfn1->u1.Event = NULL;
        }

        /* A control area deleted meanwhile already took the page off its prototype PTE */
        if (!NT_SUCCESS(Status) && !MI_IS_PFN_DELETED(Pfn1))
        {
            ASSERT(Pfn1->PteAddress->u.Soft.Transition == 1);
            MI_WRITE_INVALID_PTE(Pfn1->PteAddress, Pfn1->OriginalPte);
            MiDecrementShareCount(MI_PFN_ELEMENT(Pfn1->u4.PteFrame), Pfn1->u4.PteFrame);
            MI_SET_PFN_DELETED(Pfn1);
        }

        MiDecrementReferenceCount(Pfn1, Pages[i]);
    }

    MiReleasePfnLock(OldIrql);
}

/**
 * @brief Reads file data into pages prepared with MiInitializeReadPage.
 *
 * @param[in] FileObject
 * File to read from.
 *
 * @param[in] FileOffset
 * File offset of the first page.
 *
 * @param[in] Pages
 * Pages to fill, in file order.
 *
 * @param[in] PageCount
 * Number of pages.
 *
 * @param[in] ValidLength
 * Bytes that hold file data, everything after reads as zeroes.
 *
 * @return Status of the read.
 */
static
NTSTATUS
MiReadMappedPages(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_reads_(PageCount) PPFN_NUMBER Pages,
    _In_ ULONG PageCount,
    _In_ ULONG ValidLength)
{
    UCHAR MdlBuffer[sizeof(MDL) + MI_MAPPED_IO_PAGES * sizeof(PFN_NUMBER)];
    PEPROCESS Process = PsGetCurrentProcess();
    PMDL Mdl = (PMDL)MdlBuffer;
    NTSTATUS Status = STATUS_SUCCESS;
    IO_STATUS_BLOCK IoStatus;
    ULONG_PTR Filled = 0;
    ULONG ReadPages, i;
    PVOID Mapping;
    KEVENT Event;
    KIRQL OldIrql;

    ASSERT((PageCount != 0) && (PageCount <= MI_MAPPED_IO_PAGES));
    ASSERT(ValidLength <= (PageCount << PAGE_SHIFT));

    ReadPages = BYTES_TO_PAGES(ValidLength);
    if (ReadPages != 0)
    {
        MmInitializeMdl(Mdl, NULL, ReadPages << PAGE_SHIFT);
        Mdl->MdlFlags |= MDL_PAGES_LOCKED | MDL_IO_PAGE_READ;
        RtlCopyMemory(MmGetMdlPfnArray(Mdl), Pages, ReadPages * sizeof(PFN_NUMBER));

        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        KeRaiseIrql(APC_LEVEL, &OldIrql);

        Status = IoPageRead(FileObject, Mdl, FileOffset, &Event, &IoStatus);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, WrPageIn, KernelMode, FALSE, NULL);
            Status = IoStatus.Status;
        }

        if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
            MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);

        KeLowerIrql(OldIrql);

        if (Status == STATUS_END_OF_FILE)
            Status = STATUS_SUCCESS;
        else if (NT_SUCCESS(Status))
            Filled = min(IoStatus.Information, ValidLength);
    }

    /* Whatever the file did not fill reads as zeroes */
    if (NT_SUCCESS(Status))
    {
        for (i = (ULONG)(Filled >> PAGE_SHIFT); i < PageCount; i++)
        {
            if ((i == (Filled >> PAGE_SHIFT)) && (Filled & (PAGE_SIZE - 1)))
            {
                Mapping = MiMapPageInHyperSpace(Process, Pages[i], &OldIrql);
                RtlZeroMemory((PVOID)((ULONG_PTR)Mapping + (Filled & (PAGE_SIZE - 1))),
                              PAGE_SIZE - (Filled & (PAGE_SIZE - 1)));
                MiUnmapPageInHyperSpace(Process, Mapping, OldIrql);
            }
            else
            {
                MiZeroPhysicalPage(Pages[i]);
            }
        }
    }
    else
    {
        DPRINT1("Read of %wZ at 0x%I64x failed: 0x%lx\n",
                &FileObject->FileName, FileOffset->QuadPart, Status);
    }

    MiCompleteMappedRead(Pages, PageCount, Status);
    return Status;
}

/**
 * @brief Takes a run of modified pages of a subsection for writing.
 *
 * @param[in] Subsection
 * Subsection to look in.
 *
 * @param[in,out] Index
 * Where to start looking, receives the index of the first page taken.
 *
 * @param[in] LastIndex
 * Index to stop before.
 *
 * @param[out] Pages
 * Receives up to MI_MAPPED_IO_PAGES pages.
 *
 * @param[out] Busy
 * Set when the first modified page is already being written.
 *
 * @param[in] OldIrql
 * IRQL the PFN lock was acquired from, or MM_NOIRQL if the prototype PTEs
 * are known to be mapped.
 *
 * @return Number of pages taken. Each one is referenced and marked as being written.
 *
 * @remarks The PFN lock must be held.
 */
static
ULONG
MiTakeModifiedRun(
    _In_ PMSUBSECTION Subsection,
    _Inout_ PULONG Index,
    _In_ ULONG LastIndex,
    _Out_writes_(MI_MAPPED_IO_PAGES) PPFN_NUMBER Pages,
    _Out_ PBOOLEAN Busy,
    _In_ KIRQL OldIrql)
{
    PMMPTE PointerPte;
    ULONG Count = 0;
    MMPTE TempPte;
    PMMPFN Pfn1;

    MI_ASSERT_PFN_LOCK_HELD();
    *Busy = FALSE;

    while (((*Index + Count) < LastIndex) && (Count < MI_MAPPED_IO_PAGES))
    {
        PointerPte = &Subsection->SubsectionBase[*Index + Count];
        if ((OldIrql != MM_NOIRQL) && ((Count == 0) || MiIsPteOnPdeBoundary(PointerPte)))
            MiMakeSystemAddressValidPfn(PointerPte, OldIrql);

        TempPte = *PointerPte;
        Pfn1 = MiGetResidentPfn(&TempPte);
        if (!Pfn1 || !Pfn1->u3.e1.Modified || Pfn1->u3.e1.ReadInProgress)
        {
            if (Count != 0)
                break;

            (*Index)++;
            continue;
        }

        if (Pfn1->u3.e1.WriteInProgress)
        {
            if (Count == 0)
                *Busy = TRUE;
            break;
        }

        /* Off the lists while being written, but still resident */
        if (Pfn1->u3.e1.PageLocation == ModifiedPageList)
        {
            MiUnlinkPageFromList(Pfn1);
            Pfn1->u3.e1.PageLocation = TransitionPage;
        }

        Pfn1->u3.e2.ReferenceCount++;
        Pfn1->u3.e1.WriteInProgress = 1;
        Pfn1->u3.e1.Modified = 0;

        Pages[Count++] = MiGetPfnEntryIndex(Pfn1);
    }

    return Count;
}

/**
 * @brief Writes pages taken with MiTakeModifiedRun to their file.
 *
 * @param[in] FileObject
 * File to write to.
 *
 * @param[in] FileOffset
 * File offset of the first page.
 *
 * @param[in] Pages
 * Pages to write, in file order.
 *
 * @param[in] PageCount
 * Number of pages.
 *
 * @param[in] ModifiedWriter
 * TRUE when the caller does not hold the file locks, they are tried without waiting.
 *
 * @return Status of the write, STATUS_CANT_WAIT if the file is busy.
 */
static
NTSTATUS
MiWriteMappedPages(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_reads_(PageCount) PPFN_NUMBER Pages,
    _In_ ULONG PageCount,
    _In_ BOOLEAN ModifiedWriter)
{
    UCHAR MdlBuffer[sizeof(MDL) + MI_MAPPED_IO_PAGES * sizeof(PFN_NUMBER)];
    PERESOURCE ResourceToRelease = NULL;
    PMDL Mdl = (PMDL)MdlBuffer;
    LARGE_INTEGER EndingOffset;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    KEVENT Event;

    ASSERT((PageCount != 0) && (PageCount <= MI_MAPPED_IO_PAGES));

    MmInitializeMdl(Mdl, NULL, (SIZE_T)PageCount << PAGE_SHIFT);
    Mdl->MdlFlags |= MDL_PAGES_LOCKED;
    RtlCopyMemory(MmGetMdlPfnArray(Mdl), Pages, PageCount * sizeof(PFN_NUMBER));

    /* The file system sees a paging write at passive level, only APCs are kept out */
    KeEnterCriticalRegion();

    if (ModifiedWriter)
    {
        EndingOffset.QuadPart = FileOffset->QuadPart + ((LONGLONG)PageCount << PAGE_SHIFT);
        Status = FsRtlAcquireFileForModWriteEx(FileObject, &EndingOffset, &ResourceToRelease);
        if (!NT_SUCCESS(Status))
        {
            KeLeaveCriticalRegion();
            return Status;
        }

        IoSetTopLevelIrp((PIRP)FSRTL_MOD_WRITE_TOP_LEVEL_IRP);
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Status = IoSynchronousPageWrite(FileObject, Mdl, FileOffset, &Event, &IoStatus);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, WrPageOut, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
        MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);

    if (ModifiedWriter)
    {
        IoSetTopLevelIrp(NULL);
        if (ResourceToRelease)
            FsRtlReleaseFileForModWrite(FileObject, ResourceToRelease);
    }

    KeLeaveCriticalRegion();

    /* Pages past the end of the file have nothing to go to */
    if (Status == STATUS_END_OF_FILE)
        Status = STATUS_SUCCESS;

    return Status;
}

/**
 * @brief Finishes a write of pages taken with MiTakeModifiedRun.
 *
 * @param[in] Pages
 * Pages that were written.
 *
 * @param[in] PageCount
 * Number of pages.
 *
 * @param[in] Status
 * Result of the write, the pages stay modified when it failed.
 */
static
VOID
MiCompleteMappedWrite(
    _In_reads_(PageCount) PPFN_NUMBER Pages,
    _In_ ULONG PageCount,
    _In_ NTSTATUS Status)
{
    PMMPFN Pfn1;
    KIRQL OldIrql;
    ULONG i;

    OldIrql = MiAcquirePfnLock();

    for (i = 0; i < PageCount; i++)
    {
        Pfn1 = MI_PFN_ELEMENT(Pages[i]);
        ASSERT(Pfn1->u3.e1.WriteInProgress == 1);
        ASSERT(Pfn1->u3.e2.ReferenceCount != 0);

        Pfn1->u3.e1.WriteInProgress = 0;
        if (!NT_SUCCESS(Status))
            Pfn1->u3.e1.Modified = 1;

        MiDecrementReferenceCount(Pfn1, Pages[i]);
    }

    MiReleasePfnLock(OldIrql);
}

/**
 * @brief Writes the modified pages in a range of a data control area to the file.
 *
 * @param[in] ControlArea
 * Referenced or owned data control area.
 *
 * @param[in] StartPage
 * First file page.
 *
 * @param[in] EndPage
 * File page after the last one.
 *
 * @param[in] ModifiedWriter
 * TRUE when the caller does not hold the file locks.
 *
 * @param[out] PagesWritten
 * Receives the number of pages written.
 *
 * @return The first failure, or STATUS_SUCCESS.
 */
static
NTSTATUS
MiFlushDataFileMap(
    _In_ PCONTROL_AREA ControlArea,
    _In_ ULONG64 StartPage,
    _In_ ULONG64 EndPage,
    _In_ BOOLEAN ModifiedWriter,
    _Out_ PULONG PagesWritten)
{
    PFN_NUMBER Pages[MI_MAPPED_IO_PAGES];
    NTSTATUS Status = STATUS_SUCCESS, WriteStatus;
    LARGE_INTEGER FileOffset, Delay;
    ULONG Index, LastIndex, Count;
    PMSUBSECTION Subsection;
    BOOLEAN Busy;
    KIRQL OldIrql;

    *PagesWritten = 0;
    Delay.QuadPart = -10 * 10000LL;

    OldIrql = MiAcquirePfnLock();

    for (Subsection = (PMSUBSECTION)(ControlArea + 1);
         Subsection != NULL;
         Subsection = (PMSUBSECTION)Subsection->NextSubsection)
    {
        if (((ULONG64)Subsection->StartingSector + Subsection->PtesInSubsection) <= StartPage)
            continue;

        if (Subsection->StartingSector >= EndPage)
            break;

        Index = (StartPage > Subsection->StartingSector) ?
                (ULONG)(StartPage - Subsection->StartingSector) : 0;
        LastIndex = (ULONG)min(EndPage - Subsection->StartingSector,
                               Subsection->PtesInSubsection);

        while (Index < LastIndex)
        {
            Count = MiTakeModifiedRun(Subsection, &Index, LastIndex, Pages, &Busy, OldIrql);
            if (Busy)
            {
                /* The page gets written by someone else, it may be modified again after */
                MiReleasePfnLock(OldIrql);
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
                OldIrql = MiAcquirePfnLock();
                continue;
            }

            if (Count == 0)
                break;

            MiReleasePfnLock(OldIrql);

            FileOffset.QuadPart = ((LONGLONG)Subsection->StartingSector + Index) << PAGE_SHIFT;
            WriteStatus = MiWriteMappedPages(ControlArea->FilePointer,
                                             &FileOffset,
                                             Pages,
                                             Count,
                                             ModifiedWriter);
            MiCompleteMappedWrite(Pages, Count, WriteStatus);

            if (NT_SUCCESS(WriteStatus))
                *PagesWritten += Count;
            else if (NT_SUCCESS(Status))
                Status = WriteStatus;

            Index += Count;
            OldIrql = MiAcquirePfnLock();
        }
    }

    MiReleasePfnLock(OldIrql);
    return Status;
}

/**
 * @brief Maps every prototype PTE page of a data control area.
 *
 * @return TRUE if the PFN lock had to be released on the way.
 *
 * @remarks The PFN lock must be held.
 */
static
BOOLEAN
MiMakeDataFilePtesValid(
    _In_ PCONTROL_AREA ControlArea,
    _In_ KIRQL OldIrql)
{
    PMMPTE PointerPte, LastPte;
    PMSUBSECTION Subsection;
    BOOLEAN Released = FALSE;

    for (Subsection = (PMSUBSECTION)(ControlArea + 1);
         Subsection != NULL;
         Subsection = (PMSUBSECTION)Subsection->NextSubsection)
    {
        if (Subsection->PtesInSubsection == 0)
            continue;

        PointerPte = PAGE_ALIGN(Subsection->SubsectionBase);
        LastPte = &Subsection->SubsectionBase[Subsection->PtesInSubsection];
        while (PointerPte < LastPte)
        {
            if (MiMakeSystemAddressValidPfn(PointerPte, OldIrql))
                Released = TRUE;

            PointerPte = (PMMPTE)((ULONG_PTR)PointerPte + PAGE_SIZE);
        }
    }

    return Released;
}

/**
 * @brief Tells whether a data control area still has data to write.
 * @remarks The PFN lock must be held and the prototype PTEs mapped.
 */
static
BOOLEAN
MiHasModifiedDataFilePages(
    _In_ PCONTROL_AREA ControlArea)
{
    PMSUBSECTION Subsection;
    PMMPFN Pfn1;
    ULONG i;

    for (Subsection = (PMSUBSECTION)(ControlArea + 1);
         Subsection != NULL;
         Subsection = (PMSUBSECTION)Subsection->NextSubsection)
    {
        for (i = 0; i < Subsection->PtesInSubsection; i++)
        {
            Pfn1 = MiGetResidentPfn(&Subsection->SubsectionBase[i]);
            if (Pfn1 && (Pfn1->u3.e1.Modified || Pfn1->u3.e1.WriteInProgress))
                return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief Takes the resident pages away from a data control area being deleted.
 *
 * @remarks The PFN lock must be held and the prototype PTEs mapped. Pages still
 * used for I/O or locked are freed when their last reference goes away.
 */
static
VOID
MiDetachDataFilePages(
    _In_ PCONTROL_AREA ControlArea)
{
    PFN_NUMBER PageFrameIndex;
    PMSUBSECTION Subsection;
    PMMPTE PointerPte;
    PMMPFN Pfn1;
    ULONG i;

    for (Subsection = (PMSUBSECTION)(ControlArea + 1);
         Subsection != NULL;
         Subsection = (PMSUBSECTION)Subsection->NextSubsection)
    {
        for (i = 0; i < Subsection->PtesInSubsection; i++)
        {
            PointerPte = &Subsection->SubsectionBase[i];
            ASSERT(PointerPte->u.Hard.Valid == 0);

            if (PointerPte->u.Soft.Prototype == 1)
                continue;

            /* Shared image pages may only be left in the paging file */
            if (PointerPte->u.Soft.Transition == 0)
            {
                MiReleasePageFileSpace(*PointerPte);
                continue;
            }

            PageFrameIndex = PointerPte->u.Trans.PageFrameNumber;
            Pfn1 = MI_PFN_ELEMENT(PageFrameIndex);
            ASSERT(Pfn1->PteAddress == PointerPte);

            MiDecrementShareCount(MI_PFN_ELEMENT(Pfn1->u4.PteFrame), Pfn1->u4.PteFrame);
            MI_SET_PFN_DELETED(Pfn1);

            if (Pfn1->u3.e2.ReferenceCount == 0)
            {
                ASSERT((Pfn1->u3.e1.PageLocation == StandbyPageList) ||
                       (Pfn1->u3.e1.PageLocation == ModifiedPageList));

                MiUnlinkPageFromList(Pfn1);

                /* Shared image pages may have a copy in the paging file */
                MiReleasePageFileSpace(Pfn1->OriginalPte);

                /* Active for a moment, the free list takes it from there */
                Pfn1->u3.e1.PageLocation = ActiveAndValid;
                MiInsertPageInFreeList(PageFrameIndex);
            }
        }
    }
}

/**
 * @brief Writes out and deletes a file control area nobody references.
 *
 * @param[in] ControlArea
 * Control area owned through its BeingPurged flag.
 *
 * @param[in] WriteModified
 * TRUE to write modified pages first, the way the modified page writer does.
 *
 * @param[in] Final
 * TRUE to give up modified data that still could not be written.
 *
 * @return FALSE when the cleanup must be tried again later.
 *
 * @remarks Writes take the file locks like the modified page writer, a paging write
 * without them races with others extending the valid data length. Image pages are
 * never written back, modified ones are simply dropped.
 */
static
BOOLEAN
MiDeleteDataFileMap(
    _In_ PCONTROL_AREA ControlArea,
    _In_ BOOLEAN WriteModified,
    _In_ BOOLEAN Final)
{
    PFILE_OBJECT FileObject = ControlArea->FilePointer;
    BOOLEAN Image = (BOOLEAN)ControlArea->u.Flags.Image;
    NTSTATUS Status;
    ULONG Written;
    KIRQL OldIrql;

    ASSERT(ControlArea->u.Flags.BeingPurged == 1);

    if (Image)
        Final = TRUE;

    if (WriteModified && !Image)
    {
        /* A busy file is no reason to give up its data */
        Status = MiFlushDataFileMap(ControlArea, 0, (~0ULL), TRUE, &Written);
        if (Status == STATUS_CANT_WAIT)
            Final = FALSE;
    }

    OldIrql = MiAcquirePfnLock();

    while (MiMakeDataFilePtesValid(ControlArea, OldIrql))
        NOTHING;

    if (MiIsDataFileMapReferenced(ControlArea))
    {
        /* Picked up again, whoever lets go last brings it back */
        ControlArea->u.Flags.BeingPurged = 0;
        MiReleasePfnLock(OldIrql);
        return TRUE;
    }

    if ((ControlArea->ModifiedWriteCount != 0) ||
        (!Final && MiHasModifiedDataFilePages(ControlArea)))
    {
        MiReleasePfnLock(OldIrql);
        return FALSE;
    }

    if (!Image && MiHasModifiedDataFilePages(ControlArea))
        DPRINT1("Modified data of control area %p could not be written\n", ControlArea);

    /* Nothing reaches it once the file forgets it */
    ControlArea->u.Flags.BeingDeleted = 1;
    if (Image)
    {
        ASSERT(FileObject->SectionObjectPointer->ImageSectionObject == ControlArea);
        FileObject->SectionObjectPointer->ImageSectionObject = NULL;
    }
    else
    {
        ASSERT(FileObject->SectionObjectPointer->DataSectionObject == ControlArea);
        FileObject->SectionObjectPointer->DataSectionObject = NULL;
    }

    if (ControlArea->DereferenceList.Flink)
    {
        RemoveEntryList(&ControlArea->DereferenceList);
        ControlArea->DereferenceList.Flink = NULL;
    }

    MiDetachDataFilePages(ControlArea);
    MiReleasePfnLock(OldIrql);

    if (Image)
        MiFreeImageFileMap(ControlArea);
    else
        MiFreeDataFileMap(ControlArea);

    ObDereferenceObject(FileObject);
    return TRUE;
}

/**
 * @brief Ages and deletes unused file control areas.
 *
 * Resident pages stay available on the standby list while their control area
 * is retained. This lets short-lived mappings reuse file data without keeping
 * physical memory away from other consumers.
 */
static
VOID
NTAPI
MiDataFileCleanupThread(
    _In_ PVOID Context)
{
    PCONTROL_AREA ControlArea;
    LARGE_INTEGER Delay, RetainDelay;
    PLIST_ENTRY Entry;
    ULONG Attempt;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Context);

    Delay.QuadPart = -100 * 10000LL;
    RetainDelay.QuadPart = -60 * 1000 * 10000LL;

    for (;;)
    {
        KeWaitForSingleObject(&MiDataFileCleanupEvent, Executive, KernelMode, FALSE, NULL);

        for (;;)
        {
            KeDelayExecutionThread(KernelMode, FALSE, &RetainDelay);

ScanList:
            OldIrql = MiAcquirePfnLock();

            if (IsListEmpty(&MiDataFileCleanupList))
            {
                MiReleasePfnLock(OldIrql);
                break;
            }

            for (Entry = MiDataFileCleanupList.Flink;
                 Entry != &MiDataFileCleanupList;
                 Entry = Entry->Flink)
            {
                ControlArea = CONTAINING_RECORD(Entry, CONTROL_AREA, DereferenceList);

                if (MiIsDataFileMapReferenced(ControlArea) || ControlArea->u.Flags.Accessed)
                {
                    ControlArea->u.Flags.Accessed = 0;
                    continue;
                }

                ASSERT(ControlArea->u.Flags.BeingPurged == 0);
                RemoveEntryList(Entry);
                ControlArea->DereferenceList.Flink = NULL;
                ControlArea->u.Flags.BeingPurged = 1;
                MiReleasePfnLock(OldIrql);

                for (Attempt = 1;
                     !MiDeleteDataFileMap(ControlArea, TRUE, Attempt >= MI_CLEANUP_ATTEMPTS);
                     Attempt++)
                {
                    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
                }

                goto ScanList;
            }

            MiReleasePfnLock(OldIrql);
        }
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Prepares file backed data sections.
 */
CODE_SEG("INIT")
VOID
NTAPI
MiInitializeDataFileMaps(VOID)
{
    HANDLE ThreadHandle;
    NTSTATUS Status;

    InitializeListHead(&MiDataFileCleanupList);
    KeInitializeEvent(&MiDataFileCleanupEvent, SynchronizationEvent, FALSE);
    KeInitializeGuardedMutex(&MiDataFileExtendLock);

    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  MiDataFileCleanupThread,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x7011, Status, 0, 0);
    }

    ZwClose(ThreadHandle);
}

/**
 * @brief Finds the prototype PTE describing a file page.
 *
 * @param[in] ControlArea
 * Data control area of the file.
 *
 * @param[in] PageIndex
 * File page.
 *
 * @param[out] OutSubsection
 * Receives the subsection holding the prototype PTE.
 *
 * @return The prototype PTE, or NULL if the segment does not reach the page.
 */
PMMPTE
NTAPI
MiGetDataFileProtoPte(
    _In_ PCONTROL_AREA ControlArea,
    _In_ ULONG64 PageIndex,
    _Out_opt_ PMSUBSECTION *OutSubsection)
{
    PMSUBSECTION Subsection;

    ASSERT(ControlArea->u.Flags.File == 1);

    for (Subsection = (PMSUBSECTION)(ControlArea + 1);
         Subsection != NULL;
         Subsection = (PMSUBSECTION)Subsection->NextSubsection)
    {
        if (PageIndex < ((ULONG64)Subsection->StartingSector + Subsection->PtesInSubsection))
        {
            if (OutSubsection)
                *OutSubsection = Subsection;

            return &Subsection->SubsectionBase[PageIndex - Subsection->StartingSector];
        }
    }

    return NULL;
}

/**
 * @brief Finds the prototype PTE backing a page of a section view.
 *
 * @param[in] Vad
 * Section view.
 *
 * @param[in] Vpn
 * Virtual page inside the view.
 */
PMMPTE
NTAPI
MiGetViewProtoPte(
    _In_ PMMVAD Vad,
    _In_ ULONG_PTR Vpn)
{
    PMMPTE ProtoPte;
    ULONG64 PageIndex;

    ProtoPte = Vad->FirstPrototypePte + (Vpn - Vad->StartingVpn);
    if (ProtoPte <= Vad->LastContiguousPte)
        return ProtoPte;

    /* The view runs over more than one subsection */
    PageIndex = (((ULONG64)Vad->u2.VadFlags2.FileOffset << 16) >> PAGE_SHIFT) +
                (Vpn - Vad->StartingVpn);
    ProtoPte = MiGetDataFileProtoPte(Vad->ControlArea, PageIndex, NULL);
    ASSERT(ProtoPte != NULL);
    return ProtoPte;
}

/**
 * @brief Gets the data control area of a file, creating it when there is none.
 *
 * @param[in] FileObject
 * File to map.
 *
 * @param[in] Size
 * Size in bytes the segment must cover.
 *
 * @param[in] UserReference
 * TRUE when the new section counts as a user reference.
 *
 * @param[out] OutControlArea
 * Receives the control area with one more section reference.
 */
NTSTATUS
NTAPI
MiReferenceDataFileMap(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG64 Size,
    _In_ BOOLEAN UserReference,
    _Out_ PCONTROL_AREA *OutControlArea)
{
    PSECTION_OBJECT_POINTERS SectionPointers = FileObject->SectionObjectPointer;
    PCONTROL_AREA ControlArea, NewControlArea = NULL;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (((Size + PAGE_SIZE - 1) >> PAGE_SHIFT) > MAXULONG)
        return STATUS_SECTION_TOO_BIG;

    for (;;)
    {
        OldIrql = MiAcquirePfnLock();

        ControlArea = SectionPointers->DataSectionObject;
        if (ControlArea)
        {
            ASSERT(ControlArea->u.Flags.BeingDeleted == 0);

            /* Take it back from the cleanup list */
            if (ControlArea->DereferenceList.Flink)
            {
                RemoveEntryList(&ControlArea->DereferenceList);
                ControlArea->DereferenceList.Flink = NULL;
            }

            ControlArea->u.Flags.Accessed = 1;

            ControlArea->NumberOfSectionReferences++;
            if (UserReference)
                ControlArea->NumberOfUserReferences++;

            MiReleasePfnLock(OldIrql);

            if (NewControlArea)
                MiFreeDataFileMap(NewControlArea);

            Status = MiExtendDataFileMap(ControlArea, Size);
            if (!NT_SUCCESS(Status))
            {
                OldIrql = MiAcquirePfnLock();
                ControlArea->NumberOfSectionReferences--;
                if (UserReference)
                    ControlArea->NumberOfUserReferences--;
                MiCheckControlArea(ControlArea, OldIrql);
                return Status;
            }

            *OutControlArea = ControlArea;
            return STATUS_SUCCESS;
        }

        if (NewControlArea)
        {
            ObReferenceObject(FileObject);
            NewControlArea->NumberOfSectionReferences = 1;
            NewControlArea->NumberOfUserReferences = UserReference ? 1 : 0;
            SectionPointers->DataSectionObject = NewControlArea;
            MiReleasePfnLock(OldIrql);

            *OutControlArea = NewControlArea;
            return STATUS_SUCCESS;
        }

        MiReleasePfnLock(OldIrql);

        Status = MiAllocateDataFileMap(FileObject, Size, &NewControlArea);
        if (!NT_SUCCESS(Status))
            return Status;
    }
}

/**
 * @brief Hands a data control area without references to the cleanup thread.
 * @remarks The PFN lock must be held.
 */
VOID
NTAPI
MiQueueDataFileCleanup(
    _In_ PCONTROL_AREA ControlArea)
{
    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(ControlArea->u.Flags.File == 1);

    if (MiIsDataFileMapReferenced(ControlArea) ||
        ControlArea->u.Flags.BeingPurged)
    {
        return;
    }

    ControlArea->u.Flags.Accessed = 1;

    if (ControlArea->DereferenceList.Flink)
        return;

    InsertTailList(&MiDataFileCleanupList, &ControlArea->DereferenceList);
    KeSetEvent(&MiDataFileCleanupEvent, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief References the data control area of a file for work done without a section.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @return The control area, or NULL if the file has no data section.
 */
PCONTROL_AREA
NTAPI
MiReferenceDataFileMapForIo(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer)
{
    PCONTROL_AREA ControlArea;
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    ControlArea = SectionObjectPointer->DataSectionObject;
    if (ControlArea)
    {
        ASSERT(ControlArea->u.Flags.File == 1);
        ASSERT(ControlArea->FlushInProgressCount != MAXUSHORT);
        ControlArea->u.Flags.Accessed = 1;
        ControlArea->FlushInProgressCount++;
    }

    MiReleasePfnLock(OldIrql);
    return ControlArea;
}

/**
 * @brief Adds a work reference to a data control area already referenced otherwise.
 */
VOID
NTAPI
MiReferenceDataFileMapForIoUnsafe(
    _In_ PCONTROL_AREA ControlArea)
{
    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(ControlArea->u.Flags.File == 1);
    ASSERT(ControlArea->FlushInProgressCount != MAXUSHORT);

    ControlArea->u.Flags.Accessed = 1;
    ControlArea->FlushInProgressCount++;
}

/**
 * @brief Drops a reference taken with MiReferenceDataFileMapForIo.
 *
 * @param[in] ControlArea
 * Referenced data control area.
 *
 * @remarks An unused control area is aged by the cleanup thread so that a
 * subsequent mapping can reuse its resident pages.
 */
VOID
NTAPI
MiDereferenceDataFileMapForIo(
    _In_ PCONTROL_AREA ControlArea)
{
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    ASSERT(ControlArea->FlushInProgressCount != 0);
    ControlArea->FlushInProgressCount--;

    MiCheckControlArea(ControlArea, OldIrql);
}

/**
 * @brief Drops an I/O reference and immediately purges an unused file map.
 *
 * This is reserved for callers whose contract requires the section object
 * pointer to be cleared before they return.
 */
VOID
NTAPI
MiDereferenceDataFileMapForDelete(
    _In_ PCONTROL_AREA ControlArea)
{
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    ASSERT(ControlArea->FlushInProgressCount != 0);
    ControlArea->FlushInProgressCount--;

    if (MiIsDataFileMapReferenced(ControlArea) || ControlArea->u.Flags.BeingPurged)
    {
        MiCheckControlArea(ControlArea, OldIrql);
        return;
    }

    if (ControlArea->DereferenceList.Flink)
    {
        RemoveEntryList(&ControlArea->DereferenceList);
        ControlArea->DereferenceList.Flink = NULL;
    }

    ControlArea->u.Flags.BeingPurged = 1;
    MiReleasePfnLock(OldIrql);

    if (!MiDeleteDataFileMap(ControlArea, FALSE, FALSE))
    {
        OldIrql = MiAcquirePfnLock();
        ControlArea->u.Flags.BeingPurged = 0;
        MiQueueDataFileCleanup(ControlArea);
        MiReleasePfnLock(OldIrql);
    }
}

/**
 * @brief Starts bringing in a file page for a fault.
 *
 * @param[in] PointerProtoPte
 * Prototype PTE holding a subsection PTE.
 *
 * @param[out] PageRead
 * Receives what the read needs once the locks are released.
 *
 * @param[in] Process
 * Faulting process, or a system value for system space.
 *
 * @param[in] OldIrql
 * IRQL to return to when releasing the PFN lock.
 *
 * @return STATUS_MM_PAGE_READ_NEEDED, or STATUS_NO_MEMORY.
 *
 * @remarks Called with the PFN lock held, returns with it released.
 */
NTSTATUS
NTAPI
MiResolveMappedFileFault(
    _In_ PMMPTE PointerProtoPte,
    _Out_ PMI_PAGE_READ PageRead,
    _In_ PEPROCESS Process,
    _In_ KIRQL OldIrql)
{
    PFN_NUMBER PageFrameIndex;
    PMSUBSECTION Subsection;
    PMMPTE LastProtoPte;
    ULONG Color;

    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(PointerProtoPte->u.Hard.Valid == 0);
    ASSERT(PointerProtoPte->u.Soft.Prototype == 1);

    Subsection = MiSubsectionFromPte(PointerProtoPte);
    ASSERT(Subsection->ControlArea->u.Flags.File == 1);
    ASSERT(PointerProtoPte >= Subsection->SubsectionBase);
    ASSERT(PointerProtoPte < &Subsection->SubsectionBase[Subsection->PtesInSubsection]);

    if (Process > HYDRA_PROCESS)
        Color = MI_GET_NEXT_PROCESS_COLOR(Process);
    else
        Color = MI_GET_NEXT_COLOR();

    MI_SET_USAGE(MI_USAGE_SECTION);
    PageFrameIndex = MiRemoveAnyPage(Color);
    if (!PageFrameIndex)
    {
        MiReleasePfnLock(OldIrql);
        return STATUS_NO_MEMORY;
    }

    MiInitializeReadPage(PageFrameIndex, PointerProtoPte);

    /* The file outlives the read even if the section goes away meanwhile */
    PageRead->FileObject = Subsection->ControlArea->FilePointer;
    ObReferenceObject(PageRead->FileObject);
    PageRead->Pages[0] = PageFrameIndex;
    PageRead->PageCount = 1;

    /*
     * Take the pages that follow along, they cost one read instead of a fault each.
     * They live in the same subsection, so their file offsets follow as well.
     */
    LastProtoPte = &Subsection->SubsectionBase[Subsection->PtesInSubsection];
    while ((PageRead->PageCount < MI_READ_CLUSTER_PAGES) &&
           (MmAvailablePages > MmMinimumFreePages))
    {
        PMMPTE NextProtoPte = PointerProtoPte + PageRead->PageCount;

        if (NextProtoPte >= LastProtoPte)
            break;

        /* Prototype PTEs live in paged pool and only the faulting one is known to be there */
        if (((ULONG_PTR)NextProtoPte & (PAGE_SIZE - 1)) == 0)
            break;

        /* Only a page the file still holds is worth reading now */
        if ((NextProtoPte->u.Long == 0) ||
            (NextProtoPte->u.Hard.Valid == 1) ||
            (NextProtoPte->u.Soft.Prototype == 0))
        {
            break;
        }

        PageFrameIndex = MiRemoveAnyPage(MI_GET_NEXT_COLOR());
        if (PageFrameIndex == 0)
            break;

        MiInitializeReadPage(PageFrameIndex, NextProtoPte);
        PageRead->Pages[PageRead->PageCount] = PageFrameIndex;
        PageRead->PageCount++;
    }

    if (Subsection->ControlArea->u.Flags.Image)
    {
        PageRead->ValidLength = MiGetImagePageFileOffset((PSUBSECTION)Subsection,
                                                         PointerProtoPte,
                                                         PageRead->PageCount,
                                                         &PageRead->FileOffset);
    }
    else
    {
        PageRead->FileOffset.QuadPart =
            ((LONGLONG)Subsection->StartingSector +
             (PointerProtoPte - Subsection->SubsectionBase)) << PAGE_SHIFT;
        PageRead->ValidLength = MiGetDataFileReadLength(Subsection->ControlArea->Segment,
                                                        PageRead->FileOffset.QuadPart,
                                                        PageRead->PageCount);
    }

    MiReleasePfnLock(OldIrql);
    return STATUS_MM_PAGE_READ_NEEDED;
}

/**
 * @brief Finishes a fault that has to wait for a page read.
 *
 * @param[in,out] PageRead
 * Read set up while the fault was dispatched.
 *
 * @return STATUS_SUCCESS when the fault should be retried, otherwise the read failure.
 *
 * @remarks Called without any working set lock held.
 */
NTSTATUS
NTAPI
MiCompletePageRead(
    _Inout_ PMI_PAGE_READ PageRead)
{
    NTSTATUS Status;

    ASSERT(!MM_ANY_WS_LOCK_HELD(PsGetCurrentThread()));

    if (PageRead->Collided)
    {
        /* Another thread reads the page, its event wakes the ones queued before us */
        KeWaitForSingleObject(&PageRead->Event, WrPageIn, KernelMode, FALSE, NULL);
        if (PageRead->PreviousEvent)
            KeSetEvent(PageRead->PreviousEvent, IO_NO_INCREMENT, FALSE);

        PageRead->Collided = FALSE;
        return STATUS_SUCCESS;
    }

    Status = MiReadMappedPages(PageRead->FileObject,
                               &PageRead->FileOffset,
                               PageRead->Pages,
                               PageRead->PageCount,
                               PageRead->ValidLength);

    ObDereferenceObject(PageRead->FileObject);
    PageRead->FileObject = NULL;
    return Status;
}

/**
 * @brief Writes one run of pages from the mapped file part of the modified list.
 *
 * @return STATUS_SUCCESS when pages were written, STATUS_NO_MORE_ENTRIES when there
 * was nothing to write, otherwise the write failure.
 */
NTSTATUS
NTAPI
MiWriteModifiedMappedPages(VOID)
{
    PFN_NUMBER Pages[MI_MAPPED_IO_PAGES];
    PCONTROL_AREA ControlArea;
    PMSUBSECTION Subsection;
    LARGE_INTEGER FileOffset;
    PFILE_OBJECT FileObject;
    ULONG Index, Count;
    NTSTATUS Status;
    BOOLEAN Busy;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    if (MmModifiedMappedPageListHead.Total == 0)
    {
        MiReleasePfnLock(OldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }

    Pfn1 = MI_PFN_ELEMENT(MmModifiedMappedPageListHead.Flink);
    ASSERT(Pfn1->OriginalPte.u.Soft.Prototype == 1);

    Subsection = MiSubsectionFromPte(&Pfn1->OriginalPte);
    ControlArea = Subsection->ControlArea;
    ASSERT(ControlArea->u.Flags.BeingDeleted == 0);

    if (ControlArea->u.Flags.Image)
    {
        /* Image data never goes back to its file, the page is kept by the paging file instead */
        MiUnlinkPageFromList(Pfn1);
        MI_MAKE_SOFTWARE_PTE(&Pfn1->OriginalPte, Subsection->u.SubsectionFlags.Protection);
        MiInsertPageInList(&MmModifiedPageListHead, MiGetPfnEntryIndex(Pfn1));
        MiReleasePfnLock(OldIrql);
        return STATUS_SUCCESS;
    }

    Index = (ULONG)(Pfn1->PteAddress - Subsection->SubsectionBase);
    Count = MiTakeModifiedRun(Subsection,
                              &Index,
                              Subsection->PtesInSubsection,
                              Pages,
                              &Busy,
                              MM_NOIRQL);
    ASSERT(Count != 0);

    /* Keeps the control area around until the write is done */
    ControlArea->ModifiedWriteCount++;
    FileObject = ControlArea->FilePointer;
    ObReferenceObject(FileObject);
    FileOffset.QuadPart = ((LONGLONG)Subsection->StartingSector + Index) << PAGE_SHIFT;

    MiReleasePfnLock(OldIrql);

    Status = MiWriteMappedPages(FileObject, &FileOffset, Pages, Count, TRUE);
    MiCompleteMappedWrite(Pages, Count, Status);

    OldIrql = MiAcquirePfnLock();
    ControlArea->ModifiedWriteCount--;
    MiReleasePfnLock(OldIrql);

    ObDereferenceObject(FileObject);
    return Status;
}

/**
 * @brief Writes every modified mapped file page still in memory.
 * @remarks Used at shutdown, while the file systems still work.
 */
VOID
NTAPI
MiWriteAllMappedPages(VOID)
{
    PFN_NUMBER Pages[MI_MAPPED_IO_PAGES];
    PFN_NUMBER PageFrameIndex;
    PCONTROL_AREA ControlArea;
    PMSUBSECTION Subsection;
    LARGE_INTEGER FileOffset;
    PFILE_OBJECT FileObject;
    ULONG Index, Count;
    NTSTATUS Status;
    BOOLEAN Busy;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    for (PageFrameIndex = MmLowestPhysicalPage;
         PageFrameIndex <= MmHighestPhysicalPage;
         PageFrameIndex++)
    {
        OldIrql = MiAcquirePfnLock();

        Pfn1 = MiGetPfnEntry(PageFrameIndex);
        if (!Pfn1 ||
            MI_IS_PFN_DELETED(Pfn1) ||
            !Pfn1->u3.e1.PrototypePte ||
            !Pfn1->u3.e1.Modified ||
            Pfn1->u3.e1.WriteInProgress ||
            Pfn1->u3.e1.ReadInProgress ||
            (Pfn1->OriginalPte.u.Soft.Prototype == 0) ||
            ((Pfn1->u3.e1.PageLocation != ActiveAndValid) &&
             (Pfn1->u3.e1.PageLocation != ModifiedPageList) &&
             (Pfn1->u3.e1.PageLocation != TransitionPage)))
        {
            MiReleasePfnLock(OldIrql);
            continue;
        }

        Subsection = MiSubsectionFromPte(&Pfn1->OriginalPte);
        ControlArea = Subsection->ControlArea;
        if (ControlArea->u.Flags.Image)
        {
            MiReleasePfnLock(OldIrql);
            continue;
        }

        Index = (ULONG)(Pfn1->PteAddress - Subsection->SubsectionBase);

        Count = MiTakeModifiedRun(Subsection,
                                  &Index,
                                  Subsection->PtesInSubsection,
                                  Pages,
                                  &Busy,
                                  MM_NOIRQL);
        if (Count == 0)
        {
            MiReleasePfnLock(OldIrql);
            continue;
        }

        ControlArea->ModifiedWriteCount++;
        FileObject = ControlArea->FilePointer;
        ObReferenceObject(FileObject);
        FileOffset.QuadPart = ((LONGLONG)Subsection->StartingSector + Index) << PAGE_SHIFT;

        MiReleasePfnLock(OldIrql);

        /* The modified page writer still runs, paging writes need the file locks */
        Status = FsRtlAcquireFileForCcFlushEx(FileObject);
        if (NT_SUCCESS(Status))
        {
            Status = MiWriteMappedPages(FileObject, &FileOffset, Pages, Count, FALSE);
            FsRtlReleaseFileForCcFlush(FileObject);
        }

        MiCompleteMappedWrite(Pages, Count, Status);

        OldIrql = MiAcquirePfnLock();
        ControlArea->ModifiedWriteCount--;
        MiReleasePfnLock(OldIrql);

        ObDereferenceObject(FileObject);
    }
}

/**
 * @brief Writes the modified pages in a range of a data control area for a view flush.
 *
 * @param[in] ControlArea
 * Referenced data control area.
 *
 * @param[in] StartPage
 * First file page.
 *
 * @param[in] EndPage
 * File page after the last one.
 *
 * @param[out] IoStatusBlock
 * Receives the status and the number of bytes written.
 */
NTSTATUS
NTAPI
MiFlushDataFileView(
    _In_ PCONTROL_AREA ControlArea,
    _In_ ULONG64 StartPage,
    _In_ ULONG64 EndPage,
    _Out_ PIO_STATUS_BLOCK IoStatusBlock)
{
    ULONG Written = 0;
    NTSTATUS Status;

    Status = FsRtlAcquireFileForCcFlushEx(ControlArea->FilePointer);
    if (NT_SUCCESS(Status))
    {
        Status = MiFlushDataFileMap(ControlArea, StartPage, EndPage, FALSE, &Written);
        FsRtlReleaseFileForCcFlush(ControlArea->FilePointer);
    }

    IoStatusBlock->Status = Status;
    IoStatusBlock->Information = (ULONG_PTR)Written << PAGE_SHIFT;
    return Status;
}

/**
 * @brief Makes a file data section at least as large as asked.
 *
 * @param[in] SectionObject
 * Section to extend.
 *
 * @param[in,out] NewSize
 * Size to reach, receives the size of the section.
 */
NTSTATUS
NTAPI
MmExtendSection(
    _In_ PVOID SectionObject,
    _Inout_ PLARGE_INTEGER NewSize)
{
    PSECTION Section = SectionObject;
    PCONTROL_AREA ControlArea;
    LARGE_INTEGER FileSize;
    NTSTATUS Status;

    /* Images and paging file backed sections have a fixed size */
    if (Section->u.Flags.Image || !Section->u.Flags.File)
        return STATUS_SECTION_NOT_EXTENDED;

    /* A section never shrinks, report the size it already has */
    if (NewSize->QuadPart <= Section->SizeOfSection.QuadPart)
    {
        *NewSize = Section->SizeOfSection;
        return STATUS_SUCCESS;
    }

    ControlArea = Section->Segment->ControlArea;

    /* The cache manager sizes the file itself, a user section grows it */
    if (Section->u.Flags.UserReference)
    {
        if (!(Section->InitialPageProtection & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            return STATUS_SECTION_NOT_EXTENDED;

        Status = FsRtlGetFileSize(ControlArea->FilePointer, &FileSize);
        if (!NT_SUCCESS(Status))
            return Status;

        if (NewSize->QuadPart > FileSize.QuadPart)
        {
            Status = IoSetInformation(ControlArea->FilePointer,
                                      FileEndOfFileInformation,
                                      sizeof(LARGE_INTEGER),
                                      NewSize);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    Status = MiExtendDataFileMap(ControlArea, NewSize->QuadPart);
    if (!NT_SUCCESS(Status))
        return Status;

    Section->SizeOfSection = *NewSize;
    return STATUS_SUCCESS;
}

/**
 * @brief Moves the dirty state of the pages of a mapped view into their page frames.
 *
 * @param[in] BaseAddress
 * First page of the view.
 *
 * @param[in] Length
 * Length of the range in bytes.
 *
 * @remarks A flush only writes pages the page frame calls modified. A write through a
 * page that is already valid and writable leaves that state in the PTE alone, so the
 * caller of a flush has to hand it over first.
 */
VOID
NTAPI
MmCaptureDirtyPages(
    _In_ PVOID BaseAddress,
    _In_ SIZE_T Length)
{
    PMMPTE PointerPte = MiAddressToPte(BaseAddress);
    PMMPTE LastPte = MiAddressToPte((PUCHAR)BaseAddress + Length - 1);
    PMMPFN Pfn1;
    MMPTE TempPte;
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    for (; PointerPte <= LastPte; PointerPte++)
    {
        TempPte = *PointerPte;
        if (!TempPte.u.Hard.Valid || !MI_IS_PAGE_DIRTY(&TempPte))
            continue;

        /* Only a page of the file can be written back */
        Pfn1 = MI_PFN_ELEMENT(PFN_FROM_PTE(&TempPte));
        if (!Pfn1->u3.e1.PrototypePte)
            continue;

        Pfn1->u3.e1.Modified = 1;

        /* The next write marks it again */
        MI_MAKE_CLEAN_PAGE(&TempPte);
        MI_UPDATE_VALID_PTE(PointerPte, TempPte);
        KeInvalidateTlbEntry(MiPteToAddress(PointerPte));
    }

    MiReleasePfnLock(OldIrql);
}

/**
 * @brief Writes modified data of a file section to the file.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] Offset
 * Start of the range, or NULL for the whole file.
 *
 * @param[in] Length
 * Length of the range.
 *
 * @param[out] Iosb
 * Receives the status and the number of bytes written.
 */
NTSTATUS
NTAPI
MmFlushSegment(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER Offset,
    _In_ ULONG Length,
    _Out_opt_ PIO_STATUS_BLOCK Iosb)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PCONTROL_AREA ControlArea;
    ULONG64 StartPage, EndPage;
    ULONG Written = 0;

    if (Offset)
    {
        if ((Offset->QuadPart < 0) || ((Offset->QuadPart + Length) < Offset->QuadPart))
            return STATUS_INVALID_PARAMETER;

        StartPage = (ULONG64)Offset->QuadPart >> PAGE_SHIFT;
        EndPage = ((ULONG64)Offset->QuadPart + Length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    }
    else
    {
        StartPage = 0;
        EndPage = (~0ULL);
    }

    ControlArea = MiReferenceDataFileMapForIo(SectionObjectPointer);
    if (ControlArea)
    {
        Status = MiFlushDataFileMap(ControlArea, StartPage, EndPage, FALSE, &Written);
        MiDereferenceDataFileMapForIo(ControlArea);
    }

    if (Iosb)
    {
        Iosb->Status = Status;
        Iosb->Information = (ULONG_PTR)Written << PAGE_SHIFT;
    }

    return Status;
}

/**
 * @brief Throws away the resident data of a file section.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] Offset
 * Start of the range, or NULL for the whole file.
 *
 * @param[in] Length
 * Length of the range, 0 to reach the end of the file.
 *
 * @return FALSE if a page of the range is mapped or in use.
 */
BOOLEAN
NTAPI
MmPurgeSegment(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER Offset,
    _In_ ULONG Length)
{
    ULONG64 StartPage, EndPage;
    PCONTROL_AREA ControlArea;
    PMSUBSECTION Subsection;
    ULONG Index, LastIndex;
    BOOLEAN Result = TRUE;
    LARGE_INTEGER Delay;
    PMMPTE PointerPte;
    MMPTE TempPte;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    StartPage = Offset ? ((ULONG64)Offset->QuadPart >> PAGE_SHIFT) : 0;
    if (Offset && Length)
        EndPage = ((ULONG64)Offset->QuadPart + Length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    else
        EndPage = (~0ULL);

    ControlArea = MiReferenceDataFileMapForIo(SectionObjectPointer);
    if (!ControlArea)
        return TRUE;

    Delay.QuadPart = -10 * 10000LL;
    OldIrql = MiAcquirePfnLock();

    for (Subsection = (PMSUBSECTION)(ControlArea + 1);
         (Subsection != NULL) && Result;
         Subsection = (PMSUBSECTION)Subsection->NextSubsection)
    {
        if (((ULONG64)Subsection->StartingSector + Subsection->PtesInSubsection) <= StartPage)
            continue;

        if (Subsection->StartingSector >= EndPage)
            break;

        Index = (StartPage > Subsection->StartingSector) ?
                (ULONG)(StartPage - Subsection->StartingSector) : 0;
        LastIndex = (ULONG)min(EndPage - Subsection->StartingSector,
                               Subsection->PtesInSubsection);

        while (Index < LastIndex)
        {
            PointerPte = &Subsection->SubsectionBase[Index];
            MiMakeSystemAddressValidPfn(PointerPte, OldIrql);

            TempPte = *PointerPte;
            if (TempPte.u.Hard.Valid == 1)
            {
                /* Mapped somewhere */
                Result = FALSE;
                break;
            }

            if ((TempPte.u.Soft.Prototype == 0) && (TempPte.u.Soft.Transition == 1))
            {
                Pfn1 = MI_PFN_ELEMENT(TempPte.u.Trans.PageFrameNumber);

                if (Pfn1->u3.e1.WriteInProgress)
                {
                    MiReleasePfnLock(OldIrql);
                    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
                    OldIrql = MiAcquirePfnLock();
                    continue;
                }

                if (Pfn1->u3.e1.ReadInProgress || (Pfn1->u3.e2.ReferenceCount != 0))
                {
                    Result = FALSE;
                    break;
                }

                MiUnlinkPageFromList(Pfn1);
                MI_WRITE_INVALID_PTE(PointerPte, Pfn1->OriginalPte);
                MiDecrementShareCount(MI_PFN_ELEMENT(Pfn1->u4.PteFrame), Pfn1->u4.PteFrame);
                MI_SET_PFN_DELETED(Pfn1);

                Pfn1->u3.e1.PageLocation = ActiveAndValid;
                MiInsertPageInFreeList(TempPte.u.Trans.PageFrameNumber);
            }

            Index++;
        }
    }

    MiReleasePfnLock(OldIrql);

    MiDereferenceDataFileMapForIo(ControlArea);
    return Result;
}

/**
 * @brief Tells whether a range of a file section is in memory.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] Offset
 * Start of the range.
 *
 * @param[in] Length
 * Length of the range.
 */
BOOLEAN
NTAPI
MmIsDataSectionResident(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length)
{
    PCONTROL_AREA ControlArea;
    ULONG64 Page, EndPage;
    BOOLEAN Result = TRUE;
    PMMPTE PointerPte;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    if ((Offset < 0) || ((Offset + Length) < Offset))
        return FALSE;

    Page = (ULONG64)Offset >> PAGE_SHIFT;
    EndPage = ((ULONG64)Offset + Length + PAGE_SIZE - 1) >> PAGE_SHIFT;

    OldIrql = MiAcquirePfnLock();

    ControlArea = SectionObjectPointer->DataSectionObject;
    if (!ControlArea)
    {
        MiReleasePfnLock(OldIrql);
        return FALSE;
    }

    for (; Page < EndPage; Page++)
    {
        PointerPte = MiGetDataFileProtoPte(ControlArea, Page, NULL);
        if (!PointerPte)
        {
            Result = FALSE;
            break;
        }

        MiMakeSystemAddressValidPfn(PointerPte, OldIrql);

        Pfn1 = MiGetResidentPfn(PointerPte);
        if (!Pfn1 || Pfn1->u3.e1.ReadInProgress)
        {
            Result = FALSE;
            break;
        }
    }

    MiReleasePfnLock(OldIrql);
    return Result;
}

/**
 * @brief Makes the whole pages of a file section range read as zeroes when they are not in memory.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] Offset
 * Start of the range.
 *
 * @param[in] Length
 * Length of the range.
 *
 * @remarks Pages already in memory keep their data, it may have been written through a
 * mapping and not reached the file yet. Every whole page of the range ends up modified.
 */
NTSTATUS
NTAPI
MmZeroDataSection(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length)
{
    PFN_NUMBER PageFrameIndex;
    PCONTROL_AREA ControlArea;
    PKEVENT PreviousEvent;
    ULONG64 Page, EndPage;
    PMMPTE PointerPte;
    KEVENT Event;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    if ((Offset < 0) || ((Offset + Length) < Offset))
        return STATUS_INVALID_PARAMETER;

    Page = ((ULONG64)Offset + PAGE_SIZE - 1) >> PAGE_SHIFT;
    EndPage = ((ULONG64)Offset + Length) >> PAGE_SHIFT;

    ControlArea = MiReferenceDataFileMapForIo(SectionObjectPointer);
    if (!ControlArea)
        return STATUS_SUCCESS;

    while (Page < EndPage)
    {
        OldIrql = MiAcquirePfnLock();

        PointerPte = MiGetDataFileProtoPte(ControlArea, Page, NULL);
        if (!PointerPte)
        {
            MiReleasePfnLock(OldIrql);
            break;
        }

        MiMakeSystemAddressValidPfn(PointerPte, OldIrql);

        Pfn1 = MiGetResidentPfn(PointerPte);
        if (!Pfn1)
        {
            MI_SET_USAGE(MI_USAGE_CACHE);
            PageFrameIndex = MiRemoveZeroPage(MI_GET_NEXT_COLOR());
            if (!PageFrameIndex)
            {
                MiReleasePfnLock(OldIrql);
                MiWaitForFreePage();
                continue;
            }

            /* Set up like a finished read of zeroes, the file gets them on the next write */
            MiInitializeReadPage(PageFrameIndex, PointerPte);
            Pfn1 = MI_PFN_ELEMENT(PageFrameIndex);
            Pfn1->u3.e1.ReadInProgress = 0;
            Pfn1->u3.e1.Modified = 1;
            MiDecrementReferenceCount(Pfn1, PageFrameIndex);
        }
        else if (Pfn1->u3.e1.ReadInProgress)
        {
            /* Queue up behind the thread reading it and look again */
            KeInitializeEvent(&Event, NotificationEvent, FALSE);
            PreviousEvent = Pfn1->u1.Event;
            Pfn1->u1.Event = &Event;
            MiReleasePfnLock(OldIrql);

            KeWaitForSingleObject(&Event, WrPageIn, KernelMode, FALSE, NULL);
            if (PreviousEvent)
                KeSetEvent(PreviousEvent, IO_NO_INCREMENT, FALSE);
            continue;
        }
        else
        {
            Pfn1->u3.e1.Modified = 1;
            if (Pfn1->u3.e1.PageLocation == StandbyPageList)
            {
                MiUnlinkPageFromList(Pfn1);
                MiInsertPageInList(&MmModifiedPageListHead, MiGetPfnEntryIndex(Pfn1));
            }
        }

        MiReleasePfnLock(OldIrql);
        Page++;
    }

    MiDereferenceDataFileMapForIo(ControlArea);
    return STATUS_SUCCESS;
}

/**
 * @brief Reads a range of a file section into memory.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] Offset
 * Start of the range.
 *
 * @param[in] Length
 * Length of the range.
 *
 * @remarks Whole pages are read, the file system zeroes what lies past its valid data.
 */
NTSTATUS
NTAPI
MmMakeDataSectionResident(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length)
{
    PFN_NUMBER Pages[MI_MAPPED_IO_PAGES];
    PFN_NUMBER PageFrameIndex;
    PCONTROL_AREA ControlArea;
    PMSUBSECTION Subsection;
    LARGE_INTEGER FileOffset;
    NTSTATUS Status = STATUS_SUCCESS;
    PMMPTE PointerPte;
    ULONG64 Page, EndPage;
    ULONG Count, ValidLength;
    PKEVENT PreviousEvent;
    KEVENT Event;
    MMPTE TempPte;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    if ((Offset < 0) || ((Offset + Length) < Offset))
        return STATUS_INVALID_PARAMETER;

    ControlArea = MiReferenceDataFileMapForIo(SectionObjectPointer);
    ASSERT(ControlArea != NULL);
    if (!ControlArea)
        return STATUS_SUCCESS;

    Page = (ULONG64)Offset >> PAGE_SHIFT;
    EndPage = ((ULONG64)Offset + Length + PAGE_SIZE - 1) >> PAGE_SHIFT;

    while (Page < EndPage)
    {
        OldIrql = MiAcquirePfnLock();

        PointerPte = MiGetDataFileProtoPte(ControlArea, Page, &Subsection);
        if (!PointerPte)
        {
            MiReleasePfnLock(OldIrql);
            break;
        }

        MiMakeSystemAddressValidPfn(PointerPte, OldIrql);

        TempPte = *PointerPte;
        Pfn1 = MiGetResidentPfn(&TempPte);
        if (Pfn1)
        {
            if (!Pfn1->u3.e1.ReadInProgress)
            {
                MiReleasePfnLock(OldIrql);
                Page++;
                continue;
            }

            /* Queue up behind the thread reading it and look again */
            KeInitializeEvent(&Event, NotificationEvent, FALSE);
            PreviousEvent = Pfn1->u1.Event;
            Pfn1->u1.Event = &Event;
            MiReleasePfnLock(OldIrql);

            KeWaitForSingleObject(&Event, WrPageIn, KernelMode, FALSE, NULL);
            if (PreviousEvent)
                KeSetEvent(PreviousEvent, IO_NO_INCREMENT, FALSE);
            continue;
        }

        /* Gather the pages missing from here on in this subsection */
        Count = 0;
        while ((Count < MI_MAPPED_IO_PAGES) &&
               ((Page + Count) < EndPage) &&
               (&PointerPte[Count] < &Subsection->SubsectionBase[Subsection->PtesInSubsection]) &&
               ((Count == 0) || !MiIsPteOnPdeBoundary(&PointerPte[Count])) &&
               (PointerPte[Count].u.Hard.Valid == 0) &&
               (PointerPte[Count].u.Soft.Prototype == 1))
        {
            MI_SET_USAGE(MI_USAGE_CACHE);
            PageFrameIndex = MiRemoveAnyPage(MI_GET_NEXT_COLOR());
            if (!PageFrameIndex)
                break;

            MiInitializeReadPage(PageFrameIndex, &PointerPte[Count]);
            Pages[Count++] = PageFrameIndex;
        }

        FileOffset.QuadPart = (LONGLONG)(Page << PAGE_SHIFT);
        ValidLength = Count ? MiGetDataFileReadLength(ControlArea->Segment, FileOffset.QuadPart, Count) : 0;

        MiReleasePfnLock(OldIrql);

        if (Count == 0)
        {
            MiWaitForFreePage();
            continue;
        }

        Status = MiReadMappedPages(ControlArea->FilePointer,
                                   &FileOffset,
                                   Pages,
                                   Count,
                                   ValidLength);
        if (!NT_SUCCESS(Status))
            break;

        Page += Count;
    }

    MiDereferenceDataFileMapForIo(ControlArea);
    return Status;
}

/**
 * @brief Marks a range of a file section as modified.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] Offset
 * Start of the range.
 *
 * @param[in] Length
 * Length of the range.
 */
NTSTATUS
NTAPI
MmMakeSegmentDirty(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length)
{
    PCONTROL_AREA ControlArea;
    ULONG64 Page, EndPage;
    PMMPTE PointerPte;
    PMMPFN Pfn1;
    KIRQL OldIrql;

    if ((Offset < 0) || ((Offset + Length) < Offset))
        return STATUS_INVALID_PARAMETER;

    Page = (ULONG64)Offset >> PAGE_SHIFT;
    EndPage = ((ULONG64)Offset + Length + PAGE_SIZE - 1) >> PAGE_SHIFT;

    OldIrql = MiAcquirePfnLock();

    ControlArea = SectionObjectPointer->DataSectionObject;
    if (!ControlArea)
    {
        MiReleasePfnLock(OldIrql);
        return STATUS_NOT_MAPPED_VIEW;
    }

    for (; Page < EndPage; Page++)
    {
        PointerPte = MiGetDataFileProtoPte(ControlArea, Page, NULL);
        if (!PointerPte)
            break;

        MiMakeSystemAddressValidPfn(PointerPte, OldIrql);

        Pfn1 = MiGetResidentPfn(PointerPte);
        if (!Pfn1)
            continue;

        Pfn1->u3.e1.Modified = 1;

        /* A clean page waiting on standby has to be written now */
        if (Pfn1->u3.e1.PageLocation == StandbyPageList)
        {
            MiUnlinkPageFromList(Pfn1);
            MiInsertPageInList(&MmModifiedPageListHead, MiGetPfnEntryIndex(Pfn1));
        }
    }

    MiReleasePfnLock(OldIrql);
    return STATUS_SUCCESS;
}

/**
 * @brief Tells whether a file can be cut to a new size.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] NewFileSize
 * Size the file would get, or NULL.
 */
BOOLEAN
NTAPI
MmCanFileBeTruncated(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER NewFileSize)
{
    PCONTROL_AREA ControlArea;
    BOOLEAN Result = TRUE;
    KIRQL OldIrql;

    /* Mapped images keep their file as it is */
    if (SectionObjectPointer->ImageSectionObject &&
        !MmFlushImageSection(SectionObjectPointer, MmFlushForWrite))
    {
        return FALSE;
    }

    OldIrql = MiAcquirePfnLock();

    /* Sections and views outside the cache only allow the file to grow */
    ControlArea = SectionObjectPointer->DataSectionObject;
    if (ControlArea && (ControlArea->NumberOfUserReferences != 0))
    {
        Result = (BOOLEAN)((NewFileSize != NULL) &&
                           (NewFileSize->QuadPart >= 0) &&
                           ((ULONG64)NewFileSize->QuadPart >= ControlArea->Segment->SizeOfSegment));
    }

    MiReleasePfnLock(OldIrql);
    return Result;
}

/**
 * @brief Tells how many writable user sections a file has.
 */
ULONG
NTAPI
MmDoesFileHaveUserWritableReferences(
    _In_ PSECTION_OBJECT_POINTERS SectionPointer)
{
    PCONTROL_AREA ControlArea;
    ULONG References = 0;
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    ControlArea = SectionPointer->DataSectionObject;
    if (ControlArea)
        References = ControlArea->WritableUserReferences;

    MiReleasePfnLock(OldIrql);
    return References;
}

/* EOF */
