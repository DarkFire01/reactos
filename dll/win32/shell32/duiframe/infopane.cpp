/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Info pane module: title and link text supplied by the folder in view
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* UIFILE holding the "InfoPane" markup */
#define IDR_UIFILE_INFOPANE     33

#define IDS_INFOPANE_NAME       31123

DirectUI::IClassInfo *CInfoPaneInner::Class = NULL;

static
BOOL
ElementHasChildren(
    _In_ DirectUI::Element *pe)
{
    DirectUI::Value *pv = NULL;
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pChildren = pe->GetChildren(&pv);
    BOOL fChildren = pChildren && pChildren->GetSize();

    if (pv)
        pv->Release();
    return fChildren;
}

/* The folder's IInfoPaneProvider, reached through the view */
static
HRESULT
GetInfoPaneProvider(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_ IInfoPaneProvider **ppipp)
{
    IFolderView *pfv;
    HRESULT hr;

    *ppipp = NULL;
    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        hr = pfv->GetFolder(IID_PPV_ARGS(ppipp));
        pfv->Release();
    }
    return hr;
}

HRESULT
WINAPI
CInfoPaneInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Layout *pLayout = NULL;
    CInfoPaneInner *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CInfoPaneInner>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(1);
    if (SUCCEEDED(hr))
        hr = DirectUI::ShellBorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        hr = pe->SetLayout(pLayout);

    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CInfoPaneInner::Register()
{
    return DirectUI::ClassInfo<CInfoPaneInner, CElementWithSite, DirectUI::StandardCreator<CInfoPaneInner>>::Register(
        DUIFRAME_HINST, L"InfoPaneInner", NULL, 0, false);
}

STDMETHODIMP
CInfoPaneInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CInfoPaneInner, IFrameNotificationClient),
        QITABENT(CInfoPaneInner, IFrameShellViewClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

STDMETHODIMP
CInfoPaneInner::LayoutInitialized()
{
    WCHAR szName[32];

    LoadStringW(DUIFRAME_HINST, IDS_INFOPANE_NAME, szName, _countof(szName));
    SetAccessible(true);
    SetAccRole(ROLE_SYSTEM_PANE);
    SetAccName(szName);
    return _SetText();
}

STDMETHODIMP
CInfoPaneInner::OnContentsChanged()
{
    return _SetText();
}

/* The pane collapses unless the folder has a title or a description to show */
HRESULT
CInfoPaneInner::_SetText()
{
    IInfoPaneProvider *pipp;
    DirectUI::Element *peTitle;
    DirectUI::Element *peDescription;
    DirectUI::Element *peContainer;
    DirectUI::Element *peCreated;
    PWSTR pszTitle = NULL;
    PWSTR pszDescription = NULL;
    BOOL fShow = FALSE;

    if (SUCCEEDED(GetInfoPaneProvider(_punkSite, &pipp)))
    {
        pipp->GetTitle(&pszTitle);
        pipp->GetDescription(&pszDescription);
        pipp->Release();
    }

    if ((pszTitle || pszDescription) &&
        (ElementHasChildren(this) ||
         SUCCEEDED(DUI_CreateElementFromResource(DUIFRAME_HINST, IDR_UIFILE_INFOPANE, L"InfoPane", this, NULL,
                                                 NULL, &peCreated, NULL))))
    {
        peTitle = FindDescendent(StrToID(L"InfoPaneTitle"));
        if (peTitle)
        {
            if (pszTitle)
            {
                peTitle->SetContentString(pszTitle);
                peTitle->SetAccessible(true);
                peTitle->SetAccRole(ROLE_SYSTEM_STATICTEXT);
                peTitle->RemoveLocalValue(DirectUI::Element::HeightProp);
                fShow = TRUE;
            }
            else
            {
                peTitle->SetHeight(0);
            }
        }

        peDescription = FindDescendent(StrToID(L"InfoPaneDescription"));
        peContainer = FindDescendent(StrToID(L"InfoPaneDescriptionContainer"));
        if (peDescription)
        {
            if (pszDescription)
            {
                peDescription->SetContentString(pszDescription);
                peDescription->SetAccessible(true);
                peDescription->SetAccRole(ROLE_SYSTEM_STATICTEXT);
                if (peContainer)
                    peContainer->RemoveLocalValue(DirectUI::Element::HeightProp);
                ((DirectUI::CCBase *)peDescription)->SetNotifyHandler(s_LinkNotifyHandler, this);
                fShow = TRUE;
            }
            else if (peContainer)
            {
                peContainer->SetHeight(0);
            }
        }
    }

    if (fShow)
    {
        RemoveLocalValue(DirectUI::Element::HeightProp);
        RemoveLocalValue(DirectUI::Element::LayoutPosProp);
    }
    else
    {
        SetHeight(0);
        SetLayoutPos(ROSUI_LP_NONE);
    }

    CoTaskMemFree(pszTitle);
    CoTaskMemFree(pszDescription);
    return S_OK;
}

/* A click or Enter on the description link goes to the folder */
BOOL
WINAPI
CInfoPaneInner::s_LinkNotifyHandler(
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _Out_ LRESULT *plResult,
    _In_opt_ PVOID pvData)
{
    CInfoPaneInner *pThis = (CInfoPaneInner *)pvData;
    const NMHDR *pnmh = (const NMHDR *)lParam;
    IInfoPaneProvider *pipp;

    UNREFERENCED_PARAMETER(wParam);
    *plResult = 0;
    if (uMsg != WM_NOTIFY || (pnmh->code != NM_CLICK && pnmh->code != NM_RETURN))
        return FALSE;

    if (SUCCEEDED(GetInfoPaneProvider(pThis->_punkSite, &pipp)))
    {
        pipp->LinkClicked();
        pipp->Release();
    }
    return TRUE;
}
