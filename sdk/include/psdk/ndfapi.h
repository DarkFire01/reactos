/*
 * PROJECT:     ReactOS PSDK Headers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Network Diagnostics Framework API
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _NDFAPI_H_
#define _NDFAPI_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID NDFHANDLE;

HRESULT
WINAPI
NdfCreateWebIncident(
    _In_ LPCWSTR url,
    _Out_ NDFHANDLE *handle);

HRESULT
WINAPI
NdfExecuteDiagnosis(
    _In_ NDFHANDLE handle,
    _In_opt_ HWND hwnd);

HRESULT
WINAPI
NdfCloseIncident(
    _Inout_ NDFHANDLE handle);

#ifdef __cplusplus
}
#endif

#endif /* _NDFAPI_H_ */
