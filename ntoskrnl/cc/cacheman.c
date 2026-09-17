/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/cc/cacheman.c
 * PURPOSE:         Cache manager
 *
 * PROGRAMMERS:     David Welch (welch@cwcom.net)
 *                  Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

BOOLEAN CcPfEnablePrefetcher;
PFSN_PREFETCHER_GLOBALS CcPfGlobals;
MM_SYSTEMSIZE CcCapturedSystemSize;

static ULONG BugCheckFileId = 0x4 << 16;

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
CcPfInitializePrefetcher(VOID)
{
    /* Notify debugger */
    DbgPrintEx(DPFLTR_PREFETCHER_ID,
               DPFLTR_TRACE_LEVEL,
               "CCPF: InitializePrefetecher()\n");

    /* Setup the Prefetcher Data */
    InitializeListHead(&CcPfGlobals.ActiveTraces);
    InitializeListHead(&CcPfGlobals.CompletedTraces);
    ExInitializeFastMutex(&CcPfGlobals.CompletedTracesLock);

    /* FIXME: Setup the rest of the prefetecher */
}

CODE_SEG("INIT")
BOOLEAN
CcInitializeCacheManager(VOID)
{
    ULONG Thread;

    CcInitView();

    /* Initialize lazy-writer lists */
    InitializeListHead(&CcIdleWorkerThreadList);
    InitializeListHead(&CcExpressWorkQueue);
    InitializeListHead(&CcRegularWorkQueue);
    InitializeListHead(&CcPostTickWorkQueue);

    /* Define lazy writer threshold and the amount of workers,
      * depending on the system type
      */
    CcCapturedSystemSize = MmQuerySystemSize();
    switch (CcCapturedSystemSize)
    {
        case MmSmallSystem:
            CcNumberWorkerThreads = ExCriticalWorkerThreads - 1;
            CcDirtyPageThreshold = MmNumberOfPhysicalPages / 8;
            break;

        case MmMediumSystem:
            CcNumberWorkerThreads = ExCriticalWorkerThreads - 1;
            CcDirtyPageThreshold = MmNumberOfPhysicalPages / 4;
            break;

        case MmLargeSystem:
            CcNumberWorkerThreads = ExCriticalWorkerThreads - 2;
            CcDirtyPageThreshold = MmNumberOfPhysicalPages / 8 + MmNumberOfPhysicalPages / 4;
            break;

        default:
            CcNumberWorkerThreads = 1;
            CcDirtyPageThreshold = MmNumberOfPhysicalPages / 8;
            break;
    }

    /* Allocate a work item for all our threads */
    for (Thread = 0; Thread < CcNumberWorkerThreads; ++Thread)
    {
        PWORK_QUEUE_ITEM Item;

        Item = ExAllocatePoolWithTag(NonPagedPool, sizeof(WORK_QUEUE_ITEM), 'qWcC');
        if (Item == NULL)
        {
            CcBugCheck(0, 0, 0);
        }

        /* By default, it's obviously idle */
        ExInitializeWorkItem(Item, CcWorkerThread, Item);
        InsertTailList(&CcIdleWorkerThreadList, &Item->List);
    }

    /* Initialize our lazy writer */
    RtlZeroMemory(&LazyWriter, sizeof(LazyWriter));
    InitializeListHead(&LazyWriter.WorkQueue);
    /* Delay activation of the lazy writer */
    KeInitializeDpc(&LazyWriter.ScanDpc, CcScanDpc, NULL);
    KeInitializeTimer(&LazyWriter.ScanTimer);

    /* Lookaside list for our work items */
    ExInitializeNPagedLookasideList(&CcTwilightLookasideList, NULL, NULL, 0, sizeof(WORK_QUEUE_ENTRY), 'KWcC', 0);

    return TRUE;
}

VOID
NTAPI
CcShutdownSystem(VOID)
{
    /* NOTHING TO DO */
}

/*
 * @unimplemented
 */
LARGE_INTEGER
NTAPI
CcGetFlushedValidData (
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN BOOLEAN BcbListHeld
    )
{
	LARGE_INTEGER i;

	UNIMPLEMENTED;

	i.QuadPart = 0;
	return i;
}

/*
 * @unimplemented
 */
PVOID
NTAPI
CcRemapBcb (
    IN PVOID Bcb
    )
{
	UNIMPLEMENTED;

    return 0;
}

/* How far apart two reads may sit and still count as one after the other */
#define CC_READ_AHEAD_GAP 0x200

static
BOOLEAN
CcpIsReadContiguous(
    _In_ LONGLONG Offset,
    _In_ LONGLONG PreviousEnd)
{
    LONGLONG Gap = Offset - PreviousEnd;

    if (Gap < 0)
        Gap = -Gap;

    return (Gap <= CC_READ_AHEAD_GAP);
}

/*
 * @implemented
 */
VOID
NTAPI
CcScheduleReadAhead (
	IN	PFILE_OBJECT		FileObject,
	IN	PLARGE_INTEGER		FileOffset,
	IN	ULONG			Length
	)
{
    KIRQL OldIrql;
    BOOLEAN Sequential;
    LONGLONG ReadEnd, ReaderOffset, LastEnd, TargetEnd;
    ULONG ReadAheadUnit;
    PROS_SHARED_CACHE_MAP SharedCacheMap;
    PPRIVATE_CACHE_MAP PrivateCacheMap;

    SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;
    PrivateCacheMap = FileObject->PrivateCacheMap;

    /* If file isn't cached, if read ahead is disabled, or nothing was read, no op */
    if (SharedCacheMap == NULL || PrivateCacheMap == NULL || Length == 0 ||
        BooleanFlagOn(SharedCacheMap->Flags, READAHEAD_DISABLED))
    {
        return;
    }

    /* Read ahead moves in units of what the file system asked for */
    ReadAheadUnit = (Length + PrivateCacheMap->ReadAheadMask) & ~PrivateCacheMap->ReadAheadMask;
    ReadEnd = FileOffset->QuadPart + Length;

    /* Lock read ahead spin lock */
    KeAcquireSpinLock(&PrivateCacheMap->ReadAheadSpinLock, &OldIrql);

    /* From now on this stream reads ahead of whoever walks it */
    InterlockedOr((volatile long *)&PrivateCacheMap->UlongFlags,
                  PRIVATE_CACHE_MAP_READ_AHEAD_ENABLED);

    Sequential = BooleanFlagOn(FileObject->Flags, FO_SEQUENTIAL_ONLY);
    if (!Sequential)
    {
        /*
         * Without being told, a stream only counts as one being walked when this
         * read carries on where the last one ended, and that one carried on from
         * the one before it. A caller picking bytes out of a file is left alone.
         */
        if (!CcpIsReadContiguous(FileOffset->QuadPart, PrivateCacheMap->BeyondLastByte2.QuadPart) ||
            !CcpIsReadContiguous(PrivateCacheMap->FileOffset2.QuadPart,
                                 PrivateCacheMap->BeyondLastByte1.QuadPart))
        {
            PrivateCacheMap->ReadAheadLength[0] = 0;
            KeReleaseSpinLock(&PrivateCacheMap->ReadAheadSpinLock, OldIrql);
            return;
        }
    }

    /*
     * Reading ahead again only pays off once the reader has caught up with what
     * the last one reaches. Without this a caller taking a file a couple of
     * kilobytes at a time queues work for every single read it does.
     */
    ReaderOffset = ROUND_DOWN(ReadEnd, PAGE_SIZE);
    LastEnd = PrivateCacheMap->ReadAheadOffset[0].QuadPart;
    if ((ReadEnd + Length + 2 * ReadAheadUnit) < LastEnd)
    {
        KeReleaseSpinLock(&PrivateCacheMap->ReadAheadSpinLock, OldIrql);
        return;
    }

    /* Carry on from there, or from this read once it has gone past it */
    TargetEnd = LastEnd;
    if (ReaderOffset >= LastEnd)
        TargetEnd = ROUND_UP(ReadEnd, ReadAheadUnit);

    /* A file taken straight through is worth running twice as far ahead of */
    PrivateCacheMap->ReadAheadLength[0]++;
    if (Sequential || (PrivateCacheMap->ReadAheadLength[0] >= 3))
        TargetEnd += 2 * ReadAheadUnit;
    else
        TargetEnd += ReadAheadUnit;

    /*
     * One view at a time. A longer run holds a view while it reads it, and the
     * reader walking into that same view then waits behind it for no gain.
     */
    if ((TargetEnd - ReaderOffset) > VACB_MAPPING_GRANULARITY)
        TargetEnd = ReaderOffset + VACB_MAPPING_GRANULARITY;

    PrivateCacheMap->ReadAheadOffset[1].QuadPart = ReaderOffset;
    PrivateCacheMap->ReadAheadLength[1] = (ULONG)(TargetEnd - ReaderOffset);

    /* If read ahead isn't active yet */
    if (!PrivateCacheMap->Flags.ReadAheadActive)
    {
        PWORK_QUEUE_ENTRY WorkItem;

        /* It's active now!
         * Be careful with the mask, you don't want to mess with node code
         */
        InterlockedOr((volatile long *)&PrivateCacheMap->UlongFlags, PRIVATE_CACHE_MAP_READ_AHEAD_ACTIVE);
        KeReleaseSpinLock(&PrivateCacheMap->ReadAheadSpinLock, OldIrql);

        /* Get a work item */
        WorkItem = ExAllocateFromNPagedLookasideList(&CcTwilightLookasideList);
        if (WorkItem != NULL)
        {
            /* Reference our FO so that it doesn't go in between */
            ObReferenceObject(FileObject);

            /* We want to do read ahead! */
            WorkItem->Function = ReadAhead;
            WorkItem->Parameters.Read.FileObject = FileObject;

            /* Queue in the read ahead dedicated queue */
            CcPostWorkQueue(WorkItem, &CcExpressWorkQueue);

            return;
        }

        /* Fail path: lock again, and revert read ahead active */
        KeAcquireSpinLock(&PrivateCacheMap->ReadAheadSpinLock, &OldIrql);
        InterlockedAnd((volatile long *)&PrivateCacheMap->UlongFlags, ~PRIVATE_CACHE_MAP_READ_AHEAD_ACTIVE);
    }

    /* Done (fail) */
    KeReleaseSpinLock(&PrivateCacheMap->ReadAheadSpinLock, OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
CcSetAdditionalCacheAttributes (
	IN	PFILE_OBJECT	FileObject,
	IN	BOOLEAN		DisableReadAhead,
	IN	BOOLEAN		DisableWriteBehind
	)
{
    KIRQL OldIrql;
    PROS_SHARED_CACHE_MAP SharedCacheMap;

    CCTRACE(CC_API_DEBUG, "FileObject=%p DisableReadAhead=%d DisableWriteBehind=%d\n",
        FileObject, DisableReadAhead, DisableWriteBehind);

    SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;

    OldIrql = KeAcquireQueuedSpinLock(LockQueueMasterLock);

    if (DisableReadAhead)
    {
        SetFlag(SharedCacheMap->Flags, READAHEAD_DISABLED);
    }
    else
    {
        ClearFlag(SharedCacheMap->Flags, READAHEAD_DISABLED);
    }

    if (DisableWriteBehind)
    {
        /* FIXME: also set flag 0x200 */
        SetFlag(SharedCacheMap->Flags, WRITEBEHIND_DISABLED);
    }
    else
    {
        ClearFlag(SharedCacheMap->Flags, WRITEBEHIND_DISABLED);
    }
    KeReleaseQueuedSpinLock(LockQueueMasterLock, OldIrql);
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcSetBcbOwnerPointer (
	IN	PVOID	Bcb,
	IN	PVOID	Owner
	)
{
    PINTERNAL_BCB iBcb = CONTAINING_RECORD(Bcb, INTERNAL_BCB, PFCB);

    CCTRACE(CC_API_DEBUG, "Bcb=%p Owner=%p\n",
        Bcb, Owner);

    if (!ExIsResourceAcquiredExclusiveLite(&iBcb->Lock) && !ExIsResourceAcquiredSharedLite(&iBcb->Lock))
    {
        DPRINT1("Current thread doesn't own resource!\n");
        return;
    }

    ExSetResourceOwnerPointer(&iBcb->Lock, Owner);
}

/*
 * @implemented
 */
VOID
NTAPI
CcSetDirtyPageThreshold (
	IN	PFILE_OBJECT	FileObject,
	IN	ULONG		DirtyPageThreshold
	)
{
    PFSRTL_COMMON_FCB_HEADER Fcb;
    PROS_SHARED_CACHE_MAP SharedCacheMap;

    CCTRACE(CC_API_DEBUG, "FileObject=%p DirtyPageThreshold=%lu\n",
        FileObject, DirtyPageThreshold);

    SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;
    if (SharedCacheMap != NULL)
    {
        SharedCacheMap->DirtyPageThreshold = DirtyPageThreshold;
    }

    Fcb = FileObject->FsContext;
    if (!BooleanFlagOn(Fcb->Flags, FSRTL_FLAG_LIMIT_MODIFIED_PAGES))
    {
        SetFlag(Fcb->Flags, FSRTL_FLAG_LIMIT_MODIFIED_PAGES);
    }
}

/*
 * @implemented
 */
VOID
NTAPI
CcSetReadAheadGranularity (
	IN	PFILE_OBJECT	FileObject,
	IN	ULONG		Granularity
	)
{
    PPRIVATE_CACHE_MAP PrivateMap;

    CCTRACE(CC_API_DEBUG, "FileObject=%p Granularity=%lu\n",
        FileObject, Granularity);

    PrivateMap = FileObject->PrivateCacheMap;
    PrivateMap->ReadAheadMask = Granularity - 1;
}
