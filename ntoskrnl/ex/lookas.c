/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            ntoskrnl/ex/lookas.c
* PURPOSE:         Lookaside Lists
* PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
*/

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

LIST_ENTRY ExpNonPagedLookasideListHead;
KSPIN_LOCK ExpNonPagedLookasideListLock;
LIST_ENTRY ExpPagedLookasideListHead;
KSPIN_LOCK ExpPagedLookasideListLock;
LIST_ENTRY ExSystemLookasideListHead;
LIST_ENTRY ExPoolLookasideListHead;
GENERAL_LOOKASIDE ExpSmallNPagedPoolLookasideLists[NUMBER_POOL_LOOKASIDE_LISTS];
GENERAL_LOOKASIDE ExpSmallPagedPoolLookasideLists[NUMBER_POOL_LOOKASIDE_LISTS];

/* PRIVATE FUNCTIONS *********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
ExInitializeSystemLookasideList(IN PGENERAL_LOOKASIDE List,
                                IN POOL_TYPE Type,
                                IN ULONG Size,
                                IN ULONG Tag,
                                IN USHORT MaximumDepth,
                                IN PLIST_ENTRY ListHead)
{
    /* Initialize the list */
    List->Tag = Tag;
    List->Type = Type;
    List->Size = Size;
    InsertHeadList(ListHead, &List->ListEntry);
    List->MaximumDepth = MaximumDepth;
    List->Depth = 2;
    List->Allocate = ExAllocatePoolWithTag;
    List->Free = ExFreePool;
    InitializeSListHead(&List->ListHead);
    List->TotalAllocates = 0;
    List->AllocateHits = 0;
    List->TotalFrees = 0;
    List->FreeHits = 0;
    List->LastTotalAllocates = 0;
    List->LastAllocateHits = 0;
}

CODE_SEG("INIT")
VOID
NTAPI
ExInitPoolLookasidePointers(VOID)
{
    ULONG i;
    PKPRCB Prcb = KeGetCurrentPrcb();
    PGENERAL_LOOKASIDE Entry;

    /* Loop for all pool lists */
    for (i = 0; i < NUMBER_POOL_LOOKASIDE_LISTS; i++)
    {
        /* Initialize the non-paged list */
        Entry = &ExpSmallNPagedPoolLookasideLists[i];
        InitializeSListHead(&Entry->ListHead);

        /* Bind to PRCB */
        Prcb->PPNPagedLookasideList[i].P = Entry;
        Prcb->PPNPagedLookasideList[i].L = Entry;

        /* Initialize the paged list */
        Entry = &ExpSmallPagedPoolLookasideLists[i];
        InitializeSListHead(&Entry->ListHead);

        /* Bind to PRCB */
        Prcb->PPPagedLookasideList[i].P = Entry;
        Prcb->PPPagedLookasideList[i].L = Entry;
    }
}

CODE_SEG("INIT")
VOID
NTAPI
ExpInitLookasideLists(VOID)
{
    ULONG i;

    /* Initialize locks and lists */
    InitializeListHead(&ExpNonPagedLookasideListHead);
    InitializeListHead(&ExpPagedLookasideListHead);
    InitializeListHead(&ExSystemLookasideListHead);
    InitializeListHead(&ExPoolLookasideListHead);
    KeInitializeSpinLock(&ExpNonPagedLookasideListLock);
    KeInitializeSpinLock(&ExpPagedLookasideListLock);

    /* Initialize the system lookaside lists */
    for (i = 0; i < NUMBER_POOL_LOOKASIDE_LISTS; i++)
    {
        /* Initialize the non-paged list */
        ExInitializeSystemLookasideList(&ExpSmallNPagedPoolLookasideLists[i],
                                        NonPagedPool,
                                        (i + 1) * 8,
                                        'looP',
                                        256,
                                        &ExPoolLookasideListHead);

        /* Initialize the paged list */
        ExInitializeSystemLookasideList(&ExpSmallPagedPoolLookasideLists[i],
                                        PagedPool,
                                        (i + 1) * 8,
                                        'looP',
                                        256,
                                        &ExPoolLookasideListHead);
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
PVOID
NTAPI
ExiAllocateFromPagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside)
{
    PVOID Entry;

    Lookaside->L.TotalAllocates++;
    Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
    if (!Entry)
    {
        Lookaside->L.AllocateMisses++;
        Entry = (Lookaside->L.Allocate)(Lookaside->L.Type,
                                        Lookaside->L.Size,
                                        Lookaside->L.Tag);
    }
    return Entry;
}

/*
 * @implemented
 */
VOID
NTAPI
ExiFreeToPagedLookasideList(IN PPAGED_LOOKASIDE_LIST  Lookaside,
                            IN PVOID  Entry)
{
    Lookaside->L.TotalFrees++;
    if (ExQueryDepthSList(&Lookaside->L.ListHead) >= Lookaside->L.Depth)
    {
        Lookaside->L.FreeMisses++;
        (Lookaside->L.Free)(Entry);
    }
    else
    {
        InterlockedPushEntrySList(&Lookaside->L.ListHead, (PSLIST_ENTRY)Entry);
    }
}

/*
 * @implemented
 */
VOID
NTAPI
ExDeleteNPagedLookasideList(IN PNPAGED_LOOKASIDE_LIST Lookaside)
{
    KIRQL OldIrql;
    PVOID Entry;

    /* Pop all entries off the stack and release their resources */
    for (;;)
    {
        Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
        if (!Entry) break;
        (*Lookaside->L.Free)(Entry);
    }

    /* Remove from list */
    KeAcquireSpinLock(&ExpNonPagedLookasideListLock, &OldIrql);
    RemoveEntryList(&Lookaside->L.ListEntry);
    KeReleaseSpinLock(&ExpNonPagedLookasideListLock, OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
ExDeletePagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside)
{
    KIRQL OldIrql;
    PVOID Entry;

    /* Pop all entries off the stack and release their resources */
    for (;;)
    {
        Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
        if (!Entry) break;
        (*Lookaside->L.Free)(Entry);
    }

    /* Remove from list */
    KeAcquireSpinLock(&ExpPagedLookasideListLock, &OldIrql);
    RemoveEntryList(&Lookaside->L.ListEntry);
    KeReleaseSpinLock(&ExpPagedLookasideListLock, OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
ExInitializeNPagedLookasideList(IN PNPAGED_LOOKASIDE_LIST Lookaside,
                                IN PALLOCATE_FUNCTION Allocate OPTIONAL,
                                IN PFREE_FUNCTION Free OPTIONAL,
                                IN ULONG Flags,
                                IN SIZE_T Size,
                                IN ULONG Tag,
                                IN USHORT Depth)
{
    /* Initialize the Header */
    ExInitializeSListHead(&Lookaside->L.ListHead);
    Lookaside->L.TotalAllocates = 0;
    Lookaside->L.AllocateMisses = 0;
    Lookaside->L.TotalFrees = 0;
    Lookaside->L.FreeMisses = 0;
    Lookaside->L.Type = NonPagedPool | Flags;
    Lookaside->L.Tag = Tag;
    Lookaside->L.Size = (ULONG)Size;
    Lookaside->L.Depth = 4;
    Lookaside->L.MaximumDepth = 256;
    Lookaside->L.LastTotalAllocates = 0;
    Lookaside->L.LastAllocateMisses = 0;

    /* Set the Allocate/Free Routines */
    if (Allocate)
    {
        Lookaside->L.Allocate = Allocate;
    }
    else
    {
        Lookaside->L.Allocate = ExAllocatePoolWithTag;
    }

    if (Free)
    {
        Lookaside->L.Free = Free;
    }
    else
    {
        Lookaside->L.Free = ExFreePool;
    }

    /* Insert it into the list */
    ExInterlockedInsertTailList(&ExpNonPagedLookasideListHead,
                                &Lookaside->L.ListEntry,
                                &ExpNonPagedLookasideListLock);
}

/*
 * @implemented
 */
VOID
NTAPI
ExInitializePagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside,
                               IN PALLOCATE_FUNCTION Allocate OPTIONAL,
                               IN PFREE_FUNCTION Free OPTIONAL,
                               IN ULONG Flags,
                               IN SIZE_T Size,
                               IN ULONG Tag,
                               IN USHORT Depth)
{
    /* Initialize the Header */
    ExInitializeSListHead(&Lookaside->L.ListHead);
    Lookaside->L.TotalAllocates = 0;
    Lookaside->L.AllocateMisses = 0;
    Lookaside->L.TotalFrees = 0;
    Lookaside->L.FreeMisses = 0;
    Lookaside->L.Type = PagedPool | Flags;
    Lookaside->L.Tag = Tag;
    Lookaside->L.Size = (ULONG)Size;
    Lookaside->L.Depth = 4;
    Lookaside->L.MaximumDepth = 256;
    Lookaside->L.LastTotalAllocates = 0;
    Lookaside->L.LastAllocateMisses = 0;

    /* Set the Allocate/Free Routines */
    if (Allocate)
    {
        Lookaside->L.Allocate = Allocate;
    }
    else
    {
        Lookaside->L.Allocate = ExAllocatePoolWithTag;
    }

    if (Free)
    {
        Lookaside->L.Free = Free;
    }
    else
    {
        Lookaside->L.Free = ExFreePool;
    }

    /* Insert it into the list */
    ExInterlockedInsertTailList(&ExpPagedLookasideListHead,
                                &Lookaside->L.ListEntry,
                                &ExpPagedLookasideListLock);
}

/* Pool bits a LOOKASIDE_LIST_EX may carry; raise and quota flags go through Flags */
#define EXP_LOOKASIDE_EX_POOL_BITS 0x3E7

/* Bit 0 of a pool type tells paged pool from nonpaged pool */
#define EXP_IS_PAGED_POOL(Type) (((Type) & 1) == PagedPool)

static
PVOID
NTAPI
ExpAllocateLookasideEntryEx(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _Inout_ PLOOKASIDE_LIST_EX Lookaside)
{
    UNREFERENCED_PARAMETER(Lookaside);

    if (PoolType & POOL_QUOTA_FAIL_INSTEAD_OF_RAISE)
        return ExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, Tag);

    return ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
}

static
VOID
NTAPI
ExpFreeLookasideEntryEx(
    _In_ PVOID Buffer,
    _Inout_ PLOOKASIDE_LIST_EX Lookaside)
{
    UNREFERENCED_PARAMETER(Lookaside);
    ExFreePool(Buffer);
}

/**
 * @brief
 * Initializes a lookaside list that may use any pool type.
 *
 * @param[out] Lookaside
 * The list to initialize.
 *
 * @param[in] Allocate
 * Optional allocation routine. The pool is used when NULL.
 *
 * @param[in] Free
 * Optional free routine. The pool is used when NULL.
 *
 * @param[in] PoolType
 * Pool the entries come from.
 *
 * @param[in] Flags
 * EX_LOOKASIDE_LIST_EX_FLAGS_RAISE_ON_FAIL or
 * EX_LOOKASIDE_LIST_EX_FLAGS_FAIL_NO_RAISE.
 *
 * @param[in] Size
 * Size of each entry.
 *
 * @param[in] Tag
 * Pool tag of the entries.
 *
 * @param[in] Depth
 * Zero for the default depth, otherwise 256 to 1024.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER_4, _5 or _8 for a bad pool
 * type, flag or depth.
 */
NTSTATUS
NTAPI
ExInitializeLookasideListEx(
    _Out_ PLOOKASIDE_LIST_EX Lookaside,
    _In_opt_ PALLOCATE_FUNCTION_EX Allocate,
    _In_opt_ PFREE_FUNCTION_EX Free,
    _In_ POOL_TYPE PoolType,
    _In_ ULONG Flags,
    _In_ SIZE_T Size,
    _In_ ULONG Tag,
    _In_ USHORT Depth)
{
    PGENERAL_LOOKASIDE_POOL List = &Lookaside->L;
    ULONG PoolFlags = 0;

    if (Depth == 0)
    {
        Depth = EX_MAXIMUM_LOOKASIDE_DEPTH_BASE;
    }
    else if ((Depth < EX_MAXIMUM_LOOKASIDE_DEPTH_BASE) ||
             (Depth > EX_MAXIMUM_LOOKASIDE_DEPTH_LIMIT))
    {
        return STATUS_INVALID_PARAMETER_8;
    }

    if (Flags == EX_LOOKASIDE_LIST_EX_FLAGS_RAISE_ON_FAIL)
        PoolFlags = POOL_RAISE_IF_ALLOCATION_FAILURE;
    else if (Flags == EX_LOOKASIDE_LIST_EX_FLAGS_FAIL_NO_RAISE)
        PoolFlags = POOL_QUOTA_FAIL_INSTEAD_OF_RAISE;
    else if (Flags != 0)
        return STATUS_INVALID_PARAMETER_5;

    if ((PoolType & ~EXP_LOOKASIDE_EX_POOL_BITS) ||
        ((PoolType & 3) == 3))
    {
        return STATUS_INVALID_PARAMETER_4;
    }

    if (Size < LOOKASIDE_MINIMUM_BLOCK_SIZE)
        Size = LOOKASIDE_MINIMUM_BLOCK_SIZE;

    InitializeSListHead(&List->ListHead);
    List->Depth = 4;
    List->MaximumDepth = Depth;
    List->TotalAllocates = 0;
    List->AllocateMisses = 0;
    List->TotalFrees = 0;
    List->FreeMisses = 0;
    List->Type = PoolType | PoolFlags;
    List->Tag = Tag;
    List->Size = (ULONG)Size;
    List->AllocateEx = Allocate ? Allocate : ExpAllocateLookasideEntryEx;
    List->FreeEx = Free ? Free : ExpFreeLookasideEntryEx;
    List->LastTotalAllocates = 0;
    List->LastAllocateMisses = 0;

    if (EXP_IS_PAGED_POOL(PoolType))
    {
        ExInterlockedInsertTailList(&ExpPagedLookasideListHead,
                                    &List->ListEntry,
                                    &ExpPagedLookasideListLock);
    }
    else
    {
        ExInterlockedInsertTailList(&ExpNonPagedLookasideListHead,
                                    &List->ListEntry,
                                    &ExpNonPagedLookasideListLock);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Frees every entry held by a lookaside list.
 *
 * @param[in,out] Lookaside
 * The list to flush.
 */
VOID
NTAPI
ExFlushLookasideListEx(
    _Inout_ PLOOKASIDE_LIST_EX Lookaside)
{
    PSLIST_ENTRY Entry;
    PSLIST_ENTRY Next;

    for (Entry = InterlockedFlushSList(&Lookaside->L.ListHead); Entry; Entry = Next)
    {
        Next = Entry->Next;
        Lookaside->L.FreeEx(Entry, Lookaside);
    }
}

/**
 * @brief
 * Removes a lookaside list from the system and frees its entries.
 *
 * @param[in,out] Lookaside
 * The list to delete.
 */
VOID
NTAPI
ExDeleteLookasideListEx(
    _Inout_ PLOOKASIDE_LIST_EX Lookaside)
{
    PKSPIN_LOCK ListLock;
    KIRQL OldIrql;

    if (EXP_IS_PAGED_POOL(Lookaside->L.Type))
        ListLock = &ExpPagedLookasideListLock;
    else
        ListLock = &ExpNonPagedLookasideListLock;

    KeAcquireSpinLock(ListLock, &OldIrql);
    RemoveEntryList(&Lookaside->L.ListEntry);
    KeReleaseSpinLock(ListLock, OldIrql);

    /* Frees from now on go straight back to the pool */
    Lookaside->L.Depth = 0;
    ExFlushLookasideListEx(Lookaside);
}

/* EOF */
