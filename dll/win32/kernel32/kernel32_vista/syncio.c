/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Vista synchronisation and I/O cancellation entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Three names that were `stub` spec entries until now, which means every one
 * of them raised EXCEPTION_WINE_STUB the moment it was called. A caller that
 * resolves them with GetProcAddress and guards the result is fine either way,
 * but all three are ordinary imports of ordinary programs - Teams pulls all
 * three out of kernel32 at load time - and such a caller has no reason to
 * expect an exception from a timer creation or an id query.
 *
 * So they answer instead. Where the work can really be done it is done; where
 * it cannot, the answer is the one Windows gives for "nothing to do", which a
 * caller already has to handle.
 */

#include "k32_vista.h"

#include <ndk/exfuncs.h>

/* The named pipe file system answers connection queries through this code */
#define FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE \
    CTL_CODE(FILE_DEVICE_NAMED_PIPE, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define NDEBUG
#include <debug.h>

/*
 * @implemented
 */
HANDLE
WINAPI
CreateWaitableTimerExW(
    _In_opt_ LPSECURITY_ATTRIBUTES lpTimerAttributes,
    _In_opt_ LPCWSTR lpTimerName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    /*
     * The Ex form differs from CreateWaitableTimerW only in taking the timer
     * type as a flag rather than a BOOL, and in letting the caller ask for a
     * specific access mask instead of always getting TIMER_ALL_ACCESS.
     */
    HANDLE Handle;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES LocalAttributes;
    POBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING TimerName;
    TIMER_TYPE TimerType;

    if (dwFlags & ~CREATE_WAITABLE_TIMER_MANUAL_RESET)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    TimerType = (dwFlags & CREATE_WAITABLE_TIMER_MANUAL_RESET) ?
                NotificationTimer : SynchronizationTimer;

    if (lpTimerName != NULL)
        RtlInitUnicodeString(&TimerName, lpTimerName);

    ObjectAttributes = BaseFormatObjectAttributes(&LocalAttributes,
                                                  lpTimerAttributes,
                                                  (lpTimerName != NULL) ? &TimerName : NULL);

    Status = NtCreateTimer(&Handle, dwDesiredAccess, ObjectAttributes, TimerType);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return NULL;
    }

    /*
     * A caller that asked to create the timer needs to know it got an existing
     * one instead, and the only channel for that is the last error - which
     * stays set on the success path, exactly as CreateEvent and friends do.
     */
    if (Status == STATUS_OBJECT_NAME_EXISTS)
        SetLastError(ERROR_ALREADY_EXISTS);
    else
        SetLastError(ERROR_SUCCESS);

    return Handle;
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreateWaitableTimerExA(
    _In_opt_ LPSECURITY_ATTRIBUTES lpTimerAttributes,
    _In_opt_ LPCSTR lpTimerName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    PUNICODE_STRING TimerName;
    ANSI_STRING AnsiName;
    NTSTATUS Status;

    if (lpTimerName == NULL)
    {
        return CreateWaitableTimerExW(lpTimerAttributes, NULL,
                                      dwFlags, dwDesiredAccess);
    }

    TimerName = &NtCurrentTeb()->StaticUnicodeString;
    RtlInitAnsiString(&AnsiName, lpTimerName);
    Status = RtlAnsiStringToUnicodeString(TimerName, &AnsiName, FALSE);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return NULL;
    }

    return CreateWaitableTimerExW(lpTimerAttributes, TimerName->Buffer,
                                  dwFlags, dwDesiredAccess);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNamedPipeServerProcessId(
    _In_ HANDLE Pipe,
    _Out_ PULONG ServerProcessId)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    /*
     * The named pipe file system answers this through the same connection
     * attribute query the client id uses; only the attribute name differs.
     */
    Status = NtFsControlFile(Pipe,
                             NULL,
                             NULL,
                             NULL,
                             &IoStatusBlock,
                             FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE,
                             (PVOID)"ServerProcessId",
                             sizeof("ServerProcessId"),
                             ServerProcessId,
                             sizeof(*ServerProcessId));
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
CancelSynchronousIo(
    _In_ HANDLE hThread)
{
    /*
     * This asks the kernel to complete another thread's blocking I/O early.
     * NtCancelSynchronousIoFile is the call behind it, and here it is a stub
     * with no system service slot at all, so there is nothing to forward to.
     *
     * ERROR_NOT_FOUND is the answer Windows gives when the thread had no
     * synchronous I/O outstanding, and it is the answer every caller already
     * has to handle - CancelSynchronousIo returning FALSE is the common case,
     * not an error path. Raising an exception here, which is what this did
     * before, is the one outcome a caller cannot handle.
     */
    DPRINT1("CancelSynchronousIo(%p): no kernel support, reporting nothing to cancel\n",
            hThread);

    if (hThread == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

/* EOF */
