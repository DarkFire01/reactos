/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     CPU set enumeration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * CPU sets are the Windows 10 way of describing the processors a thread may
 * be scheduled on, replacing the group affinity masks for callers that care
 * about core topology - which core belongs to which cache, which is parked,
 * how efficient it is. xul.dll imports this at load time, so Firefox does not
 * start without it.
 *
 * There is no scheduler here that honours CPU sets, and no topology to report
 * beyond the processor count, so every logical processor is described as its
 * own set: same core, same cache, same NUMA node, same efficiency class. That
 * is a true description of what a caller can do with the answer here - it can
 * see how many processors there are and nothing more useful.
 *
 * The Id numbering matters more than it looks. Windows hands out opaque ids
 * starting at 256, and a caller passes them back to SetThreadSelectedCpuSets
 * rather than treating them as indices. Starting anywhere else would still be
 * legal, but 256 is what callers see on Windows and some of them assume a
 * non-zero id means "real".
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* Where Windows starts numbering CPU set ids */
#define CPU_SET_ID_BASE 256

/*
 * @implemented
 */
BOOL
WINAPI
GetSystemCpuSetInformation(
    _Out_writes_bytes_opt_(BufferLength) PSYSTEM_CPU_SET_INFORMATION Information,
    _In_ ULONG BufferLength,
    _Out_ PULONG ReturnedLength,
    _In_opt_ HANDLE Process,
    _Reserved_ ULONG Flags)
{
    SYSTEM_INFO SystemInfo;
    ULONG Needed, Index, Count;

    UNREFERENCED_PARAMETER(Process);

    if (ReturnedLength == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Reserved, and Windows refuses a caller that passes anything here */
    if (Flags != 0)
    {
        *ReturnedLength = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    GetSystemInfo(&SystemInfo);
    Count = SystemInfo.dwNumberOfProcessors;
    if (Count == 0)
        Count = 1;

    Needed = Count * sizeof(SYSTEM_CPU_SET_INFORMATION);

    /*
     * Say how much is needed either way. A caller asks once with no buffer to
     * size the allocation and again to fill it, so the length has to be right
     * on the failing call as well - that call is not an error to the caller,
     * it is half the protocol.
     */
    *ReturnedLength = Needed;

    if (Information == NULL || BufferLength < Needed)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    RtlZeroMemory(Information, Needed);

    for (Index = 0; Index < Count; Index++)
    {
        PSYSTEM_CPU_SET_INFORMATION Entry = &Information[Index];

        Entry->Size = sizeof(SYSTEM_CPU_SET_INFORMATION);
        Entry->Type = CpuSetInformation;

        Entry->CpuSet.Id = CPU_SET_ID_BASE + Index;
        Entry->CpuSet.Group = 0;
        Entry->CpuSet.LogicalProcessorIndex = (UCHAR)Index;
        Entry->CpuSet.CoreIndex = (UCHAR)Index;
        Entry->CpuSet.LastLevelCacheIndex = 0;
        Entry->CpuSet.NumaNodeIndex = 0;
        Entry->CpuSet.EfficiencyClass = 0;
        Entry->CpuSet.AllFlags = 0;
        Entry->CpuSet.SchedulingClass = 0;
        Entry->CpuSet.AllocationTag = 0;
    }

    return TRUE;
}

/* EOF */
