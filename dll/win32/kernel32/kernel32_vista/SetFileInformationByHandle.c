/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Changing file information through a handle
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/iofuncs.h>

/**
 * @brief
 * Renames a file, turning a DOS target path into the NT path the file system
 * expects.
 *
 * @remarks
 * A name starting with ':' renames a stream and is passed through unchanged.
 */
static
BOOL
BasepSetFileRenameInfo(
    _In_ HANDLE FileHandle,
    _In_reads_bytes_(Size) PFILE_RENAME_INFO Info,
    _In_ DWORD Size)
{
    UNICODE_STRING NtName = { 0 };
    PFILE_RENAME_INFORMATION Rename;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING Name;
    PWSTR DosName;
    ULONG RenameSize;
    NTSTATUS Status;

    if (Size < sizeof(FILE_RENAME_INFO) ||
        Info->FileNameLength > Size - FIELD_OFFSET(FILE_RENAME_INFO, FileName))
    {
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }

    if (Info->FileNameLength > UNICODE_STRING_MAX_BYTES - sizeof(UNICODE_NULL))
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }

    if (Info->FileNameLength >= sizeof(WCHAR) && Info->FileName[0] == L':')
    {
        Name.Buffer = Info->FileName;
        Name.Length = (USHORT)Info->FileNameLength;
    }
    else
    {
        /* The name is not terminated, which the path conversion needs */
        DosName = RtlAllocateHeap(RtlGetProcessHeap(), 0, Info->FileNameLength + sizeof(UNICODE_NULL));
        if (!DosName)
        {
            BaseSetLastNTError(STATUS_NO_MEMORY);
            return FALSE;
        }

        RtlCopyMemory(DosName, Info->FileName, Info->FileNameLength);
        DosName[Info->FileNameLength / sizeof(WCHAR)] = UNICODE_NULL;

        if (!RtlDosPathNameToNtPathName_U(DosName, &NtName, NULL, NULL))
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, DosName);
            BaseSetLastNTError(STATUS_OBJECT_NAME_INVALID);
            return FALSE;
        }

        RtlFreeHeap(RtlGetProcessHeap(), 0, DosName);
        Name = NtName;
    }

    RenameSize = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + Name.Length;
    Rename = RtlAllocateHeap(RtlGetProcessHeap(), 0, RenameSize);
    if (!Rename)
    {
        RtlFreeUnicodeString(&NtName);
        BaseSetLastNTError(STATUS_NO_MEMORY);
        return FALSE;
    }

    Rename->ReplaceIfExists = Info->ReplaceIfExists;
    Rename->RootDirectory = Info->RootDirectory;
    Rename->FileNameLength = Name.Length;
    RtlCopyMemory(Rename->FileName, Name.Buffer, Name.Length);

    Status = NtSetInformationFile(FileHandle, &IoStatusBlock, Rename, RenameSize, FileRenameInformation);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Rename);
    RtlFreeUnicodeString(&NtName);

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Changes the basic, rename, disposition, allocation, end of file or I/O
 * priority information of a file.
 *
 * @return
 * FALSE with ERROR_BAD_LENGTH when the buffer is too small for the class,
 * and ERROR_INVALID_PARAMETER for a class that cannot be set.
 */
BOOL
WINAPI
SetFileInformationByHandle(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _In_reads_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize)
{
    FILE_INFORMATION_CLASS NtClass;
    IO_STATUS_BLOCK IoStatusBlock;
    ULONG MinimumSize;
    NTSTATUS Status;

    switch (FileInformationClass)
    {
        case FileBasicInfo:
            NtClass = FileBasicInformation;
            MinimumSize = sizeof(FILE_BASIC_INFO);
            break;

        case FileRenameInfo:
            return BasepSetFileRenameInfo(hFile, lpFileInformation, dwBufferSize);

        case FileDispositionInfo:
            NtClass = FileDispositionInformation;
            MinimumSize = sizeof(FILE_DISPOSITION_INFO);
            break;

        case FileAllocationInfo:
            NtClass = FileAllocationInformation;
            MinimumSize = sizeof(FILE_ALLOCATION_INFO);
            break;

        case FileEndOfFileInfo:
            NtClass = FileEndOfFileInformation;
            MinimumSize = sizeof(FILE_END_OF_FILE_INFO);
            break;

        case FileIoPriorityHintInfo:
            NtClass = FileIoPriorityHintInformation;
            MinimumSize = sizeof(FILE_IO_PRIORITY_HINT_INFO);
            break;

        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }

    if (dwBufferSize < MinimumSize)
    {
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }

    if (FileInformationClass == FileIoPriorityHintInfo &&
        (ULONG)((PFILE_IO_PRIORITY_HINT_INFO)lpFileInformation)->PriorityHint >= MaximumIoPriorityHintType)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtSetInformationFile(hFile, &IoStatusBlock, lpFileInformation, dwBufferSize, NtClass);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/* EOF */
