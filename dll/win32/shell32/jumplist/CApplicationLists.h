/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IApplicationDestinations and IApplicationDocumentLists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "jumplist.h"

/* Both objects work on the automatic list of an application, by default the calling one */
template <class TInterface>
class CApplicationListsBase :
    public CPinZeroAlloc,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public TInterface
{
public:
    CApplicationListsBase() :
        m_pszAppID(NULL),
        m_pszAppPath(NULL)
    {
    }

    virtual ~CApplicationListsBase()
    {
        CoTaskMemFree(m_pszAppPath);
        CoTaskMemFree(m_pszAppID);
    }

    STDMETHODIMP
    SetAppID(
        _In_ PCWSTR pszAppID) override
    {
        CoTaskMemFree(m_pszAppID);
        m_pszAppID = NULL;
        return SHStrDupW(pszAppID, &m_pszAppID);
    }

protected:
    HRESULT
    _EnsureAppID(VOID);

    HRESULT
    _CreateAutoDestList(
        _Outptr_ IAutomaticDestinationList **ppadl);

    PWSTR m_pszAppID;
    PWSTR m_pszAppPath;
};

class CApplicationDestinations :
    public CComCoClass<CApplicationDestinations, &CLSID_ApplicationDestinations>,
    public CApplicationListsBase<IApplicationDestinations>
{
public:
    // *** IApplicationDestinations methods ***
    STDMETHODIMP RemoveDestination(IUnknown *punk) override;
    STDMETHODIMP RemoveAllDestinations() override;

    DECLARE_REGISTRY_RESOURCEID(IDR_APPLICATIONDESTINATIONS)
    DECLARE_NOT_AGGREGATABLE(CApplicationDestinations)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CApplicationDestinations)
        COM_INTERFACE_ENTRY_IID(IID_IApplicationDestinations, IApplicationDestinations)
    END_COM_MAP()
};

class CApplicationDocumentLists :
    public CComCoClass<CApplicationDocumentLists, &CLSID_ApplicationDocumentLists>,
    public CApplicationListsBase<IApplicationDocumentLists>
{
public:
    // *** IApplicationDocumentLists methods ***
    STDMETHODIMP GetList(APPDOCLISTTYPE listtype, UINT cItemsDesired, REFIID riid, void **ppv) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_APPLICATIONDOCUMENTLISTS)
    DECLARE_NOT_AGGREGATABLE(CApplicationDocumentLists)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CApplicationDocumentLists)
        COM_INTERFACE_ENTRY_IID(IID_IApplicationDocumentLists, IApplicationDocumentLists)
    END_COM_MAP()
};
