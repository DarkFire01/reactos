/*
 * PROJECT:     ReactOS Service Control Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Service process watch and failure actions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "services.h"

#include <netevent.h>
#include <strsafe.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* The registry value keeps 32 bit slots for the three pointers on every architecture */
#define SCM_FAILURE_HEADER_32       (5 * sizeof(DWORD))

/* Services one process can host that a single crash handles */
#define SCM_CRASH_BATCH             32

/* How long a restart waits for the crashed image to be torn down */
#define SCM_RESTART_IMAGE_WAIT_MS   10000

typedef struct _SCM_RESTART
{
    PSERVICE Service;
    DWORD Delay;
} SCM_RESTART, *PSCM_RESTART;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Reads the action a failure of this service calls for, given how often it already failed.
 *
 * @param[in] Service
 * The service that terminated.
 *
 * @param[out] Action
 * The action to take, SC_ACTION_NONE when nothing is configured.
 */
static
VOID
ScmGetFailureAction(
    _Inout_ PSERVICE Service,
    _Out_ SC_ACTION *Action)
{
    HKEY ServiceKey;
    PBYTE Data = NULL;
    DWORD Size = 0;
    DWORD Type;
    DWORD ResetPeriod, Count, HeaderSize;
    DWORD Now;

    Action->Type = SC_ACTION_NONE;
    Action->Delay = 0;

    if (ScmOpenServiceKey(Service->lpServiceName, KEY_READ, &ServiceKey) != ERROR_SUCCESS)
        return;

    if (RegQueryValueExW(ServiceKey, L"FailureActions", NULL, &Type, NULL, &Size) == ERROR_SUCCESS &&
        Type == REG_BINARY &&
        Size >= SCM_FAILURE_HEADER_32)
    {
        Data = HeapAlloc(GetProcessHeap(), 0, Size);
        if (Data && RegQueryValueExW(ServiceKey, L"FailureActions", NULL, &Type, Data, &Size) != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, Data);
            Data = NULL;
        }
    }

    RegCloseKey(ServiceKey);

    if (!Data)
        return;

    /* Windows stores 32 bit pointer slots; ChangeServiceConfig2 here writes the native structure */
    ResetPeriod = ((PDWORD)Data)[0];
    Count = ((PDWORD)Data)[3];
    HeaderSize = SCM_FAILURE_HEADER_32;
    if (Size != HeaderSize + Count * sizeof(SC_ACTION) && Size >= sizeof(SERVICE_FAILURE_ACTIONSW))
    {
        Count = ((LPSERVICE_FAILURE_ACTIONSW)Data)->cActions;
        HeaderSize = sizeof(SERVICE_FAILURE_ACTIONSW);
    }

    if (Count == 0 || Size < HeaderSize + Count * sizeof(SC_ACTION))
    {
        HeapFree(GetProcessHeap(), 0, Data);
        return;
    }

    /* The failure count starts over once the service ran clean for the reset period */
    Now = GetTickCount();
    if (ResetPeriod != INFINITE &&
        Service->dwFailureCount != 0 &&
        (ULONGLONG)(Now - Service->dwLastFailure) >= (ULONGLONG)ResetPeriod * 1000)
    {
        Service->dwFailureCount = 0;
    }

    Service->dwFailureCount++;
    Service->dwLastFailure = Now;

    *Action = ((SC_ACTION *)(Data + HeaderSize))[min(Service->dwFailureCount, Count) - 1];
    HeapFree(GetProcessHeap(), 0, Data);
}

static
DWORD
WINAPI
ScmRestartThread(
    _In_ PVOID Parameter)
{
    PSCM_RESTART Restart = Parameter;
    BOOLEAN Released = FALSE;
    DWORD Waited;
    DWORD Error;

    Sleep(Restart->Delay);

    /* The stop sequence has to let go of the dead image before a new process can start */
    for (Waited = 0; !Released && Waited < SCM_RESTART_IMAGE_WAIT_MS; Waited += 100)
    {
        ScmLockDatabaseShared();
        Released = (Restart->Service->lpImage == NULL);
        ScmUnlockDatabase();
        if (!Released)
            Sleep(100);
    }

    Error = ScmStartService(Restart->Service, 0, NULL);
    DPRINT1("Restarted '%S' after its failure: %lu\n", Restart->Service->lpServiceName, Error);

    ScmLockDatabaseExclusive();
    ScmDereferenceService(Restart->Service);
    ScmUnlockDatabase();

    HeapFree(GetProcessHeap(), 0, Restart);
    return 0;
}

/**
 * @brief
 * Logs the crash and carries out the configured action, the database lock held.
 */
static
VOID
ScmHandleServiceCrash(
    _Inout_ PSERVICE Service,
    _In_ DWORD PreviousState)
{
    WCHAR Count[16];
    WCHAR Delay[16];
    LPCWSTR Strings[5];
    PSCM_RESTART Restart;
    SC_ACTION Action;
    HANDLE Thread;

    /* A stop the service asked for is not a failure */
    if (PreviousState == SERVICE_STOP_PENDING || Service->bDeleted)
        return;

    ScmGetFailureAction(Service, &Action);

    DPRINT1("Service '%S' terminated unexpectedly (failure %lu), action %lu after %lu ms\n",
            Service->lpServiceName, Service->dwFailureCount, Action.Type, Action.Delay);

    StringCchPrintfW(Count, ARRAYSIZE(Count), L"%lu", Service->dwFailureCount);
    StringCchPrintfW(Delay, ARRAYSIZE(Delay), L"%lu", Action.Delay);
    Strings[0] = Service->lpDisplayName;
    Strings[1] = Count;
    Strings[2] = Delay;
    Strings[3] = L"";
    Strings[4] = (Action.Type == SC_ACTION_RESTART) ? L"Restart the service" : L"No action";

    if (Action.Type == SC_ACTION_NONE)
    {
        ScmLogEvent(EVENT_SERVICE_CRASH_NO_ACTION, EVENTLOG_ERROR_TYPE, 2, Strings);
        return;
    }

    ScmLogEvent(EVENT_SERVICE_CRASH, EVENTLOG_ERROR_TYPE, 5, Strings);

    /* Reboot and run-command actions are not carried out yet */
    if (Action.Type != SC_ACTION_RESTART || Service->dwStartType == SERVICE_DISABLED)
        return;

    Restart = HeapAlloc(GetProcessHeap(), 0, sizeof(*Restart));
    if (!Restart)
        return;

    Restart->Service = Service;
    Restart->Delay = Action.Delay;
    ScmReferenceService(Service);

    Thread = CreateThread(NULL, 0, ScmRestartThread, Restart, 0, NULL);
    if (!Thread)
    {
        ScmDereferenceService(Service);
        HeapFree(GetProcessHeap(), 0, Restart);
        return;
    }

    CloseHandle(Thread);
}

/**
 * @brief
 * Runs when a service process ends, marking what it still hosted as stopped.
 *
 * @param[in] Context
 * The process id of the image.
 */
static
VOID
CALLBACK
ScmServiceProcessExited(
    _In_ PVOID Context,
    _In_ BOOLEAN TimedOut)
{
    DWORD ProcessId = (DWORD)(ULONG_PTR)Context;
    PSERVICE Crashed[SCM_CRASH_BATCH];
    DWORD PreviousState[SCM_CRASH_BATCH];
    PLIST_ENTRY Entry;
    PSERVICE Service;
    HANDLE Thread;
    ULONG Found = 0;
    ULONG i;

    UNREFERENCED_PARAMETER(TimedOut);

    ScmLockDatabaseExclusive();

    for (Entry = ServiceListHead.Flink; Entry != &ServiceListHead && Found < SCM_CRASH_BATCH; Entry = Entry->Flink)
    {
        Service = CONTAINING_RECORD(Entry, SERVICE, ServiceListEntry);
        if (Service->lpImage == NULL ||
            Service->lpImage->dwProcessId != ProcessId ||
            Service->Status.dwCurrentState == SERVICE_STOPPED)
        {
            continue;
        }

        PreviousState[Found] = Service->Status.dwCurrentState;
        Crashed[Found++] = Service;
    }

    for (i = 0; i < Found; i++)
    {
        Service = Crashed[i];

        Service->Status.dwCurrentState = SERVICE_STOPPED;
        Service->Status.dwWin32ExitCode = ERROR_PROCESS_ABORTED;
        Service->Status.dwServiceSpecificExitCode = 0;
        Service->Status.dwCheckPoint = 0;
        Service->Status.dwWaitHint = 0;

        ScmHandleServiceCrash(Service, PreviousState[i]);

        /* The stop sequence drops the running reference and the image, as for a reported stop */
        Thread = CreateThread(NULL, 0, ScmStopThread, Service, 0, NULL);
        if (Thread)
            CloseHandle(Thread);
        else
            ScmDereferenceService(Service);
    }

    ScmUnlockDatabase();
}

/**
 * @brief
 * Starts watching a newly started service process for its end.
 */
VOID
ScmWatchServiceImage(
    _Inout_ PSERVICE_IMAGE Image)
{
    if (Image->hProcess == INVALID_HANDLE_VALUE || Image->hProcess == NULL || Image->hProcessWait != NULL)
        return;

    if (!DuplicateHandle(GetCurrentProcess(),
                         Image->hProcess,
                         GetCurrentProcess(),
                         &Image->hWatchProcess,
                         SYNCHRONIZE,
                         FALSE,
                         0))
    {
        Image->hWatchProcess = NULL;
        return;
    }

    if (!RegisterWaitForSingleObject(&Image->hProcessWait,
                                     Image->hWatchProcess,
                                     ScmServiceProcessExited,
                                     (PVOID)(ULONG_PTR)Image->dwProcessId,
                                     INFINITE,
                                     WT_EXECUTEONLYONCE))
    {
        CloseHandle(Image->hWatchProcess);
        Image->hWatchProcess = NULL;
        Image->hProcessWait = NULL;
    }
}

/**
 * @brief
 * Drops the watch before the image goes away. The callback only looks services
 * up by process id under the database lock, so it may still be running.
 */
VOID
ScmUnwatchServiceImage(
    _Inout_ PSERVICE_IMAGE Image)
{
    if (Image->hProcessWait != NULL)
    {
        UnregisterWait(Image->hProcessWait);
        Image->hProcessWait = NULL;
    }

    if (Image->hWatchProcess != NULL)
    {
        CloseHandle(Image->hWatchProcess);
        Image->hWatchProcess = NULL;
    }
}

/* EOF */
