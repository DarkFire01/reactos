/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IApplicationDestinations and IApplicationDocumentLists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "CApplicationLists.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* Without SetAppID the lists are those of the calling process */
template <class TInterface>
HRESULT
CApplicationListsBase<TInterface>::_EnsureAppID(VOID)
{
    CComPtr<IApplicationResolver> par;
    WCHAR szExe[MAX_PATH];
    HRESULT hr;

    if (m_pszAppID)
        return S_OK;

    hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IApplicationResolver, &par));
    if (SUCCEEDED(hr))
        hr = par->GetAppIDForProcess(GetCurrentProcessId(), &m_pszAppID, NULL, NULL, NULL);
    if (FAILED(hr))
        return hr;

    if (!GetModuleFileNameW(NULL, szExe, _countof(szExe)) || !szExe[0])
        return HRESULT_FROM_WIN32(GetLastError());
    return SHStrDupW(szExe, &m_pszAppPath);
}

template <class TInterface>
HRESULT
CApplicationListsBase<TInterface>::_CreateAutoDestList(
    _Outptr_ IAutomaticDestinationList **ppadl)
{
    CComPtr<IAutomaticDestinationList> padl;
    HRESULT hr;

    *ppadl = NULL;
    hr = CoCreateInstance(CLSID_AutomaticDestinationList, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IAutomaticDestinationList, &padl));
    if (SUCCEEDED(hr))
        hr = padl->Initialize(m_pszAppID, m_pszAppPath, NULL);
    if (SUCCEEDED(hr))
        *ppadl = padl.Detach();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CApplicationDestinations::RemoveDestination(
    _In_ IUnknown *punk)
{
    CComPtr<IAutomaticDestinationList> padl;
    HRESULT hr;

    hr = _EnsureAppID();
    if (SUCCEEDED(hr))
        hr = _CreateAutoDestList(&padl);
    if (SUCCEEDED(hr))
        hr = padl->RemoveDestination(punk);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CApplicationDestinations::RemoveAllDestinations()
{
    CComPtr<IAutomaticDestinationList> padl;
    HRESULT hr;

    hr = _EnsureAppID();
    if (SUCCEEDED(hr))
        hr = _CreateAutoDestList(&padl);
    if (SUCCEEDED(hr))
        hr = padl->ClearList(FALSE);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CApplicationDocumentLists::GetList(
    _In_ APPDOCLISTTYPE listtype,
    _In_ UINT cItemsDesired,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IAutomaticDestinationList> padl;
    DESTLISTTYPE type;
    HRESULT hr;

    *ppv = NULL;
    hr = _EnsureAppID();
    if (SUCCEEDED(hr))
        hr = _CreateAutoDestList(&padl);
    if (FAILED(hr))
        return hr;

    if (listtype == ADLT_RECENT)
        type = DLT_RECENT;
    else if (listtype == ADLT_FREQUENT)
        type = DLT_FREQUENT;
    else
        return E_INVALIDARG;

    return padl->GetList(type, cItemsDesired, riid, ppv);
}
