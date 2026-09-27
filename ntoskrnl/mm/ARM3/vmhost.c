/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Memory routines the second level translation host publishes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The kernel offers a host that the virtualization stack binds itself into,
 * and the host publishes a table of routines the driver calls by slot. These
 * are the ones about memory. The table itself is in ex/exthost.c, which is
 * also where a slot with nothing behind it is answered.
 *
 * What the driver asks for here is a record of which range of a process's
 * virtual addresses stands behind which range of a guest's physical ones. The
 * driver keeps the guest's own map, so the kernel's part is to remember the
 * ranges, keep their pages where they are while a guest is using them, and
 * hand the record back when the driver splits, joins or drops a range.
 *
 * Every one of these runs in the process that owns the range: the driver
 * attaches to it before calling.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include "miarm.h"

/* TYPES **********************************************************************/

/* What one process has handed to a guest */
typedef struct _MI_VM_CONTEXT
{
    LIST_ENTRY ListEntry;
    PEPROCESS Process;
    LIST_ENTRY RangeListHead;
    /* A process backs one partition, which is settled by its first range */
    ULONG64 PartitionId;
} MI_VM_CONTEXT, *PMI_VM_CONTEXT;

/* One run of virtual addresses standing behind one run of guest physical ones */
typedef struct _MI_VM_RANGE
{
    LIST_ENTRY ListEntry;
    ULONG64 BasePage;
    ULONG64 GuestBasePage;
    ULONG64 PageCount;
    ULONG64 PartitionId;
    ULONG Flags;
    /* What holds the pages while a guest is looking at them */
    PMDL Mdl;
    LONG PinCount;
} MI_VM_RANGE, *PMI_VM_RANGE;

/* One run of addresses a caller wants made ready */
typedef struct _MI_VM_RANGE_REQUEST
{
    ULONG64 BaseVa;
    ULONG64 Length;
} MI_VM_RANGE_REQUEST, *PMI_VM_RANGE_REQUEST;

/* GLOBALS ********************************************************************/

#define MI_VM_HOST_TAG 'HmVM'

/*
 * What it takes to tell the hypervisor where a guest physical page is. The
 * kernel owns the translations of a range it was given, so it is the one
 * that fills them in, and that is a hypercall with its input in a page.
 */
#define MI_VM_MAP_GPA_PAGES         0x004B
#define MI_VM_HYPERCALL_REP_SHIFT   32
#define MI_VM_HYPERCALL_STATUS_MASK 0xFFFF
#define MI_VM_MAP_GPA_FULL_ACCESS   0x0000000F

typedef struct _MI_VM_MAP_GPA_INPUT
{
    ULONG64 TargetPartitionId;
    ULONG64 TargetGpaBase;
    ULONG MapFlags;
    ULONG Reserved;
    ULONG64 SourcePageList[ANYSIZE_ARRAY];
} MI_VM_MAP_GPA_INPUT, *PMI_VM_MAP_GPA_INPUT;

/* The only flag a caller may ask for, which says the range is written through */
#define MI_VM_RANGE_FLAGS 0x00000001

/* One page for a hypercall input, and where the processor sees it */
static PMI_VM_MAP_GPA_INPUT MiVmHypercallInput;
static ULONG64 MiVmHypercallInputPa;

static LIST_ENTRY MiVmContextListHead;
static KGUARDED_MUTEX MiVmContextLock;

/* PRIVATE FUNCTIONS **********************************************************/

/* Finds what a process has handed out, making the record if it has none */
static
PMI_VM_CONTEXT
NTAPI
MiVmContextForProcess(
    _In_ PEPROCESS Process,
    _In_ BOOLEAN Create)
{
    PLIST_ENTRY Entry;
    PMI_VM_CONTEXT Context;

    for (Entry = MiVmContextListHead.Flink;
         Entry != &MiVmContextListHead;
         Entry = Entry->Flink)
    {
        Context = CONTAINING_RECORD(Entry, MI_VM_CONTEXT, ListEntry);

        if (Context->Process == Process)
            return Context;
    }

    if (!Create)
        return NULL;

    Context = ExAllocatePoolZero(NonPagedPool, sizeof(*Context), MI_VM_HOST_TAG);
    if (Context == NULL)
        return NULL;

    Context->Process = Process;
    Context->PartitionId = 0;
    InitializeListHead(&Context->RangeListHead);
    InsertTailList(&MiVmContextListHead, &Context->ListEntry);

    return Context;
}

/* Finds the range a page of virtual address falls in */
static
PMI_VM_RANGE
NTAPI
MiVmRangeForPage(
    _In_ PMI_VM_CONTEXT Context,
    _In_ ULONG64 BasePage)
{
    PLIST_ENTRY Entry;
    PMI_VM_RANGE Range;

    for (Entry = Context->RangeListHead.Flink;
         Entry != &Context->RangeListHead;
         Entry = Entry->Flink)
    {
        Range = CONTAINING_RECORD(Entry, MI_VM_RANGE, ListEntry);

        if ((BasePage >= Range->BasePage) &&
            (BasePage < (Range->BasePage + Range->PageCount)))
        {
            return Range;
        }
    }

    return NULL;
}

/* Finds the range a guest physical page falls in */
static
PMI_VM_RANGE
NTAPI
MiVmRangeForGuestPage(
    _In_ PMI_VM_CONTEXT Context,
    _In_ ULONG64 GuestPage)
{
    PLIST_ENTRY Entry;
    PMI_VM_RANGE Range;

    for (Entry = Context->RangeListHead.Flink;
         Entry != &Context->RangeListHead;
         Entry = Entry->Flink)
    {
        Range = CONTAINING_RECORD(Entry, MI_VM_RANGE, ListEntry);

        if ((GuestPage >= Range->GuestBasePage) &&
            (GuestPage < (Range->GuestBasePage + Range->PageCount)))
        {
            return Range;
        }
    }

    return NULL;
}

/* Puts a range in its place, so that the list reads in address order */
static
VOID
NTAPI
MiVmInsertRange(
    _Inout_ PMI_VM_CONTEXT Context,
    _Inout_ PMI_VM_RANGE Range)
{
    PLIST_ENTRY Entry;
    PMI_VM_RANGE Other;

    for (Entry = Context->RangeListHead.Flink;
         Entry != &Context->RangeListHead;
         Entry = Entry->Flink)
    {
        Other = CONTAINING_RECORD(Entry, MI_VM_RANGE, ListEntry);

        if (Other->BasePage > Range->BasePage)
            break;
    }

    InsertTailList(Entry, &Range->ListEntry);
}

/* Lets go of the pages a range was holding */
static
VOID
NTAPI
MiVmReleaseRange(
    _Inout_ PMI_VM_RANGE Range)
{
    if (Range->Mdl == NULL)
        return;

    MmUnlockPages(Range->Mdl);
    IoFreeMdl(Range->Mdl);

    Range->Mdl = NULL;
    Range->PinCount = 0;
}

/* Does a range cover addresses a process could have asked for? */
static
_Must_inspect_result_
BOOLEAN
NTAPI
MiVmRangeIsSane(
    _In_ ULONG64 BaseVa,
    _In_ ULONG64 GuestBase,
    _In_ ULONG64 PageCount)
{
    ULONG64 Last;

    if ((PageCount == 0) || (PageCount > (MAXULONG_PTR / PAGE_SIZE)))
        return FALSE;

    if (((BaseVa | GuestBase) & (PAGE_SIZE - 1)) != 0)
        return FALSE;

    Last = BaseVa + (PageCount * PAGE_SIZE) - 1;
    if ((Last < BaseVa) || (Last > (ULONG64)(ULONG_PTR)MmHighestUserAddress))
        return FALSE;

    Last = GuestBase + (PageCount * PAGE_SIZE) - 1;
    if (Last < GuestBase)
        return FALSE;

    return TRUE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Readies the record of what processes have handed to guests.
 */
CODE_SEG("INIT")
VOID
NTAPI
MiInitializeVmHost(VOID)
{
    PHYSICAL_ADDRESS Low, High, Boundary;

    InitializeListHead(&MiVmContextListHead);
    KeInitializeGuardedMutex(&MiVmContextLock);

    /*
     * A hypercall reads its input by physical address, so the page it is
     * written in has to be one whose address does not move.
     */
    Low.QuadPart = 0;
    High.QuadPart = MAXLONGLONG;
    Boundary.QuadPart = 0;

    MiVmHypercallInput = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE,
                                                                Low,
                                                                High,
                                                                Boundary,
                                                                MmCached);
    if (MiVmHypercallInput == NULL)
    {
        DPRINT1("Mm: no page to fill a guest map from\n");
        return;
    }

    MiVmHypercallInputPa = MmGetPhysicalAddress(MiVmHypercallInput).QuadPart;
}

/**
 * @brief
 * Takes what creating a range will need, before anything can fail.
 *
 * @param[out] Preallocation
 * Receives what to hand back to VmCreateMemoryRange().
 *
 * @param[in] Process
 * The process the range will belong to.
 */
NTSTATUS
NTAPI
VmPreallocateForRangeCreate(
    _Out_ PVOID *Preallocation,
    _In_ PEPROCESS Process)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range;

    if ((Preallocation == NULL) || (Process == NULL))
        return STATUS_INVALID_PARAMETER;

    *Preallocation = NULL;

    Range = ExAllocatePoolZero(NonPagedPool, sizeof(*Range), MI_VM_HOST_TAG);
    if (Range == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireGuardedMutex(&MiVmContextLock);
    Context = MiVmContextForProcess(Process, TRUE);
    KeReleaseGuardedMutex(&MiVmContextLock);

    if (Context == NULL)
    {
        ExFreePoolWithTag(Range, MI_VM_HOST_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Preallocation = Range;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives back what creating a range would have needed.
 */
VOID
NTAPI
VmFreePreallocationForRangeCreate(
    _In_opt_ PVOID Preallocation)
{
    if (Preallocation != NULL)
        ExFreePoolWithTag(Preallocation, MI_VM_HOST_TAG);
}

/**
 * @brief
 * Records that a run of this process's virtual addresses stands behind a run
 * of a guest's physical ones.
 *
 * @param[in] BaseVa
 * Where the range starts in the process.
 *
 * @param[in] GuestBase
 * The guest physical address the range starts at.
 *
 * @param[in] PageCount
 * How many pages long it is.
 *
 * @param[in] PartitionId
 * Whose guest physical space those addresses are in.
 *
 * @param[in] Preallocation
 * What VmPreallocateForRangeCreate() gave, or NULL to allocate here.
 *
 * @param[in] Flags
 * What the caller asks of the range. Only bit zero is defined.
 */
NTSTATUS
NTAPI
VmCreateMemoryRange(
    _In_ ULONG64 GuestBase,
    _In_ ULONG64 BaseVa,
    _In_ ULONG64 PageCount,
    _In_ ULONG64 PartitionId,
    _In_opt_ PVOID Preallocation,
    _In_ ULONG Flags)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range = Preallocation;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!MiVmRangeIsSane(BaseVa, GuestBase, PageCount))
        return STATUS_INVALID_PARAMETER;

    if ((Flags & ~MI_VM_RANGE_FLAGS) != 0)
        return STATUS_INVALID_PARAMETER;

    if (Range == NULL)
    {
        Range = ExAllocatePoolZero(NonPagedPool, sizeof(*Range), MI_VM_HOST_TAG);
        if (Range == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), TRUE);
    if (Context == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else if ((Context->PartitionId != 0) &&
             (Context->PartitionId != PartitionId))
    {
        /* A process stands behind one guest, not several */
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (MiVmRangeForPage(Context, BaseVa / PAGE_SIZE) != NULL)
    {
        Status = STATUS_CONFLICTING_ADDRESSES;
    }
    else
    {
        Context->PartitionId = PartitionId;

        Range->BasePage = BaseVa / PAGE_SIZE;
        Range->GuestBasePage = GuestBase / PAGE_SIZE;
        Range->PageCount = PageCount;
        Range->PartitionId = PartitionId;
        Range->Flags = Flags;
        Range->Mdl = NULL;
        Range->PinCount = 0;

        MiVmInsertRange(Context, Range);
        Range = NULL;
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    if (Range != NULL)
        ExFreePoolWithTag(Range, MI_VM_HOST_TAG);

    return Status;
}

/**
 * @brief
 * Drops the record of a range, and whatever it was holding.
 */
NTSTATUS
NTAPI
VmDeleteMemoryRange(
    _In_ ULONG64 GuestBase,
    _In_ ULONG64 BaseVa,
    _In_ ULONG64 PageCount,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range = NULL;
    NTSTATUS Status = STATUS_NOT_FOUND;

    UNREFERENCED_PARAMETER(BaseVa);
    UNREFERENCED_PARAMETER(PageCount);

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
    if (Context != NULL)
    {
        Range = MiVmRangeForGuestPage(Context, GuestBase / PAGE_SIZE);
        if ((Range != NULL) && (Range->PartitionId == PartitionId))
        {
            RemoveEntryList(&Range->ListEntry);
            Status = STATUS_SUCCESS;
        }
        else
        {
            Range = NULL;
        }
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    if (Range != NULL)
    {
        MiVmReleaseRange(Range);
        ExFreePoolWithTag(Range, MI_VM_HOST_TAG);
    }

    return Status;
}

/**
 * @brief
 * Cuts a range in two at an address, so that its halves can be treated apart.
 *
 * @param[in] SplitVa
 * Where the second half starts. An address a range already starts at asks for
 * nothing.
 */
NTSTATUS
NTAPI
VmSplitMemoryRange(
    _In_ ULONG64 SplitVa,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range, Tail;
    ULONG64 SplitPage = SplitVa / PAGE_SIZE;
    NTSTATUS Status;

    if ((SplitVa & (PAGE_SIZE - 1)) != 0)
        return STATUS_INVALID_PARAMETER;

    Tail = ExAllocatePoolZero(NonPagedPool, sizeof(*Tail), MI_VM_HOST_TAG);
    if (Tail == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
    Range = (Context != NULL) ? MiVmRangeForPage(Context, SplitPage) : NULL;

    if ((Range == NULL) || (Range->PartitionId != PartitionId))
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (Range->BasePage == SplitPage)
    {
        /* It is already cut here */
        Status = STATUS_SUCCESS;
    }
    else
    {
        ULONG64 Taken = SplitPage - Range->BasePage;

        Tail->BasePage = SplitPage;
        Tail->GuestBasePage = Range->GuestBasePage + Taken;
        Tail->PageCount = Range->PageCount - Taken;
        Tail->PartitionId = Range->PartitionId;
        Tail->Flags = Range->Flags;

        /*
         * What holds the pages stays with the half it was taken for, because
         * one lock cannot be split. The other half takes its own when it is
         * next pinned.
         */
        Tail->Mdl = NULL;
        Tail->PinCount = 0;

        Range->PageCount = Taken;

        InsertHeadList(&Range->ListEntry, &Tail->ListEntry);
        Tail = NULL;
        Status = STATUS_SUCCESS;
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    if (Tail != NULL)
        ExFreePoolWithTag(Tail, MI_VM_HOST_TAG);

    return Status;
}

/**
 * @brief
 * Joins a range with the one that follows it, when the two run on from each
 * other in both the process and the guest.
 *
 * @param[in] BaseVa
 * An address in the first of the two.
 */
NTSTATUS
NTAPI
VmMergeMemoryRanges(
    _In_ ULONG64 BaseVa,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range, Next = NULL;
    NTSTATUS Status;

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
    Range = (Context != NULL) ? MiVmRangeForPage(Context, BaseVa / PAGE_SIZE) : NULL;

    if ((Range == NULL) || (Range->PartitionId != PartitionId))
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (Range->ListEntry.Flink == &Context->RangeListHead)
    {
        /* Nothing follows it, so there is nothing to join */
        Status = STATUS_SUCCESS;
    }
    else
    {
        Next = CONTAINING_RECORD(Range->ListEntry.Flink, MI_VM_RANGE, ListEntry);

        if ((Next->BasePage != (Range->BasePage + Range->PageCount)) ||
            (Next->GuestBasePage != (Range->GuestBasePage + Range->PageCount)) ||
            (Next->Flags != Range->Flags) ||
            (Next->Mdl != NULL) ||
            (Range->Mdl != NULL))
        {
            Next = NULL;
            Status = STATUS_SUCCESS;
        }
        else
        {
            Range->PageCount += Next->PageCount;
            RemoveEntryList(&Next->ListEntry);
            Status = STATUS_SUCCESS;
        }
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    if (Next != NULL)
        ExFreePoolWithTag(Next, MI_VM_HOST_TAG);

    return Status;
}

/**
 * @brief
 * Holds a range's pages where they are, so that a guest can be given them.
 *
 * @remarks
 * A guest reaches a page without the kernel knowing, so the page cannot be
 * taken away while the range is pinned. The whole range is locked at once,
 * which is what the caller asked for by pinning it.
 */
NTSTATUS
NTAPI
VmPinMemoryRange(
    _In_ ULONG64 GuestBase,
    _In_ ULONG64 BaseVa,
    _In_ ULONG64 PageCount,
    _In_ ULONG Flags,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range;
    PMDL Mdl;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Flags);

    if (!MiVmRangeIsSane(BaseVa, GuestBase, PageCount))
        return STATUS_INVALID_PARAMETER;

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
    Range = (Context != NULL) ? MiVmRangeForGuestPage(Context, GuestBase / PAGE_SIZE)
                              : NULL;

    if ((Range == NULL) || (Range->PartitionId != PartitionId))
    {
        KeReleaseGuardedMutex(&MiVmContextLock);
        return STATUS_NOT_FOUND;
    }

    /* Already held, so this is one more reason to keep holding it */
    if (Range->Mdl != NULL)
    {
        Range->PinCount++;
        KeReleaseGuardedMutex(&MiVmContextLock);
        return STATUS_SUCCESS;
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)(Range->BasePage * PAGE_SIZE),
                        (ULONG)(Range->PageCount * PAGE_SIZE),
                        FALSE,
                        FALSE,
                        NULL);
    if (Mdl == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    _SEH2_TRY
    {
        MmProbeAndLockPages(Mdl, UserMode, IoModifyAccess);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        IoFreeMdl(Mdl);
        return Status;
    }

    KeAcquireGuardedMutex(&MiVmContextLock);

    if (Range->Mdl == NULL)
    {
        Range->Mdl = Mdl;
        Range->PinCount = 1;
        Mdl = NULL;
    }
    else
    {
        Range->PinCount++;
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    /* Somebody else got there first, so this one was not needed */
    if (Mdl != NULL)
    {
        MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Lets a range's pages move again, once nothing is looking at them.
 */
NTSTATUS
NTAPI
VmUnpinMemoryRange(
    _In_ ULONG64 GuestBase,
    _In_ ULONG64 BaseVa,
    _In_ ULONG64 PageCount,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range;
    PMDL Mdl = NULL;

    UNREFERENCED_PARAMETER(BaseVa);
    UNREFERENCED_PARAMETER(PageCount);

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
    Range = (Context != NULL) ? MiVmRangeForGuestPage(Context, GuestBase / PAGE_SIZE)
                              : NULL;

    if ((Range == NULL) || (Range->PartitionId != PartitionId))
    {
        KeReleaseGuardedMutex(&MiVmContextLock);
        return STATUS_NOT_FOUND;
    }

    if ((Range->PinCount != 0) && (--Range->PinCount == 0))
    {
        Mdl = Range->Mdl;
        Range->Mdl = NULL;
    }

    KeReleaseGuardedMutex(&MiVmContextLock);

    if (Mdl != NULL)
    {
        MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Puts the pages behind a run of guest physical addresses into the guest map.
 *
 * @param[in] GuestBasePage
 * The first guest physical page of the run.
 *
 * @param[in] PageCount
 * How many pages it covers.
 *
 * @param[in] PartitionId
 * Whose map to fill in.
 *
 * @remarks
 * The range record says which of this process's addresses stand behind
 * those guest ones, and a pinned range is already where it is going to stay,
 * so the page numbers can be handed straight over.
 */
static
NTSTATUS
NTAPI
MiVmBackGuestPages(
    _In_ ULONG64 GuestBasePage,
    _In_ ULONG64 PageCount,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_CONTEXT Context;
    PMI_VM_RANGE Range;
    PPFN_NUMBER Pages;
    ULONG64 Index, Offset, Count, Result;

    if (PageCount == 0)
        PageCount = 1;

    if (PageCount > ((PAGE_SIZE - FIELD_OFFSET(MI_VM_MAP_GPA_INPUT, SourcePageList)) /
                     sizeof(ULONG64)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireGuardedMutex(&MiVmContextLock);

    Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
    Range = (Context != NULL) ? MiVmRangeForGuestPage(Context, GuestBasePage) : NULL;

    if ((Range == NULL) || (Range->PartitionId != PartitionId))
    {
        DPRINT1("Mm: nothing of this process stands behind guest page %I64x\n",
                GuestBasePage);
        KeReleaseGuardedMutex(&MiVmContextLock);
        return STATUS_NOT_FOUND;
    }

    /*
     * A guest is about to reach these pages, so they have to be held where
     * they are whether the caller asked for that or not. Pinning waits, so
     * the record is let go of first and looked up again after.
     */
    if (Range->Mdl == NULL)
    {
        ULONG64 BaseVa = Range->BasePage * PAGE_SIZE;
        ULONG64 GuestBase = Range->GuestBasePage * PAGE_SIZE;
        ULONG64 Pages = Range->PageCount;
        NTSTATUS Locked;

        KeReleaseGuardedMutex(&MiVmContextLock);

        Locked = VmPinMemoryRange(GuestBase, BaseVa, Pages, 0, PartitionId);
        if (!NT_SUCCESS(Locked))
            return Locked;

        KeAcquireGuardedMutex(&MiVmContextLock);

        Context = MiVmContextForProcess(PsGetCurrentProcess(), FALSE);
        Range = (Context != NULL) ? MiVmRangeForGuestPage(Context, GuestBasePage)
                                  : NULL;

        if ((Range == NULL) ||
            (Range->PartitionId != PartitionId) ||
            (Range->Mdl == NULL))
        {
            KeReleaseGuardedMutex(&MiVmContextLock);
            return STATUS_NOT_FOUND;
        }
    }

    Offset = GuestBasePage - Range->GuestBasePage;
    Count = Range->PageCount - Offset;
    if (Count > PageCount)
        Count = PageCount;

    Pages = MmGetMdlPfnArray(Range->Mdl);

    MiVmHypercallInput->TargetPartitionId = PartitionId;
    MiVmHypercallInput->TargetGpaBase = GuestBasePage;
    MiVmHypercallInput->MapFlags = MI_VM_MAP_GPA_FULL_ACCESS;
    MiVmHypercallInput->Reserved = 0;

    for (Index = 0; Index < Count; Index++)
        MiVmHypercallInput->SourcePageList[Index] = Pages[Offset + Index];

    KeReleaseGuardedMutex(&MiVmContextLock);

    Result = HvlInvokeHypercall(MI_VM_MAP_GPA_PAGES |
                                   (Count << MI_VM_HYPERCALL_REP_SHIFT),
                               MiVmHypercallInputPa,
                               0);
    if ((Result & MI_VM_HYPERCALL_STATUS_MASK) != 0)
    {
        DPRINT1("Mm: guest page %I64x was refused, %I64x\n",
                GuestBasePage,
                Result);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Makes a run of this process's addresses ready for a guest to reach.
 *
 * @param[in] RangeList
 * The runs to make ready, as base and length pairs.
 *
 * @param[out] Result
 * Where the caller wants to be told what came of it. Nothing is written,
 * because nothing here has anything to add.
 *
 * @param[in] RangeCount
 * How many runs @p RangeList holds.
 *
 * @remarks
 * A guest reaches a page without the kernel seeing it, so the page has to be
 * there before the guest is let at it. Touching each one is what brings it in,
 * and a range that is pinned is already there and stays.
 */
NTSTATUS
NTAPI
VmAccessFault(
    _In_reads_(RangeCount) PVOID RangeList,
    _Out_opt_ PVOID Result,
    _In_ ULONG64 RangeCount,
    _In_ ULONG Access,
    _In_ ULONG Flags,
    _In_ ULONG Reserved,
    _In_ ULONG64 PartitionId)
{
    PMI_VM_RANGE_REQUEST Ranges = RangeList;
    ULONG64 Index;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Result);
    UNREFERENCED_PARAMETER(Access);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Reserved);

    if ((RangeList == NULL) || (RangeCount == 0))
        return STATUS_INVALID_PARAMETER;

    if (MiVmHypercallInput == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < RangeCount; Index++)
    {
        Status = MiVmBackGuestPages(Ranges[Index].BaseVa,
                                    Ranges[Index].Length,
                                    PartitionId);
        if (!NT_SUCCESS(Status))
            break;
    }

    return Status;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Maps a view of a section into the calling process so that it can be run.
 *
 * @param[in] SectionHandle
 * The section, which has to have been opened for execute.
 *
 * @param[in,out] BaseAddress
 * Where to map it, or NULL to be given somewhere. Receives where it went.
 *
 * @param[in,out] SectionOffset
 * How far into the section the view starts. Rounded down as usual.
 *
 * @param[in,out] ViewSize
 * How much to map. Receives how much was mapped.
 *
 * @remarks
 * The name says what it skips on Windows, which is the guard that stops a
 * process mapping memory it can execute. Nothing here guards that, so this is
 * an ordinary executable mapping.
 */
NTSTATUS
NTAPI
VmMapSectionExecuteNoAcg(
    _In_ HANDLE SectionHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PLARGE_INTEGER SectionOffset,
    _Inout_ PSIZE_T ViewSize)
{
    return ZwMapViewOfSection(SectionHandle,
                              ZwCurrentProcess(),
                              BaseAddress,
                              0,
                              0,
                              SectionOffset,
                              ViewSize,
                              ViewUnmap,
                              0,
                              PAGE_EXECUTE);
}

/* EOF */
