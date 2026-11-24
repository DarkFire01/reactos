/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            dll/win32/kernel32/client/synch.c
 * PURPOSE:         Wrappers for the NT Wait Implementation
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/
#include <k32.h>

#define NDEBUG
#include <debug.h>

#undef InterlockedIncrement
#undef InterlockedDecrement
#undef InterlockedExchange
#undef InterlockedExchangeAdd
#undef InterlockedCompareExchange

/* FUNCTIONS *****************************************************************/

HANDLE
TranslateStdHandle(IN HANDLE hHandle)
{
    PRTL_USER_PROCESS_PARAMETERS Ppb = NtCurrentPeb()->ProcessParameters;

    switch ((ULONG_PTR)hHandle)
    {
        case STD_INPUT_HANDLE:  return Ppb->StandardInput;
        case STD_OUTPUT_HANDLE: return Ppb->StandardOutput;
        case STD_ERROR_HANDLE:  return Ppb->StandardError;
    }

    return hHandle;
}

/*
 * @implemented
 */
BOOL
WINAPI
RegisterWaitForSingleObject(OUT PHANDLE phNewWaitObject,
                            IN HANDLE hObject,
                            IN WAITORTIMERCALLBACK Callback,
                            IN PVOID Context,
                            IN ULONG dwMilliseconds,
                            IN ULONG dwFlags)
{
    NTSTATUS Status;

    /* Get real handle */
    hObject = TranslateStdHandle(hObject);

    /* Check for console handle */
    if ((IsConsoleHandle(hObject)) && (VerifyConsoleIoHandle(hObject)))
    {
        /* Get the real wait handle */
        hObject = GetConsoleInputWaitHandle();
    }

    /* Register the wait now */
    Status = RtlRegisterWait(phNewWaitObject,
                             hObject,
                             Callback,
                             Context,
                             dwMilliseconds,
                             dwFlags);
    if (!NT_SUCCESS(Status))
    {
        /* Return failure */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* All good */
    return TRUE;
}