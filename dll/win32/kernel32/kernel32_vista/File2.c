/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows 8 file creation and copy routines
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* The COPYFILE2 types are only declared for Windows 7 and up */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x602

#include "k32_vista.h"
#include <fileapi.h>

#define BASEP_COPYFILE2_VALID_FLAGS (COPY_FILE_FAIL_IF_EXISTS | \
                                     COPY_FILE_RESTARTABLE | \
                                     COPY_FILE_OPEN_SOURCE_FOR_WRITE | \
                                     COPY_FILE_ALLOW_DECRYPTED_DESTINATION | \
                                     COPY_FILE_COPY_SYMLINK | \
                                     COPY_FILE_NO_BUFFERING | \
                                     COPY_FILE_REQUEST_SECURITY_PRIVILEGES | \
                                     COPY_FILE_RESUME_FROM_PAUSE | \
                                     COPY_FILE_NO_OFFLOAD)

typedef struct _BASEP_COPYFILE2_CONTEXT
{
    PCOPYFILE2_PROGRESS_ROUTINE ProgressRoutine;
    PVOID CallbackContext;
    ULONGLONG ChunkNumber;
    ULONGLONG StreamBytesReported;
} BASEP_COPYFILE2_CONTEXT, *PBASEP_COPYFILE2_CONTEXT;

/**
 * @brief
 * Turns CopyFileExW progress reports into COPYFILE2 messages.
 */
static
DWORD
CALLBACK
BasepCopyFile2Progress(
    _In_ LARGE_INTEGER TotalFileSize,
    _In_ LARGE_INTEGER TotalBytesTransferred,
    _In_ LARGE_INTEGER StreamSize,
    _In_ LARGE_INTEGER StreamBytesTransferred,
    _In_ DWORD dwStreamNumber,
    _In_ DWORD dwCallbackReason,
    _In_ HANDLE hSourceFile,
    _In_ HANDLE hDestinationFile,
    _In_opt_ LPVOID lpData)
{
    PBASEP_COPYFILE2_CONTEXT Context = lpData;
    COPYFILE2_MESSAGE Message;

    RtlZeroMemory(&Message, sizeof(Message));

    if (dwCallbackReason == CALLBACK_STREAM_SWITCH)
    {
        Context->ChunkNumber = 0;
        Context->StreamBytesReported = 0;

        Message.Type = COPYFILE2_CALLBACK_STREAM_STARTED;
        Message.Info.StreamStarted.dwStreamNumber = dwStreamNumber;
        Message.Info.StreamStarted.hSourceFile = hSourceFile;
        Message.Info.StreamStarted.hDestinationFile = hDestinationFile;
        Message.Info.StreamStarted.uliStreamSize.QuadPart = StreamSize.QuadPart;
        Message.Info.StreamStarted.uliTotalFileSize.QuadPart = TotalFileSize.QuadPart;
    }
    else
    {
        Message.Type = COPYFILE2_CALLBACK_CHUNK_FINISHED;
        Message.Info.ChunkFinished.dwStreamNumber = dwStreamNumber;
        Message.Info.ChunkFinished.hSourceFile = hSourceFile;
        Message.Info.ChunkFinished.hDestinationFile = hDestinationFile;
        Message.Info.ChunkFinished.uliChunkNumber.QuadPart = Context->ChunkNumber;
        Message.Info.ChunkFinished.uliChunkSize.QuadPart =
            StreamBytesTransferred.QuadPart - Context->StreamBytesReported;
        Message.Info.ChunkFinished.uliStreamSize.QuadPart = StreamSize.QuadPart;
        Message.Info.ChunkFinished.uliStreamBytesTransferred.QuadPart = StreamBytesTransferred.QuadPart;
        Message.Info.ChunkFinished.uliTotalFileSize.QuadPart = TotalFileSize.QuadPart;
        Message.Info.ChunkFinished.uliTotalBytesTransferred.QuadPart = TotalBytesTransferred.QuadPart;

        Context->ChunkNumber++;
        Context->StreamBytesReported = StreamBytesTransferred.QuadPart;
    }

    switch (Context->ProgressRoutine(&Message, Context->CallbackContext))
    {
        case COPYFILE2_PROGRESS_CONTINUE:
            return PROGRESS_CONTINUE;

        case COPYFILE2_PROGRESS_CANCEL:
            return PROGRESS_CANCEL;

        case COPYFILE2_PROGRESS_QUIET:
            return PROGRESS_QUIET;

        /* Pausing needs a restartable copy, so it stops like STOP does */
        default:
            return PROGRESS_STOP;
    }
}

/**
 * @brief
 * Copies a file, reporting progress through COPYFILE2 messages.
 *
 * @return
 * S_OK on success, E_INVALIDARG for a bad parameter block or copy flag, and
 * the copy error as an HRESULT otherwise.
 */
HRESULT
WINAPI
CopyFile2(
    _In_ PCWSTR pwszExistingFileName,
    _In_ PCWSTR pwszNewFileName,
    _In_opt_ COPYFILE2_EXTENDED_PARAMETERS *pExtendedParameters)
{
    BASEP_COPYFILE2_CONTEXT Context = { 0 };
    LPPROGRESS_ROUTINE ProgressRoutine = NULL;
    LPBOOL Cancel = NULL;
    DWORD CopyFlags = 0;

    if (pExtendedParameters)
    {
        if (pExtendedParameters->dwSize != sizeof(*pExtendedParameters))
            return E_INVALIDARG;

        CopyFlags = pExtendedParameters->dwCopyFlags;
        Cancel = pExtendedParameters->pfCancel;

        if (pExtendedParameters->pProgressRoutine)
        {
            Context.ProgressRoutine = pExtendedParameters->pProgressRoutine;
            Context.CallbackContext = pExtendedParameters->pvCallbackContext;
            ProgressRoutine = BasepCopyFile2Progress;
        }
    }

    if (CopyFlags & ~BASEP_COPYFILE2_VALID_FLAGS)
        return E_INVALIDARG;

    if (!CopyFileExW(pwszExistingFileName,
                     pwszNewFileName,
                     ProgressRoutine,
                     &Context,
                     Cancel,
                     CopyFlags))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

/**
 * @brief
 * Creates or opens a file, taking the CreateFileW extras from a parameter
 * block.
 */
HANDLE
WINAPI
CreateFile2(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_ DWORD dwCreationDisposition,
    _In_opt_ LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams)
{
    LPSECURITY_ATTRIBUTES SecurityAttributes = NULL;
    DWORD FlagsAndAttributes = 0;
    HANDLE TemplateFile = NULL;

    if (pCreateExParams)
    {
        if (pCreateExParams->dwSize < sizeof(*pCreateExParams))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }

        FlagsAndAttributes = pCreateExParams->dwFileAttributes |
                             pCreateExParams->dwFileFlags |
                             pCreateExParams->dwSecurityQosFlags;
        SecurityAttributes = pCreateExParams->lpSecurityAttributes;
        TemplateFile = pCreateExParams->hTemplateFile;
    }

    return CreateFileW(lpFileName,
                       dwDesiredAccess,
                       dwShareMode,
                       SecurityAttributes,
                       dwCreationDisposition,
                       FlagsAndAttributes,
                       TemplateFile);
}

/* EOF */
