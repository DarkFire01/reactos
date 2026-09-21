/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ke functions of Vista+
 * COPYRIGHT:   2016 Pierre Schweitzer (pierre@reactos.org)
 *              2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include <ntdef.h>
#include <ntifs.h>

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

/* Returns 0, the value for a thread that was running with its user affinity */
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_max_(APC_LEVEL)
NTKRNLVISTAAPI
KAFFINITY
NTAPI
KeSetSystemAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    KeSetSystemAffinityThread(Affinity);
    return 0;
}

_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_max_(APC_LEVEL)
NTKRNLVISTAAPI
VOID
NTAPI
KeRevertToUserAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    if (Affinity == 0)
        KeRevertToUserAffinityThread();
    else
        KeSetSystemAffinityThread(Affinity);
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
