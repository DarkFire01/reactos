/*
 * PROJECT:     ReactOS Restart Manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Sessions an installer opens to find out what holds its files
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * An installer opens a session, registers the files, processes and services it
 * is about to replace, and asks which applications are using them. Nothing here
 * tracks who holds a file, so the list always comes back empty and the caller
 * goes ahead with the install, which is what it does on a quiet machine anyway.
 */

#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <restartmanager.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(rstrtmgr);

#define RM_MAX_SESSIONS 64

typedef struct _RM_SESSION
{
    BOOL InUse;
    WCHAR Key[CCH_RM_SESSION_KEY + 1];
} RM_SESSION;

static RM_SESSION RmSessions[RM_MAX_SESSIONS];
static CRITICAL_SECTION RmSessionLock;
static CRITICAL_SECTION_DEBUG RmSessionLockDebug =
{
    0, 0, &RmSessionLock,
    { &RmSessionLockDebug.ProcessLocksList, &RmSessionLockDebug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": RmSessionLock") }
};
static CRITICAL_SECTION RmSessionLock = { &RmSessionLockDebug, -1, 0, 0, 0, 0 };

/* The key only has to tell this process's sessions apart */
static
VOID
RmMakeSessionKey(
    _In_ DWORD SessionHandle,
    _Out_writes_(CCH_RM_SESSION_KEY + 1) LPWSTR Key)
{
    FILETIME Now;
    ULONG Index;
    ULONG Words[4];

    GetSystemTimeAsFileTime(&Now);
    Words[0] = Now.dwLowDateTime;
    Words[1] = Now.dwHighDateTime;
    Words[2] = GetCurrentProcessId();
    Words[3] = SessionHandle;

    for (Index = 0; Index < ARRAYSIZE(Words); Index++)
    {
        ULONG Shift;

        for (Shift = 32; Shift > 0; Shift -= 4)
        {
            ULONG Nibble = (Words[Index] >> (Shift - 4)) & 0xF;
            *Key++ = (WCHAR)(Nibble < 10 ? L'0' + Nibble : L'A' + Nibble - 10);
        }
    }

    *Key = UNICODE_NULL;
}

static
BOOL
RmIsSession(
    _In_ DWORD SessionHandle)
{
    return SessionHandle < RM_MAX_SESSIONS && RmSessions[SessionHandle].InUse;
}

/*
 * @implemented
 */
DWORD WINAPI
RmStartSession(
    DWORD *pSessionHandle,
    DWORD dwSessionFlags,
    WCHAR strSessionKey[])
{
    DWORD Index;
    DWORD Error = ERROR_MAX_SESSIONS_REACHED;

    TRACE("RmStartSession(%p %lu %p)\n", pSessionHandle, dwSessionFlags, strSessionKey);

    if (pSessionHandle == NULL || strSessionKey == NULL || dwSessionFlags != 0)
        return ERROR_BAD_ARGUMENTS;

    EnterCriticalSection(&RmSessionLock);

    for (Index = 0; Index < RM_MAX_SESSIONS; Index++)
    {
        if (RmSessions[Index].InUse)
            continue;

        RmSessions[Index].InUse = TRUE;
        RmMakeSessionKey(Index, RmSessions[Index].Key);

        *pSessionHandle = Index;
        wcscpy(strSessionKey, RmSessions[Index].Key);
        Error = ERROR_SUCCESS;
        break;
    }

    LeaveCriticalSection(&RmSessionLock);

    return Error;
}

/*
 * @implemented
 *
 * Sessions do not outlive the process that started them, so only a key this
 * process handed out is known here.
 */
DWORD WINAPI
RmJoinSession(
    DWORD *pSessionHandle,
    const WCHAR strSessionKey[])
{
    DWORD Index;
    DWORD Error = ERROR_INVALID_HANDLE;

    TRACE("RmJoinSession(%p %s)\n", pSessionHandle, debugstr_w(strSessionKey));

    if (pSessionHandle == NULL || strSessionKey == NULL)
        return ERROR_BAD_ARGUMENTS;

    EnterCriticalSection(&RmSessionLock);

    for (Index = 0; Index < RM_MAX_SESSIONS; Index++)
    {
        if (!RmSessions[Index].InUse)
            continue;

        if (_wcsicmp(RmSessions[Index].Key, strSessionKey) == 0)
        {
            *pSessionHandle = Index;
            Error = ERROR_SUCCESS;
            break;
        }
    }

    LeaveCriticalSection(&RmSessionLock);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
RmEndSession(
    DWORD dwSessionHandle)
{
    DWORD Error = ERROR_INVALID_HANDLE;

    TRACE("RmEndSession(%lu)\n", dwSessionHandle);

    EnterCriticalSection(&RmSessionLock);

    if (RmIsSession(dwSessionHandle))
    {
        RmSessions[dwSessionHandle].InUse = FALSE;
        Error = ERROR_SUCCESS;
    }

    LeaveCriticalSection(&RmSessionLock);

    return Error;
}

/*
 * @unimplemented
 *
 * The resources are accepted and forgotten, so RmGetList reports nothing.
 */
DWORD WINAPI
RmRegisterResources(
    DWORD dwSessionHandle,
    UINT nFiles,
    LPCWSTR rgsFileNames[],
    UINT nApplications,
    RM_UNIQUE_PROCESS rgApplications[],
    UINT nServices,
    LPCWSTR rgsServiceNames[])
{
    FIXME("(%lu %u %p %u %p %u %p) resources are not tracked\n", dwSessionHandle,
          nFiles, rgsFileNames, nApplications, rgApplications, nServices, rgsServiceNames);

    if ((nFiles != 0 && rgsFileNames == NULL) ||
        (nApplications != 0 && rgApplications == NULL) ||
        (nServices != 0 && rgsServiceNames == NULL))
    {
        return ERROR_BAD_ARGUMENTS;
    }

    if (!RmIsSession(dwSessionHandle))
        return ERROR_INVALID_HANDLE;

    return ERROR_SUCCESS;
}

/*
 * @unimplemented
 */
DWORD WINAPI
RmGetList(
    DWORD dwSessionHandle,
    UINT *pnProcInfoNeeded,
    UINT *pnProcInfo,
    RM_PROCESS_INFO rgAffectedApps[],
    LPDWORD lpdwRebootReasons)
{
    FIXME("(%lu %p %p %p %p) no application is ever reported\n", dwSessionHandle,
          pnProcInfoNeeded, pnProcInfo, rgAffectedApps, lpdwRebootReasons);

    if (pnProcInfoNeeded == NULL || lpdwRebootReasons == NULL)
        return ERROR_BAD_ARGUMENTS;

    if (!RmIsSession(dwSessionHandle))
        return ERROR_INVALID_HANDLE;

    *pnProcInfoNeeded = 0;
    if (pnProcInfo != NULL)
        *pnProcInfo = 0;

    *lpdwRebootReasons = RmRebootReasonNone;

    return ERROR_SUCCESS;
}

/*
 * @unimplemented
 */
DWORD WINAPI
RmShutdown(
    DWORD dwSessionHandle,
    ULONG lActionFlags,
    RM_WRITE_STATUS_CALLBACK fnStatus)
{
    FIXME("(%lu 0x%lx %p) nothing to shut down\n", dwSessionHandle, lActionFlags, fnStatus);

    if (!RmIsSession(dwSessionHandle))
        return ERROR_INVALID_HANDLE;

    if (fnStatus != NULL)
        fnStatus(100);

    return ERROR_SUCCESS;
}

/*
 * @unimplemented
 */
DWORD WINAPI
RmRestart(
    DWORD dwSessionHandle,
    DWORD dwRestartFlags,
    RM_WRITE_STATUS_CALLBACK fnStatus)
{
    FIXME("(%lu 0x%lx %p) nothing to restart\n", dwSessionHandle, dwRestartFlags, fnStatus);

    if (dwRestartFlags != 0)
        return ERROR_BAD_ARGUMENTS;

    if (!RmIsSession(dwSessionHandle))
        return ERROR_INVALID_HANDLE;

    if (fnStatus != NULL)
        fnStatus(100);

    return ERROR_SUCCESS;
}

/*
 * @implemented
 *
 * Shutdown and restart finish inside their own call, so there is never a task
 * left to cancel.
 */
DWORD WINAPI
RmCancelCurrentTask(
    DWORD dwSessionHandle)
{
    TRACE("RmCancelCurrentTask(%lu)\n", dwSessionHandle);

    if (!RmIsSession(dwSessionHandle))
        return ERROR_INVALID_HANDLE;

    return ERROR_SUCCESS;
}
