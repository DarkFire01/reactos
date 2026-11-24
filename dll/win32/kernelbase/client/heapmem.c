/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            dll/win32/kernel32/client/heapmem.c
 * PURPOSE:         Heap Memory APIs (wrappers for RtlHeap*)
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

RTL_HANDLE_TABLE BaseHeapHandleTable;
HANDLE BaseHeap;
ULONG_PTR SystemRangeStart;

/* PRIVATE FUNCTIONS **********************************************************/


/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
BOOL
NTAPI
GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer)
{
    SYSTEM_PERFORMANCE_INFORMATION PerformanceInfo;
    VM_COUNTERS VmCounters;
    QUOTA_LIMITS QuotaLimits;
    ULONGLONG PageFile, PhysicalMemory;
    NTSTATUS Status;

    if (lpBuffer->dwLength != sizeof(*lpBuffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Query performance information */
    Status = NtQuerySystemInformation(SystemPerformanceInformation,
                                      &PerformanceInfo,
                                      sizeof(PerformanceInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Calculate memory load */
    lpBuffer->dwMemoryLoad = ((DWORD)(BaseStaticServerData->SysInfo.NumberOfPhysicalPages -
                                      PerformanceInfo.AvailablePages) * 100) /
                                      BaseStaticServerData->SysInfo.NumberOfPhysicalPages;

    /* Save physical memory */
    PhysicalMemory = BaseStaticServerData->SysInfo.NumberOfPhysicalPages *
                     BaseStaticServerData->SysInfo.PageSize;
    lpBuffer->ullTotalPhys = PhysicalMemory;

    /* Now save available physical memory */
    PhysicalMemory = PerformanceInfo.AvailablePages *
                     BaseStaticServerData->SysInfo.PageSize;
    lpBuffer->ullAvailPhys = PhysicalMemory;

    /* Query VM and Quota Limits */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       &QuotaLimits,
                                       sizeof(QUOTA_LIMITS),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessVmCounters,
                                       &VmCounters,
                                       sizeof(VM_COUNTERS),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Save the commit limit */
    lpBuffer->ullTotalPageFile = min(QuotaLimits.PagefileLimit,
                                     PerformanceInfo.CommitLimit);
    lpBuffer->ullTotalPageFile *= BaseStaticServerData->SysInfo.PageSize;

    /* Calculate how many pages are left */
    PageFile = PerformanceInfo.CommitLimit - PerformanceInfo.CommittedPages;

    /* Save the total */
    lpBuffer->ullAvailPageFile = min(PageFile,
                                     QuotaLimits.PagefileLimit -
                                     VmCounters.PagefileUsage);
    lpBuffer->ullAvailPageFile *= BaseStaticServerData->SysInfo.PageSize;

    /* Now calculate the total virtual space */
    lpBuffer->ullTotalVirtual = (BaseStaticServerData->SysInfo.MaximumUserModeAddress -
                                 BaseStaticServerData->SysInfo.MinimumUserModeAddress) + 1;

    /* And finally the available virtual space */
    lpBuffer->ullAvailVirtual = lpBuffer->ullTotalVirtual - VmCounters.VirtualSize;
    lpBuffer->ullAvailExtendedVirtual = 0;

    return TRUE;
}

/* EOF */
