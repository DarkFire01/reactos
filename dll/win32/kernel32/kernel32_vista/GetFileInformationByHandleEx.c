/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Querying file information through a handle
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/iofuncs.h>

typedef enum _BASEP_FILE_QUERY_TYPE
{
    BasepQueryFile,
    BasepQueryDirectory,
    BasepQueryVolume
} BASEP_FILE_QUERY_TYPE;

/**
 * @brief
 * Retrieves information about a file, a directory listing or the storage of
 * the volume the file resides on.
 *
 * @return
 * FALSE with ERROR_INVALID_PARAMETER for a class that cannot be queried,
 * such as the set only classes, and ERROR_BAD_LENGTH when the buffer is too
 * small for the class.
 */
BOOL
WINAPI
GetFileInformationByHandleEx(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _Out_writes_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize)
{
    BASEP_FILE_QUERY_TYPE QueryType = BasepQueryFile;
    FILE_INFORMATION_CLASS NtClass = 0;
    IO_STATUS_BLOCK IoStatusBlock;
    BOOLEAN RestartScan = FALSE;
    ULONG MinimumSize;
    NTSTATUS Status;

    switch (FileInformationClass)
    {
        case FileBasicInfo:
            NtClass = FileBasicInformation;
            MinimumSize = sizeof(FILE_BASIC_INFO);
            break;

        case FileStandardInfo:
            NtClass = FileStandardInformation;
            MinimumSize = sizeof(FILE_STANDARD_INFO);
            break;

        case FileNameInfo:
            NtClass = FileNameInformation;
            MinimumSize = sizeof(FILE_NAME_INFO);
            break;

        case FileNormalizedNameInfo:
            NtClass = FileNormalizedNameInformation;
            MinimumSize = sizeof(FILE_NAME_INFO);
            break;

        case FileStreamInfo:
            NtClass = FileStreamInformation;
            MinimumSize = sizeof(FILE_STREAM_INFO);
            break;

        case FileCompressionInfo:
            NtClass = FileCompressionInformation;
            MinimumSize = sizeof(FILE_COMPRESSION_INFO);
            break;

        case FileAttributeTagInfo:
            NtClass = FileAttributeTagInformation;
            MinimumSize = sizeof(FILE_ATTRIBUTE_TAG_INFO);
            break;

        case FileRemoteProtocolInfo:
            NtClass = FileRemoteProtocolInformation;
            MinimumSize = sizeof(FILE_REMOTE_PROTOCOL_INFO);
            break;

        case FileAlignmentInfo:
            NtClass = FileAlignmentInformation;
            MinimumSize = sizeof(FILE_ALIGNMENT_INFO);
            break;

        case FileCaseSensitiveInfo:
            NtClass = FileCaseSensitiveInformation;
            MinimumSize = sizeof(FILE_CASE_SENSITIVE_INFO);
            break;

        case FileIdInfo:
            /* FILE_ID_INFO is not in the headers yet; the kernel fails short buffers with the same error */
            NtClass = FileIdInformation;
            MinimumSize = 0;
            break;

        case FileStorageInfo:
            /* FILE_STORAGE_INFO has the layout of the volume sector size information */
            QueryType = BasepQueryVolume;
            MinimumSize = sizeof(FILE_STORAGE_INFO);
            break;

        case FileIdBothDirectoryRestartInfo:
            RestartScan = TRUE;
            /* Fall through */
        case FileIdBothDirectoryInfo:
            QueryType = BasepQueryDirectory;
            NtClass = FileIdBothDirectoryInformation;
            MinimumSize = sizeof(FILE_ID_BOTH_DIR_INFO);
            break;

        case FileFullDirectoryRestartInfo:
            RestartScan = TRUE;
            /* Fall through */
        case FileFullDirectoryInfo:
            QueryType = BasepQueryDirectory;
            NtClass = FileFullDirectoryInformation;
            MinimumSize = sizeof(FILE_FULL_DIR_INFORMATION);
            break;

        case FileIdExtdDirectoryRestartInfo:
            RestartScan = TRUE;
            /* Fall through */
        case FileIdExtdDirectoryInfo:
            /* FILE_ID_EXTD_DIR_INFO is not in the headers yet; the kernel fails short buffers the same way */
            QueryType = BasepQueryDirectory;
            NtClass = FileIdExtdDirectoryInformation;
            MinimumSize = 0;
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

    switch (QueryType)
    {
        case BasepQueryVolume:
            Status = NtQueryVolumeInformationFile(hFile,
                                                  &IoStatusBlock,
                                                  lpFileInformation,
                                                  dwBufferSize,
                                                  FileFsSectorSizeInformation);
            break;

        case BasepQueryDirectory:
            Status = NtQueryDirectoryFile(hFile,
                                          NULL,
                                          NULL,
                                          NULL,
                                          &IoStatusBlock,
                                          lpFileInformation,
                                          dwBufferSize,
                                          NtClass,
                                          FALSE,
                                          NULL,
                                          RestartScan);

            /* An asynchronous handle still gets a synchronous answer */
            if (Status == STATUS_PENDING)
            {
                Status = NtWaitForSingleObject(hFile, FALSE, NULL);
                if (NT_SUCCESS(Status))
                    Status = IoStatusBlock.Status;
            }
            break;

        default:
            Status = NtQueryInformationFile(hFile,
                                            &IoStatusBlock,
                                            lpFileInformation,
                                            dwBufferSize,
                                            NtClass);
            break;
    }

    /* A file without any stream reports the end of the list */
    if (NT_SUCCESS(Status) &&
        FileInformationClass == FileStreamInfo &&
        IoStatusBlock.Information == 0)
        Status = STATUS_END_OF_FILE;

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/* EOF */
