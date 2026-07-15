/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Whea (Windows Hardware Error Architecture) functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Initializes the common header of a WHEA error record.
 *
 * @param[out] Header
 * The error record header to initialize.
 *
 * @unimplemented
 * ReactOS does not implement the Windows Hardware Error Architecture.
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
 * Reports a hardware error to the WHEA subsystem.
 *
 * @param[in] ErrorPacket
 * The error packet describing the hardware error.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement the Windows Hardware Error Architecture.
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
 * Adds a hardware error source to the WHEA subsystem.
 *
 * @param[in] ErrorSource
 * The error source configuration to add.
 *
 * @param[in] Context
 * Optional context associated with the error source.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement the Windows Hardware Error Architecture.
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
 * Configures a hardware error source.
 *
 * @param[in] SourceType
 * The type of the error source to configure.
 *
 * @param[in] Configuration
 * The error source configuration.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement the Windows Hardware Error Architecture.
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
 * Retrieves a registered hardware error source by identifier.
 *
 * @param[in] ErrorSourceId
 * The identifier of the error source to retrieve.
 *
 * @param[out] ErrorSource
 * Receives the error source descriptor.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement the Windows Hardware Error Architecture.
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
