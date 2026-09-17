/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Whea (Windows Hardware Error Architecture) functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Fills in the common header of an error record.
 *
 * @param[out] Header
 * The error record header to initialize.
 *
 * @unimplemented
 */
VOID
NTAPI
WheaInitializeRecordHeader(
    _Out_ PVOID Header)
{
    UNREFERENCED_PARAMETER(Header);
}

/**
 * @brief
 * Reports a hardware error.
 *
 * @param[in] ErrorPacket
 * The packet describing the error.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
WheaReportHwError(
    _In_ PVOID ErrorPacket)
{
    UNREFERENCED_PARAMETER(ErrorPacket);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Adds an error source.
 *
 * @param[in] ErrorSource
 * The error source descriptor to add.
 *
 * @param[in] Context
 * Optional context of the error source.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
WheaAddErrorSource(
    _In_ PVOID ErrorSource,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(ErrorSource);
    UNREFERENCED_PARAMETER(Context);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Configures an error source.
 *
 * @param[in] SourceType
 * Type of the error source to configure.
 *
 * @param[in] Configuration
 * The configuration to apply.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
WheaConfigureErrorSource(
    _In_ ULONG SourceType,
    _In_ PVOID Configuration)
{
    UNREFERENCED_PARAMETER(SourceType);
    UNREFERENCED_PARAMETER(Configuration);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Looks an error source up by its identifier.
 *
 * @param[in] ErrorSourceId
 * Identifier of the error source.
 *
 * @param[out] ErrorSource
 * Receives the error source descriptor.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
WheaGetErrorSource(
    _In_ ULONG ErrorSourceId,
    _Out_ PVOID *ErrorSource)
{
    UNREFERENCED_PARAMETER(ErrorSourceId);

    if (ErrorSource != NULL)
        *ErrorSource = NULL;

    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
