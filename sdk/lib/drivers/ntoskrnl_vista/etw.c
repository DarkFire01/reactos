/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Etw functions of Vista+
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 Justin Miller (justin.miller@reactos.org)
 */

#include <ntdef.h>
#include <ntifs.h>

/**
 * @brief
 * Tells whether an event is enabled for a provider.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @param[in] EventDescriptor
 * Describes the event being asked about.
 *
 * @return
 * FALSE. ReactOS has no tracing back end, so providers can skip building the
 * event altogether.
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
 * Writes an event to the sessions that enabled the provider.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @param[in] EventDescriptor
 * Describes the event to write.
 *
 * @param[in] ActivityId
 * Optional activity to tie the event to.
 *
 * @param[in] UserDataCount
 * Number of entries in @p UserData.
 *
 * @param[in] UserData
 * Optional payload descriptors.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
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
 * Registers an event provider.
 *
 * @param[in] ProviderId
 * The GUID of the provider.
 *
 * @param[in] EnableCallback
 * Optional callback run when the provider is enabled or disabled.
 *
 * @param[in] CallbackContext
 * Optional context handed to @p EnableCallback.
 *
 * @param[out] RegHandle
 * Receives the registration handle.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
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
 * Drops an event provider registration.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
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
