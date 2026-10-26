/*
 * PROJECT:     ReactOS Shell Account Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Precompiled header and private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#define NTOS_MODE_USER

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propsys.h>
#include <userenv.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ndk/rtlfuncs.h>
#include <ntsecapi.h>
#include <ntsam.h>
#include <atlbase.h>
#include <atlcom.h>
#include <strsafe.h>

#include "resource.h"

EXTERN_C const CLSID CLSID_LocalUserAccounts;
EXTERN_C const IID IID_ILocalUserAccounts;

EXTERN_C const PROPERTYKEY PKEY_SAM_Name;
EXTERN_C const PROPERTYKEY PKEY_SAM_FullName;
EXTERN_C const PROPERTYKEY PKEY_SAM_AdminComment;
EXTERN_C const PROPERTYKEY PKEY_SAM_UserComment;
EXTERN_C const PROPERTYKEY PKEY_SAM_SecurityID;
EXTERN_C const PROPERTYKEY PKEY_SAM_UserAccountControl;
EXTERN_C const PROPERTYKEY PKEY_SAM_UserPicture;
EXTERN_C const PROPERTYKEY PKEY_SAM_Type;
EXTERN_C const PROPERTYKEY PKEY_SAM_ResidualID;

/* Value of PKEY_SAM_Type for user accounts */
#define SAM_ACCOUNT_TYPE_USER   1

/* Local user accounts of the account domain, then of the builtin domain */
MIDL_INTERFACE("3C708557-C99D-4FA3-9231-56518418B4E4")
ILocalUserAccounts : public IUnknown
{
    STDMETHOD(GetEnumerator)(
        _COM_Outptr_ IUnknown **ppunkEnum) PURE;
    STDMETHOD(GetCount)(
        _Out_ UINT *pcAccounts) PURE;
    STDMETHOD(GetAt)(
        _In_ UINT iAccount,
        _COM_Outptr_ IPropertyStore **ppps) PURE;
    STDMETHOD(Reset)() PURE;
    STDMETHOD(Create)(
        _In_ PCWSTR pszUserName,
        _COM_Outptr_ IPropertyStore **ppps) PURE;
    STDMETHOD(Delete)(
        _In_ IPropertyStore *pps) PURE;
    STDMETHOD(FindByName)(
        _In_ PCWSTR pszUserName,
        _COM_Outptr_ IPropertyStore **ppps) PURE;
    STDMETHOD(FindByRID)(
        _In_ ULONG ulRid,
        _COM_Outptr_ IPropertyStore **ppps) PURE;
    STDMETHOD(Advise)(
        _In_ IUnknown *punkNotify,
        _Out_ DWORD *pdwCookie) PURE;
    STDMETHOD(Unadvise)(
        _In_ DWORD dwCookie) PURE;
};

/* samutil.cpp */
HRESULT
WINAPI
SamHResultFromStatus(
    _In_ NTSTATUS Status);

HRESULT
WINAPI
CopySidToTaskMem(
    _In_ PSID psid,
    _Outptr_ PSID *ppsidCopy);

HRESULT
WINAPI
GetAccountDomainSid(
    _Outptr_ PSID *ppsidDomain);

HRESULT
WINAPI
GetBuiltinDomainSid(
    _Outptr_ PSID *ppsidDomain);

HRESULT
WINAPI
OpenSamDomain(
    _In_ PSID psidDomain,
    _Out_ SAM_HANDLE *phServer,
    _Out_ SAM_HANDLE *phDomain);

VOID
WINAPI
CloseSamDomain(
    _In_opt_ SAM_HANDLE hServer,
    _In_opt_ SAM_HANDLE hDomain);

HRESULT
WINAPI
LookupUserRid(
    _In_ PSID psidDomain,
    _In_ PCWSTR pszUserName,
    _Out_ ULONG *pulRid);

HRESULT
WINAPI
EnumerateDomainUsers(
    _In_ PSID psidDomain,
    _Outptr_result_buffer_maybenull_(*pcUsers) PSAM_RID_ENUMERATION *pprgUsers,
    _Out_ ULONG *pcUsers);

HRESULT
WINAPI
GetUserPicturesDirectory(
    _Out_writes_(cchDir) PWSTR pszDir,
    _In_ UINT cchDir);

/* sampropstore.cpp */
HRESULT
WINAPI
CreateSamUserPropertyStore(
    _In_ PSID psidDomain,
    _In_ ULONG ulRid,
    _COM_Outptr_ IPropertyStore **ppps);

/* localaccounts.cpp */
class CLocalUserAccounts :
    public CComCoClass<CLocalUserAccounts, &CLSID_LocalUserAccounts>,
    public CComObjectRootEx<CComMultiThreadModel>,
    public ILocalUserAccounts
{
public:
    CLocalUserAccounts();
    ~CLocalUserAccounts();

    /* ILocalUserAccounts */
    STDMETHODIMP GetEnumerator(
        _COM_Outptr_ IUnknown **ppunkEnum) override;
    STDMETHODIMP GetCount(
        _Out_ UINT *pcAccounts) override;
    STDMETHODIMP GetAt(
        _In_ UINT iAccount,
        _COM_Outptr_ IPropertyStore **ppps) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Create(
        _In_ PCWSTR pszUserName,
        _COM_Outptr_ IPropertyStore **ppps) override;
    STDMETHODIMP Delete(
        _In_ IPropertyStore *pps) override;
    STDMETHODIMP FindByName(
        _In_ PCWSTR pszUserName,
        _COM_Outptr_ IPropertyStore **ppps) override;
    STDMETHODIMP FindByRID(
        _In_ ULONG ulRid,
        _COM_Outptr_ IPropertyStore **ppps) override;
    STDMETHODIMP Advise(
        _In_ IUnknown *punkNotify,
        _Out_ DWORD *pdwCookie) override;
    STDMETHODIMP Unadvise(
        _In_ DWORD dwCookie) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_LOCALUSERACCOUNTS)
    DECLARE_NOT_AGGREGATABLE(CLocalUserAccounts)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CLocalUserAccounts)
        COM_INTERFACE_ENTRY_IID(IID_ILocalUserAccounts, ILocalUserAccounts)
    END_COM_MAP()

private:
    HRESULT _EnsureEnumeration();

    PSAM_RID_ENUMERATION _prgBuiltinUsers;
    ULONG _cBuiltinUsers;
    PSAM_RID_ENUMERATION _prgAccountUsers;
    ULONG _cAccountUsers;
};
