/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet extension query
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_EXTENSION_QUERY
{
    ULONG Size;
    PCWSTR Name;
    ULONG Version;
    NET_EXTENSION_TYPE Type;
} NET_EXTENSION_QUERY;

FORCEINLINE
VOID
NET_EXTENSION_QUERY_INIT(
    _Out_ NET_EXTENSION_QUERY *Extension,
    _In_ PCWSTR Name,
    _In_ ULONG Version,
    _In_ NET_EXTENSION_TYPE Type)
{
    RtlZeroMemory(Extension, sizeof(*Extension));

    Extension->Size = sizeof(*Extension);
    Extension->Name = Name;
    Extension->Version = Version;
    Extension->Type = Type;
}

#ifdef __cplusplus
}
#endif
