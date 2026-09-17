/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ke functions of Vista+
 * COPYRIGHT:   2016 Pierre Schweitzer (pierre@reactos.org)
 *              2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 Justin Miller (justin.miller@reactos.org)
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

/**
 * @brief
 * Returns how many processors the system can ever run.
 *
 * @return
 * The count of processors that are online. ReactOS never brings a processor
 * up after boot, so this is also the most it will ever run.
 */
NTKRNLVISTAAPI
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
