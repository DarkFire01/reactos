/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     CancelIoEx
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "k32_vista.h"

/**
 * @brief Requests cancellation of I/O issued by any thread of the process on a handle.
 *
 * @param[in] hFile Handle the I/O was issued on.
 * @param[in] lpOverlapped The request to cancel, or NULL for all requests on hFile.
 *
 * @return FALSE with ERROR_NOT_FOUND when nothing matching was pending.
 */
BOOL
WINAPI
CancelIoEx(
    _In_ HANDLE hFile,
    _In_opt_ LPOVERLAPPED lpOverlapped)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    Status = NtCancelIoFileEx(hFile, (PIO_STATUS_BLOCK)lpOverlapped, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
