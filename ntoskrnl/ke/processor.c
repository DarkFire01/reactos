/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Portable processor related routines
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

KAFFINITY KeActiveProcessors = 0;

/* Number of processors */
CCHAR KeNumberProcessors = 0;

#ifdef CONFIG_SMP

/* Theoretical maximum number of processors that can be handled.
 * Set once at run-time. Returned by KeQueryMaximumProcessorCount(). */
ULONG KeMaximumProcessors = MAXIMUM_PROCESSORS;

/* Maximum number of logical processors that can be started
 * (including dynamically) at run-time. If 0: do not perform checks. */
ULONG KeNumprocSpecified = 0;

/* Maximum number of logical processors that can be started
 * at boot-time. If 0: do not perform checks. */
ULONG KeBootprocSpecified = 0;

#endif // CONFIG_SMP

/* FUNCTIONS *****************************************************************/

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    return KeActiveProcessors;
}

/**
 * @brief
 * Counts the processors that are running, and optionally hands back which.
 *
 * @param[out] ActiveProcessors
 * Receives the affinity of the running processors, when asked for.
 *
 * @return
 * How many processors are running.
 */
ULONG
NTAPI
KeQueryActiveProcessorCount(
    _Out_opt_ PKAFFINITY ActiveProcessors)
{
    RTL_BITMAP Bitmap;
    KAFFINITY ActiveMap = KeQueryActiveProcessors();

    if (ActiveProcessors != NULL)
    {
        *ActiveProcessors = ActiveMap;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)&ActiveMap, sizeof(ActiveMap) * 8);
    return RtlNumberOfSetBits(&Bitmap);
}

/**
 * @brief
 * Returns how many processors the system can ever run.
 *
 * @return
 * The count of processors that are online. ReactOS never brings a processor
 * up after boot, so this is also the most it will ever run.
 */
ULONG
NTAPI
KeQueryMaximumProcessorCount(VOID)
{
    return KeQueryActiveProcessorCount(NULL);
}

/**
 * Retrieves the number of the current processor.
 *
 * \param ProcessorNumber Pointer to a PROCESSOR_NUMBER structure that receives the processor number.
 *
 * \return NTSTATUS The status of the operation.
 */
NTSTATUS
NTAPI
NtGetCurrentProcessorNumberEx(
    _Out_ PPROCESSOR_NUMBER ProcessorNumber)
{
    _SEH2_TRY
    {
        ProbeForWrite(ProcessorNumber, sizeof(PROCESSOR_NUMBER), __alignof(PROCESSOR_NUMBER));
        ProcessorNumber->Group = 0; // TODO: Support processor groups
        ProcessorNumber->Number = (UCHAR)KeGetCurrentProcessorNumber();
        ProcessorNumber->Reserved = 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        return _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

/*
 * The kernel is compiled for an older target than the drivers that call this, where
 * KE_PROCESSOR_CHANGE_NOTIFY_CONTEXT still ended at Status. A caller reads the later shape, so
 * the notification is built in that one and the extra field is always there to be read.
 */
typedef struct _KI_PROCESSOR_CHANGE_NOTIFY_CONTEXT
{
    KE_PROCESSOR_CHANGE_NOTIFY_STATE State;
    ULONG NtNumber;
    NTSTATUS Status;
    PROCESSOR_NUMBER ProcNumber;
} KI_PROCESSOR_CHANGE_NOTIFY_CONTEXT;

typedef struct _KI_PROCESSOR_CHANGE_CALLBACK
{
    LIST_ENTRY ListEntry;
    PPROCESSOR_CALLBACK_FUNCTION CallbackFunction;
    PVOID CallbackContext;
} KI_PROCESSOR_CHANGE_CALLBACK, *PKI_PROCESSOR_CHANGE_CALLBACK;

#define TAG_PROCESSOR_CALLBACK 'cPeK'

/* Self referencing so that no separate initialization is needed before the first caller. */
static LIST_ENTRY KiProcessorChangeCallbackList =
{
    &KiProcessorChangeCallbackList,
    &KiProcessorChangeCallbackList
};

static KSPIN_LOCK KiProcessorChangeCallbackLock;

/* Tell one callback about one processor, and report back what it made of it. */
static
NTSTATUS
KiNotifyProcessorChange(
    _In_ PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
    _In_opt_ PVOID CallbackContext,
    _In_ KE_PROCESSOR_CHANGE_NOTIFY_STATE State,
    _In_ ULONG Number)
{
    KI_PROCESSOR_CHANGE_NOTIFY_CONTEXT ChangeContext;
    NTSTATUS OperationStatus = STATUS_SUCCESS;

    ChangeContext.State = State;
    ChangeContext.NtNumber = Number;
    ChangeContext.Status = STATUS_SUCCESS;
    ChangeContext.ProcNumber.Group = 0;
    ChangeContext.ProcNumber.Number = (UCHAR)Number;
    ChangeContext.ProcNumber.Reserved = 0;

    CallbackFunction(CallbackContext,
                     (PKE_PROCESSOR_CHANGE_NOTIFY_CONTEXT)&ChangeContext,
                     &OperationStatus);

    return OperationStatus;
}

/**
 * \brief Registers a callback to be told when a processor is added to the system.
 *
 * \param CallbackFunction The routine to call.
 * \param CallbackContext Passed back to the routine untouched.
 * \param Flags KE_PROCESSOR_CHANGE_ADD_EXISTING to also be told about the processors that are
 *        already running, each of which is reported as an add that has completed.
 *
 * \return An opaque handle for KeDeregisterProcessorChangeCallback, or NULL.
 *
 * \remarks No processor is ever added after boot here, so a registration that survives this
 *          call will not be called again. The list is still kept so that the handle stays
 *          valid and deregistration behaves.
 */
PVOID
NTAPI
KeRegisterProcessorChangeCallback(
    _In_ PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
    _In_opt_ PVOID CallbackContext,
    _In_ ULONG Flags)
{
    PKI_PROCESSOR_CHANGE_CALLBACK Callback;
    NTSTATUS Status;
    ULONG Number;
    ULONG Undo;

    if (CallbackFunction == NULL)
        return NULL;

    Callback = ExAllocatePoolWithTag(NonPagedPool,
                                     sizeof(*Callback),
                                     TAG_PROCESSOR_CALLBACK);
    if (Callback == NULL)
        return NULL;

    Callback->CallbackFunction = CallbackFunction;
    Callback->CallbackContext = CallbackContext;

    if (Flags & KE_PROCESSOR_CHANGE_ADD_EXISTING)
    {
        for (Number = 0; Number < (ULONG)KeNumberProcessors; Number++)
        {
            Status = KiNotifyProcessorChange(CallbackFunction,
                                             CallbackContext,
                                             KeProcessorAddCompleteNotify,
                                             Number);
            if (NT_SUCCESS(Status))
                continue;

            /* It refused one, so take back the ones it has already been told about. */
            for (Undo = 0; Undo < Number; Undo++)
            {
                KiNotifyProcessorChange(CallbackFunction,
                                        CallbackContext,
                                        KeProcessorAddFailureNotify,
                                        Undo);
            }

            ExFreePoolWithTag(Callback, TAG_PROCESSOR_CALLBACK);
            return NULL;
        }
    }

    ExInterlockedInsertTailList(&KiProcessorChangeCallbackList,
                                &Callback->ListEntry,
                                &KiProcessorChangeCallbackLock);

    return Callback;
}

/**
 * \brief Drops a registration made by KeRegisterProcessorChangeCallback.
 *
 * \param CallbackHandle The handle that registration returned.
 */
VOID
NTAPI
KeDeregisterProcessorChangeCallback(
    _In_ PVOID CallbackHandle)
{
    PKI_PROCESSOR_CHANGE_CALLBACK Callback = CallbackHandle;
    KIRQL OldIrql;

    if (Callback == NULL)
        return;

    KeAcquireSpinLock(&KiProcessorChangeCallbackLock, &OldIrql);
    RemoveEntryList(&Callback->ListEntry);
    KeReleaseSpinLock(&KiProcessorChangeCallbackLock, OldIrql);

    ExFreePoolWithTag(Callback, TAG_PROCESSOR_CALLBACK);
}

/**
 * \brief Returns the highest NUMA node number in the system.
 *
 * \return The highest node number, which is one less than the count of nodes.
 */
USHORT
NTAPI
KeQueryHighestNodeNumber(VOID)
{
    return (USHORT)(KeNumberNodes - 1);
}

/**
 * \brief Returns the NUMA node of the processor running the caller.
 *
 * \return The node number, always 0 since every processor lives in node 0.
 */
USHORT
NTAPI
KeGetCurrentNodeNumber(VOID)
{
    return 0;
}

/*
 * ReactOS runs the single processor group model, so every logical processor
 * lives in group 0 and the group aware APIs below sit on the plain ones.
 */

/**
 * @brief
 * Turns a group and number pair into a system wide processor index.
 *
 * @param[in] ProcNumber
 * The processor number to translate.
 *
 * @return
 * The processor index, or INVALID_PROCESSOR_INDEX when @p ProcNumber names no
 * active processor.
 */
ULONG
NTAPI
KeGetProcessorIndexFromNumber(
    _In_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcNumber->Reserved != 0 ||
        ProcNumber->Group != 0 ||
        ProcNumber->Number >= (ULONG)KeNumberProcessors)
    {
        return INVALID_PROCESSOR_INDEX;
    }

    return ProcNumber->Number;
}

/**
 * @brief
 * Turns a system wide processor index into a group and number pair.
 *
 * @param[in] ProcIndex
 * The processor index to translate.
 *
 * @param[out] ProcNumber
 * Receives the matching processor number.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER when @p ProcIndex is out of
 * range.
 */
NTSTATUS
NTAPI
KeGetProcessorNumberFromIndex(
    _In_ ULONG ProcIndex,
    _Out_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcIndex >= (ULONG)KeNumberProcessors)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ProcNumber->Group = 0;
    ProcNumber->Number = (UCHAR)ProcIndex;
    ProcNumber->Reserved = 0;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns how many processors are active in a group.
 *
 * @param[in] GroupNumber
 * The group to look at, or ALL_PROCESSOR_GROUPS for every group.
 *
 * @return
 * The active processor count, or zero for a group that does not exist.
 */
ULONG
NTAPI
KeQueryActiveProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0 && GroupNumber != ALL_PROCESSOR_GROUPS)
    {
        return 0;
    }

    return KeQueryActiveProcessorCount(NULL);
}

/**
 * @brief
 * Returns how many processors a group can ever run.
 *
 * @param[in] GroupNumber
 * The group to look at, or ALL_PROCESSOR_GROUPS for every group.
 *
 * @return
 * The maximum processor count, or zero for a group that does not exist.
 */
ULONG
NTAPI
KeQueryMaximumProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0 && GroupNumber != ALL_PROCESSOR_GROUPS)
    {
        return 0;
    }

    return KeQueryMaximumProcessorCount();
}

/**
 * @brief
 * Returns how many processor groups the system supports.
 *
 * @return
 * One, as ReactOS only ever builds group 0.
 */
USHORT
NTAPI
KeQueryMaximumGroupCount(VOID)
{
    return 1;
}

/**
 * @brief
 * Returns the affinity of the processors that are active in a group.
 *
 * @param[in] GroupNumber
 * The group to look at.
 *
 * @return
 * The affinity mask, or zero for a group that does not exist.
 */
KAFFINITY
NTAPI
KeQueryGroupAffinity(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0)
    {
        return 0;
    }

    return KeQueryActiveProcessors();
}

/**
 * @brief
 * Returns the affinity of the processors assigned to a group.
 *
 * @param[in] GroupNumber
 * The group to look at.
 *
 * @return
 * The affinity mask, or zero for a group that does not exist.
 */
KAFFINITY
NTAPI
KeProcessorGroupAffinity(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0)
    {
        return 0;
    }

    return KeQueryActiveProcessors();
}

/**
 * @brief
 * Returns the active processors of a NUMA node.
 *
 * @param[in] NodeNumber
 * The node to look at.
 *
 * @param[out] Affinity
 * Optionally receives the group affinity of the node.
 *
 * @param[out] Count
 * Optionally receives the active processor count of the node.
 *
 * @remarks
 * ReactOS models one NUMA node, and every processor belongs to it.
 */
VOID
NTAPI
KeQueryNodeActiveAffinity(
    _In_ USHORT NodeNumber,
    _Out_opt_ PGROUP_AFFINITY Affinity,
    _Out_opt_ PUSHORT Count)
{
    KAFFINITY ActiveProcessors = 0;

    if (NodeNumber == 0)
    {
        ActiveProcessors = KeQueryActiveProcessors();
    }

    if (Affinity != NULL)
    {
        RtlZeroMemory(Affinity, sizeof(*Affinity));
        Affinity->Mask = ActiveProcessors;
        Affinity->Group = 0;
    }

    if (Count != NULL)
    {
        *Count = (NodeNumber == 0) ? (USHORT)KeQueryActiveProcessorCount(NULL) : 0;
    }
}

/**
 * @brief
 * Pins the current thread to an affinity and hands back what to restore.
 *
 * @param[in] Affinity
 * The affinity to apply.
 *
 * @return
 * The affinity that was in force, or zero when the thread was on its user
 * affinity. Either value put back through KeRevertToUserAffinityThreadEx()
 * returns the thread to where it was.
 */
KAFFINITY
NTAPI
KeSetSystemAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    PKTHREAD Thread = KeGetCurrentThread();
    KAFFINITY Previous = 0;

    /* A thread already being held to an affinity has to be put back on that
       one rather than released, or the caller that set it loses its hold */
    if (Thread->SystemAffinityActive)
        Previous = Thread->Affinity;

    KeSetSystemAffinityThread(Affinity);
    return Previous;
}

/**
 * @brief
 * Puts the affinity of the current thread back.
 *
 * @param[in] Affinity
 * The value handed out by KeSetSystemAffinityThreadEx().
 */
VOID
NTAPI
KeRevertToUserAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    if (Affinity != 0)
    {
        KeSetSystemAffinityThread(Affinity);
    }
    else
    {
        KeRevertToUserAffinityThread();
    }
}

/**
 * @brief
 * Pins the current thread to a group affinity.
 *
 * @param[in] Affinity
 * The group affinity to apply.
 *
 * @param[out] PreviousAffinity
 * Optionally receives the group affinity that was in force.
 */
VOID
NTAPI
KeSetSystemGroupAffinityThread(
    _In_ PGROUP_AFFINITY Affinity,
    _Out_opt_ PGROUP_AFFINITY PreviousAffinity)
{
    PKTHREAD Thread = KeGetCurrentThread();

    /* Read before the change, and leave an empty mask behind when the thread
       was on its user affinity: that is what sends the matching revert back */
    if (PreviousAffinity != NULL)
    {
        RtlZeroMemory(PreviousAffinity, sizeof(*PreviousAffinity));

        if (Thread->SystemAffinityActive)
            PreviousAffinity->Mask = Thread->Affinity;
    }

    KeSetSystemAffinityThread(Affinity->Mask);
}

/**
 * @brief
 * Puts the group affinity of the current thread back.
 *
 * @param[in] PreviousAffinity
 * The group affinity handed out by KeSetSystemGroupAffinityThread().
 */
VOID
NTAPI
KeRevertToUserGroupAffinityThread(
    _In_ PGROUP_AFFINITY PreviousAffinity)
{
    if (PreviousAffinity != NULL && PreviousAffinity->Mask != 0)
    {
        KeSetSystemAffinityThread(PreviousAffinity->Mask);
    }
    else
    {
        KeRevertToUserAffinityThread();
    }
}

/**
 * @brief
 * Brings a processor that was added at run time online.
 *
 * @param[in] ProcessorState
 * The starting state of the new processor.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
KeStartDynamicProcessor(
    _In_ PVOID ProcessorState)
{
    UNREFERENCED_PARAMETER(ProcessorState);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Returns the processor the caller is running on.
 *
 * @param[out] ProcNumber
 * Optionally receives the processor as a group and number pair.
 *
 * @return
 * The system wide index of the current processor.
 */
ULONG
NTAPI
KeGetCurrentProcessorNumberEx(
    _Out_opt_ PPROCESSOR_NUMBER ProcNumber)
{
    ULONG Index = KeGetCurrentProcessorNumber();

    if (ProcNumber != NULL)
    {
        ProcNumber->Group = 0;
        ProcNumber->Number = (UCHAR)Index;
        ProcNumber->Reserved = 0;
    }

    return Index;
}

/**
 * @brief
 * Picks the processor a DPC runs on, by group and number.
 *
 * @param[in,out] Dpc
 * The DPC to retarget. One that is already queued keeps its processor.
 *
 * @param[in] ProcNumber
 * The processor to run it on.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER when @p ProcNumber names no
 * active processor.
 */
NTSTATUS
NTAPI
KeSetTargetProcessorDpcEx(
    _Inout_ PKDPC Dpc,
    _In_ PPROCESSOR_NUMBER ProcNumber)
{
    ULONG Index = KeGetProcessorIndexFromNumber(ProcNumber);

    if (Index == INVALID_PROCESSOR_INDEX)
        return STATUS_INVALID_PARAMETER;

    if (Dpc->DpcData == NULL)
        KeSetTargetProcessorDpc(Dpc, (CCHAR)Index);

    return STATUS_SUCCESS;
}

/* Room for one record, zeroed and labeled; the needed length grows whether or not it fits */
static
PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX
KiReserveRelationRecord(
    _Inout_ PUCHAR *Cursor,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Relationship,
    _In_ ULONG RecordSize,
    _In_ ULONG BufferLength,
    _Inout_ PULONG Needed,
    _Inout_ PNTSTATUS Status)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Record;

    *Needed += RecordSize;
    if (*Needed > BufferLength)
    {
        *Status = STATUS_INFO_LENGTH_MISMATCH;
        return NULL;
    }

    Record = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)*Cursor;
    RtlZeroMemory(Record, RecordSize);
    Record->Relationship = Relationship;
    Record->Size = RecordSize;
    *Cursor += RecordSize;

    return Record;
}

/**
 * @brief
 * Describes the packages, cores, caches, NUMA nodes and groups of the
 * processors.
 *
 * @param[in] ProcessorNumber
 * The processor to describe, or NULL for every processor.
 *
 * @param[in] RelationshipType
 * The kind of record to return, or RelationAll.
 *
 * @param[out] Information
 * Receives the records.
 *
 * @param[in,out] Length
 * The size of @p Information on input, the size every record needs on output.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INFO_LENGTH_MISMATCH when the buffer is too small,
 * STATUS_INVALID_PARAMETER for a processor that does not exist, or
 * STATUS_UNSUCCESSFUL when nothing matched.
 *
 * @remarks
 * ReactOS does not read the processor topology, so every processor is its own
 * core, all of them share one package, and each cache belongs to the processor
 * that reported it.
 */
NTSTATUS
NTAPI
KeQueryLogicalProcessorRelationship(
    _In_opt_ PPROCESSOR_NUMBER ProcessorNumber,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_opt_(*Length) PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Information,
    _Inout_ PULONG Length)
{
    const ULONG ProcessorRecordSize =
        FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor.GroupMask) + sizeof(GROUP_AFFINITY);
    const ULONG CacheRecordSize =
        FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache.GroupMask) + sizeof(GROUP_AFFINITY);
    const ULONG NodeRecordSize =
        FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode.GroupMask) + sizeof(GROUP_AFFINITY);
    const ULONG GroupRecordSize =
        ALIGN_UP_BY(FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group.GroupInfo) +
                    sizeof(PROCESSOR_GROUP_INFO), sizeof(ULONG));
    BOOLEAN All = (RelationshipType == RelationAll);
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Record;
    KAFFINITY CoresSeen = 0, PackagesSeen = 0, Mask;
    ULONG First, Last, Index, CacheIndex, Node;
    ULONG BufferLength = *Length, Needed = 0;
    PUCHAR Cursor = (PUCHAR)Information;
    NTSTATUS Status = STATUS_SUCCESS;
    PKPRCB Prcb;

    if (ProcessorNumber != NULL)
    {
        First = KeGetProcessorIndexFromNumber(ProcessorNumber);
        if (First >= (ULONG)KeNumberProcessors)
            return STATUS_INVALID_PARAMETER;
        Last = First;
    }
    else
    {
        First = 0;
        Last = KeNumberProcessors - 1;
    }

    for (Index = First; Index <= Last; Index++)
    {
        Prcb = KiProcessorBlock[Index];

        if (All || RelationshipType == RelationProcessorPackage)
        {
            Mask = KeActiveProcessors;
            if (ProcessorNumber != NULL || !(PackagesSeen & Mask))
            {
                PackagesSeen |= Mask;
                Record = KiReserveRelationRecord(&Cursor, RelationProcessorPackage, ProcessorRecordSize,
                                                 BufferLength, &Needed, &Status);
                if (Record != NULL)
                {
                    Record->Processor.GroupCount = 1;
                    Record->Processor.GroupMask[0].Mask = Mask;
                }
            }
        }

        if (All || RelationshipType == RelationProcessorCore)
        {
            Mask = Prcb->MultiThreadProcessorSet;
            if (ProcessorNumber != NULL || !(CoresSeen & Mask))
            {
                CoresSeen |= Mask;
                Record = KiReserveRelationRecord(&Cursor, RelationProcessorCore, ProcessorRecordSize,
                                                 BufferLength, &Needed, &Status);
                if (Record != NULL)
                {
                    if (Mask != Prcb->SetMember)
                        Record->Processor.Flags = LTP_PC_SMT;
                    Record->Processor.GroupCount = 1;
                    Record->Processor.GroupMask[0].Mask = Mask;
                }
            }
        }

        if (All || RelationshipType == RelationCache)
        {
            for (CacheIndex = 0; CacheIndex < Prcb->CacheCount && CacheIndex < ARRAYSIZE(Prcb->Cache); CacheIndex++)
            {
                Record = KiReserveRelationRecord(&Cursor, RelationCache, CacheRecordSize,
                                                 BufferLength, &Needed, &Status);
                if (Record == NULL)
                    continue;

                Record->Cache.Level = Prcb->Cache[CacheIndex].Level;
                Record->Cache.Associativity = Prcb->Cache[CacheIndex].Associativity;
                Record->Cache.LineSize = Prcb->Cache[CacheIndex].LineSize;
                Record->Cache.CacheSize = Prcb->Cache[CacheIndex].Size;
                Record->Cache.Type = Prcb->Cache[CacheIndex].Type;
                Record->Cache.GroupCount = 1;
                Record->Cache.GroupMask.Mask = Prcb->SetMember;
            }
        }
    }

    if (All || RelationshipType == RelationNumaNode)
    {
        for (Node = 0; Node < KeNumberNodes; Node++)
        {
            Mask = KeNodeBlock[Node]->ProcessorMask;
            if (Mask == 0)
                continue;
            if (ProcessorNumber != NULL && !(Mask & AFFINITY_MASK(First)))
                continue;

            Record = KiReserveRelationRecord(&Cursor, RelationNumaNode, NodeRecordSize,
                                             BufferLength, &Needed, &Status);
            if (Record != NULL)
            {
                Record->NumaNode.NodeNumber = Node;
                Record->NumaNode.GroupCount = 1;
                Record->NumaNode.GroupMask.Mask = Mask;
            }
        }
    }

    /* The group record describes the whole system, so a single processor query leaves it out */
    if (RelationshipType == RelationGroup || (All && ProcessorNumber == NULL))
    {
        Record = KiReserveRelationRecord(&Cursor, RelationGroup, GroupRecordSize,
                                         BufferLength, &Needed, &Status);
        if (Record != NULL)
        {
            Record->Group.MaximumGroupCount = KeQueryMaximumGroupCount();
            Record->Group.ActiveGroupCount = 1;
            Record->Group.GroupInfo[0].MaximumProcessorCount = (UCHAR)KeQueryMaximumProcessorCountEx(0);
            Record->Group.GroupInfo[0].ActiveProcessorCount = (UCHAR)KeQueryActiveProcessorCountEx(0);
            Record->Group.GroupInfo[0].ActiveProcessorMask = KeActiveProcessors;
        }
    }

    if (NT_SUCCESS(Status) && Needed == 0)
        Status = STATUS_UNSUCCESSFUL;

    *Length = Needed;
    return Status;
}
