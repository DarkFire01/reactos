/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/synchansi.c
 * PURPOSE:         Synchronization functions (ANSI)
 */

/* INCLUDES ******************************************************************/

#include <k32.h>

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
HANDLE
WINAPI
CreateWaitableTimerA(IN LPSECURITY_ATTRIBUTES lpTimerAttributes OPTIONAL,
                     IN BOOL bManualReset,
                     IN LPCSTR lpTimerName OPTIONAL)
{
    ConvertWin32AnsiObjectApiToUnicodeApi(WaitableTimer, lpTimerName, lpTimerAttributes, bManualReset);
}

/*
 * @implemented
 */
HANDLE
WINAPI
OpenWaitableTimerA(IN DWORD dwDesiredAccess,
                   IN BOOL bInheritHandle,
                   IN LPCSTR lpTimerName)
{
    ConvertOpenWin32AnsiObjectApiToUnicodeApi(WaitableTimer, dwDesiredAccess, bInheritHandle, lpTimerName);
}

/*
 * @implemented
 */
HANDLE
WINAPI
DECLSPEC_HOTPATCH
CreateSemaphoreA(IN LPSECURITY_ATTRIBUTES lpSemaphoreAttributes  OPTIONAL,
                 IN LONG lInitialCount,
                 IN LONG lMaximumCount,
                 IN LPCSTR lpName  OPTIONAL)
{
    ConvertWin32AnsiObjectApiToUnicodeApi(Semaphore, lpName, lpSemaphoreAttributes, lInitialCount, lMaximumCount);
}

/*
 * @implemented
 */
HANDLE
WINAPI
DECLSPEC_HOTPATCH
OpenSemaphoreA(IN DWORD dwDesiredAccess,
               IN BOOL bInheritHandle,
               IN LPCSTR lpName)
{
    ConvertOpenWin32AnsiObjectApiToUnicodeApi(Semaphore, dwDesiredAccess, bInheritHandle, lpName);
}

/*
 * @implemented
 */
HANDLE
WINAPI
DECLSPEC_HOTPATCH
OpenMutexA(IN DWORD dwDesiredAccess,
           IN BOOL bInheritHandle,
           IN LPCSTR lpName)
{
    ConvertOpenWin32AnsiObjectApiToUnicodeApi(Mutex, dwDesiredAccess, bInheritHandle, lpName);
}

/*
 * @implemented
 */
BOOL
WINAPI
UnregisterWait(IN HANDLE WaitHandle)
{
    NTSTATUS Status;

    /* Check for invalid handle */
    if (!WaitHandle)
    {
        /* Fail */
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    /* Deregister the wait and check status */
    Status = RtlDeregisterWaitEx(WaitHandle, NULL);
    if (!(NT_SUCCESS(Status)) || (Status == STATUS_PENDING))
    {
        /* Failure or non-blocking call */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* All good */
    return TRUE;
}

HANDLE
WINAPI
DECLSPEC_HOTPATCH
CreateSemaphoreExA(IN LPSECURITY_ATTRIBUTES lpSemaphoreAttributes OPTIONAL,
                   IN LONG lInitialCount,
                   IN LONG lMaximumCount,
                   IN LPCSTR lpName  OPTIONAL,
                   IN DWORD dwFlags OPTIONAL,
                   IN DWORD dwDesiredAccess OPTIONAL)
{
    ConvertAnsiToUnicodePrologue

    if (!lpName)
    {
        return CreateSemaphoreExW(lpSemaphoreAttributes,
                                  lInitialCount,
                                  lMaximumCount,
                                  NULL,
                                  dwFlags,
                                  dwDesiredAccess);
    }

    ConvertAnsiToUnicodeBody(lpName)

    if (NT_SUCCESS(Status))
    {
        return CreateSemaphoreExW(lpSemaphoreAttributes,
                                  lInitialCount,
                                  lMaximumCount,
                                  UnicodeCache->Buffer,
                                  dwFlags,
                                  dwDesiredAccess);
    }

    ConvertAnsiToUnicodeEpilogue
}

/* EOF */
