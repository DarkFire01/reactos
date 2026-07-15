/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Etw functions of Vista+
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include <ntdef.h>
#include <ntifs.h>

/**
 * @brief
 * Determines whether an event described by @p EventDescriptor is enabled for
 * the provider identified by @p RegHandle.
 *
 * @param[in] RegHandle
 * The provider registration handle returned by EtwRegister().
 *
 * @param[in] EventDescriptor
 * Describes the event whose enabled state is queried.
 *
 * @return
 * TRUE if the event is enabled and should be written, FALSE otherwise.
 *
 * @remarks
 * ReactOS does not implement an ETW tracing back-end, so no events are ever
 * reported as enabled. This lets providers cheaply skip event generation.
 */
_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTKRNLVISTAAPI
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);

    return FALSE;
}

/**
 * @brief
 * Writes an ETW event to the sessions that have enabled the provider.
 *
 * @param[in] RegHandle
 * The provider registration handle returned by EtwRegister().
 *
 * @param[in] EventDescriptor
 * Describes the event to write.
 *
 * @param[in] ActivityId
 * Optional activity identifier to associate with the event.
 *
 * @param[in] UserDataCount
 * The number of EVENT_DATA_DESCRIPTOR entries in @p UserData.
 *
 * @param[in] UserData
 * Optional array of event payload descriptors.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement an ETW tracing back-end.
 */
_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Registers an ETW event provider.
 *
 * @param[in] ProviderId
 * The GUID identifying the provider.
 *
 * @param[in] EnableCallback
 * Optional callback invoked when the provider is enabled or disabled.
 *
 * @param[in] CallbackContext
 * Optional context passed to @p EnableCallback.
 *
 * @param[out] RegHandle
 * Receives the provider registration handle.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement an ETW tracing back-end.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Deregisters an ETW event provider.
 *
 * @param[in] RegHandle
 * The provider registration handle returned by EtwRegister().
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement an ETW tracing back-end.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    return STATUS_NOT_IMPLEMENTED;
}
