/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/iocompl.c
 * PURPOSE:         Io Completion functions
 * PROGRAMMERS:     Ariadne (ariadne@xs4all.nl)
 *                  Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

#include <k32.h>
#define NDEBUG
#include <debug.h>

/*
 * SetFileCompletionNotificationModes is not entirely Vista-exclusive,
 * it was actually added to Windows 2003 in SP2. Headers restrict it from
 * pre-Vista though so define the flags we need for it.
 */
#if (_WIN32_WINNT < 0x0600)
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define FILE_SKIP_SET_EVENT_ON_HANDLE        0x2
#endif

/*
 * @implemented
 */
BOOL
WINAPI
SetFileCompletionNotificationModes(IN HANDLE FileHandle,
                                   IN UCHAR Flags)
{
    NTSTATUS Status;
    FILE_IO_COMPLETION_NOTIFICATION_INFORMATION FileInformation;
    IO_STATUS_BLOCK IoStatusBlock;

    if (Flags & ~(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    FileInformation.Flags = Flags;

    Status = NtSetInformationFile(FileHandle,
                                  &IoStatusBlock,
                                  &FileInformation,
                                  sizeof(FileInformation),
                                  FileIoCompletionNotificationInformation);
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
HANDLE
WINAPI
CreateIoCompletionPort(IN HANDLE FileHandle,
                       IN HANDLE ExistingCompletionPort,
                       IN ULONG_PTR CompletionKey,
                       IN DWORD NumberOfConcurrentThreads)
{
    NTSTATUS Status;
    HANDLE NewPort;
    FILE_COMPLETION_INFORMATION CompletionInformation;
    IO_STATUS_BLOCK IoStatusBlock;

    /* Check if this is a new port */
    NewPort = ExistingCompletionPort;
    if (!ExistingCompletionPort)
    {
        /* Create it */
        Status = NtCreateIoCompletion(&NewPort,
                                      IO_COMPLETION_ALL_ACCESS,
                                      NULL,
                                      NumberOfConcurrentThreads);
        if (!NT_SUCCESS(Status))
        {
            /* Convert error and fail */
            BaseSetLastNTError(Status);
            return FALSE;
        }
    }

    /* Check if no actual file is being associated with the completion port */
    if (FileHandle == INVALID_HANDLE_VALUE)
    {
        /* Was there a port already associated? */
        if (ExistingCompletionPort)
        {
            /* You are not allowed using an old port and dropping the handle */
            NewPort = NULL;
            BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        }
    }
    else
    {
        /* We have a file handle, so associated it with this completion port */
        CompletionInformation.Port = NewPort;
        CompletionInformation.Key = (PVOID)CompletionKey;
        Status = NtSetInformationFile(FileHandle,
                                      &IoStatusBlock,
                                      &CompletionInformation,
                                      sizeof(FILE_COMPLETION_INFORMATION),
                                      FileCompletionInformation);
        if (!NT_SUCCESS(Status))
        {
            /* Convert the error code and close the newly created port, if any */
            BaseSetLastNTError(Status);
            if (!ExistingCompletionPort) NtClose(NewPort);
            return FALSE;
        }
    }

    /* Return the newly created port, if any */
    return NewPort;
}

C_ASSERT(sizeof(OVERLAPPED_ENTRY) == sizeof(FILE_IO_COMPLETION_INFORMATION));

/**
 * @brief
 * Removes up to ulCount completions from a port at once, waiting for the first.
 *
 * @return
 * TRUE with at least one entry. FALSE with WAIT_TIMEOUT when none came, WAIT_IO_COMPLETION
 * when an alertable wait ran an APC, ERROR_ABANDONED_WAIT_0 when the port was closed, or
 * ERROR_INVALID_PARAMETER.
 */
BOOL
WINAPI
GetQueuedCompletionStatusEx(
    _In_ HANDLE CompletionPort,
    _Out_writes_to_(ulCount, *ulNumEntriesRemoved) LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    _In_ ULONG ulCount,
    _Out_ PULONG ulNumEntriesRemoved,
    _In_ DWORD dwMilliseconds,
    _In_ BOOL fAlertable)
{
    LARGE_INTEGER Time;
    PLARGE_INTEGER TimePtr;
    NTSTATUS Status;

    if (!lpCompletionPortEntries || !ulNumEntriesRemoved || !ulCount)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    TimePtr = BaseFormatTimeOut(&Time, dwMilliseconds);

    /* An alerted wait that ran no APC goes on waiting */
    do
    {
        Status = NtRemoveIoCompletionEx(CompletionPort,
                                        (PFILE_IO_COMPLETION_INFORMATION)lpCompletionPortEntries,
                                        ulCount,
                                        ulNumEntriesRemoved,
                                        TimePtr,
                                        fAlertable != FALSE);
    } while (fAlertable && Status == STATUS_ALERTED);

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    switch (Status)
    {
        case STATUS_TIMEOUT:
            SetLastError(WAIT_TIMEOUT);
            return FALSE;
        case STATUS_ABANDONED_WAIT_0:
            SetLastError(ERROR_ABANDONED_WAIT_0);
            return FALSE;
        case STATUS_USER_APC:
            SetLastError(WAIT_IO_COMPLETION);
            return FALSE;
        default:
            return TRUE;
    }
}

/*
 * @implemented
 */
BOOL
WINAPI
GetQueuedCompletionStatus(IN HANDLE CompletionHandle,
                          IN LPDWORD lpNumberOfBytesTransferred,
                          OUT PULONG_PTR lpCompletionKey,
                          OUT LPOVERLAPPED *lpOverlapped,
                          IN DWORD dwMilliseconds)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    ULONG_PTR CompletionKey;
    LARGE_INTEGER Time;
    PLARGE_INTEGER TimePtr;

    /* Convert the timeout and then call the native API */
    TimePtr = BaseFormatTimeOut(&Time, dwMilliseconds);
    Status = NtRemoveIoCompletion(CompletionHandle,
                                  (PVOID*)&CompletionKey,
                                  (PVOID*)lpOverlapped,
                                  &IoStatus,
                                  TimePtr);
    if (!(NT_SUCCESS(Status)) || (Status == STATUS_TIMEOUT))
    {
        /* Clear out the overlapped output */
        *lpOverlapped = NULL;

        /* Check what kind of error we got */
        if (Status == STATUS_TIMEOUT)
        {
            /* Timeout error is set directly since there's no conversion */
            SetLastError(WAIT_TIMEOUT);
        }
        else
        {
            /* Any other error gets converted */
            BaseSetLastNTError(Status);
        }

        /* This is a failure case */
        return FALSE;
    }

    /* Write back the output parameters */
    *lpCompletionKey = CompletionKey;
    *lpNumberOfBytesTransferred = IoStatus.Information;

    /* Check for error */
    if (!NT_SUCCESS(IoStatus.Status))
    {
        /* Convert and fail */
        BaseSetLastNTError(IoStatus.Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
PostQueuedCompletionStatus(IN HANDLE CompletionHandle,
                           IN DWORD dwNumberOfBytesTransferred,
                           IN ULONG_PTR dwCompletionKey,
                           IN LPOVERLAPPED lpOverlapped)
{
    NTSTATUS Status;

    /* Call the native API */
    Status = NtSetIoCompletion(CompletionHandle,
                               (PVOID)dwCompletionKey,
                               (PVOID)lpOverlapped,
                               STATUS_SUCCESS,
                               dwNumberOfBytesTransferred);
    if (!NT_SUCCESS(Status))
    {
        /* Convert the error and fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Success path */
    return TRUE;
}

/*
 * @implemented
 */
/**
 * @brief
 * Gets the result of an overlapped operation, waiting for it up to a timeout and alertably if asked.
 *
 * @return
 * TRUE once the operation completed successfully. FALSE with ERROR_IO_INCOMPLETE while it is
 * pending and no wait is asked for, WAIT_TIMEOUT or WAIT_IO_COMPLETION when the wait ends early,
 * or the operation's own error.
 */
BOOL
WINAPI
GetOverlappedResultEx(
    _In_ HANDLE hFile,
    _In_ LPOVERLAPPED lpOverlapped,
    _Out_ LPDWORD lpNumberOfBytesTransferred,
    _In_ DWORD dwMilliseconds,
    _In_ BOOL bAlertable)
{
    DWORD WaitStatus;

    if (lpOverlapped->Internal == STATUS_PENDING)
    {
        if (!dwMilliseconds)
        {
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }

        WaitStatus = WaitForSingleObjectEx(lpOverlapped->hEvent ? lpOverlapped->hEvent : hFile,
                                           dwMilliseconds,
                                           bAlertable);
        if (WaitStatus == WAIT_TIMEOUT || WaitStatus == WAIT_IO_COMPLETION)
        {
            SetLastError(WaitStatus);
            return FALSE;
        }

        /* The wait itself failed and set the error */
        if (WaitStatus != WAIT_OBJECT_0)
            return FALSE;
    }

    *lpNumberOfBytesTransferred = (DWORD)lpOverlapped->InternalHigh;
    if (!NT_SUCCESS(lpOverlapped->Internal))
    {
        BaseSetLastNTError(lpOverlapped->Internal);
        return FALSE;
    }
    return TRUE;
}

BOOL
WINAPI
GetOverlappedResult(IN HANDLE hFile,
                    IN LPOVERLAPPED lpOverlapped,
                    OUT LPDWORD lpNumberOfBytesTransferred,
                    IN BOOL bWait)
{
    DWORD WaitStatus;
    HANDLE hObject;

    /* Check for pending operation */
    if (lpOverlapped->Internal == STATUS_PENDING)
    {
        /* Check if the caller is okay with waiting */
        if (!bWait)
        {
            /* Set timeout */
            WaitStatus = WAIT_TIMEOUT;
        }
        else
        {
            /* Wait for the result */
            hObject = lpOverlapped->hEvent ? lpOverlapped->hEvent : hFile;
            WaitStatus = WaitForSingleObject(hObject, INFINITE);
        }

        /* Check for timeout */
        if (WaitStatus == WAIT_TIMEOUT)
        {
            /* We have to override the last error with INCOMPLETE instead */
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }

        /* Fail if we had an error -- the last error is already set */
        if (WaitStatus) return FALSE;
    }

    /* Return bytes transferred */
    *lpNumberOfBytesTransferred = lpOverlapped->InternalHigh;

    /* Check for failure during I/O */
    if (!NT_SUCCESS(lpOverlapped->Internal))
    {
        /* Set the error and fail */
        BaseSetLastNTError(lpOverlapped->Internal);
        return FALSE;
    }

    /* All done */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
BindIoCompletionCallback(IN HANDLE FileHandle,
                         IN LPOVERLAPPED_COMPLETION_ROUTINE Function,
                         IN ULONG Flags)
{
    NTSTATUS Status;

    /* Call RTL */
    Status = RtlSetIoCompletionCallback(FileHandle,
                                        (PIO_APC_ROUTINE)Function,
                                        Flags);
    if (!NT_SUCCESS(Status))
    {
        /* Set error and fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}

/* EOF */
