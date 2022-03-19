/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * PURPOSE:         Vista functions
 * PROGRAMMERS:     Thomas Weidenmueller (w3seek@reactos.com)
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

/* INCLUDES *******************************************************************/

#include <k32.h>
#include <psapi.h>

#define NDEBUG
#include <debug.h>

BOOL
WINAPI
SetProcessWorkingSetSizeEx(IN HANDLE hProcess,
                           IN SIZE_T dwMinimumWorkingSetSize,
                           IN SIZE_T dwMaximumWorkingSetSize,
                           IN DWORD Flags);

/* PUBLIC FUNCTIONS ***********************************************************/

//#if (_WIN32_WINNT >= 0x500)

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetConsoleHistoryInfo(PCONSOLE_HISTORY_INFO lpConsoleHistoryInfo)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETSETHISTORYINFO HistoryInfoRequest = &ApiMessage.Data.HistoryInfoRequest;

    if (lpConsoleHistoryInfo->cbSize != sizeof(CONSOLE_HISTORY_INFO))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetHistory),
                        sizeof(*HistoryInfoRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    lpConsoleHistoryInfo->HistoryBufferSize      = HistoryInfoRequest->HistoryBufferSize;
    lpConsoleHistoryInfo->NumberOfHistoryBuffers = HistoryInfoRequest->NumberOfHistoryBuffers;
    lpConsoleHistoryInfo->dwFlags                = HistoryInfoRequest->dwFlags;

    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleHistoryInfo(IN PCONSOLE_HISTORY_INFO lpConsoleHistoryInfo)
{
    CONSOLE_API_MESSAGE ApiMessage;
    PCONSOLE_GETSETHISTORYINFO HistoryInfoRequest = &ApiMessage.Data.HistoryInfoRequest;

    if (lpConsoleHistoryInfo->cbSize != sizeof(CONSOLE_HISTORY_INFO))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    HistoryInfoRequest->HistoryBufferSize      = lpConsoleHistoryInfo->HistoryBufferSize;
    HistoryInfoRequest->NumberOfHistoryBuffers = lpConsoleHistoryInfo->NumberOfHistoryBuffers;
    HistoryInfoRequest->dwFlags                = lpConsoleHistoryInfo->dwFlags;

    CsrClientCallServer((PCSR_API_MESSAGE)&ApiMessage,
                        NULL,
                        CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetHistory),
                        sizeof(*HistoryInfoRequest));
    if (!NT_SUCCESS(ApiMessage.Status))
    {
        BaseSetLastNTError(ApiMessage.Status);
        return FALSE;
    }

    return TRUE;
}


/*
 * @unimplemented
 */
DWORD
WINAPI
DECLSPEC_HOTPATCH
GetConsoleOriginalTitleW(OUT LPWSTR lpConsoleTitle,
                         IN DWORD nSize)
{
    DPRINT1("GetConsoleOriginalTitleW(0x%p, 0x%x) UNIMPLEMENTED!\n", lpConsoleTitle, nSize);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}


/*
 * @unimplemented
 */
DWORD
WINAPI
DECLSPEC_HOTPATCH
GetConsoleOriginalTitleA(OUT LPSTR lpConsoleTitle,
                         IN DWORD nSize)
{
    DPRINT1("GetConsoleOriginalTitleA(0x%p, 0x%x) UNIMPLEMENTED!\n", lpConsoleTitle, nSize);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}


/*
 * @unimplemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetConsoleScreenBufferInfoEx(IN HANDLE hConsoleOutput,
                             OUT PCONSOLE_SCREEN_BUFFER_INFOEX lpConsoleScreenBufferInfoEx)
{
    DPRINT1("GetConsoleScreenBufferInfoEx(0x%p, 0x%p) UNIMPLEMENTED!\n", hConsoleOutput, lpConsoleScreenBufferInfoEx);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}


/*
 * @unimplemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetConsoleScreenBufferInfoEx(IN HANDLE hConsoleOutput,
                             IN PCONSOLE_SCREEN_BUFFER_INFOEX lpConsoleScreenBufferInfoEx)
{
    DPRINT1("SetConsoleScreenBufferInfoEx(0x%p, 0x%p) UNIMPLEMENTED!\n", hConsoleOutput, lpConsoleScreenBufferInfoEx);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}


/*
 * @unimplemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetCurrentConsoleFontEx(IN HANDLE hConsoleOutput,
                        IN BOOL bMaximumWindow,
                        OUT PCONSOLE_FONT_INFOEX lpConsoleCurrentFontEx)
{
    DPRINT1("GetCurrentConsoleFontEx(0x%p, 0x%x, 0x%p) UNIMPLEMENTED!\n", hConsoleOutput, bMaximumWindow, lpConsoleCurrentFontEx);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL
WINAPI
K32EmptyWorkingSet(HANDLE hProcess)
{
    return SetProcessWorkingSetSizeEx(hProcess, (SIZE_T)-1, (SIZE_T)-1, 0);
}

/***********************************************************************
 *         K32GetPerformanceInfo   (kernelbase.@)
 */
BOOL WINAPI K32GetPerformanceInfo( PPERFORMANCE_INFORMATION info, DWORD size )
{
    SYSTEM_PERFORMANCE_INFORMATION perf;
    SYSTEM_BASIC_INFORMATION basic;
    SYSTEM_PROCESS_INFORMATION *process, *spi;
    DWORD info_size;
    NTSTATUS status;

    DPRINT1( "(%p, %d)\n", info, size );

    if (size < sizeof(*info))
    {
        SetLastError( ERROR_BAD_LENGTH );
        return FALSE;
    }

    status = NtQuerySystemInformation( SystemPerformanceInformation, &perf, sizeof(perf), NULL );
    if (!NT_SUCCESS( status )) return FALSE;
    status = NtQuerySystemInformation( SystemBasicInformation, &basic, sizeof(basic), NULL );
    if (!NT_SUCCESS( status )) return FALSE;

    info->cb                 = sizeof(*info);
    info->CommitTotal        = perf.CommittedPages;
    info->CommitLimit        = perf.CommitLimit;
    info->CommitPeak         = perf.PeakCommitment;
    info->PhysicalTotal      = basic.NumberOfPhysicalPages;
    info->PhysicalAvailable  = perf.AvailablePages;
    info->SystemCache        = 0;
    info->KernelTotal        = perf.PagedPoolPages + perf.NonPagedPoolPages;
    info->KernelPaged        = perf.PagedPoolPages;
    info->KernelNonpaged     = perf.NonPagedPoolPages;
    info->PageSize           = basic.PageSize;

    /* fields from SYSTEM_PROCESS_INFORMATION */
    NtQuerySystemInformation( SystemProcessInformation, NULL, 0, &info_size );
    for (;;)
    {
        process = HeapAlloc( GetProcessHeap(), 0, info_size );
        if (!process)
        {
            SetLastError( ERROR_OUTOFMEMORY );
            return FALSE;
        }
        status = NtQuerySystemInformation( SystemProcessInformation, process, info_size, &info_size );
        if (!status) break;
        HeapFree( GetProcessHeap(), 0, process );
        if (status != STATUS_INFO_LENGTH_MISMATCH)
        {
            SetLastError( RtlNtStatusToDosError( status ) );
            return FALSE;
        }
    }
    info->HandleCount = info->ProcessCount = info->ThreadCount = 0;
    spi = process;
    for (;;)
    {
        info->ProcessCount++;
        info->HandleCount += spi->HandleCount;
        info->ThreadCount += spi->NumberOfThreads;
        if (spi->NextEntryOffset == 0) break;
        spi = (SYSTEM_PROCESS_INFORMATION *)((char *)spi + spi->NextEntryOffset);
    }
    HeapFree( GetProcessHeap(), 0, process );
    return TRUE;
}

HRESULT
WINAPI
WerRegisterMemoryBlock(PVOID pvAddress,
                       DWORD dwSize)
{
    UNIMPLEMENTED;
    return S_OK;
}

//#endif // (_WIN32_WINNT >= _WIN32_WINNT_VISTA)

/* EOF */
