/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         See COPYING in the top level directory
 * FILE:            dll/win32/kernel32/client/timerqueue.c
 * PURPOSE:         Timer Queue Functions
 * PROGRAMMERS:     Thomas Weidenmueller <w3seek@reactos.com>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

HANDLE BasepDefaultTimerQueue = NULL;

/* PRIVATE FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
CancelTimerQueueTimer(IN HANDLE TimerQueue,
                      IN HANDLE Timer)
{
    NTSTATUS Status;

    if (!TimerQueue)
    {
        /* Use the default timer queue */
        TimerQueue = BasepDefaultTimerQueue;
        if (!TimerQueue)
        {
            /* A timer is being cancelled before one was created... fail */
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
    }

    /* Delete the timer */
    Status = RtlDeleteTimer(TimerQueue, Timer, NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
DeleteTimerQueue(IN HANDLE TimerQueue)
{
    return DeleteTimerQueueEx(TimerQueue, NULL);
}

/*
 * @implemented
 */
HANDLE
WINAPI
SetTimerQueueTimer(IN HANDLE TimerQueue,
                   IN WAITORTIMERCALLBACK Callback,
                   IN PVOID Parameter,
                   IN DWORD DueTime,
                   IN DWORD Period,
                   IN BOOL PreferIo)
{
    HANDLE NewTimer = NULL;
    CreateTimerQueueTimer(&NewTimer, TimerQueue, Callback, Parameter, DueTime, Period, PreferIo);
    return NewTimer;
}
