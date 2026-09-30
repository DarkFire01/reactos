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

/* Self referencing so that no separate initialisation is needed before the first caller. */
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
 * eturn An opaque handle for KeDeregisterProcessorChangeCallback, or NULL.
 *
 * emarks No processor is ever added after boot here, so a registration that survives this
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
 * eturn The highest node number, which is one less than the count of nodes.
 */
USHORT
NTAPI
KeQueryHighestNodeNumber(VOID)
{
    return (USHORT)(KeNumberNodes - 1);
}

/**
 * \brief Converts a reading of the auxiliary counter into the performance counter timebase.
 *
 * \param AuxiliaryCounterValue The reading to convert.
 * \param PerformanceCounterValue Receives the same instant on the performance counter.
 * \param ConversionError Receives how far the conversion may be out, in performance
 *        counter ticks. Optional.
 *
 * eturn STATUS_NOT_SUPPORTED when the platform has no auxiliary counter, otherwise
 *         whatever the timer reports.
 */
NTSTATUS
NTAPI
KeConvertAuxiliaryCounterToPerformanceCounter(
    _In_ ULONG64 AuxiliaryCounterValue,
    _Out_ PULONG64 PerformanceCounterValue,
    _Out_opt_ PULONG64 ConversionError)
{
    if (PerformanceCounterValue == NULL)
        return STATUS_INVALID_PARAMETER;

    /* The counter is a platform feature, so a HAL without it has nothing to convert. */
    if (HalTimerConvertAuxiliaryCounterToPerformanceCounter == NULL)
    {
        *PerformanceCounterValue = 0;
        if (ConversionError != NULL)
            *ConversionError = 0;

        return STATUS_NOT_SUPPORTED;
    }

    return HalTimerConvertAuxiliaryCounterToPerformanceCounter(AuxiliaryCounterValue,
                                                               PerformanceCounterValue,
                                                               ConversionError);
}

/* PROCESSOR RELATIONSHIPS ***************************************************/

/*
 * The machine is one processor group, so a relationship between processors
 * always covers processors of group zero and one group affinity describes it.
 */
#define KI_PROCESSOR_RELATION_SIZE                                             \
    (FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor.GroupMask) \
     + sizeof(GROUP_AFFINITY))

#define KI_NUMA_RELATION_SIZE                                                  \
    (FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode.GroupMask)  \
     + sizeof(GROUP_AFFINITY))

#define KI_GROUP_RELATION_SIZE                                                 \
    (FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group.GroupInfo)     \
     + sizeof(PROCESSOR_GROUP_INFO))

#ifdef _WIN64
C_ASSERT(KI_PROCESSOR_RELATION_SIZE == 48);
C_ASSERT(KI_NUMA_RELATION_SIZE == 48);
C_ASSERT(KI_GROUP_RELATION_SIZE == 80);
#endif

typedef struct _KI_RELATION_BUFFER
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;
    ULONG Length;
    ULONG Used;
    BOOLEAN Overflow;
} KI_RELATION_BUFFER, *PKI_RELATION_BUFFER;

/* Starts an entry, or only counts what one would have taken when it does not fit */
static
PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX
KiTakeRelation(
    _Inout_ PKI_RELATION_BUFFER Buffer,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Relationship,
    _In_ ULONG Size)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;

    Buffer->Used += Size;
    if (Buffer->Used > Buffer->Length)
    {
        Buffer->Overflow = TRUE;
        return NULL;
    }

    Entry = Buffer->Entry;
    RtlZeroMemory(Entry, Size);
    Entry->Relationship = Relationship;
    Entry->Size = Size;
    Buffer->Entry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((PUCHAR)Entry + Size);

    return Entry;
}

/* Reports a run of processors, which is what a core and a package both are */
static
VOID
KiAddProcessorRelation(
    _Inout_ PKI_RELATION_BUFFER Buffer,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Relationship,
    _In_ KAFFINITY Processors,
    _In_ UCHAR Flags)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;

    Entry = KiTakeRelation(Buffer, Relationship, KI_PROCESSOR_RELATION_SIZE);
    if (Entry == NULL)
        return;

    Entry->Processor.Flags = Flags;
    Entry->Processor.GroupCount = 1;
    Entry->Processor.GroupMask[0].Mask = Processors;
}

/* Reports one NUMA node and the processors that belong to it */
static
VOID
KiAddNumaRelation(
    _Inout_ PKI_RELATION_BUFFER Buffer,
    _In_ USHORT NodeNumber,
    _In_ KAFFINITY Processors)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;

    Entry = KiTakeRelation(Buffer, RelationNumaNode, KI_NUMA_RELATION_SIZE);
    if (Entry == NULL)
        return;

    Entry->NumaNode.NodeNumber = NodeNumber;
    Entry->NumaNode.GroupCount = 1;
    Entry->NumaNode.GroupMasks[0].Mask = Processors;
}

/* Reports the one group the machine runs in */
static
VOID
KiAddGroupRelation(
    _Inout_ PKI_RELATION_BUFFER Buffer)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;

    Entry = KiTakeRelation(Buffer, RelationGroup, KI_GROUP_RELATION_SIZE);
    if (Entry == NULL)
        return;

    Entry->Group.MaximumGroupCount = 1;
    Entry->Group.ActiveGroupCount = 1;
    Entry->Group.GroupInfo[0].MaximumProcessorCount = KeNumberProcessors;
    Entry->Group.GroupInfo[0].ActiveProcessorCount = KeNumberProcessors;
    Entry->Group.GroupInfo[0].ActiveProcessorMask = KeActiveProcessors;
}

/**
 * @brief
 * Describes how the logical processors of the machine relate to each other.
 *
 * @param[in] ProcessorNumber
 * Narrows the answer to the relationships one processor takes part in, or
 * NULL for all of them.
 *
 * @param[in] RelationshipType
 * The kind of relationship being asked about, or RelationAll for every kind
 * the kernel keeps.
 *
 * @param[out] Information
 * Receives the relationships, one entry each.
 *
 * @param[in,out] Length
 * The room in @p Information on the way in, and how much of it the answer
 * needs on the way out.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INFO_LENGTH_MISMATCH when there was more to say than
 * there was room for, STATUS_INVALID_PARAMETER for a processor that is not
 * running, or STATUS_UNSUCCESSFUL when there was nothing to say at all.
 *
 * @remarks
 * Caches, dies and modules are not tracked, so asking about one of those is
 * answered with nothing.
 */
NTSTATUS
NTAPI
KeQueryLogicalProcessorRelationship(
    _In_opt_ PPROCESSOR_NUMBER ProcessorNumber,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_opt_(*Length) PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Information,
    _Inout_ PULONG Length)
{
    KI_RELATION_BUFFER Buffer;
    KAFFINITY Wanted, Processors;
    PKPRCB Prcb;
    UCHAR Flags;
    LONG Index;
    USHORT Node;
    BOOLEAN All;

    if (Length == NULL)
        return STATUS_INVALID_PARAMETER;

    if (ProcessorNumber != NULL)
    {
        if ((ProcessorNumber->Group != 0) ||
            (ProcessorNumber->Number >= (UCHAR)KeNumberProcessors))
        {
            return STATUS_INVALID_PARAMETER;
        }

        Wanted = AFFINITY_MASK(ProcessorNumber->Number);
    }
    else
    {
        Wanted = KeActiveProcessors;
    }

    All = (RelationshipType == RelationAll);

    Buffer.Entry = Information;
    Buffer.Length = (Information != NULL) ? *Length : 0;
    Buffer.Used = 0;
    Buffer.Overflow = FALSE;

    /* A package is the whole machine, as nothing here ever divides one up */
    if (All || (RelationshipType == RelationProcessorPackage))
        KiAddProcessorRelation(&Buffer, RelationProcessorPackage, KeActiveProcessors, 0);

    if (All || (RelationshipType == RelationProcessorCore))
    {
        for (Index = 0; Index < KeNumberProcessors; Index++)
        {
            Prcb = KiProcessorBlock[Index];

            /* One entry for each core, which the first of its threads stands for */
            if (Prcb->MultiThreadSetMaster != Prcb)
                continue;

            Processors = Prcb->MultiThreadProcessorSet;
            if ((Processors & Wanted) == 0)
                continue;

            /* A core holding more than this one thread is a core that threads */
            Flags = (Prcb->SetMember != Processors) ? LTP_PC_SMT : 0;

            KiAddProcessorRelation(&Buffer, RelationProcessorCore, Processors, Flags);
        }
    }

    /* Both kinds of node question are answered the same way, in groups */
    if (All ||
        (RelationshipType == RelationNumaNode) ||
        (RelationshipType == RelationNumaNodeEx))
    {
        for (Node = 0; Node < KeNumberNodes; Node++)
        {
            Processors = KeNodeBlock[Node]->ProcessorMask;
            if ((Processors & Wanted) == 0)
                continue;

            KiAddNumaRelation(&Buffer, Node, Processors);
        }
    }

    /* The groups belong to the machine rather than to a processor in it */
    if ((RelationshipType == RelationGroup) ||
        (All && (ProcessorNumber == NULL)))
    {
        KiAddGroupRelation(&Buffer);
    }

    *Length = Buffer.Used;

    if (Buffer.Overflow)
        return STATUS_INFO_LENGTH_MISMATCH;

    if (Buffer.Used == 0)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}
