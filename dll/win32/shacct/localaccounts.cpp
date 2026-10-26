/*
 * PROJECT:     ReactOS Shell Account Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Local user account collection (CLSID_LocalUserAccounts)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

typedef HRESULT (WINAPI *PFN_GET_DOMAIN_SID)(_Outptr_ PSID *ppsidDomain);

/* The account domain is searched first, then the builtin domain */
static const PFN_GET_DOMAIN_SID s_rgpfnDomains[] =
{
    GetAccountDomainSid,
    GetBuiltinDomainSid,
};

CLocalUserAccounts::CLocalUserAccounts()
    : _prgBuiltinUsers(NULL),
      _cBuiltinUsers(0),
      _prgAccountUsers(NULL),
      _cAccountUsers(0)
{
}

CLocalUserAccounts::~CLocalUserAccounts()
{
    Reset();
}

HRESULT
CLocalUserAccounts::_EnsureEnumeration()
{
    PSID psidDomain;
    HRESULT hr = S_OK;

    if (!_prgAccountUsers)
    {
        hr = GetAccountDomainSid(&psidDomain);
        if (SUCCEEDED(hr))
        {
            hr = EnumerateDomainUsers(psidDomain, &_prgAccountUsers, &_cAccountUsers);
            CoTaskMemFree(psidDomain);
        }
    }

    if (!_prgBuiltinUsers)
    {
        hr = GetBuiltinDomainSid(&psidDomain);
        if (FAILED(hr))
            return hr;

        hr = EnumerateDomainUsers(psidDomain, &_prgBuiltinUsers, &_cBuiltinUsers);
        CoTaskMemFree(psidDomain);
    }

    return hr;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::GetEnumerator(
    _COM_Outptr_ IUnknown **ppunkEnum)
{
    if (!ppunkEnum)
        return E_POINTER;

    *ppunkEnum = NULL;
    return E_NOTIMPL;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::GetCount(
    _Out_ UINT *pcAccounts)
{
    HRESULT hr;

    if (!pcAccounts)
        return E_POINTER;

    Lock();
    hr = _EnsureEnumeration();
    if (SUCCEEDED(hr))
        *pcAccounts = _cBuiltinUsers + _cAccountUsers;
    Unlock();

    return hr;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::GetAt(
    _In_ UINT iAccount,
    _COM_Outptr_ IPropertyStore **ppps)
{
    PSID psidDomain = NULL;
    ULONG ulRid = 0;
    HRESULT hr;

    if (!ppps)
        return E_POINTER;

    *ppps = NULL;

    Lock();
    hr = _EnsureEnumeration();
    if (SUCCEEDED(hr))
    {
        if (iAccount >= _cAccountUsers + _cBuiltinUsers)
        {
            hr = E_INVALIDARG;
        }
        else if (iAccount < _cAccountUsers)
        {
            ulRid = _prgAccountUsers[iAccount].RelativeId;
            hr = GetAccountDomainSid(&psidDomain);
        }
        else
        {
            ulRid = _prgBuiltinUsers[iAccount - _cAccountUsers].RelativeId;
            hr = GetBuiltinDomainSid(&psidDomain);
        }
    }
    Unlock();

    if (SUCCEEDED(hr))
    {
        hr = CreateSamUserPropertyStore(psidDomain, ulRid, ppps);
        CoTaskMemFree(psidDomain);
    }

    return hr;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::Reset()
{
    Lock();

    if (_prgAccountUsers)
    {
        SamFreeMemory(_prgAccountUsers);
        _prgAccountUsers = NULL;
    }
    _cAccountUsers = 0;

    if (_prgBuiltinUsers)
    {
        SamFreeMemory(_prgBuiltinUsers);
        _prgBuiltinUsers = NULL;
    }
    _cBuiltinUsers = 0;

    Unlock();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::Create(
    _In_ PCWSTR pszUserName,
    _COM_Outptr_ IPropertyStore **ppps)
{
    UNREFERENCED_PARAMETER(pszUserName);

    if (!ppps)
        return E_POINTER;

    *ppps = NULL;
    return E_NOTIMPL;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::Delete(
    _In_ IPropertyStore *pps)
{
    UNREFERENCED_PARAMETER(pps);
    return E_NOTIMPL;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::FindByName(
    _In_ PCWSTR pszUserName,
    _COM_Outptr_ IPropertyStore **ppps)
{
    PSID psidDomain;
    ULONG ulRid;
    UINT i;
    HRESULT hr;

    if (!ppps)
        return E_POINTER;

    *ppps = NULL;

    for (i = 0; i < ARRAYSIZE(s_rgpfnDomains); i++)
    {
        if (FAILED(s_rgpfnDomains[i](&psidDomain)))
            continue;

        if (SUCCEEDED(LookupUserRid(psidDomain, pszUserName, &ulRid)))
        {
            hr = CreateSamUserPropertyStore(psidDomain, ulRid, ppps);
            if (SUCCEEDED(hr))
            {
                CoTaskMemFree(psidDomain);
                return hr;
            }
        }

        CoTaskMemFree(psidDomain);
    }

    /* Not a user in either domain */
    return E_INVALIDARG;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::FindByRID(
    _In_ ULONG ulRid,
    _COM_Outptr_ IPropertyStore **ppps)
{
    PSID psidDomain;
    UINT i;
    HRESULT hr = E_INVALIDARG;

    if (!ppps)
        return E_POINTER;

    *ppps = NULL;

    for (i = 0; i < ARRAYSIZE(s_rgpfnDomains); i++)
    {
        hr = s_rgpfnDomains[i](&psidDomain);
        if (FAILED(hr))
            continue;

        hr = CreateSamUserPropertyStore(psidDomain, ulRid, ppps);
        CoTaskMemFree(psidDomain);
        if (SUCCEEDED(hr))
            break;
    }

    return hr;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::Advise(
    _In_ IUnknown *punkNotify,
    _Out_ DWORD *pdwCookie)
{
    UNREFERENCED_PARAMETER(punkNotify);

    if (pdwCookie)
        *pdwCookie = 0;

    return E_NOTIMPL;
}

HRESULT
STDMETHODCALLTYPE
CLocalUserAccounts::Unadvise(
    _In_ DWORD dwCookie)
{
    UNREFERENCED_PARAMETER(dwCookie);
    return E_NOTIMPL;
}
