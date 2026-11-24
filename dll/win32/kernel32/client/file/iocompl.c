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
