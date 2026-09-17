/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Extended system information queries of Windows 8+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Retrieves system information for the classes that take a per-query input
 * buffer.
 *
 * @param[in] SystemInformationClass
 * The class of information to retrieve.
 *
 * @param[in] InputBuffer
 * The class specific input buffer.
 *
 * @param[in] InputBufferLength
 * Size of @p InputBuffer, in bytes.
 *
 * @param[out] SystemInformation
 * Receives the requested information.
 *
 * @param[in] SystemInformationLength
 * Size of @p SystemInformation, in bytes.
 *
 * @param[out] ReturnLength
 * Optionally receives the number of bytes written or required.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
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
 * Kernel mode entry point of NtQuerySystemInformationEx().
 *
 * @param[in] SystemInformationClass
 * The class of information to retrieve.
 *
 * @param[in] InputBuffer
 * The class specific input buffer.
 *
 * @param[in] InputBufferLength
 * Size of @p InputBuffer, in bytes.
 *
 * @param[out] SystemInformation
 * Receives the requested information.
 *
 * @param[in] SystemInformationLength
 * Size of @p SystemInformation, in bytes.
 *
 * @param[out] ReturnLength
 * Optionally receives the number of bytes written or required.
 *
 * @return
 * The status returned by NtQuerySystemInformationEx().
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

/* EOF */
