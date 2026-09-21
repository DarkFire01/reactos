/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ke functions of Vista+
 * COPYRIGHT:   2016 Pierre Schweitzer (pierre@reactos.org)
 *              2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 Justin Miller (justin.miller@reactos.org)
 */

#include "ntoskrnl_vista.h"

NTKRNLVISTAAPI
ULONG
NTAPI
KeQueryActiveProcessorCount(OUT PKAFFINITY ActiveProcessors OPTIONAL)
{
    RTL_BITMAP Bitmap;
    KAFFINITY ActiveMap = KeQueryActiveProcessors();

    if (ActiveProcessors != NULL)
    {
        *ActiveProcessors = ActiveMap;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)&ActiveMap,  sizeof(ActiveMap) * 8);
    return RtlNumberOfSetBits(&Bitmap);
}

NTKRNLVISTAAPI
USHORT
NTAPI
KeQueryHighestNodeNumber()
{
	return 0;
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

NTKRNLVISTAAPI
USHORT
NTAPI
KeGetCurrentNodeNumber()
{
	return 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKRNLVISTAAPI
BOOLEAN
NTAPI
KeSetCoalescableTimer(
    _Inout_ PKTIMER Timer,
    _In_ LARGE_INTEGER DueTime,
    _In_ ULONG Period,
    _In_ ULONG TolerableDelay,
    _In_opt_ PKDPC Dpc)
{
    return KeSetTimerEx(Timer, DueTime, Period, Dpc);
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
NTKRNLVISTAAPI
ULONG
NTAPI
KeGetProcessorIndexFromNumber(
    _In_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcNumber->Reserved != 0 ||
        ProcNumber->Group != 0 ||
        ProcNumber->Number >= KeQueryMaximumProcessorCount())
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
NTKRNLVISTAAPI
NTSTATUS
NTAPI
KeGetProcessorNumberFromIndex(
    _In_ ULONG ProcIndex,
    _Out_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcIndex >= KeQueryMaximumProcessorCount())
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
 * Zero, which tells KeRevertToUserAffinityThreadEx() to put the thread back on
 * its user affinity.
 */
NTKRNLVISTAAPI
KAFFINITY
NTAPI
KeSetSystemAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    KeSetSystemAffinityThread(Affinity);
    return 0;
}

/**
 * @brief
 * Puts the affinity of the current thread back.
 *
 * @param[in] Affinity
 * The value handed out by KeSetSystemAffinityThreadEx().
 */
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
VOID
NTAPI
KeSetSystemGroupAffinityThread(
    _In_ PGROUP_AFFINITY Affinity,
    _Out_opt_ PGROUP_AFFINITY PreviousAffinity)
{
    KeSetSystemAffinityThread(Affinity->Mask);

    /* An empty mask sends the matching revert back to the user affinity */
    if (PreviousAffinity != NULL)
    {
        RtlZeroMemory(PreviousAffinity, sizeof(*PreviousAffinity));
    }
}

/**
 * @brief
 * Puts the group affinity of the current thread back.
 *
 * @param[in] PreviousAffinity
 * The group affinity handed out by KeSetSystemGroupAffinityThread().
 */
NTKRNLVISTAAPI
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
 * Registers a callback for processors that come and go at run time.
 *
 * @param[in] CallbackFunction
 * The callback to register.
 *
 * @param[in] CallbackContext
 * Optional context handed to the callback.
 *
 * @param[in] Flags
 * Registration flags.
 *
 * @return
 * NULL, as nothing is registered.
 *
 * @unimplemented
 */
NTKRNLVISTAAPI
PVOID
NTAPI
KeRegisterProcessorChangeCallback(
    _In_ PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
    _In_opt_ PVOID CallbackContext,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(CallbackFunction);
    UNREFERENCED_PARAMETER(CallbackContext);
    UNREFERENCED_PARAMETER(Flags);

    return NULL;
}

/**
 * @brief
 * Returns the interrupt time along with the performance counter reading that
 * goes with it.
 *
 * @param[out] PerfCounter
 * Receives the performance counter value.
 *
 * @return
 * The interrupt time, in 100 nanosecond units.
 */
NTKRNLVISTAAPI
ULONGLONG
NTAPI
KeQueryInterruptTimePrecise(
    _Out_ PULONGLONG PerfCounter)
{
    LARGE_INTEGER Counter;

    Counter = KeQueryPerformanceCounter(NULL);
    *PerfCounter = (ULONGLONG)Counter.QuadPart;

    return (ULONGLONG)KeQueryInterruptTime();
}

/**
 * @brief
 * Initializes a timer through the KTIMER2 entry point.
 *
 * @param[out] Timer
 * The timer to initialize.
 *
 * @remarks
 * The KTIMER2 family maps onto the classic dispatcher timer on ReactOS.
 */
NTKRNLVISTAAPI
VOID
NTAPI
KeInitializeTimer2(
    _Out_ PKTIMER Timer)
{
    KeInitializeTimerEx(Timer, NotificationTimer);
}

/**
 * @brief
 * Arms a timer through the KTIMER2 entry point.
 *
 * @param[in,out] Timer
 * The timer to arm.
 *
 * @param[in] DueTime
 * Expiration time, in 100 nanosecond units.
 *
 * @param[in] Period
 * Period of a recurring timer, in milliseconds, or zero for a one shot timer.
 *
 * @param[in] Dpc
 * Optional DPC to queue on expiration.
 *
 * @return
 * TRUE when the timer was already armed, FALSE otherwise.
 */
NTKRNLVISTAAPI
BOOLEAN
NTAPI
KeSetTimer2(
    _Inout_ PKTIMER Timer,
    _In_ LARGE_INTEGER DueTime,
    _In_ LONGLONG Period,
    _In_opt_ PKDPC Dpc)
{
    return KeSetTimerEx(Timer, DueTime, (LONG)Period, Dpc);
}

/**
 * @brief
 * Cancels a timer through the KTIMER2 entry point.
 *
 * @param[in,out] Timer
 * The timer to cancel.
 *
 * @return
 * TRUE when the timer was pending, FALSE otherwise.
 */
NTKRNLVISTAAPI
BOOLEAN
NTAPI
KeCancelTimer2(
    _Inout_ PKTIMER Timer)
{
    return KeCancelTimer(Timer);
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
NTKRNLVISTAAPI
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
