/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Extended system information queries of Windows 8+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Retrieves system information, accepting a per-query input buffer used by
 * information classes that require additional parameters.
 *
 * @param[in] SystemInformationClass
 * The class of system information to retrieve.
 *
 * @param[in] InputBuffer
 * The class-specific input buffer.
 *
 * @param[in] InputBufferLength
 * Size, in bytes, of @p InputBuffer.
 *
 * @param[out] SystemInformation
 * Receives the requested information.
 *
 * @param[in] SystemInformationLength
 * Size, in bytes, of @p SystemInformation.
 *
 * @param[out] ReturnLength
 * Optionally receives the number of bytes written or required.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement the extended system information queries.
 */
NTSTATUS
NTAPI
NtQuerySystemInformationEx(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength)
{
    UNREFERENCED_PARAMETER(SystemInformationClass);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(SystemInformation);
    UNREFERENCED_PARAMETER(SystemInformationLength);

    if (ReturnLength != NULL)
        *ReturnLength = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Kernel-mode wrapper for NtQuerySystemInformationEx().
 *
 * @param[in] SystemInformationClass
 * The class of system information to retrieve.
 *
 * @param[in] InputBuffer
 * The class-specific input buffer.
 *
 * @param[in] InputBufferLength
 * Size, in bytes, of @p InputBuffer.
 *
 * @param[out] SystemInformation
 * Receives the requested information.
 *
 * @param[in] SystemInformationLength
 * Size, in bytes, of @p SystemInformation.
 *
 * @param[out] ReturnLength
 * Optionally receives the number of bytes written or required.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement the extended system information queries.
 */
NTSTATUS
NTAPI
ZwQuerySystemInformationEx(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength)
{
    return NtQuerySystemInformationEx(SystemInformationClass,
                                      InputBuffer,
                                      InputBufferLength,
                                      SystemInformation,
                                      SystemInformationLength,
                                      ReturnLength);
}
