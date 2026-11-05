/*
 * PROJECT:     ReactOS Network Diagnostics Framework API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Incident entry points of the diagnostics client
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Incidents are diagnosed by the Diagnostic Policy Service and its helper classes,
 * none of which ReactOS has. No incident can be created, so no handle names one.
 */

#include <windef.h>
#include <winbase.h>
#include <ndfapi.h>

HRESULT
WINAPI
NdfCreateWebIncident(
    _In_ LPCWSTR url,
    _Out_ NDFHANDLE *handle)
{
    if (!handle)
        return E_INVALIDARG;

    *handle = NULL;

    if (!url)
        return E_INVALIDARG;

    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

HRESULT
WINAPI
NdfExecuteDiagnosis(
    _In_ NDFHANDLE handle,
    _In_opt_ HWND hwnd)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(hwnd);

    return E_INVALIDARG;
}

HRESULT
WINAPI
NdfCloseIncident(
    _Inout_ NDFHANDLE handle)
{
    UNREFERENCED_PARAMETER(handle);

    return E_INVALIDARG;
}
