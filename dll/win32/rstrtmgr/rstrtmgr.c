/*
 * PROJECT:     ReactOS Restart Manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Restart Manager sessions and affected application reporting
 * COPYRIGHT:   Copyright 2026 Justin Miller (justin.miller@reactos.org)
 */

#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winsvc.h>
#include <ntsecapi.h>
#include <restartmanager.h>
#define NTOS_MODE_USER
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>

/* Windows allows this many open sessions before failing with ERROR_MAX_SESSIONS_REACHED */
#define RMP_MAX_SESSIONS 64

typedef enum _RMP_SESSION_STATE
{
    RmpSessionFree = 0,
    RmpSessionOpen,
    RmpSessionShutDown,
    RmpSessionRestarted
} RMP_SESSION_STATE;

typedef struct _RMP_SESSION
{
    RMP_SESSION_STATE State;
    WCHAR Key[CCH_RM_SESSION_KEY + 1];
    UINT FileCount;
    PWSTR *Files;
    UINT ProcessCount;
    PRM_UNIQUE_PROCESS Processes;
    UINT ServiceCount;
    PWSTR *Services;
} RMP_SESSION, *PRMP_SESSION;

typedef struct _RMP_WINDOW_SEARCH
{
    DWORD ProcessId;
    BOOL Found;
} RMP_WINDOW_SEARCH, *PRMP_WINDOW_SEARCH;

static RMP_SESSION RmpSessions[RMP_MAX_SESSIONS];
static CRITICAL_SECTION RmpLock;

/* Caller holds RmpLock */
static
PRMP_SESSION
RmpLookupSession(
    _In_ DWORD SessionHandle)
{
    if (SessionHandle >= RMP_MAX_SESSIONS)
        return NULL;

    if (RmpSessions[SessionHandle].State == RmpSessionFree)
        return NULL;

    return &RmpSessions[SessionHandle];
}

static
VOID
RmpFreeStrings(
    _In_reads_opt_(Count) PWSTR *Strings,
    _In_ UINT Count)
{
    UINT i;

    if (!Strings)
        return;

    for (i = 0; i < Count; i++)
        HeapFree(GetProcessHeap(), 0, Strings[i]);

    HeapFree(GetProcessHeap(), 0, Strings);
}

static
VOID
RmpReleaseSession(
    _Inout_ PRMP_SESSION Session)
{
    RmpFreeStrings(Session->Files, Session->FileCount);
    RmpFreeStrings(Session->Services, Session->ServiceCount);

    if (Session->Processes)
        HeapFree(GetProcessHeap(), 0, Session->Processes);

    ZeroMemory(Session, sizeof(*Session));
}

static
PVOID
RmpGrowArray(
    _In_opt_ PVOID Array,
    _In_ SIZE_T ElementSize,
    _In_ UINT NewCount)
{
    SIZE_T Size = ElementSize * NewCount;

    if (!Array)
        return HeapAlloc(GetProcessHeap(), 0, Size);

    return HeapReAlloc(GetProcessHeap(), 0, Array, Size);
}

/* Appends the strings not already present, comparing case insensitively */
static
DWORD
RmpAddStrings(
    _Inout_ PWSTR **List,
    _Inout_ UINT *ListCount,
    _In_ UINT Count,
    _In_reads_(Count) LPCWSTR Strings[])
{
    PWSTR *NewList;
    PWSTR Copy;
    SIZE_T Length;
    UINT i, j;

    for (i = 0; i < Count; i++)
    {
        for (j = 0; j < *ListCount; j++)
        {
            if (_wcsicmp((*List)[j], Strings[i]) == 0)
                break;
        }

        if (j < *ListCount)
            continue;

        Length = wcslen(Strings[i]) + 1;
        Copy = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(WCHAR));
        if (!Copy)
            return ERROR_OUTOFMEMORY;

        NewList = RmpGrowArray(*List, sizeof(**List), *ListCount + 1);
        if (!NewList)
        {
            HeapFree(GetProcessHeap(), 0, Copy);
            return ERROR_OUTOFMEMORY;
        }

        CopyMemory(Copy, Strings[i], Length * sizeof(WCHAR));
        NewList[*ListCount] = Copy;
        *List = NewList;
        (*ListCount)++;
    }

    return ERROR_SUCCESS;
}

static
DWORD
RmpAddProcesses(
    _Inout_ PRMP_SESSION Session,
    _In_ UINT Count,
    _In_reads_(Count) RM_UNIQUE_PROCESS Processes[])
{
    PRM_UNIQUE_PROCESS NewList;
    UINT i, j;

    for (i = 0; i < Count; i++)
    {
        for (j = 0; j < Session->ProcessCount; j++)
        {
            if (Session->Processes[j].dwProcessId == Processes[i].dwProcessId &&
                CompareFileTime(&Session->Processes[j].ProcessStartTime,
                                &Processes[i].ProcessStartTime) == 0)
            {
                break;
            }
        }

        if (j < Session->ProcessCount)
            continue;

        NewList = RmpGrowArray(Session->Processes,
                               sizeof(*Session->Processes),
                               Session->ProcessCount + 1);
        if (!NewList)
            return ERROR_OUTOFMEMORY;

        NewList[Session->ProcessCount] = Processes[i];
        Session->Processes = NewList;
        Session->ProcessCount++;
    }

    return ERROR_SUCCESS;
}

static
BOOL
CALLBACK
RmpFindWindowProc(
    _In_ HWND Window,
    _In_ LPARAM Param)
{
    PRMP_WINDOW_SEARCH Search = (PRMP_WINDOW_SEARCH)Param;
    DWORD ProcessId = 0;

    GetWindowThreadProcessId(Window, &ProcessId);
    if (ProcessId != Search->ProcessId)
        return TRUE;

    if (!IsWindowVisible(Window) || GetWindow(Window, GW_OWNER))
        return TRUE;

    Search->Found = TRUE;
    return FALSE;
}

static
HANDLE
RmpOpenProcess(
    _In_ DWORD ProcessId)
{
    HANDLE ProcessHandle;

    ProcessHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, ProcessId);
    if (!ProcessHandle && GetLastError() == ERROR_ACCESS_DENIED)
        ProcessHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);

    return ProcessHandle;
}

static
VOID
RmpQueryImageName(
    _In_ HANDLE ProcessHandle,
    _Out_writes_(NameLength) PWSTR Name,
    _In_ SIZE_T NameLength)
{
    UCHAR Buffer[sizeof(UNICODE_STRING) + MAX_PATH * sizeof(WCHAR)];
    PUNICODE_STRING ImagePath = (PUNICODE_STRING)Buffer;
    USHORT Start, End, Length;
    NTSTATUS Status;

    Name[0] = UNICODE_NULL;

    Status = NtQueryInformationProcess(ProcessHandle,
                                       ProcessImageFileName,
                                       Buffer,
                                       sizeof(Buffer),
                                       NULL);
    if (!NT_SUCCESS(Status) || !ImagePath->Buffer)
        return;

    End = ImagePath->Length / sizeof(WCHAR);
    Start = End;
    while (Start > 0 && ImagePath->Buffer[Start - 1] != L'\\')
        Start--;

    Length = (USHORT)min(End - Start, NameLength - 1);
    CopyMemory(Name, &ImagePath->Buffer[Start], Length * sizeof(WCHAR));
    Name[Length] = UNICODE_NULL;
}

static
VOID
RmpDescribeProcess(
    _In_ const RM_UNIQUE_PROCESS *Process,
    _Out_ PRM_PROCESS_INFO Info,
    _Inout_ PDWORD Reasons)
{
    RMP_WINDOW_SEARCH Search;
    FILETIME Created, Exited, KernelTime, UserTime;
    HANDLE ProcessHandle;
    DWORD ExitCode;
    ULONG BreakOnTermination = 0;
    NTSTATUS Status;

    ZeroMemory(Info, sizeof(*Info));
    Info->Process = *Process;
    Info->ApplicationType = RmUnknownApp;
    Info->AppStatus = RmStatusStoppedOther;
    Info->TSSessionId = (DWORD)RM_INVALID_TS_SESSION;

    ProcessHandle = RmpOpenProcess(Process->dwProcessId);
    if (!ProcessHandle)
    {
        /* The process exists, but this caller cannot look at it */
        if (GetLastError() == ERROR_ACCESS_DENIED)
        {
            Info->AppStatus = RmStatusRunning;
            *Reasons |= RmRebootReasonPermissionDenied;
        }
        return;
    }

    if (!GetExitCodeProcess(ProcessHandle, &ExitCode) || ExitCode != STILL_ACTIVE)
    {
        CloseHandle(ProcessHandle);
        return;
    }

    /* Windows reports the live process with its real creation time */
    if (GetProcessTimes(ProcessHandle, &Created, &Exited, &KernelTime, &UserTime))
        Info->Process.ProcessStartTime = Created;

    Info->AppStatus = RmStatusRunning;
    ProcessIdToSessionId(Process->dwProcessId, &Info->TSSessionId);
    RmpQueryImageName(ProcessHandle, Info->strAppName, ARRAYSIZE(Info->strAppName));

    Status = NtQueryInformationProcess(ProcessHandle,
                                       ProcessBreakOnTermination,
                                       &BreakOnTermination,
                                       sizeof(BreakOnTermination),
                                       NULL);
    if (!NT_SUCCESS(Status))
        BreakOnTermination = 0;

    CloseHandle(ProcessHandle);

    if (Process->dwProcessId == GetCurrentProcessId())
    {
        Info->ApplicationType = RmCritical;
        *Reasons |= RmRebootReasonDetectedSelf;
        return;
    }

    if (BreakOnTermination)
    {
        Info->ApplicationType = RmCritical;
        *Reasons |= RmRebootReasonCriticalProcess;
        return;
    }

    if (_wcsicmp(Info->strAppName, L"explorer.exe") == 0)
    {
        Info->ApplicationType = RmExplorer;
        return;
    }

    Search.ProcessId = Process->dwProcessId;
    Search.Found = FALSE;
    EnumWindows(RmpFindWindowProc, (LPARAM)&Search);
    if (Search.Found)
        Info->ApplicationType = RmMainWindow;
}

static
VOID
RmpDescribeService(
    _In_opt_ SC_HANDLE Manager,
    _In_ PCWSTR ServiceName,
    _Out_ PRM_PROCESS_INFO Info,
    _Inout_ PDWORD Reasons)
{
    SERVICE_STATUS_PROCESS Status;
    FILETIME Created, Exited, KernelTime, UserTime;
    SC_HANDLE Service, ControlHandle;
    HANDLE ProcessHandle;
    DWORD Needed;

    ZeroMemory(Info, sizeof(*Info));
    wcsncpy(Info->strServiceShortName, ServiceName, CCH_RM_MAX_SVC_NAME);
    Info->ApplicationType = RmService;
    Info->AppStatus = RmStatusStoppedOther;
    Info->bRestartable = TRUE;

    if (!Manager)
        return;

    Service = OpenServiceW(Manager, ServiceName, SERVICE_QUERY_STATUS);
    if (!Service)
        return;

    if (!QueryServiceStatusEx(Service,
                              SC_STATUS_PROCESS_INFO,
                              (LPBYTE)&Status,
                              sizeof(Status),
                              &Needed) ||
        Status.dwCurrentState == SERVICE_STOPPED ||
        Status.dwProcessId == 0)
    {
        CloseServiceHandle(Service);
        return;
    }

    CloseServiceHandle(Service);

    Info->Process.dwProcessId = Status.dwProcessId;
    Info->AppStatus = RmStatusRunning;
    ProcessIdToSessionId(Status.dwProcessId, &Info->TSSessionId);

    ProcessHandle = RmpOpenProcess(Status.dwProcessId);
    if (ProcessHandle)
    {
        if (GetProcessTimes(ProcessHandle, &Created, &Exited, &KernelTime, &UserTime))
            Info->Process.ProcessStartTime = Created;
        CloseHandle(ProcessHandle);
    }

    Needed = ARRAYSIZE(Info->strAppName);
    if (!GetServiceDisplayNameW(Manager, ServiceName, Info->strAppName, &Needed))
        Info->strAppName[0] = UNICODE_NULL;

    /* A service this caller cannot stop and start again forces a reboot */
    ControlHandle = OpenServiceW(Manager, ServiceName, SERVICE_STOP | SERVICE_START);
    if (ControlHandle)
    {
        CloseServiceHandle(ControlHandle);
    }
    else if (GetLastError() == ERROR_ACCESS_DENIED)
    {
        Info->bRestartable = FALSE;
        *Reasons |= RmRebootReasonPermissionDenied;
    }
}

/* Windows reports shutdown in 12 even steps and restart in 4 */
#define RMP_SHUTDOWN_STEPS  12
#define RMP_RESTART_STEPS   4

static
VOID
RmpReportProgress(
    _In_opt_ RM_WRITE_STATUS_CALLBACK StatusCallback,
    _In_ UINT Steps)
{
    UINT i;

    if (!StatusCallback)
        return;

    for (i = 1; i <= Steps; i++)
        StatusCallback(i * 100 / Steps);
}

DWORD
WINAPI
RmStartSession(
    _Out_ DWORD *pSessionHandle,
    _Reserved_ DWORD dwSessionFlags,
    _Out_writes_(CCH_RM_SESSION_KEY + 1) WCHAR strSessionKey[])
{
    static const WCHAR HexDigits[] = L"0123456789abcdef";
    PRMP_SESSION Session;
    UCHAR KeyBytes[RM_SESSION_KEY_LEN];
    DWORD Handle;
    UINT i;

    UNREFERENCED_PARAMETER(dwSessionFlags);

    if (!pSessionHandle || !strSessionKey)
        return ERROR_BAD_ARGUMENTS;

    if (!RtlGenRandom(KeyBytes, sizeof(KeyBytes)))
        return GetLastError();

    EnterCriticalSection(&RmpLock);

    for (Handle = 0; Handle < RMP_MAX_SESSIONS; Handle++)
    {
        if (RmpSessions[Handle].State == RmpSessionFree)
            break;
    }

    if (Handle == RMP_MAX_SESSIONS)
    {
        LeaveCriticalSection(&RmpLock);
        return ERROR_MAX_SESSIONS_REACHED;
    }

    Session = &RmpSessions[Handle];
    ZeroMemory(Session, sizeof(*Session));
    Session->State = RmpSessionOpen;
    for (i = 0; i < sizeof(KeyBytes); i++)
    {
        Session->Key[i * 2] = HexDigits[KeyBytes[i] >> 4];
        Session->Key[i * 2 + 1] = HexDigits[KeyBytes[i] & 0xF];
    }
    Session->Key[CCH_RM_SESSION_KEY] = UNICODE_NULL;

    CopyMemory(strSessionKey, Session->Key, sizeof(Session->Key));
    *pSessionHandle = Handle;

    LeaveCriticalSection(&RmpLock);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
RmEndSession(
    _In_ DWORD dwSessionHandle)
{
    PRMP_SESSION Session;

    EnterCriticalSection(&RmpLock);

    Session = RmpLookupSession(dwSessionHandle);
    if (!Session)
    {
        LeaveCriticalSection(&RmpLock);
        return ERROR_INVALID_HANDLE;
    }

    RmpReleaseSession(Session);

    LeaveCriticalSection(&RmpLock);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
RmRegisterResources(
    _In_ DWORD dwSessionHandle,
    _In_ UINT nFiles,
    _In_reads_opt_(nFiles) LPCWSTR rgsFileNames[],
    _In_ UINT nApplications,
    _In_reads_opt_(nApplications) RM_UNIQUE_PROCESS rgApplications[],
    _In_ UINT nServices,
    _In_reads_opt_(nServices) LPCWSTR rgsServiceNames[])
{
    PRMP_SESSION Session;
    DWORD Error;
    UINT i;

    EnterCriticalSection(&RmpLock);

    Session = RmpLookupSession(dwSessionHandle);
    if (!Session)
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    if ((nFiles && !rgsFileNames) ||
        (nApplications && !rgApplications) ||
        (nServices && !rgsServiceNames))
    {
        Error = ERROR_BAD_ARGUMENTS;
        goto Quit;
    }

    for (i = 0; i < nFiles; i++)
    {
        if (!rgsFileNames[i])
        {
            Error = ERROR_BAD_ARGUMENTS;
            goto Quit;
        }
    }

    for (i = 0; i < nServices; i++)
    {
        if (!rgsServiceNames[i])
        {
            Error = ERROR_BAD_ARGUMENTS;
            goto Quit;
        }
    }

    /* Resources can only be added before the first shutdown */
    if (Session->State != RmpSessionOpen)
    {
        Error = ERROR_REQUEST_OUT_OF_SEQUENCE;
        goto Quit;
    }

    Error = RmpAddStrings(&Session->Files, &Session->FileCount, nFiles, rgsFileNames);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    Error = RmpAddProcesses(Session, nApplications, rgApplications);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    Error = RmpAddStrings(&Session->Services, &Session->ServiceCount, nServices, rgsServiceNames);

Quit:
    LeaveCriticalSection(&RmpLock);
    return Error;
}

DWORD
WINAPI
RmGetList(
    _In_ DWORD dwSessionHandle,
    _Out_ UINT *pnProcInfoNeeded,
    _Inout_ UINT *pnProcInfo,
    _Inout_updates_opt_(*pnProcInfo) RM_PROCESS_INFO rgAffectedApps[],
    _Out_ LPDWORD lpdwRebootReasons)
{
    PRM_PROCESS_INFO Entries = NULL;
    PRMP_SESSION Session;
    SC_HANDLE Manager = NULL;
    DWORD Reasons = RmRebootReasonNone;
    DWORD Error;
    UINT Count, i;

    EnterCriticalSection(&RmpLock);

    Session = RmpLookupSession(dwSessionHandle);
    if (!Session)
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    if (!pnProcInfoNeeded || !pnProcInfo || !lpdwRebootReasons ||
        (*pnProcInfo && !rgAffectedApps))
    {
        Error = ERROR_BAD_ARGUMENTS;
        goto Quit;
    }

    /* Open handles on registered files are not tracked, so only processes and services are reported */
    Count = Session->ProcessCount + Session->ServiceCount;
    if (Count)
    {
        Entries = HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*Entries));
        if (!Entries)
        {
            Error = ERROR_OUTOFMEMORY;
            goto Quit;
        }
    }

    for (i = 0; i < Session->ProcessCount; i++)
        RmpDescribeProcess(&Session->Processes[i], &Entries[i], &Reasons);

    if (Session->ServiceCount)
        Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);

    for (i = 0; i < Session->ServiceCount; i++)
    {
        RmpDescribeService(Manager,
                           Session->Services[i],
                           &Entries[Session->ProcessCount + i],
                           &Reasons);
    }

    if (Manager)
        CloseServiceHandle(Manager);

    *pnProcInfoNeeded = Count;
    *lpdwRebootReasons = Reasons;

    if (Count > *pnProcInfo)
    {
        Error = ERROR_MORE_DATA;
        goto Quit;
    }

    if (Count)
        CopyMemory(rgAffectedApps, Entries, Count * sizeof(*Entries));

    *pnProcInfo = Count;
    Error = ERROR_SUCCESS;

Quit:
    LeaveCriticalSection(&RmpLock);

    if (Entries)
        HeapFree(GetProcessHeap(), 0, Entries);

    return Error;
}

DWORD
WINAPI
RmShutdown(
    _In_ DWORD dwSessionHandle,
    _In_ ULONG lActionFlags,
    _In_opt_ RM_WRITE_STATUS_CALLBACK fnStatus)
{
    PRMP_SESSION Session;

    UNREFERENCED_PARAMETER(lActionFlags);

    EnterCriticalSection(&RmpLock);

    Session = RmpLookupSession(dwSessionHandle);
    if (!Session)
    {
        LeaveCriticalSection(&RmpLock);
        return ERROR_INVALID_HANDLE;
    }

    /* Shutdown may repeat, but not once a restart has happened */
    if (Session->State == RmpSessionRestarted)
    {
        LeaveCriticalSection(&RmpLock);
        return ERROR_REQUEST_OUT_OF_SEQUENCE;
    }

    Session->State = RmpSessionShutDown;
    LeaveCriticalSection(&RmpLock);

    RmpReportProgress(fnStatus, RMP_SHUTDOWN_STEPS);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
RmRestart(
    _In_ DWORD dwSessionHandle,
    _Reserved_ DWORD dwRestartFlags,
    _In_opt_ RM_WRITE_STATUS_CALLBACK fnStatus)
{
    PRMP_SESSION Session;

    UNREFERENCED_PARAMETER(dwRestartFlags);

    EnterCriticalSection(&RmpLock);

    Session = RmpLookupSession(dwSessionHandle);
    if (!Session)
    {
        LeaveCriticalSection(&RmpLock);
        return ERROR_INVALID_HANDLE;
    }

    if (Session->State == RmpSessionOpen)
    {
        LeaveCriticalSection(&RmpLock);
        return ERROR_REQUEST_OUT_OF_SEQUENCE;
    }

    Session->State = RmpSessionRestarted;
    LeaveCriticalSection(&RmpLock);

    RmpReportProgress(fnStatus, RMP_RESTART_STEPS);
    return ERROR_SUCCESS;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_ LPVOID lpvReserved)
{
    DWORD i;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&RmpLock);
            break;

        case DLL_PROCESS_DETACH:
            if (lpvReserved)
                break;

            for (i = 0; i < RMP_MAX_SESSIONS; i++)
                RmpReleaseSession(&RmpSessions[i]);

            DeleteCriticalSection(&RmpLock);
            break;
    }

    return TRUE;
}
