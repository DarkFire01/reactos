/*
 * PROJECT:     ReactOS Authentication UI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Precompiled header and private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <objbase.h>
#include <shlwapi.h>
#include <powrprof.h>
#include <atlbase.h>
#include <atlcom.h>
#include <strsafe.h>

#include "resource.h"

EXTERN_C const CLSID CLSID_ShutdownChoices;
EXTERN_C const IID IID_IShutdownChoices;
EXTERN_C const IID IID_IEnumShutdownChoices;

/* Shut down choices, one bit each; the start menu stores them in Start_PowerButtonAction */
#define SDC_LOGOFF              0x00000001
#define SDC_SHUTDOWN            0x00000002
#define SDC_RESTART             0x00000004
#define SDC_SLEEP               0x00000010
#define SDC_HIBERNATE           0x00000040
#define SDC_DISCONNECT          0x00000080
#define SDC_SWITCHUSER          0x00000100
#define SDC_LOCK                0x00000200
#define SDC_UNDOCK              0x00000400
#define SDC_INSTALLUPDATES      0x00020000
#define SDC_DISABLED            0x00040000
#define SDC_DISABLEDOTHER       0x00080000
#define SDC_SEPARATOR           0x00400000

#define SDC_DISABLEDMASK        (SDC_DISABLED | SDC_DISABLEDOTHER)
#define SDC_DEFAULTMASK         (SDC_SEPARATOR | SDC_UNDOCK | SDC_LOCK | SDC_SWITCHUSER | \
                                 SDC_HIBERNATE | SDC_SLEEP | SDC_RESTART | SDC_SHUTDOWN)

/* Hidden window that tells its parent through WM_NOTIFY when the choices may have changed */
struct DECLSPEC_NOVTABLE IShutdownChoiceListener : public IUnknown
{
    STDMETHOD(Initialize)(
        _In_ HWND hwndParent,
        _In_ DWORD dwFlags) PURE;
    STDMETHOD(GetWindow)(
        _Out_ HWND *phwnd) PURE;
    STDMETHOD(Refresh)() PURE;
    STDMETHOD(StartListening)() PURE;
};

struct DECLSPEC_NOVTABLE IEnumShutdownChoices : public IUnknown
{
    STDMETHOD(Next)(
        _In_ ULONG celt,
        _Out_writes_to_(celt, *pceltFetched) DWORD *rgChoices,
        _Out_opt_ ULONG *pceltFetched) PURE;
    STDMETHOD(Skip)(
        _In_ ULONG celt) PURE;
    STDMETHOD(Reset)() PURE;
    STDMETHOD(Clone)(
        _COM_Outptr_ IEnumShutdownChoices **ppenum) PURE;
};

struct DECLSPEC_NOVTABLE IShutdownChoices : public IUnknown
{
    STDMETHOD(Refresh)() PURE;
    STDMETHOD(CreateListener)(
        _COM_Outptr_ IShutdownChoiceListener **ppListener) PURE;
    STDMETHOD(SetChoiceMask)(
        _In_ DWORD dwChoiceMask) PURE;
    STDMETHOD(GetChoiceMask)(
        _Out_ DWORD *pdwChoiceMask) PURE;
    STDMETHOD(SetShowBadChoicesAsDisabled)(
        _In_ BOOL fShow) PURE;
    STDMETHOD(GetChoiceEnumerator)(
        _COM_Outptr_ IEnumShutdownChoices **ppenum) PURE;
    STDMETHOD(GetDefaultChoice)(
        _Out_ DWORD *pdwChoice) PURE;
    STDMETHOD(UserHasShutdownRights)() PURE;
    STDMETHOD(GetChoiceName)(
        _In_ DWORD dwChoice,
        _In_ DWORD dwFlags,
        _Out_writes_(cchName) PWSTR pszName,
        _In_ UINT cchName) PURE;
    STDMETHOD(GetChoiceDesc)(
        _In_ DWORD dwChoice,
        _Out_writes_(cchDesc) PWSTR pszDesc,
        _In_ UINT cchDesc) PURE;
};

class CShutdownChoices :
    public CComCoClass<CShutdownChoices, &CLSID_ShutdownChoices>,
    public CComObjectRootEx<CComSingleThreadModel>,
    public IShutdownChoices
{
private:
    DWORD m_dwChoiceMask;
    BOOL m_fShowBadChoices;

    DWORD TestChoice(
        _In_ DWORD dwChoice);

public:
    CShutdownChoices();

    // IShutdownChoices
    STDMETHODIMP Refresh() override;
    STDMETHODIMP CreateListener(
        _COM_Outptr_ IShutdownChoiceListener **ppListener) override;
    STDMETHODIMP SetChoiceMask(
        _In_ DWORD dwChoiceMask) override;
    STDMETHODIMP GetChoiceMask(
        _Out_ DWORD *pdwChoiceMask) override;
    STDMETHODIMP SetShowBadChoicesAsDisabled(
        _In_ BOOL fShow) override;
    STDMETHODIMP GetChoiceEnumerator(
        _COM_Outptr_ IEnumShutdownChoices **ppenum) override;
    STDMETHODIMP GetDefaultChoice(
        _Out_ DWORD *pdwChoice) override;
    STDMETHODIMP UserHasShutdownRights() override;
    STDMETHODIMP GetChoiceName(
        _In_ DWORD dwChoice,
        _In_ DWORD dwFlags,
        _Out_writes_(cchName) PWSTR pszName,
        _In_ UINT cchName) override;
    STDMETHODIMP GetChoiceDesc(
        _In_ DWORD dwChoice,
        _Out_writes_(cchDesc) PWSTR pszDesc,
        _In_ UINT cchDesc) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_SHUTDOWNCHOICES)
    DECLARE_NOT_AGGREGATABLE(CShutdownChoices)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CShutdownChoices)
        COM_INTERFACE_ENTRY_IID(IID_IShutdownChoices, IShutdownChoices)
    END_COM_MAP()
};
