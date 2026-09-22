/*
 * PROJECT:     ReactOS Event Aggregation library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Aggregate event entry points the graphics kernel imports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * An aggregate event combines WNF state conditions into one trigger. ReactOS
 * has no WNF, so every well formed request is refused and no event is ever
 * handed out. Both entry points answer with Win32 error codes.
 */

#include <ntifs.h>

/* The request an aggregate event is created from */
typedef struct _CEAP_EVENT_REQUEST
{
    ULONG64 TriggerId;
    ULONG Flags;
    ULONG ConditionCount;
    PULONG64 Conditions;
    ULONG Operator;
    PVOID StartCallback;
    PVOID StopCallback;
    PVOID Context;
} CEAP_EVENT_REQUEST, *PCEAP_EVENT_REQUEST;

#define CEAP_VALID_FLAGS            0x0000000F
#define CEAP_MAXIMUM_CONDITIONS     32

static
BOOLEAN
CeapIsRequestValid(
    _In_opt_ PCEAP_EVENT_REQUEST Request)
{
    if (Request == NULL)
        return FALSE;

    if (Request->Flags & ~CEAP_VALID_FLAGS)
        return FALSE;

    if (Request->ConditionCount > CEAP_MAXIMUM_CONDITIONS)
        return FALSE;

    /* Something has to trigger the event, a trigger id or at least one condition */
    if ((Request->TriggerId == 0) && (Request->ConditionCount == 0))
        return FALSE;

    if ((Request->ConditionCount != 0) && (Request->Conditions == NULL))
        return FALSE;

    return (Request->StartCallback != NULL) || (Request->StopCallback != NULL);
}

/**
 * @brief
 * Creates an event that fires when a set of WNF conditions is met.
 *
 * @param[in] Request
 * Describes the trigger, the conditions and the callbacks.
 *
 * @param[out] Handle
 * Receives the event handle. It is left untouched here.
 *
 * @return
 * ERROR_INVALID_PARAMETER for a malformed request, otherwise ERROR_NOT_SUPPORTED.
 */
ULONG
NTAPI
EACreateAggregateEvent(
    _In_ PVOID Request,
    _Out_ PVOID *Handle)
{
    if ((Handle == NULL) || !CeapIsRequestValid(Request))
        return RtlNtStatusToDosError(STATUS_INVALID_PARAMETER);

    return RtlNtStatusToDosError(STATUS_NOT_SUPPORTED);
}

/**
 * @brief
 * Deletes an event created by EACreateAggregateEvent.
 *
 * @param[in] Handle
 * The event handle.
 *
 * @return
 * ERROR_INVALID_PARAMETER for a NULL handle, otherwise ERROR_NOT_FOUND,
 * since no event can exist.
 */
ULONG
NTAPI
EADeleteAggregateEvent(
    _In_opt_ PVOID Handle)
{
    if (Handle == NULL)
        return RtlNtStatusToDosError(STATUS_INVALID_PARAMETER);

    return RtlNtStatusToDosError(STATUS_NOT_FOUND);
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}
