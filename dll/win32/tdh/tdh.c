/*
 * PROJECT:     ReactOS Trace Data Helper
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Describing the events a provider declares
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A consumer of traces asks this how to read the events a provider writes, out
 * of the manifest that provider registered. Nothing here keeps manifests, so
 * every event is one it knows nothing about.
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/**
 * @brief
 * Describes one event of a provider's manifest.
 *
 * @return
 * ERROR_NOT_FOUND, because no manifest is kept to look in.
 */
ULONG
WINAPI
TdhGetManifestEventInformation(
    _In_ LPGUID ProviderGuid,
    _In_ PVOID EventDescriptor,
    _Out_ PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    UNREFERENCED_PARAMETER(ProviderGuid);
    UNREFERENCED_PARAMETER(EventDescriptor);
    UNREFERENCED_PARAMETER(Buffer);

    if (BufferSize != NULL)
        *BufferSize = 0;

    return ERROR_NOT_FOUND;
}

/* EOF */
