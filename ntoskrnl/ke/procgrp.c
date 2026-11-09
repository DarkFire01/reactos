/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor group and extended affinity interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* Every processor lives in group 0 */

static
ULONG
KiCountAffinity(
    _In_ KAFFINITY Affinity)
{
    ULONG Count = 0;

    while (Affinity)
    {
        Affinity &= Affinity - 1;
        Count++;
    }

    return Count;
}

ULONG
NTAPI
KeQueryActiveProcessorCount(
    _Out_opt_ PKAFFINITY ActiveProcessors)
{
    KAFFINITY Active = KeActiveProcessors;

    if (ActiveProcessors)
        *ActiveProcessors = Active;

    return KiCountAffinity(Active);
}

ULONG
NTAPI
KeQueryActiveProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0 && GroupNumber != ALL_PROCESSOR_GROUPS)
        return 0;

    return KiCountAffinity(KeActiveProcessors);
}

ULONG
NTAPI
KeQueryMaximumProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0 && GroupNumber != ALL_PROCESSOR_GROUPS)
        return 0;

    return KeNumberProcessors;
}

USHORT
NTAPI
KeQueryActiveGroupCount(VOID)
{
    return 1;
}

USHORT
NTAPI
KeQueryMaximumGroupCount(VOID)
{
    return 1;
}

KAFFINITY
NTAPI
KeQueryGroupAffinity(
    _In_ USHORT GroupNumber)
{
    return GroupNumber == 0 ? KeActiveProcessors : 0;
}

ULONG
NTAPI
KeGetCurrentProcessorNumberEx(
    _Out_opt_ PPROCESSOR_NUMBER ProcNumber)
{
    ULONG Number = KeGetCurrentProcessorNumber();

    if (ProcNumber)
    {
        ProcNumber->Group = 0;
        ProcNumber->Number = (UCHAR)Number;
        ProcNumber->Reserved = 0;
    }

    return Number;
}

ULONG
NTAPI
KeGetProcessorIndexFromNumber(
    _In_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcNumber->Group != 0 || ProcNumber->Number >= KeNumberProcessors)
        return INVALID_PROCESSOR_INDEX;

    return ProcNumber->Number;
}

NTSTATUS
NTAPI
KeGetProcessorNumberFromIndex(
    _In_ ULONG ProcIndex,
    _Out_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcIndex >= (ULONG)KeNumberProcessors)
        return STATUS_INVALID_PARAMETER;

    ProcNumber->Group = 0;
    ProcNumber->Number = (UCHAR)ProcIndex;
    ProcNumber->Reserved = 0;
    return STATUS_SUCCESS;
}

/* Returns the previous system affinity, or 0 when the thread was on its user affinity */
KAFFINITY
NTAPI
KeSetSystemAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    PKTHREAD Thread = KeGetCurrentThread();
    KAFFINITY Previous = Thread->SystemAffinityActive ? Thread->Affinity : 0;

    KeSetSystemAffinityThread(Affinity);
    return Previous;
}

VOID
NTAPI
KeRevertToUserAffinityThreadEx(
    _In_ KAFFINITY Affinity)
{
    if (Affinity)
        KeSetSystemAffinityThread(Affinity);
    else if (KeGetCurrentThread()->SystemAffinityActive)
        KeRevertToUserAffinityThread();
}

VOID
NTAPI
KeSetSystemGroupAffinityThread(
    _In_ PGROUP_AFFINITY Affinity,
    _Out_opt_ PGROUP_AFFINITY PreviousAffinity)
{
    KAFFINITY Previous;

    ASSERT(Affinity->Group == 0);

    Previous = KeSetSystemAffinityThreadEx(Affinity->Mask);
    if (PreviousAffinity)
    {
        RtlZeroMemory(PreviousAffinity, sizeof(*PreviousAffinity));
        PreviousAffinity->Mask = Previous;
    }
}

VOID
NTAPI
KeRevertToUserGroupAffinityThread(
    _In_ PGROUP_AFFINITY PreviousAffinity)
{
    KeRevertToUserAffinityThreadEx(PreviousAffinity->Mask);
}

/* Nothing suspends the interrupt clock, so there is no sleep time to take out */
ULONGLONG
NTAPI
KeQueryUnbiasedInterruptTime(VOID)
{
    return KeQueryInterruptTime();
}
