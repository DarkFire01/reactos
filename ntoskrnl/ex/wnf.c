/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Notification Facility kernel interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * ReactOS has no notification facility yet. Publishing is accepted and
 * dropped, since nothing could subscribe to the data, and subscribing fails
 * so callers know no callback will ever arrive.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Publishes new data for a notification state.
 *
 * @param[in] StateName
 * The state to publish to.
 *
 * @param[in] Buffer
 * The new state data.
 *
 * @param[in] Length
 * The size of @p Buffer, in bytes.
 *
 * @param[in] TypeId
 * Optional GUID describing the data.
 *
 * @param[in] ExplicitScope
 * Optional scope, such as a session ID or a process handle.
 *
 * @param[in] MatchingChangeStamp
 * The change stamp the state must have when @p CheckStamp is set.
 *
 * @param[in] CheckStamp
 * Whether @p MatchingChangeStamp is checked.
 *
 * @return
 * STATUS_SUCCESS. There are no subscribers, so the data is dropped.
 */
NTSTATUS
NTAPI
ZwUpdateWnfStateData(
    _In_ const VOID *StateName,
    _In_reads_bytes_opt_(Length) const VOID *Buffer,
    _In_opt_ ULONG Length,
    _In_opt_ LPCGUID TypeId,
    _In_opt_ const VOID *ExplicitScope,
    _In_ ULONG MatchingChangeStamp,
    _In_ LOGICAL CheckStamp)
{
    UNREFERENCED_PARAMETER(StateName);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(TypeId);
    UNREFERENCED_PARAMETER(ExplicitScope);
    UNREFERENCED_PARAMETER(MatchingChangeStamp);
    UNREFERENCED_PARAMETER(CheckStamp);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Registers a callback for changes of a notification state.
 *
 * @param[out] Subscription
 * Receives the subscription. It is set to NULL.
 *
 * @param[in] StateName
 * The state to watch.
 *
 * @param[in] DeliveryOption
 * Which changes to deliver.
 *
 * @param[in] OriginalChangeStamp
 * The change stamp the caller has already seen.
 *
 * @param[in] Callback
 * The routine to call on a change.
 *
 * @param[in] CallbackContext
 * Passed through to @p Callback.
 *
 * @return
 * STATUS_NOT_SUPPORTED.
 */
NTSTATUS
NTAPI
ExSubscribeWnfStateChange(
    _Out_ PVOID *Subscription,
    _In_ const VOID *StateName,
    _In_ ULONG DeliveryOption,
    _In_ ULONG OriginalChangeStamp,
    _In_ PVOID Callback,
    _In_opt_ PVOID CallbackContext)
{
    UNREFERENCED_PARAMETER(StateName);
    UNREFERENCED_PARAMETER(DeliveryOption);
    UNREFERENCED_PARAMETER(OriginalChangeStamp);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(CallbackContext);

    *Subscription = NULL;
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief
 * Reads the current data of the state a subscription watches.
 *
 * @param[in] Subscription
 * The subscription made with ExSubscribeWnfStateChange.
 *
 * @param[out] ChangeStamp
 * Receives the change stamp of the data.
 *
 * @param[out] Buffer
 * Receives the data.
 *
 * @param[in,out] BufferSize
 * The size of @p Buffer on input, the size of the data on output.
 *
 * @return
 * STATUS_OBJECT_NAME_NOT_FOUND, since no subscription can exist.
 */
NTSTATUS
NTAPI
ExQueryWnfStateData(
    _In_ PVOID Subscription,
    _Out_ PULONG ChangeStamp,
    _Out_writes_bytes_to_opt_(*BufferSize, *BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    UNREFERENCED_PARAMETER(Subscription);
    UNREFERENCED_PARAMETER(ChangeStamp);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferSize);

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/**
 * @brief
 * Removes a subscription made with ExSubscribeWnfStateChange.
 *
 * @param[in] Subscription
 * The subscription to remove.
 */
VOID
NTAPI
ExUnsubscribeWnfStateChange(
    _In_opt_ PVOID Subscription)
{
    UNREFERENCED_PARAMETER(Subscription);
}

/* EOF */
