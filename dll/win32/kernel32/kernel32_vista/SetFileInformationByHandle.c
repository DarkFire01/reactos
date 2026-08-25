/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Setting file information by handle
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/iofuncs.h>

#define NDEBUG
#include <debug.h>

/*
 * @implemented
 *
 * Only a handful of the classes may be set; the rest report the file
 * information the query side answers, and Windows rejects them here. Each
 * settable class shares its layout with the native structure it names, so
 * the caller's buffer goes to the kernel untouched.
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetFileInformationByHandle(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _In_reads_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize)
{
    FILE_INFORMATION_CLASS NativeClass;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    switch (FileInformationClass)
    {
        case FileBasicInfo:
            NativeClass = FileBasicInformation;
            break;

        case FileRenameInfo:
            NativeClass = FileRenameInformation;
            break;

        case FileDispositionInfo:
            NativeClass = FileDispositionInformation;
            break;

        case FileAllocationInfo:
            NativeClass = FileAllocationInformation;
            break;

        case FileEndOfFileInfo:
            NativeClass = FileEndOfFileInformation;
            break;

        case FileIoPriorityHintInfo:
            NativeClass = FileIoPriorityHintInformation;
            break;

        default:
            DPRINT1("Class %d cannot be set\n", FileInformationClass);
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }

    Status = NtSetInformationFile(hFile,
                                  &IoStatusBlock,
                                  lpFileInformation,
                                  dwBufferSize,
                                  NativeClass);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}
