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
 * ReactOS implements the single processor group model: every logical processor
 * lives in group 0. The routines below present the multi-group ("Ex") APIs on
 * top of the legacy single-group primitives.
 */

/**
 * @brief
 * Translates a processor number (group/number pair) into a system-wide
 * processor index.
 *
 * @param[in] ProcNumber
 * The processor number to translate.
 *
 * @return
 * The zero-based system-wide processor index, or INVALID_PROCESSOR_INDEX
 * (0xFFFFFFFF) if @p ProcNumber does not identify an active processor.
 */
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
 * Translates a system-wide processor index into a processor number
 * (group/number pair).
 *
 * @param[in] ProcIndex
 * The zero-based system-wide processor index.
 *
 * @param[out] ProcNumber
 * Receives the corresponding processor number.
 *
 * @return
 * STATUS_SUCCESS on success, or STATUS_INVALID_PARAMETER if @p ProcIndex is out
 * of range.
 */
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
 * Returns the number of active processors in the given group.
 *
 * @param[in] GroupNumber
 * The processor group, or ALL_PROCESSOR_GROUPS to count all groups.
 *
 * @return
 * The number of active processors in the group, or 0 for a non-existent group.
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
 * Returns the maximum number of processors that may become active in the given
 * group.
 *
 * @param[in] GroupNumber
 * The processor group, or ALL_PROCESSOR_GROUPS to count all groups.
 *
 * @return
 * The maximum processor count in the group, or 0 for a non-existent group.
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
 * Returns the maximum number of processor groups the system supports.
 *
 * @return
 * The maximum group count. ReactOS always reports a single group.
 */
USHORT
NTAPI
KeQueryMaximumGroupCount(VOID)
{
    return 1;
}

/**
 * @brief
 * Returns the affinity mask of active processors in the given group.
 *
 * @param[in] GroupNumber
 * The processor group to query.
 *
 * @return
 * The affinity mask of active processors, or 0 for a non-existent group.
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
 * Returns the affinity mask of processors assigned to the given group.
 *
 * @param[in] GroupNumber
 * The processor group to query.
 *
 * @return
 * The affinity mask of the group's processors, or 0 for a non-existent group.
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
 * Returns the active processor affinity for a NUMA node.
 *
 * @param[in] NodeNumber
 * The NUMA node to query.
 *
 * @param[out] Affinity
 * Optionally receives the group affinity of the node's active processors.
 *
 * @param[out] Count
 * Optionally receives the number of active processors on the node.
 *
 * @remarks
 * ReactOS models a single NUMA node (node 0) containing every processor.
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
 * Sets the system affinity of the current thread and returns the previous
 * affinity so it can later be restored.
 *
 * @param[in] Affinity
 * The affinity mask to apply.
 *
 * @return
 * The previous affinity, to be passed to KeRevertToUserAffinityThreadEx().
 *
 * @remarks
 * ReactOS reports the previous affinity as 0, which directs the matching
 * revert call to restore the thread's user affinity.
 */
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
 * Restores the affinity of the current thread saved by
 * KeSetSystemAffinityThreadEx().
 *
 * @param[in] Affinity
 * The affinity value previously returned by KeSetSystemAffinityThreadEx().
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
 * Sets the group affinity of the current thread and returns the previous
 * group affinity.
 *
 * @param[in] Affinity
 * The group affinity to apply.
 *
 * @param[out] PreviousAffinity
 * Optionally receives the previous group affinity.
 */
VOID
NTAPI
KeSetSystemGroupAffinityThread(
    _In_ PGROUP_AFFINITY Affinity,
    _Out_opt_ PGROUP_AFFINITY PreviousAffinity)
{
    KeSetSystemAffinityThread(Affinity->Mask);

    if (PreviousAffinity != NULL)
    {
        RtlZeroMemory(PreviousAffinity, sizeof(*PreviousAffinity));
    }
}

/**
 * @brief
 * Restores the group affinity of the current thread saved by
 * KeSetSystemGroupAffinityThread().
 *
 * @param[in] PreviousAffinity
 * The group affinity previously saved.
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
 * Registers a callback that is invoked when processors are dynamically added.
 *
 * @param[in] CallbackFunction
 * The callback to register.
 *
 * @param[in] CallbackContext
 * Optional context passed to the callback.
 *
 * @param[in] Flags
 * Registration flags.
 *
 * @return
 * A handle to pass to KeDeregisterProcessorChangeCallback(), or NULL on
 * failure.
 *
 * @unimplemented
 * ReactOS does not support dynamic processor hot-add, so no callback is
 * registered and NULL is returned.
 */
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
 * Returns the current interrupt time together with a matching performance
 * counter reading.
 *
 * @param[out] PerfCounter
 * Receives the current performance counter value.
 *
 * @return
 * The current interrupt time, in 100-nanosecond units.
 */
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
 * Initializes a timer object using the KTIMER2 entry point.
 *
 * @param[out] Timer
 * The timer object to initialize.
 *
 * @remarks
 * ReactOS maps the KTIMER2 API family onto the classic dispatcher timer.
 */
VOID
NTAPI
KeInitializeTimer2(
    _Out_ PKTIMER Timer)
{
    KeInitializeTimerEx(Timer, NotificationTimer);
}

/**
 * @brief
 * Sets a timer object using the KTIMER2 entry point.
 *
 * @param[in,out] Timer
 * The timer object to set.
 *
 * @param[in] DueTime
 * The expiration time, in 100-nanosecond units.
 *
 * @param[in] Period
 * The period for a periodic timer, in milliseconds, or 0 for a one-shot timer.
 *
 * @param[in] Dpc
 * Optional DPC to queue on expiration.
 *
 * @return
 * TRUE if the timer was already set, FALSE otherwise.
 *
 * @remarks
 * ReactOS maps the KTIMER2 API family onto the classic dispatcher timer.
 */
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
 * Cancels a timer object using the KTIMER2 entry point.
 *
 * @param[in,out] Timer
 * The timer object to cancel.
 *
 * @return
 * TRUE if the timer was pending, FALSE otherwise.
 *
 * @remarks
 * ReactOS maps the KTIMER2 API family onto the classic dispatcher timer.
 */
BOOLEAN
NTAPI
KeCancelTimer2(
    _Inout_ PKTIMER Timer)
{
    return KeCancelTimer(Timer);
}

/**
 * @brief
 * Brings a dynamically added processor online.
 *
 * @param[in] ProcessorState
 * The initial state of the processor being started.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not support dynamic processor hot-add.
 */
NTSTATUS
NTAPI
KeStartDynamicProcessor(
    _In_ PVOID ProcessorState)
{
    UNREFERENCED_PARAMETER(ProcessorState);

    return STATUS_NOT_IMPLEMENTED;
}
