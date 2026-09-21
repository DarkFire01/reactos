/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Queue performance counter set
 *
 * The Windows 11 24H2 class extension no longer publishes these counters, so
 * the set is never registered and its callback never runs. The calls stay so
 * the translator sources do not change.
 */

#pragma once

#include <pcwdata.h>

struct NETADAPTER_QUEUE_PC;

inline
NTSTATUS
RegisterNetAdapterCxQueueCounterSet(
    _In_ PPCW_CALLBACK Callback,
    _In_opt_ PVOID CallbackContext)
{
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(CallbackContext);

    return STATUS_SUCCESS;
}

inline
VOID
UnregisterNetAdapterCxQueueCounterSet(
    VOID)
{
}

inline
NTSTATUS
AddNetAdapterCxQueueCounterSet(
    _In_ PPCW_BUFFER Buffer,
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Id,
    _In_ NETADAPTER_QUEUE_PC const *Values)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(Id);
    UNREFERENCED_PARAMETER(Values);

    return STATUS_SUCCESS;
}
