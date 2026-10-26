/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Preview pane module: preview handler host and status cover sheets
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Strings of the preview pane */
#define IDS_PREVIEWPANE_NAME        38228
#define IDS_PREVIEW_SELECTFILE      38245
#define IDS_PREVIEW_NOPREVIEW       38246
#define IDS_PREVIEW_FILEINUSE       38247
#define IDS_PREVIEW_PREVIEWERERROR  38248
#define IDS_PREVIEW_CANNOTPREVIEW   38249
#define IDS_PREVIEW_LOCATION        38250
#define IDS_PREVIEW_UNSAFE          50258

/* Cover sheet states set by _UpdateCoverSheets */
#define COVER_NOSELECTION   0
#define COVER_NOPREVIEW     1
#define COVER_UNSAFE        2
#define COVER_ERROR         3
#define COVER_PREVIEWER     4
#define COVER_THUMBNAIL     5

/* Content alignment of the cover sheets: wrapped and centered */
#define COVER_CONTENTALIGN  13

/* Layout position that lets a fill layout child cover its parent */
#define COVER_LAYOUTPOS_SHOWN   10

/* Messages to a preview handler thread */
#define PHM_QUIT            (WM_USER + 1)
#define PHM_LOADFILE        (WM_USER + 2)
#define PHM_SHOWFILE        (WM_USER + 3)
#define PHM_UNLOAD          (WM_USER + 4)
#define PHM_RESIZE          (WM_USER + 5)
#define PHM_SETFOCUS        (WM_USER + 6)
#define PHM_QUERYFOCUS      (WM_USER + 7)
#define PHM_OPLOCKBROKEN    (WM_USER + 8)

/* Answers posted back to the frame thread */
#define PHM_LOADED          (WM_USER + 0x14)
#define PHM_SHOWN           (WM_USER + 0x15)
#define PHM_UNLOADED        (WM_USER + 0x16)
#define PHM_ACCELERATOR     (WM_USER + 0x17)
#define PHM_ERROR           (WM_USER + 0x18)

/* Sent to the host window with a NotifyError when a preview fails */
#define RPHM_PREVIEWERROR   (WM_USER + 0xC8)

/* Preview handler threads kept alive, one per handler class */
#define PREVIEWHOST_CACHE_MAX   4

/* Zone action that guards the preview pane */
#ifndef URLACTION_SHELL_PREVIEW
#define URLACTION_SHELL_PREVIEW 0x0000180F
#endif

static const WCHAR c_szPreviewHandlerRunning[] = L"PreviewHandlerRunning";
static const WCHAR c_szPreviewHandlerRendering[] = L"PreviewHandlerRendering";
static const WCHAR c_szReadingPaneVisible[] = L"ReadingPaneSizer_Visible";
static const WCHAR c_szHostClass[] = L"Shell Preview Extension Host";
static const WCHAR c_szFGMsgClass[] = L"Shell Preview Extension Host Foreground Msg";
static const WCHAR c_szBGMsgClass[] = L"Shell Preview Extension Host Background Msg";
static const WCHAR c_szPreviewerClass[] = L"Shell Preview Extension Host Previewer";
static const WCHAR c_szPreviewHandlerShellEx[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
static const WCHAR c_szThumbnailProviderShellEx[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const WCHAR c_szAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

/* Group policy that removes the preview pane */
static const GUID c_polidNoReadingPane =
    { 0x33dcce8d, 0xb6f8, 0x4b7a, { 0xac, 0x93, 0x45, 0xe3, 0x01, 0xf9, 0x56, 0xaa } };

DirectUI::IClassInfo *CReadingPaneModuleInner::Class = NULL;
DirectUI::IClassInfo *CReadingPaneModuleHost::Class = NULL;

/* Helpers */

static
HRESULT
HResultFromLastError(VOID)
{
    DWORD dwError = GetLastError();
    HRESULT hr = HRESULT_FROM_WIN32(dwError);

    return SUCCEEDED(hr) ? E_FAIL : hr;
}

/* The frame task manager lives behind the frame's IObjectProvider */
static
HRESULT
QueryFrameTaskManager(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_ IFrameTaskManager **ppftm)
{
    IObjectProvider *pop;
    HRESULT hr;

    *ppftm = NULL;
    hr = IUnknown_QueryService(punkSite, IID_IFrameManager, IID_PPV_ARGS(&pop));
    if (SUCCEEDED(hr))
    {
        hr = pop->QueryObject(OID_FrameTaskManager, IID_PPV_ARGS(ppftm));
        pop->Release();
    }
    return hr;
}

static
BOOL
ReadBrowserBagBOOL(
    _In_opt_ IUnknown *punkSite,
    _In_ PCWSTR pszName)
{
    IPropertyBag *ppb;
    BOOL fValue = FALSE;

    if (SUCCEEDED(IUnknown_QueryService(punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        fValue = PropertyBag_ReadBOOLDefault(ppb, pszName, FALSE);
        ppb->Release();
    }
    return fValue;
}

/* The user setting that turns preview handlers off, with the machine default as fallback */
static
BOOL
IsReadingPaneEnabled(VOID)
{
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);
    DWORD dwType = REG_DWORD;

    if (SHGetValueW(HKEY_CURRENT_USER, c_szAdvancedKey, L"ShowPreviewHandlers", &dwType, &dwValue,
                    &cbValue) == ERROR_SUCCESS)
    {
        return dwValue != 0;
    }

    cbValue = sizeof(dwValue);
    if (SHGetValueW(HKEY_LOCAL_MACHINE, c_szAdvancedKey, L"DefaultValue", &dwType, &dwValue,
                    &cbValue) == ERROR_SUCCESS)
    {
        return dwValue != 0;
    }
    return TRUE;
}

static
BOOL
IsReadingPaneVisible(
    _In_opt_ IUnknown *punkSite)
{
    return ReadBrowserBagBOOL(punkSite, c_szReadingPaneVisible);
}

/* The first selected item is the one the pane previews */
static
HRESULT
GetPreviewItemFromArray(
    _In_ IShellItemArray *psia,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IShellItem *psi;
    HRESULT hr;

    *ppv = NULL;
    hr = psia->GetItemAt(0, &psi);
    if (SUCCEEDED(hr))
    {
        hr = psi->QueryInterface(riid, ppv);
        psi->Release();
    }
    return hr;
}

/* The association array of an item, built from its extension when the item cannot bind one */
static
HRESULT
GetItemAssociations(
    _In_ IShellItem *psi,
    _COM_Outptr_ IQueryAssociations **ppqa)
{
    PWSTR pszName;
    HRESULT hr;

    hr = psi->BindToHandler(NULL, BHID_AssociationArray, IID_PPV_ARGS(ppqa));
    if (SUCCEEDED(hr))
        return hr;

    *ppqa = NULL;
    hr = psi->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &pszName);
    if (FAILED(hr))
        return hr;

    hr = AssocCreate(CLSID_QueryAssociations, IID_PPV_ARGS(ppqa));
    if (SUCCEEDED(hr))
    {
        PCWSTR pszExt = PathFindExtensionW(pszName);

        hr = (*ppqa)->Init(ASSOCF_INIT_DEFAULTTOSTAR, *pszExt ? pszExt : L"*", NULL, NULL);
        if (FAILED(hr))
        {
            (*ppqa)->Release();
            *ppqa = NULL;
        }
    }
    CoTaskMemFree(pszName);
    return hr;
}

static
HRESULT
GetItemShellExtension(
    _In_ IShellItem *psi,
    _In_ PCWSTR pszExtension,
    _Out_writes_(cch) PWSTR pszClsid,
    _In_ DWORD cch)
{
    IQueryAssociations *pqa;
    HRESULT hr;

    hr = GetItemAssociations(psi, &pqa);
    if (SUCCEEDED(hr))
    {
        hr = pqa->GetString(ASSOCF_NOUSERSETTINGS, ASSOCSTR_SHELLEXTENSION, pszExtension, pszClsid, &cch);
        pqa->Release();
    }
    return hr;
}

/* Only handlers listed under PreviewHandlers may run */
static
HRESULT
ValidateHandlerRegistration(
    _In_ REFCLSID rclsid)
{
    static const WCHAR c_szHandlersKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
    WCHAR szClsid[39];

    StringFromGUID2(rclsid, szClsid, _countof(szClsid));
    if (SHRegGetValueW(HKEY_CURRENT_USER, c_szHandlersKey, szClsid, SRRF_RT_REG_SZ | SRRF_NOEXPAND, NULL, NULL,
                       NULL) == ERROR_SUCCESS ||
        SHRegGetValueW(HKEY_LOCAL_MACHINE, c_szHandlersKey, szClsid, SRRF_RT_REG_SZ | SRRF_NOEXPAND, NULL, NULL,
                       NULL) == ERROR_SUCCESS)
    {
        return S_OK;
    }
    return REGDB_E_CLASSNOTREG;
}

static
HRESULT
GetPreviewerCLSID(
    _In_ IShellItem *psi,
    _Out_ CLSID *pclsid)
{
    WCHAR szClsid[39];
    CLSID clsid;
    HRESULT hr;

    *pclsid = CLSID_NULL;
    hr = GetItemShellExtension(psi, c_szPreviewHandlerShellEx, szClsid, _countof(szClsid));
    if (SUCCEEDED(hr))
        hr = CLSIDFromString(szClsid, &clsid);
    if (SUCCEEDED(hr))
        hr = ValidateHandlerRegistration(clsid);
    if (SUCCEEDED(hr))
        *pclsid = clsid;
    return hr;
}

/* Items from zones that forbid previews stay closed */
static
BOOL
AllowPreviewForItem(
    _In_ IShellItem *psi)
{
    IInternetSecurityManager *pism;
    PWSTR pszUrl;
    BOOL fAllow = TRUE;

    if (FAILED(psi->GetDisplayName(SIGDN_URL, &pszUrl)))
        return TRUE;

    if (SUCCEEDED(CoCreateInstance(CLSID_InternetSecurityManager, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pism))))
    {
        DWORD dwPolicy = URLPOLICY_DISALLOW;

        if (FAILED(pism->ProcessUrlAction(pszUrl, URLACTION_SHELL_PREVIEW, (BYTE *)&dwPolicy, sizeof(dwPolicy), NULL,
                                          0, PUAF_NOUI, 0)) ||
            dwPolicy != URLPOLICY_ALLOW)
        {
            fAllow = FALSE;
        }
        pism->Release();
    }
    CoTaskMemFree(pszUrl);
    return fAllow;
}

static
HRESULT
GetPreviewerName(
    _Out_writes_(cch) PWSTR pszName,
    _In_ UINT cch,
    _In_ PCWSTR pszClsid)
{
    WCHAR szKey[MAX_PATH];
    HKEY hkey;
    LSTATUS lStatus;
    HRESULT hr;

    hr = StringCchPrintfW(szKey, _countof(szKey), L"Software\\Classes\\CLSID\\%s", pszClsid);
    if (FAILED(hr))
        return hr;

    lStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szKey, 0, KEY_QUERY_VALUE, &hkey);
    if (lStatus != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(lStatus);

    hr = SHLoadRegUIStringW(hkey, L"DisplayName", pszName, cch);
    RegCloseKey(hkey);
    return hr;
}

/* CReadingPaneModuleInner */

HRESULT
WINAPI
CReadingPaneModuleInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Layout *pLayout = NULL;
    DirectUI::Element *peHost;
    CReadingPaneModuleInner *pe;
    WCHAR szName[64];
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CReadingPaneModuleInner>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(1);
    if (SUCCEEDED(hr))
        hr = DirectUI::ShellBorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        hr = pe->SetLayout(pLayout);

    if (SUCCEEDED(hr))
    {
        hr = CReadingPaneModuleHost::Create(pe, NULL, &peHost);
        if (SUCCEEDED(hr))
        {
            hr = pe->Add(peHost);
            if (FAILED(hr))
                peHost->Destroy(true);
            else
                pe->_pHost = (CReadingPaneModuleHost *)peHost;
        }
    }

    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    LoadStringW(DUIFRAME_HINST, IDS_PREVIEWPANE_NAME, szName, _countof(szName));
    pe->SetAccessible(true);
    pe->SetAccName(szName);
    pe->SetAccRole(ROLE_SYSTEM_OUTLINE);

    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CReadingPaneModuleInner::Register()
{
    return DirectUI::ClassInfo<CReadingPaneModuleInner, CElementWithSite,
                               DirectUI::StandardCreator<CReadingPaneModuleInner>>::Register(
        DUIFRAME_HINST, L"ReadingPaneModuleInner", NULL, 0, false);
}

STDMETHODIMP
CReadingPaneModuleInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CReadingPaneModuleInner, IFrameShellViewClient),
        QITABENT(CReadingPaneModuleInner, IFrameNotificationClient),
        QITABENT(CReadingPaneModuleInner, IInputObject),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

/* The host shares the module's site, and through it the callback reaches the browser bag */
STDMETHODIMP
CReadingPaneModuleInner::SetSite(
    IUnknown *punkSite)
{
    IUnknown_Set(&_punkSite, punkSite);
    if (_pHost)
    {
        IUnknown_Set(&_pHost->_punkSite, punkSite);
        IUnknown_SetSite(_pHost->_pphc, _pHost->_punkSite);
    }
    return S_OK;
}

void
CReadingPaneModuleInner::OnDestroy()
{
    if (_pfwiPending)
    {
        _pfwiPending->SetDispatchable(FALSE);
        IUnknown_SafeReleaseAndNullPtr(&_pfwiPending);
    }
    DirectUI::Element::OnDestroy();
}

/* A preview failure from the host turns into the matching message on the cover sheet */
void
CReadingPaneModuleInner::OnEvent(
    DirectUI::Event *pev)
{
    PREVIEWERROREVENT *pee;
    WCHAR szText[1024];
    UINT uID = 0;

    if (pev->uidType.m_Value != CReadingPaneModuleHost::PreviewError().m_Value)
    {
        DirectUI::Element::OnEvent(pev);
        return;
    }

    pee = (PREVIEWERROREVENT *)pev;
    szText[0] = UNICODE_NULL;
    switch (pee->error.hrReason)
    {
        case PREVIEW_E_FILEINUSE:
            uID = IDS_PREVIEW_FILEINUSE;
            break;

        case PREVIEW_E_CANNOTPREVIEW:
            uID = IDS_PREVIEW_CANNOTPREVIEW;
            break;

        case PREVIEW_E_PREVIEWERFAILED:
        {
            WCHAR szClsid[40];
            WCHAR szName[200];
            WCHAR szFormat[100];

            uID = IDS_PREVIEW_CANNOTPREVIEW;
            if (StringFromGUID2(pee->error.clsidPreviewer, szClsid, 39) &&
                SUCCEEDED(GetPreviewerName(szName, _countof(szName), szClsid)))
            {
                uID = 0;
                if (LoadStringW(DUIFRAME_HINST, IDS_PREVIEW_PREVIEWERERROR, szFormat, _countof(szFormat)))
                    StringCchPrintfW(szText, _countof(szText), szFormat, szName);
            }
            break;
        }

        case PREVIEW_E_LOCATION:
            uID = IDS_PREVIEW_LOCATION;
            break;
    }

    if (uID)
        LoadStringW(DUIFRAME_HINST, uID, szText, _countof(szText));

    _UpdateCoverSheets(COVER_ERROR, szText, pee->error.hrReason);
    pev->fHandled = true;
}

/* The cover sheets sit on the pane background, in front of the reading pane thumbnail */
DirectUI::Element *
CReadingPaneModuleInner::_EnsureCoverPage(
    _In_ PCWSTR pszID)
{
    DirectUI::Element *pe;
    ATOM atomID = StrToID(pszID);

    /* An ID nobody registered yet has no atom, and atom 0 would match an element without an ID */
    pe = atomID ? GetRoot()->FindDescendent(atomID) : NULL;
    if (pe)
        return pe;

    if (FAILED(DirectUI::Element::Create(0, _peReadingPane, NULL, &pe)) || !pe)
        return NULL;

    if (FAILED(_peReadingPane->Add(pe)))
    {
        pe->Destroy(true);
        return NULL;
    }

    pe->SetContentAlign(COVER_CONTENTALIGN);
    pe->SetID(pszID);
    return pe;
}

STDMETHODIMP
CReadingPaneModuleInner::LayoutInitialized()
{
    WCHAR szName[64];

    if (!_pHost)
        return S_OK;

    _peReadingPane = GetRoot()->FindDescendent(StrToID(L"ReadingPane"));
    if (_peReadingPane)
    {
        LoadStringW(DUIFRAME_HINST, IDS_PREVIEWPANE_NAME, szName, _countof(szName));
        _peReadingPane->SetAccName(szName);
        _peReadingPane->SetAccessible(true);

        _peTransitionSheet = _EnsureCoverPage(L"RPaneCoverSheet_Transition");
        _peNoSelSheet = _EnsureCoverPage(L"RPaneCoverSheet_NoSel");
        if (_peNoSelSheet)
        {
            _peNoSelSheet->SetAccessible(true);
            _peNoSelSheet->SetAccRole(ROLE_SYSTEM_STATICTEXT);
        }
    }

    _Update();
    _pHost->Update(TRUE);
    return S_OK;
}

/* Bag changes are ignored while the host itself is writing them from a dispatch */
STDMETHODIMP
CReadingPaneModuleInner::Notify(
    PCWSTR pszPropertyName)
{
    BOOL fVisibility;
    IPropertyBag *ppb;

    if (!_pHost || _pHost->_fDispatching)
        return S_OK;

    fVisibility = !StrCmpICW(pszPropertyName, c_szReadingPaneVisible);
    if (fVisibility && _peReadingPane)
        _peReadingPane->SetAccState(IsReadingPaneVisible(_punkSite) ? 0 : STATE_SYSTEM_INVISIBLE);

    if (!fVisibility &&
        StrCmpICW(pszPropertyName, c_szPreviewHandlerRunning) &&
        StrCmpICW(pszPropertyName, c_szPreviewHandlerRendering))
    {
        return S_OK;
    }

    if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        if (PropertyBag_ReadBOOLDefault(ppb, pszPropertyName, FALSE))
            _Update();
        if (fVisibility)
            _pHost->Update(FALSE);
        ppb->Release();
    }
    return S_OK;
}

STDMETHODIMP
CReadingPaneModuleInner::OnNavigateAway()
{
    IPropertyBag *ppb;

    if (!_pHost || !_pHost->_pph)
        return S_OK;

    if (SUCCEEDED(IUnknown_QueryService(_pHost->_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        if (PropertyBag_ReadBOOLDefault(ppb, c_szPreviewHandlerRunning, FALSE))
            _pHost->_UnloadCurrentPreviewer(ppb);
        ppb->Release();
    }
    return S_OK;
}

STDMETHODIMP
CReadingPaneModuleInner::OnSelectedItemChanged()
{
    _Update();
    if (_pHost)
        _pHost->Update(TRUE);
    return S_OK;
}

STDMETHODIMP
CReadingPaneModuleInner::OnSelectionChanged()
{
    _Update();
    if (_pHost)
        _pHost->Update(FALSE);
    return S_OK;
}

STDMETHODIMP
CReadingPaneModuleInner::OnContentsChanged()
{
    _Update();
    if (_pHost)
        _pHost->Update(FALSE);
    return S_OK;
}

/* Focus is inside the pane when it is on the handler's window or one of its children */
STDMETHODIMP
CReadingPaneModuleInner::HasFocusIO()
{
    HWND hwndFocus = NULL;
    HWND hwndHost;

    if (!_pHost)
        return S_FALSE;

    if (_pHost->_pph && FAILED(_pHost->_pph->QueryWindowFocus(&hwndFocus)))
        hwndFocus = NULL;

    hwndHost = _pHost->GetHWND();
    if (hwndFocus == hwndHost || IsChild(hwndHost, hwndFocus))
        return S_OK;
    return S_FALSE;
}

/* With nothing selected the pane asks for a selection; otherwise a work item looks at the item */
HRESULT
CReadingPaneModuleInner::_Update()
{
    IShellItemArray *psia = NULL;
    IFolderView *pfv;
    HRESULT hr = S_OK;
    BOOL fNoSelection = TRUE;

    if (!_peNoSelSheet || !_peTransitionSheet)
        return S_OK;

    if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv))))
    {
        if (SUCCEEDED(pfv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&psia))))
        {
            fNoSelection = FALSE;
            if (_pfwiPending)
            {
                _pfwiPending->SetDispatchable(FALSE);
                IUnknown_SafeReleaseAndNullPtr(&_pfwiPending);
            }
            hr = _QueueUpdateWorkItem(psia);
            _fPaneVisible = IsReadingPaneVisible(_punkSite);
            psia->Release();
        }
        pfv->Release();
    }

    if (fNoSelection)
    {
        _fPaneVisible = FALSE;
        _UpdateCoverSheets(COVER_NOSELECTION, NULL, S_OK);
    }
    return hr;
}

HRESULT
CReadingPaneModuleInner::_QueueUpdateWorkItem(
    _In_ IShellItemArray *psia)
{
    IFrameTaskManager *pftm;
    IFrameWorkItem *pfwi;
    HRESULT hr;

    hr = QueryFrameTaskManager(_punkSite, &pftm);
    if (FAILED(hr))
        return hr;

    hr = CReadingPaneUpdateWorkItem::CreateInstance(this, psia, &pfwi);
    if (SUCCEEDED(hr))
    {
        IUnknown_Set((IUnknown **)&_pfwiPending, pfwi);
        hr = pftm->StartWorkItem(_pfwiPending, TOID_ReadingPaneUpdateTask, FTMF_REPLACE);
        pfwi->Release();
    }
    pftm->Release();
    return hr;
}

/* States without their own text keep the sheet out of the layout */
void
CReadingPaneModuleInner::_UpdateCoverSheets(
    _In_ int iState,
    _In_opt_ PCWSTR pszText,
    _In_ HRESULT hrStatus)
{
    static const struct
    {
        int iState;
        UINT uID;
    } c_rgCoverText[] =
    {
        { COVER_NOSELECTION, IDS_PREVIEW_SELECTFILE },
        { COVER_NOPREVIEW, IDS_PREVIEW_NOPREVIEW },
        { COVER_UNSAFE, IDS_PREVIEW_UNSAFE },
    };
    int iTransitionPos = ROSUI_LP_AUTO;
    BOOL fKnown = FALSE;
    SIZE_T i;

    for (i = 0; i < _countof(c_rgCoverText); i++)
    {
        if (c_rgCoverText[i].iState == iState)
        {
            WCHAR szText[512];

            _peNoSelSheet->SetLayoutPos(COVER_LAYOUTPOS_SHOWN);
            if (LoadStringW(DUIFRAME_HINST, c_rgCoverText[i].uID, szText, _countof(szText)))
            {
                _peNoSelSheet->SetContentString(szText);
                _peNoSelSheet->SetAccName(szText);
            }
            fKnown = TRUE;
            break;
        }
    }

    if (!fKnown)
    {
        switch (iState)
        {
            case COVER_ERROR:
                _peNoSelSheet->SetLayoutPos(COVER_LAYOUTPOS_SHOWN);
                _peNoSelSheet->SetContentString(pszText);
                _peNoSelSheet->SetAccName(pszText);
                fKnown = TRUE;
                break;

            case COVER_PREVIEWER:
                _peNoSelSheet->SetLayoutPos(ROSUI_LP_NONE);
                fKnown = TRUE;
                break;

            case COVER_THUMBNAIL:
                _peNoSelSheet->SetLayoutPos(ROSUI_LP_NONE);
                iTransitionPos = ROSUI_LP_NONE;
                fKnown = TRUE;
                break;
        }
    }

    if (fKnown)
        _peTransitionSheet->SetLayoutPos(iTransitionPos);

    _hrStatus = hrStatus;
    _fCoverShown = (iState != COVER_NOSELECTION);
}

/* CReadingPaneUpdateWorkItem */

HRESULT
WINAPI
CReadingPaneUpdateWorkItem::CreateInstance(
    _In_ CReadingPaneModuleInner *pInner,
    _In_ IShellItemArray *psia,
    _COM_Outptr_ IFrameWorkItem **ppfwi)
{
    CReadingPaneUpdateWorkItem *pItem;
    HRESULT hr;

    *ppfwi = NULL;
    pItem = new CReadingPaneUpdateWorkItem();
    if (!pItem)
        return E_OUTOFMEMORY;

    pItem->_pInner = pInner;
    hr = CoMarshalInterThreadInterfaceInStream(IID_IShellItemArray, psia, &pItem->_pstmItems);
    if (SUCCEEDED(hr))
        hr = pItem->QueryInterface(IID_PPV_ARGS(ppfwi));
    pItem->Release();
    return hr;
}

/* Streams that were never unmarshaled still hold a reference */
static
VOID
ReleaseMarshalStream(
    _Inout_ IStream **ppstm)
{
    IUnknown *punk;

    if (!*ppstm)
        return;

    if (SUCCEEDED(CoGetInterfaceAndReleaseStream(*ppstm, IID_PPV_ARGS(&punk))))
        punk->Release();
    *ppstm = NULL;
}

CReadingPaneUpdateWorkItem::~CReadingPaneUpdateWorkItem()
{
    ReleaseMarshalStream(&_pstmItems);
    ReleaseMarshalStream(&_pstmItem);
}

STDMETHODIMP
CReadingPaneUpdateWorkItem::DoWork()
{
    IShellItemArray *psia = NULL;
    IShellItem *psi;
    HRESULT hr;

    hr = CoGetInterfaceAndReleaseStream(_pstmItems, IID_PPV_ARGS(&psia));
    _pstmItems = NULL;
    if (SUCCEEDED(hr))
    {
        hr = GetPreviewItemFromArray(psia, IID_PPV_ARGS(&psi));
        if (SUCCEEDED(hr))
        {
            hr = CoMarshalInterThreadInterfaceInStream(IID_IUnknown, psi, &_pstmItem);
            psi->Release();
        }
        psia->Release();
    }
    return hr;
}

/*
 * A registered previewer gets the pane unless the zone forbids it; items without one show
 * their thumbnail when a thumbnail handler exists, otherwise "No preview available."
 */
STDMETHODIMP
CReadingPaneUpdateWorkItem::Dispatch()
{
    IShellItem *psi = NULL;
    WCHAR szClsid[39];
    CLSID clsid;
    BOOL fPreviewer;
    BOOL fUnsafe = FALSE;
    BOOL fThumbnail;
    int iState = COVER_NOPREVIEW;
    HRESULT hrStatus = S_OK;
    HRESULT hr;

    hr = CoGetInterfaceAndReleaseStream(_pstmItem, IID_PPV_ARGS(&psi));
    _pstmItem = NULL;
    if (FAILED(hr))
        return hr;

    fPreviewer = IsReadingPaneEnabled() && IsReadingPaneVisible(_pInner->_punkSite);
    if (fPreviewer)
    {
        if (FAILED(GetPreviewerCLSID(psi, &clsid)))
            fPreviewer = FALSE;
        else
            fUnsafe = !AllowPreviewForItem(psi);
    }

    fThumbnail = SUCCEEDED(GetItemShellExtension(psi, c_szThumbnailProviderShellEx, szClsid, _countof(szClsid)));

    if (fPreviewer)
    {
        if (fUnsafe)
        {
            iState = COVER_UNSAFE;
            hrStatus = PREVIEW_E_UNSAFE;
        }
        else
        {
            iState = COVER_PREVIEWER;
        }
    }
    else if (fThumbnail)
    {
        iState = COVER_THUMBNAIL;
    }
    else
    {
        hrStatus = PREVIEW_E_NOPREVIEW;
    }

    _pInner->_UpdateCoverSheets(iState, NULL, hrStatus);
    psi->Release();
    return hr;
}

/* CReadingPaneCallback */

CReadingPaneCallback::CReadingPaneCallback(
    _In_ HWND hwndHost) :
    _cRef(1),
    _hwndHost(hwndHost)
{
}

STDMETHODIMP
CReadingPaneCallback::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CReadingPaneCallback, IPreviewHostCallback),
        QITABENT(CReadingPaneCallback, IObjectWithSite),
        {0},
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CReadingPaneCallback::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CReadingPaneCallback::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* A failed load leaves nothing running or rendering */
STDMETHODIMP
CReadingPaneCallback::OnLoad(
    HRESULT hr)
{
    IPropertyBag *ppb;
    HRESULT hrQuery;

    hrQuery = IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb));
    if (FAILED(hrQuery))
        return hrQuery;

    if (FAILED(hr))
    {
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRendering, 0);
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRunning, 0);
    }
    ppb->Release();
    return hrQuery;
}

/* The handler drew its first frame: the pane switches from the thumbnail to the handler */
STDMETHODIMP
CReadingPaneCallback::OnShow(
    HRESULT hr)
{
    IPropertyBag *ppb;
    HRESULT hrQuery;

    hrQuery = IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb));
    if (FAILED(hrQuery))
        return hrQuery;

    if (FAILED(hr))
    {
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRendering, 0);
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRunning, 0);
    }
    else
    {
        ShowWindow(_hwndHost, SW_SHOW);
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRendering, 1);
    }
    ppb->Release();
    return hrQuery;
}

STDMETHODIMP
CReadingPaneCallback::OnUnload(
    HRESULT hr)
{
    IPropertyBag *ppb;
    HRESULT hrQuery;

    hrQuery = IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb));
    if (FAILED(hrQuery))
        return hrQuery;

    if (FAILED(hr))
    {
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRendering, 0);
        SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRunning, 0);
    }
    ppb->Release();
    return hrQuery;
}

/* Tab leaves the handler through the host window; other keys go to the browser */
STDMETHODIMP
CReadingPaneCallback::OnTranslateAccelerator(
    const MSG *pmsg,
    HRESULT *phr)
{
    IInputObject *pio;

    *phr = S_FALSE;
    if (IsVK_TABCycler(pmsg) && (pmsg->message == WM_KEYDOWN || pmsg->message == WM_KEYUP))
    {
        PostMessageW(_hwndHost, pmsg->message, pmsg->wParam, pmsg->lParam);
        *phr = S_OK;
    }
    else if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&pio))))
    {
        *phr = pio->TranslateAcceleratorIO((MSG *)pmsg);
        pio->Release();
    }
    return S_OK;
}

STDMETHODIMP
CReadingPaneCallback::OnError(
    const NotifyError *pError)
{
    OnUnload(pError->hrReason);
    SendMessageW(_hwndHost, RPHM_PREVIEWERROR, (WPARAM)pError, 0);
    return S_OK;
}

/* CReadingPaneModuleHost */

HRESULT
WINAPI
CReadingPaneModuleHost::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CReadingPaneModuleHost *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CReadingPaneModuleHost>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->DirectUI::HWNDHost::Initialize(0, 3, peParent, pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    pe->SetID(L"ReadingPaneModuleHost");
    pe->SetAccessible(true);
    pe->SetAccRole(ROLE_SYSTEM_PANE);
    pe->SetLayoutPos(ROSUI_LP_CLIENT);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CReadingPaneModuleHost::Register()
{
    return DirectUI::ClassInfo<CReadingPaneModuleHost, DirectUI::HWNDHost,
                               DirectUI::StandardCreator<CReadingPaneModuleHost>>::Register(
        DUIFRAME_HINST, L"CReadingPaneModuleHost", NULL, 0, false);
}

UID
WINAPI
CReadingPaneModuleHost::PreviewError()
{
    static const BYTE s_bPreviewErrorId = 0;
    UID uid;

    uid.m_Value = (PVOID)&s_bPreviewErrorId;
    return uid;
}

HWND
CReadingPaneModuleHost::CreateHWND(
    HWND hwndParent)
{
    CReadingPaneCallback *pCallback;
    WNDCLASSEXW wc;
    RECT rc;
    HWND hwnd;
    HRESULT hr;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = s_ModuleHostWndProc;
    wc.cbWndExtra = sizeof(LONG_PTR);
    wc.hInstance = DUIFRAME_HINST;
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = c_szHostClass;
    RegisterClassExW(&wc);

    if (!GetWindowRect(hwndParent, &rc))
        return NULL;

    hwnd = SHFusionCreateWindow(c_szHostClass, c_szHostClass, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                rc.right - rc.left, rc.bottom - rc.top, hwndParent, NULL, DUIFRAME_HINST, this);
    if (!hwnd)
        return NULL;

    pCallback = new CReadingPaneCallback(hwnd);
    if (pCallback)
    {
        hr = pCallback->QueryInterface(IID_PPV_ARGS(&_pphc));
        pCallback->Release();
    }
    else
    {
        hr = E_OUTOFMEMORY;
    }

    /* The module may have handed out its site before the window existed */
    if (SUCCEEDED(hr) && _punkSite)
        IUnknown_SetSite(_pphc, _punkSite);
    if (SUCCEEDED(hr))
        hr = CPreviewHost::CreateInstance(DUIFRAME_HINST, hwnd, _pphc, IID_PPV_ARGS(&_pph));

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        hwnd = NULL;
    }
    return hwnd;
}

void
CReadingPaneModuleHost::OnDestroy()
{
    IUnknown_SetSite(_pphc, NULL);
    IUnknown_SafeReleaseAndNullPtr(&_pph);
    IUnknown_SafeReleaseAndNullPtr(&_pphc);
    IUnknown_SafeReleaseAndNullPtr(&_psiPreviewed);
    if (_pfwiPending)
    {
        _pfwiPending->SetDispatchable(FALSE);
        IUnknown_SafeReleaseAndNullPtr(&_pfwiPending);
    }
    IUnknown_SafeReleaseAndNullPtr(&_punkSite);
    DirectUI::HWNDHost::OnDestroy();
}

/* Keyboard focus arriving from outside the pane goes on to the handler */
void
CReadingPaneModuleHost::OnKeyFocusMoved(
    DirectUI::Element *peFrom,
    DirectUI::Element *peTo)
{
    BOOL fFromInside = (peFrom == this || IsDescendent(peFrom));

    if ((peTo == this || IsDescendent(peTo)) && !fFromInside && _pph)
        _pph->SetFocus();
}

LRESULT
CALLBACK
CReadingPaneModuleHost::s_ModuleHostWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    CReadingPaneModuleHost *pThis = (CReadingPaneModuleHost *)GetWindowLongPtrW(hwnd, 0);

    switch (uMsg)
    {
        case WM_CREATE:
            SetWindowLongPtrW(hwnd, 0, (LONG_PTR)((CREATESTRUCTW *)lParam)->lpCreateParams);
            break;

        case WM_SIZE:
            if (pThis && pThis->_pph)
                pThis->_pph->WindowSized();
            break;

        case WM_SETTINGCHANGE:
            if (pThis)
                pThis->Update(FALSE);
            break;

        case RPHM_PREVIEWERROR:
            if (pThis)
            {
                PREVIEWERROREVENT ev;

                ZeroMemory(&ev, sizeof(ev));
                ev.uidType = PreviewError();
                ev.error = *(const NotifyError *)wParam;
                pThis->FireEvent(&ev, true, false);
            }
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* One update at a time: the selection is handed to a work item that loads the previewer */
void
CReadingPaneModuleHost::Update(
    _In_ BOOL fSelectedItemChanged)
{
    IShellItemArray *psia;
    IPropertyBag *ppb;
    IFolderView *pfv;
    DWORD dwPolicy = 0;
    DWORD cbPolicy = sizeof(dwPolicy);
    BOOL fQueued = FALSE;

    if (!_pph || _fUpdatePending)
        return;
    if (SUCCEEDED(SHWindowsPolicyGetValue(c_polidNoReadingPane, &dwPolicy, &cbPolicy)) && dwPolicy)
        return;

    _fUpdatePending = TRUE;
    if (FAILED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        _fUpdatePending = FALSE;
        return;
    }

    if (!fSelectedItemChanged)
    {
        BOOL fVisible;

        if (SUCCEEDED(SHPropertyBag_ReadBOOL(ppb, c_szReadingPaneVisible, &fVisible)) && !fVisible)
            ShowWindow(GetHWND(), SW_HIDE);
    }

    if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv))))
    {
        if (SUCCEEDED(pfv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&psia))))
        {
            if (_pfwiPending)
            {
                _pfwiPending->SetDispatchable(FALSE);
                IUnknown_SafeReleaseAndNullPtr(&_pfwiPending);
            }
            fQueued = SUCCEEDED(_QueueUpdateWorkItem(fSelectedItemChanged, ppb, psia));
            psia->Release();
        }
        else if (PropertyBag_ReadBOOLDefault(ppb, c_szPreviewHandlerRunning, FALSE))
        {
            _UnloadCurrentPreviewer(ppb);
        }
        pfv->Release();
    }

    if (!fQueued)
        _fUpdatePending = FALSE;
    ppb->Release();
}

HRESULT
CReadingPaneModuleHost::_QueueUpdateWorkItem(
    _In_ BOOL fSelectedItemChanged,
    _In_ IPropertyBag *ppb,
    _In_ IShellItemArray *psia)
{
    IFrameTaskManager *pftm;
    IFrameWorkItem *pfwi;
    HRESULT hr;

    hr = QueryFrameTaskManager(_punkSite, &pftm);
    if (FAILED(hr))
        return hr;

    hr = CReadingPaneHostUpdateWorkItem::CreateInstance(this, fSelectedItemChanged, ppb, psia, &pfwi);
    if (SUCCEEDED(hr))
    {
        IUnknown_Set((IUnknown **)&_pfwiPending, pfwi);
        hr = pftm->StartWorkItem(pfwi, TOID_ReadingPaneHostUpdateTask, FTMF_REPLACE);
        pfwi->Release();
    }
    pftm->Release();
    return hr;
}

void
CReadingPaneModuleHost::_UnloadCurrentPreviewer(
    _In_ IPropertyBag *ppb)
{
    IUnknown_SafeReleaseAndNullPtr(&_psiPreviewed);
    SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRendering, 0);
    SHPropertyBag_WriteInt(ppb, c_szPreviewHandlerRunning, 0);
    _pph->Unload();
}

/* CReadingPaneHostUpdateWorkItem */

HRESULT
WINAPI
CReadingPaneHostUpdateWorkItem::CreateInstance(
    _In_ CReadingPaneModuleHost *pHost,
    _In_ BOOL fSelectedItemChanged,
    _In_ IPropertyBag *ppb,
    _In_ IShellItemArray *psia,
    _COM_Outptr_ IFrameWorkItem **ppfwi)
{
    CReadingPaneHostUpdateWorkItem *pItem;
    HRESULT hr;

    *ppfwi = NULL;
    pItem = new CReadingPaneHostUpdateWorkItem();
    if (!pItem)
        return E_OUTOFMEMORY;

    pItem->_fSelectedItemChanged = fSelectedItemChanged;
    pItem->_pHost = pHost;
    IUnknown_Set((IUnknown **)&pItem->_ppb, ppb);
    hr = CoMarshalInterThreadInterfaceInStream(IID_IShellItemArray, psia, &pItem->_pstmItems);
    if (SUCCEEDED(hr))
        hr = pItem->QueryInterface(IID_PPV_ARGS(ppfwi));
    pItem->Release();
    return hr;
}

CReadingPaneHostUpdateWorkItem::~CReadingPaneHostUpdateWorkItem()
{
    ReleaseMarshalStream(&_pstmItems);
    ReleaseMarshalStream(&_pstmItem);
    IUnknown_SafeReleaseAndNullPtr(&_ppb);
}

STDMETHODIMP
CReadingPaneHostUpdateWorkItem::DoWork()
{
    IShellItemArray *psia = NULL;
    IShellItem *psi;
    HRESULT hr;

    hr = CoGetInterfaceAndReleaseStream(_pstmItems, IID_PPV_ARGS(&psia));
    _pstmItems = NULL;
    if (SUCCEEDED(hr))
    {
        hr = GetPreviewItemFromArray(psia, IID_PPV_ARGS(&psi));
        if (SUCCEEDED(hr))
        {
            hr = CoMarshalInterThreadInterfaceInStream(IID_IShellItem, psi, &_pstmItem);
            psi->Release();
        }
        psia->Release();
    }
    _fItemReady = SUCCEEDED(hr);
    return S_OK;
}

/*
 * The handler is reloaded for a different item or an explicit selection change. When the pane
 * is hidden or disabled a running handler is unloaded.
 */
STDMETHODIMP
CReadingPaneHostUpdateWorkItem::Dispatch()
{
    CReadingPaneModuleHost *pHost = _pHost;
    IShellItem *psi = NULL;
    BOOL fNewItem;
    BOOL fRunning;
    HRESULT hr = S_OK;

    if (_fItemReady)
    {
        pHost->_fDispatching = TRUE;
        hr = CoGetInterfaceAndReleaseStream(_pstmItem, IID_PPV_ARGS(&psi));
        _pstmItem = NULL;
        if (SUCCEEDED(hr))
        {
            int iOrder;

            /* An item that cannot be compared counts as the one already shown */
            fNewItem = TRUE;
            if (pHost->_psiPreviewed)
            {
                fNewItem = SUCCEEDED(psi->Compare(pHost->_psiPreviewed, SICHINT_CANONICAL, &iOrder)) &&
                           iOrder != 0;
            }

            fRunning = PropertyBag_ReadBOOLDefault(_ppb, c_szPreviewHandlerRunning, FALSE);
            if (IsReadingPaneEnabled() && IsReadingPaneVisible(pHost->_punkSite))
            {
                if (fNewItem || _fSelectedItemChanged || !fRunning)
                {
                    if (fRunning)
                        pHost->_UnloadCurrentPreviewer(_ppb);

                    if (SUCCEEDED(pHost->_pph->Load(psi)))
                    {
                        SHPropertyBag_WriteInt(_ppb, c_szPreviewHandlerRunning, 1);
                        if (SUCCEEDED(pHost->_pph->DoPreview()))
                            IUnknown_Set((IUnknown **)&pHost->_psiPreviewed, psi);
                        else
                            pHost->_UnloadCurrentPreviewer(_ppb);
                    }
                }
            }
            else if (fRunning)
            {
                pHost->_UnloadCurrentPreviewer(_ppb);
            }
            psi->Release();
        }
        pHost->_fDispatching = FALSE;
    }
    pHost->_fUpdatePending = FALSE;
    return hr;
}

/* CPreviewHost */

CPreviewHost::CPreviewHost() :
    _cRef(1),
    _uCookie(1)
{
}

CPreviewHost::~CPreviewHost()
{
    if (_pCurrent)
    {
        _pCurrent->Release();
        _pCurrent = NULL;
    }

    if (_hdpaCache)
    {
        for (int i = 0; i < DPA_GetPtrCount(_hdpaCache); i++)
        {
            CRichPreviewThreadData *pData = (CRichPreviewThreadData *)DPA_GetPtr(_hdpaCache, i);

            if (pData)
                pData->Release();
        }
        DPA_Destroy(_hdpaCache);
        _hdpaCache = NULL;
    }

    IUnknown_SafeReleaseAndNullPtr(&_pphc);
    if (_hwndMsg)
    {
        DestroyWindow(_hwndMsg);
        _hwndMsg = NULL;
    }
}

HRESULT
WINAPI
CPreviewHost::CreateInstance(
    _In_ HINSTANCE hinst,
    _In_ HWND hwndHost,
    _In_ IPreviewHostCallback *pphc,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CPreviewHost *pHost;
    HRESULT hr;

    *ppv = NULL;
    pHost = new CPreviewHost();
    if (!pHost)
        return E_OUTOFMEMORY;

    pHost->_hdpaCache = DPA_Create(PREVIEWHOST_CACHE_MAX);
    hr = pHost->_hdpaCache ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        pHost->_hinst = hinst;
        hr = pHost->_CreateMsgWindow();
    }
    if (SUCCEEDED(hr))
    {
        IUnknown_Set((IUnknown **)&pHost->_pphc, pphc);
        pHost->_hwndHost = hwndHost;
        hr = pHost->QueryInterface(riid, ppv);
    }
    pHost->Release();
    return hr;
}

STDMETHODIMP
CPreviewHost::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CPreviewHost, IPreviewHost),
        {0},
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CPreviewHost::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CPreviewHost::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* Handler threads answer through this message-only window on the frame thread */
HRESULT
CPreviewHost::_CreateMsgWindow()
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_GLOBALCLASS;
    wc.lpfnWndProc = s_MsgWndProc;
    wc.hInstance = _hinst;
    wc.lpszClassName = c_szFGMsgClass;
    RegisterClassExW(&wc);

    _hwndMsg = SHFusionCreateWindowEx(0, c_szFGMsgClass, NULL, WS_DISABLED, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL,
                                      this);
    return _hwndMsg ? S_OK : HResultFromLastError();
}

/* Answers for an older load than the current one are dropped */
LRESULT
CALLBACK
CPreviewHost::s_MsgWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    CPreviewHost *pThis = (CPreviewHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    BOOL fCurrent = pThis && pThis->_pCurrent && pThis->_uCookie == (UINT)wParam;

    switch (uMsg)
    {
        case WM_CREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lParam)->lpCreateParams);
            break;

        case PHM_LOADED:
            if (fCurrent)
            {
                if (FAILED((HRESULT)lParam) && pThis->_pCurrent->_hwndPreviewer)
                    ShowWindow(pThis->_pCurrent->_hwndPreviewer, SW_HIDE);
                pThis->_pphc->OnLoad((HRESULT)lParam);
            }
            break;

        case PHM_SHOWN:
            if (fCurrent && pThis->_uCookie > pThis->_uUnloadCookie)
            {
                if (SUCCEEDED((HRESULT)lParam))
                    ShowWindow(pThis->_pCurrent->_hwndPreviewer, SW_SHOW);
                pThis->_pphc->OnShow((HRESULT)lParam);
            }
            break;

        case PHM_UNLOADED:
            if (fCurrent)
                pThis->_pphc->OnUnload((HRESULT)lParam);
            break;

        case PHM_ACCELERATOR:
            if (pThis)
            {
                HRESULT hr;

                pThis->_pphc->OnTranslateAccelerator((const MSG *)wParam, &hr);
            }
            LocalFree((HLOCAL)wParam);
            break;

        case PHM_ERROR:
            if (fCurrent)
                pThis->_pphc->OnError((const NotifyError *)lParam);
            delete (NotifyError *)lParam;
            break;

        default:
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

/* A cached thread is reused only once it is idle again */
CRichPreviewThreadData *
CPreviewHost::_FindInCache(
    _In_ REFCLSID rclsid)
{
    for (int i = 0; i < DPA_GetPtrCount(_hdpaCache); i++)
    {
        CRichPreviewThreadData *pData = (CRichPreviewThreadData *)DPA_GetPtr(_hdpaCache, i);
        DWORD dwIndex;

        if (!pData || !IsEqualCLSID(pData->_clsid, rclsid))
            continue;

        pData->AddRef();
        if (!pData->_hevIdle ||
            FAILED(CoWaitForMultipleHandles(0, 2000, 1, &pData->_hevIdle, &dwIndex)))
        {
            pData->Release();
            DPA_DeletePtr(_hdpaCache, i);
            pData->Release();
            return NULL;
        }
        return pData;
    }
    return NULL;
}

/* New handler threads go to the front; the oldest one is shut down when the cache is full */
HRESULT
CPreviewHost::_CreateAndAddToCache(
    _In_ REFCLSID rclsid,
    _Outptr_ CRichPreviewThreadData **ppData)
{
    CRichPreviewThreadParams *pParams;
    CRichPreviewThreadData *pData;
    CRichPreviewThreadData *pNew = NULL;
    HRESULT hr = E_OUTOFMEMORY;

    *ppData = NULL;
    pData = new CRichPreviewThreadData();
    if (pData)
    {
        pParams = new CRichPreviewThreadParams();
        if (pParams)
        {
            pParams->_hinst = _hinst;
            pParams->_clsid = rclsid;
            pParams->_hwndFGMsg = _hwndMsg;
            pParams->_hwndHost = _hwndHost;
            if (SHCreateThread(CRichPreviewBackgroundThreadData::s_ThreadProc, pParams,
                               CTF_COINIT_STA | CTF_WAIT_NO_REENTRANCY,
                               CRichPreviewBackgroundThreadData::s_SyncThreadProc))
            {
                hr = pParams->_hr;
                if (SUCCEEDED(hr))
                {
                    pData->_clsid = rclsid;
                    pData->_hwndBG = pParams->_hwndBG;
                    pData->_hevIdle = pParams->_hevIdle;
                    pData->_hwndPreviewer = pParams->_hwndPreviewer;
                    pData->AddRef();
                    pNew = pData;
                }
            }
            pParams->Release();
        }
        pData->Release();
    }

    if (FAILED(hr))
        return hr;

    if (DPA_GetPtrCount(_hdpaCache) == PREVIEWHOST_CACHE_MAX)
    {
        CRichPreviewThreadData *pOldest = (CRichPreviewThreadData *)DPA_GetPtr(_hdpaCache,
                                                                               PREVIEWHOST_CACHE_MAX - 1);

        pOldest->_PostMessageToBGThread(PHM_UNLOAD, 0, 0);
        pOldest->Release();
        DPA_DeletePtr(_hdpaCache, PREVIEWHOST_CACHE_MAX - 1);
    }

    if (DPA_InsertPtr(_hdpaCache, 0, pNew) == -1)
    {
        pNew->Release();
        return E_OUTOFMEMORY;
    }

    pNew->AddRef();
    *ppData = pNew;
    return S_OK;
}

STDMETHODIMP
CPreviewHost::Load(
    IShellItem *psi)
{
    CRichPreviewThreadData *pData;
    IStream *pstm;
    CLSID clsid;
    HRESULT hr;

    if (!psi)
        return E_INVALIDARG;

    hr = GetPreviewerCLSID(psi, &clsid);
    if (FAILED(hr))
        return hr;

    pData = _FindInCache(clsid);
    if (pData)
    {
        if (_pCurrent)
            _pCurrent->_PostMessageToBGThread(PHM_UNLOAD, 0, 0);
    }
    else
    {
        hr = _CreateAndAddToCache(clsid, &pData);
        if (FAILED(hr))
            return hr;
    }

    if (_pCurrent)
        _pCurrent->Release();
    _uCookie++;
    _pCurrent = pData;

    hr = CoMarshalInterThreadInterfaceInStream(IID_IShellItem, psi, &pstm);
    if (SUCCEEDED(hr))
    {
        hr = pData->_PostMessageToBGThread(PHM_LOADFILE, (WPARAM)pstm, _uCookie);
        if (FAILED(hr))
            pstm->Release();
    }
    return hr;
}

STDMETHODIMP
CPreviewHost::Unload()
{
    _uUnloadCookie = _uCookie;
    if (!_pCurrent)
        return S_OK;

    if (_pCurrent->_hwndPreviewer)
        ShowWindow(_pCurrent->_hwndPreviewer, SW_HIDE);
    return _pCurrent->_PostMessageToBGThread(PHM_UNLOAD, 0, 0);
}

STDMETHODIMP
CPreviewHost::DoPreview()
{
    if (!_pCurrent)
        return E_UNEXPECTED;
    return _pCurrent->_PostMessageToBGThread(PHM_SHOWFILE, 0, 0);
}

STDMETHODIMP
CPreviewHost::WindowSized()
{
    RECT rc;

    if (!GetClientRect(_hwndHost, &rc))
        return HResultFromLastError();

    for (int i = 0; i < DPA_GetPtrCount(_hdpaCache); i++)
    {
        CRichPreviewThreadData *pData = (CRichPreviewThreadData *)DPA_GetPtr(_hdpaCache, i);

        if (pData)
            pData->_PostMessageToBGThread(PHM_RESIZE, 0, 0);
    }
    return S_OK;
}

STDMETHODIMP
CPreviewHost::SetFocus()
{
    if (!_pCurrent)
        return E_UNEXPECTED;
    return _pCurrent->_PostMessageToBGThread(PHM_SETFOCUS, 0, 0);
}

/* Blocks for up to five seconds while the handler thread answers */
STDMETHODIMP
CPreviewHost::QueryWindowFocus(
    HWND *phwnd)
{
    CQueryFocusParams *pParams = NULL;
    DWORD dwIndex;
    HRESULT hr;

    *phwnd = NULL;
    if (!_pCurrent)
        return S_OK;

    hr = CQueryFocusParams::CreateInstance(&pParams);
    if (FAILED(hr))
        return hr;

    pParams->AddRef();
    hr = _pCurrent->_PostMessageToBGThread(PHM_QUERYFOCUS, 0, (LPARAM)pParams);
    if (FAILED(hr))
    {
        pParams->Release();
    }
    else
    {
        hr = CoWaitForMultipleHandles(0, 5000, 1, &pParams->_hevDone, &dwIndex);
        if (SUCCEEDED(hr))
        {
            hr = pParams->_hr;
            *phwnd = pParams->_hwndFocus;
        }
    }
    pParams->Release();
    return hr;
}

/* CRichPreviewThreadData */

ULONG
CRichPreviewThreadData::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

/* The last frame side reference ends the handler thread */
ULONG
CRichPreviewThreadData::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
    {
        if (_hwndBG)
            _PostMessageToBGThread(PHM_QUIT, 0, 0);
        delete this;
    }
    return cRef;
}

HRESULT
CRichPreviewThreadData::_PostMessageToBGThread(
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (!_hwndBG)
        return E_UNEXPECTED;
    if (!PostMessageW(_hwndBG, uMsg, wParam, lParam))
        return HResultFromLastError();
    return S_OK;
}

/* CRichPreviewThreadParams */

ULONG
CRichPreviewThreadParams::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

ULONG
CRichPreviewThreadParams::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* CQueryFocusParams */

HRESULT
WINAPI
CQueryFocusParams::CreateInstance(
    _Outptr_ CQueryFocusParams **ppParams)
{
    CQueryFocusParams *pParams;

    if (*ppParams)
        return E_INVALIDARG;

    pParams = new CQueryFocusParams();
    if (!pParams)
        return E_OUTOFMEMORY;

    pParams->_cRef = 1;
    pParams->_hevDone = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!pParams->_hevDone)
    {
        HRESULT hr = HResultFromLastError();

        delete pParams;
        return hr;
    }

    *ppParams = pParams;
    return S_OK;
}

ULONG
CQueryFocusParams::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

ULONG
CQueryFocusParams::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
    {
        if (_hevDone)
            CloseHandle(_hevDone);
        delete this;
    }
    return cRef;
}

/* CRichPreviewBackgroundThreadData */

CRichPreviewBackgroundThreadData::~CRichPreviewBackgroundThreadData()
{
    _CleanupTempFile();
    if (_hevIdle)
    {
        if (_pph && ResetEvent(_hevIdle))
        {
            if (_fLoaded)
            {
                _pph->Unload();
                IUnknown_SetSite(_pph, NULL);
            }
            IUnknown_SafeReleaseAndNullPtr(&_pph);
            SetEvent(_hevIdle);
        }
        CloseHandle(_hevIdle);
    }
    DestroyWindow(_hwndBG);
    if (_fOwnPreviewerWindow && _hwndPreviewer)
        DestroyWindow(_hwndPreviewer);
    IUnknown_SafeReleaseAndNullPtr(&_powCreator);
}

STDMETHODIMP
CRichPreviewBackgroundThreadData::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CRichPreviewBackgroundThreadData, IPreviewHandlerFrame),
        QITABENT(CRichPreviewBackgroundThreadData, IHlinkFrame),
        QITABENT(CRichPreviewBackgroundThreadData, IServiceProvider),
        {0},
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CRichPreviewBackgroundThreadData::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CRichPreviewBackgroundThreadData::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* Keys the handler passes back: Alt and Ctrl letters, function keys and Tab both ways */
STDMETHODIMP
CRichPreviewBackgroundThreadData::GetWindowContext(
    PREVIEWHANDLERFRAMEINFO *pinfo)
{
    ACCEL rgAccel[26 + 26 + 12 + 2];
    UINT cAccel = 0;
    int i;

    ZeroMemory(rgAccel, sizeof(rgAccel));
    for (i = 0; i < 26; i++, cAccel++)
    {
        rgAccel[cAccel].fVirt = FVIRTKEY | FALT;
        rgAccel[cAccel].key = (WORD)('A' + i);
    }
    for (i = 0; i < 26; i++, cAccel++)
    {
        rgAccel[cAccel].fVirt = FVIRTKEY | FCONTROL;
        rgAccel[cAccel].key = (WORD)('A' + i);
    }
    for (i = 0; i < 12; i++, cAccel++)
    {
        rgAccel[cAccel].fVirt = FVIRTKEY;
        rgAccel[cAccel].key = (WORD)(VK_F1 + i);
    }
    rgAccel[cAccel].fVirt = FVIRTKEY;
    rgAccel[cAccel].key = VK_TAB;
    cAccel++;
    rgAccel[cAccel].fVirt = FVIRTKEY | FSHIFT;
    rgAccel[cAccel].key = VK_TAB;
    cAccel++;

    pinfo->haccel = CreateAcceleratorTableW(rgAccel, cAccel);
    if (!pinfo->haccel)
        return HResultFromLastError();

    pinfo->cAccelEntries = CopyAcceleratorTableW(pinfo->haccel, NULL, 0);
    return S_OK;
}

/* The key goes to the frame thread, retargeted at the host window */
STDMETHODIMP
CRichPreviewBackgroundThreadData::TranslateAccelerator(
    MSG *pmsg)
{
    MSG *pmsgCopy;

    pmsgCopy = (MSG *)LocalAlloc(LPTR, sizeof(*pmsgCopy));
    if (!pmsgCopy)
        return E_OUTOFMEMORY;

    *pmsgCopy = *pmsg;
    pmsgCopy->hwnd = _hwndHost;
    if (!PostMessageW(_hwndFGMsg, PHM_ACCELERATOR, (WPARAM)pmsgCopy, 0))
    {
        LocalFree(pmsgCopy);
        return E_FAIL;
    }
    return S_OK;
}

/* Web links clicked inside a preview open in the browser */
STDMETHODIMP
CRichPreviewBackgroundThreadData::Navigate(
    DWORD grfHLNF,
    LPBC pbc,
    IBindStatusCallback *pibsc,
    IHlink *pihlNavigate)
{
    IMoniker *pmk = NULL;
    IUriContainer *puc;
    IUri *puri;
    BSTR bstrUri = NULL;
    DWORD dwScheme;
    HRESULT hr;

    hr = pihlNavigate->GetMonikerReference(HLINKGETREF_DEFAULT, &pmk, NULL);
    if (FAILED(hr))
        return hr;

    hr = pmk->QueryInterface(IID_PPV_ARGS(&puc));
    if (SUCCEEDED(hr))
    {
        hr = puc->GetIUri(&puri);
        if (SUCCEEDED(hr) && puri)
        {
            hr = puri->GetAbsoluteUri(&bstrUri);
            if (SUCCEEDED(hr))
                hr = puri->GetScheme(&dwScheme);
            if (SUCCEEDED(hr))
            {
                if (dwScheme == URL_SCHEME_HTTP || dwScheme == URL_SCHEME_HTTPS)
                {
                    SHELLEXECUTEINFOW sei;

                    ZeroMemory(&sei, sizeof(sei));
                    sei.cbSize = sizeof(sei);
                    sei.fMask = SEE_MASK_CLASSNAME | SEE_MASK_FLAG_DDEWAIT;
                    sei.lpFile = bstrUri ? bstrUri : L"";
                    sei.lpClass = L"http";
                    sei.nShow = SW_SHOWNORMAL;
                    if (!ShellExecuteExW(&sei))
                        hr = HResultFromLastError();
                }
                else
                {
                    hr = E_FAIL;
                }
            }
            SysFreeString(bstrUri);
            puri->Release();
        }
        else if (SUCCEEDED(hr))
        {
            hr = E_FAIL;
        }
        puc->Release();
    }
    pmk->Release();
    return hr;
}

/* Handlers may ask their frame for the preview host and get the frame itself */
STDMETHODIMP
CRichPreviewBackgroundThreadData::QueryService(
    REFGUID guidService,
    REFIID riid,
    void **ppv)
{
    if (IsEqualGUID(guidService, IID_IPreviewHost))
        return QueryInterface(riid, ppv);

    *ppv = NULL;
    return E_NOINTERFACE;
}

/*
 * The window creator places the handler window in the surrogate process; without it the
 * handler draws into a child window made on this thread.
 */
HRESULT
CRichPreviewBackgroundThreadData::_InitializePreviewerWindow()
{
    HWND hwndPreviewer = NULL;
    RECT rc;
    HRESULT hr;

    if (!ImpersonateSelf(SecurityImpersonation))
        return HResultFromLastError();

    hr = CoCreateInstance(CLSID_PreviewWindowCreator, NULL, CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING,
                          IID_PPV_ARGS(&_powCreator));
    if (SUCCEEDED(hr))
        hr = _powCreator->GetWindow(&hwndPreviewer);

    if (FAILED(hr))
    {
        WNDCLASSEXW wc;

        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = _hinst;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = c_szPreviewerClass;
        RegisterClassExW(&wc);

        hwndPreviewer = CreateWindowExW(0, c_szPreviewerClass, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0,
                                        _hwndHost, NULL, _hinst, NULL);
        hr = hwndPreviewer ? S_OK : HResultFromLastError();
        _fOwnPreviewerWindow = SUCCEEDED(hr);
    }

    if (SUCCEEDED(hr))
    {
        if (GetClientRect(_hwndHost, &rc) && (_fOwnPreviewerWindow || SetParent(hwndPreviewer, _hwndHost)))
        {
            MoveWindow(hwndPreviewer, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
            ShowWindow(hwndPreviewer, SW_SHOW);
            _hwndPreviewer = hwndPreviewer;
        }
        else
        {
            hr = HResultFromLastError();
        }
    }

    RevertToSelf();
    return hr;
}

HRESULT
CRichPreviewBackgroundThreadData::_InitializeMessageWindow()
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_GLOBALCLASS;
    wc.lpfnWndProc = s_BGWndProc;
    wc.hInstance = _hinst;
    wc.lpszClassName = c_szBGMsgClass;
    RegisterClassExW(&wc);

    _hwndBG = CreateWindowExW(0, c_szBGMsgClass, NULL, WS_DISABLED, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, this);
    return _hwndBG ? S_OK : HResultFromLastError();
}

/* Runs on the new thread before SHCreateThread returns: builds the windows and reports them */
DWORD
WINAPI
CRichPreviewBackgroundThreadData::s_SyncThreadProc(
    _In_ void *pv)
{
    CRichPreviewThreadParams *pParams = (CRichPreviewThreadParams *)pv;
    CRichPreviewBackgroundThreadData *pData;
    HRESULT hr = E_OUTOFMEMORY;

    pParams->AddRef();
    pParams->_hwndBG = NULL;
    pParams->_hevIdle = NULL;
    pParams->_hr = E_UNEXPECTED;

    pData = new CRichPreviewBackgroundThreadData();
    if (pData)
    {
        pData->_hinst = pParams->_hinst;
        pData->_clsid = pParams->_clsid;
        pData->_hwndFGMsg = pParams->_hwndFGMsg;
        pData->_hwndHost = pParams->_hwndHost;
        pData->_hevIdle = CreateEventW(NULL, TRUE, TRUE, NULL);
        hr = pData->_hevIdle ? S_OK : HResultFromLastError();
        if (SUCCEEDED(hr))
            hr = pData->_InitializePreviewerWindow();
        if (SUCCEEDED(hr))
            hr = pData->_InitializeMessageWindow();

        if (SUCCEEDED(hr))
        {
            pParams->_hwndBG = pData->_hwndBG;
            pParams->_hwndPreviewer = pData->_hwndPreviewer;
            pParams->_hevIdle = pData->_hevIdle;
            pParams->_pData = pData;
            pData->AddRef();
        }
        pData->Release();
    }

    pParams->_hr = hr;
    if (FAILED(hr))
    {
        pParams->_hwndBG = NULL;
        pParams->_hwndPreviewer = NULL;
        pParams->_hevIdle = NULL;
        pParams->_pData = NULL;
    }
    return 0;
}

/* The handler thread pumps messages until the frame side lets it go */
DWORD
WINAPI
CRichPreviewBackgroundThreadData::s_ThreadProc(
    _In_ void *pv)
{
    CRichPreviewThreadParams *pParams = (CRichPreviewThreadParams *)pv;
    BOOL fRun = TRUE;
    MSG msg;

    if (!pParams)
        return 0;

    if (SUCCEEDED(pParams->_hr))
    {
        while (fRun)
        {
            if (MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0)
                continue;

            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    fRun = FALSE;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (pParams->_pData)
    {
        pParams->_pData->Release();
        pParams->_pData = NULL;
    }
    pParams->Release();
    return 0;
}

LRESULT
CALLBACK
CRichPreviewBackgroundThreadData::s_BGWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    CRichPreviewBackgroundThreadData *pThis =
        (CRichPreviewBackgroundThreadData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_CREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lParam)->lpCreateParams);
            break;

        case PHM_QUIT:
            PostQuitMessage(0);
            break;

        case PHM_LOADFILE:
            if (pThis && wParam)
                pThis->_DoLoadFile((IStream *)wParam, (UINT)lParam);
            break;

        case PHM_SHOWFILE:
            if (pThis)
                pThis->_DoShowFile();
            break;

        case PHM_UNLOAD:
            if (pThis)
            {
                HRESULT hr = pThis->_UnloadNow();

                PostMessageW(pThis->_hwndFGMsg, PHM_UNLOADED, pThis->_uCookie, hr);
            }
            break;

        case PHM_RESIZE:
            if (pThis)
                pThis->_DoWindowResized();
            break;

        case PHM_SETFOCUS:
            if (pThis && pThis->_pph && ResetEvent(pThis->_hevIdle))
            {
                pThis->_pph->SetFocus();
                SetEvent(pThis->_hevIdle);
            }
            break;

        case PHM_QUERYFOCUS:
            if (lParam)
            {
                CQueryFocusParams *pParams = (CQueryFocusParams *)lParam;

                if (pThis)
                {
                    if (pThis->_pph)
                    {
                        pParams->_hr = ResetEvent(pThis->_hevIdle) ? S_OK : HResultFromLastError();
                        if (SUCCEEDED(pParams->_hr))
                        {
                            pParams->_hr = pThis->_pph->QueryFocus(&pParams->_hwndFocus);
                            SetEvent(pThis->_hevIdle);
                        }
                    }
                    else
                    {
                        pParams->_hr = E_UNEXPECTED;
                    }
                    if (pParams->_hevDone)
                        SetEvent(pParams->_hevDone);
                }
                pParams->Release();
            }
            break;

        case PHM_OPLOCKBROKEN:
            if (pThis)
            {
                pThis->_UnloadNow();
                pThis->_NotifyError(PREVIEW_E_FILEINUSE);
            }
            break;

        default:
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

void
CRichPreviewBackgroundThreadData::_NotifyError(
    _In_ HRESULT hrReason)
{
    NotifyError *pError = new NotifyError;

    if (pError)
    {
        pError->hrReason = hrReason;
        pError->clsidPreviewer = _clsid;
        PostMessageW(_hwndFGMsg, PHM_ERROR, _uCookie, (LPARAM)pError);
    }
}

/* Stream initialization: the item's own stream, or the file opened for reading */
BOOL
CRichPreviewBackgroundThreadData::_InitializeWithStream(
    _In_ IShellItem *psi,
    _In_ IInitializeWithStream *piws)
{
    IStream *pstm = NULL;
    PWSTR pszPath = NULL;
    BOOL fInitialized = FALSE;
    HRESULT hr;

    psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);

    hr = psi->BindToHandler(NULL, BHID_Stream, IID_PPV_ARGS(&pstm));
    if (FAILED(hr) && pszPath)
        hr = SHCreateStreamOnFileEx(pszPath, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, NULL,
                                    &pstm);
    if (SUCCEEDED(hr))
    {
        fInitialized = SUCCEEDED(piws->Initialize(pstm, STGM_READ));
        pstm->Release();
    }

    CoTaskMemFree(pszPath);
    return fInitialized;
}

/* Stream first, then the item, then a file path */
HRESULT
CRichPreviewBackgroundThreadData::_InitializePreviewer(
    _In_ IShellItem *psi)
{
    IInitializeWithStream *piws;
    IInitializeWithItem *piwi;
    IInitializeWithFile *piwf;
    BOOL fInitialized = FALSE;
    BOOL fTried = FALSE;
    PWSTR pszPath;

    if (SUCCEEDED(_pph->QueryInterface(IID_PPV_ARGS(&piws))))
    {
        fInitialized = _InitializeWithStream(psi, piws);
        piws->Release();
    }

    if (!fInitialized && SUCCEEDED(_pph->QueryInterface(IID_PPV_ARGS(&piwi))))
    {
        fInitialized = SUCCEEDED(piwi->Initialize(psi, STGM_READ));
        piwi->Release();
    }

    if (!fInitialized && SUCCEEDED(_pph->QueryInterface(IID_PPV_ARGS(&piwf))))
    {
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
        {
            if (SUCCEEDED(piwf->Initialize(pszPath, STGM_READ)))
                fInitialized = TRUE;
            else
                fTried = TRUE;
            CoTaskMemFree(pszPath);
        }
        piwf->Release();
    }

    if (!fInitialized)
    {
        _NotifyError(fTried ? PREVIEW_E_PREVIEWERFAILED : PREVIEW_E_CANNOTPREVIEW);
        return E_NOINTERFACE;
    }
    return S_OK;
}

/* Handlers run out of process when their surrogate is available, in process otherwise */
static
HRESULT
CreatePreviewHandler(
    _In_ REFCLSID rclsid,
    _COM_Outptr_ IPreviewHandler **ppph)
{
    HRESULT hr;

    hr = CoCreateInstance(rclsid, NULL, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(ppph));
    if (FAILED(hr))
        hr = CoCreateInstance(rclsid, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(ppph));
    return hr;
}

void
CRichPreviewBackgroundThreadData::_DoLoadFile(
    _In_ IStream *pstm,
    _In_ UINT uCookie)
{
    IShellItem *psi;
    HRESULT hr;

    _uCookie = uCookie;
    hr = CoGetInterfaceAndReleaseStream(pstm, IID_PPV_ARGS(&psi));
    if (SUCCEEDED(hr))
    {
        if (!AllowPreviewForItem(psi))
        {
            hr = E_FAIL;
        }
        else if (!ResetEvent(_hevIdle))
        {
            hr = HResultFromLastError();
        }
        else
        {
            if (_pph)
            {
                if (_fLoaded)
                {
                    _pph->Unload();
                    IUnknown_SetSite(_pph, NULL);
                    _CleanupTempFile();
                    _fLoaded = FALSE;
                }
            }
            else
            {
                hr = CreatePreviewHandler(_clsid, &_pph);
                if (FAILED(hr))
                    _NotifyError(PREVIEW_E_PREVIEWERFAILED);
            }

            if (SUCCEEDED(hr))
            {
                hr = _InitializePreviewer(psi);
                if (FAILED(hr))
                {
                    IUnknown_SafeReleaseAndNullPtr(&_pph);
                }
                else
                {
                    IUnknown_SetSite(_pph, static_cast<IServiceProvider *>(this));
                    _fLoaded = TRUE;
                }
            }
            SetEvent(_hevIdle);
        }
        psi->Release();
    }
    PostMessageW(_hwndFGMsg, PHM_LOADED, _uCookie, hr);
}

/* The handler gets its window, the system colors and message font, then draws */
void
CRichPreviewBackgroundThreadData::_DoShowFile()
{
    IPreviewHandlerVisuals *pphv;
    HRESULT hr = E_FAIL;

    if (_pph && _fLoaded)
    {
        hr = ResetEvent(_hevIdle) ? S_OK : HResultFromLastError();
        if (SUCCEEDED(hr))
        {
            if (_hwndPreviewer)
            {
                RECT rc = { 0, 0, 0, 0 };

                GetClientRect(_hwndPreviewer, &rc);
                hr = _pph->SetWindow(_hwndPreviewer, &rc);
                if (SUCCEEDED(hr))
                {
                    if (SUCCEEDED(_pph->QueryInterface(IID_PPV_ARGS(&pphv))))
                    {
                        NONCLIENTMETRICSW ncm;

                        pphv->SetBackgroundColor(GetSysColor(COLOR_WINDOW));
                        pphv->SetTextColor(GetSysColor(COLOR_WINDOWTEXT));
                        ZeroMemory(&ncm, sizeof(ncm));
                        ncm.cbSize = sizeof(ncm);
                        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
                            pphv->SetFont(&ncm.lfMessageFont);
                        pphv->Release();
                    }
                    hr = _pph->DoPreview();
                }
            }
            else
            {
                hr = E_FAIL;
            }
            SetEvent(_hevIdle);
        }
    }
    PostMessageW(_hwndFGMsg, PHM_SHOWN, _uCookie, hr);
}

/* Local copies made for previewing are deleted when the handler lets go of them */
void
CRichPreviewBackgroundThreadData::_CleanupTempFile()
{
    WCHAR szCache[MAX_PATH];

    if (_pszTempFile && PathFileExistsW(_pszTempFile) &&
        SHGetSpecialFolderPathW(NULL, szCache, CSIDL_INTERNET_CACHE, FALSE) &&
        PathIsPrefixW(szCache, _pszTempFile))
    {
        DeleteFileW(_pszTempFile);
    }
    CoTaskMemFree(_pszTempFile);
    _pszTempFile = NULL;
}

HRESULT
CRichPreviewBackgroundThreadData::_UnloadNow()
{
    HRESULT hr = E_FAIL;

    if (!_pph || !_fLoaded || !ResetEvent(_hevIdle))
        return hr;

    hr = _pph->Unload();
    IUnknown_SetSite(_pph, NULL);
    _CleanupTempFile();
    _fLoaded = FALSE;
    SetEvent(_hevIdle);
    return hr;
}

void
CRichPreviewBackgroundThreadData::_DoWindowResized()
{
    RECT rc = { 0, 0, 0, 0 };

    if (!_pph || !ResetEvent(_hevIdle))
        return;

    if (GetClientRect(_hwndHost, &rc))
    {
        MoveWindow(_hwndPreviewer, 0, 0, rc.right - rc.left, rc.bottom - rc.top, FALSE);
        _pph->SetRect(&rc);
    }
    SetEvent(_hevIdle);
}
