/*
 * PROJECT:     ReactOS Event Trace Data Helper
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Event schema and property queries
 * COPYRIGHT:   Copyright 2026 Justin Miller (justin.miller@reactos.org)
 */

#include <windef.h>
#include <winbase.h>
#include <tdh.h>

/*
 * No provider manifests or MOF classes are registered for decoding, so every
 * query that passes validation ends with the "schema not found" result.
 */

static
BOOLEAN
TdhpValidateContext(
    _In_ ULONG ContextCount,
    _In_reads_opt_(ContextCount) PTDH_CONTEXT Context)
{
    ULONG i;

    if (ContextCount && !Context)
        return FALSE;

    for (i = 0; i < ContextCount; i++)
    {
        if (Context[i].ParameterType >= TDH_CONTEXT_MAXIMUM)
            return FALSE;

        if (Context[i].ParameterType == TDH_CONTEXT_POINTERSIZE &&
            Context[i].ParameterValue != 4 &&
            Context[i].ParameterValue != 8)
        {
            return FALSE;
        }
    }

    return TRUE;
}

static
BOOLEAN
TdhpValidateDescriptors(
    _In_ ULONG Count,
    _In_reads_opt_(Count) PPROPERTY_DATA_DESCRIPTOR Descriptors)
{
    ULONG i;

    if (!Count || !Descriptors)
        return FALSE;

    for (i = 0; i < Count; i++)
    {
        if (!Descriptors[i].PropertyName)
            return FALSE;
    }

    return TRUE;
}

/* A sized buffer needs a pointer and a pointer needs a size */
static
BOOLEAN
TdhpValidateOptionalBuffer(
    _In_opt_ PVOID Buffer,
    _In_ ULONG BufferSize)
{
    return (Buffer != NULL) == (BufferSize != 0);
}

TDHSTATUS
WINAPI
TdhGetEventInformation(
    _In_ PEVENT_RECORD Event,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT TdhContext,
    _Out_writes_bytes_opt_(*BufferSize) PTRACE_EVENT_INFO Buffer,
    _Inout_ PULONG BufferSize)
{
    if (!Event || !BufferSize)
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateOptionalBuffer(Buffer, *BufferSize))
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateContext(TdhContextCount, TdhContext))
        return ERROR_INVALID_PARAMETER;

    return ERROR_NOT_FOUND;
}

TDHSTATUS
WINAPI
TdhGetEventMapInformation(
    _In_ PEVENT_RECORD pEvent,
    _In_ PWSTR pMapName,
    _Out_writes_bytes_opt_(*pBufferSize) PEVENT_MAP_INFO pBuffer,
    _Inout_ ULONG *pBufferSize)
{
    if (!pEvent || !pMapName || !pBufferSize)
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateOptionalBuffer(pBuffer, *pBufferSize))
        return ERROR_INVALID_PARAMETER;

    return ERROR_NOT_FOUND;
}

TDHSTATUS
WINAPI
TdhGetPropertySize(
    _In_ PEVENT_RECORD pEvent,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT pTdhContext,
    _In_ ULONG PropertyDataCount,
    _In_reads_(PropertyDataCount) PPROPERTY_DATA_DESCRIPTOR pPropertyData,
    _Out_ ULONG *pPropertySize)
{
    if (!pEvent || !pPropertySize)
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateDescriptors(PropertyDataCount, pPropertyData))
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateContext(TdhContextCount, pTdhContext))
        return ERROR_INVALID_PARAMETER;

    return ERROR_NOT_FOUND;
}

TDHSTATUS
WINAPI
TdhGetProperty(
    _In_ PEVENT_RECORD pEvent,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT pTdhContext,
    _In_ ULONG PropertyDataCount,
    _In_reads_(PropertyDataCount) PPROPERTY_DATA_DESCRIPTOR pPropertyData,
    _In_ ULONG BufferSize,
    _Out_writes_bytes_(BufferSize) PBYTE pBuffer)
{
    if (!pEvent || !pBuffer || !BufferSize)
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateDescriptors(PropertyDataCount, pPropertyData))
        return ERROR_INVALID_PARAMETER;

    if (!TdhpValidateContext(TdhContextCount, pTdhContext))
        return ERROR_INVALID_PARAMETER;

    return ERROR_NOT_FOUND;
}
