/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Command bar module: the folder band and its fixed buttons
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define PROPERTY_STORE_LOCAL     1
#define PROPERTY_STORE_SPECIFIED 2

/* UIFILE holding the "FolderBand" markup */
#define IDR_UIFILE_FOLDERBAND   32

/* Preview pane button icons and descriptions; the second of each pair is for an open pane */
#define IDI_PREVIEWPANE_SHOW    16757
#define IDS_PREVIEWPANE_SHOW    50209

/* Browser command that opens help */
#define FCIDM_HELPCONTENTS      0x7091

/* Worker window timer that rebuilds the command buttons */
#define IDT_REFRESHBUTTONS      1
#define REFRESH_DELAY_FIRST     20
#define REFRESH_DELAY           100

/* Explorer pane state bits */
#define EPS_FORCED              0x20000

/* Group policy that removes the preview pane */
static const GUID c_polidNoReadingPane =
    { 0x33dcce8d, 0xb6f8, 0x4b7a, { 0xac, 0x93, 0x45, 0xe3, 0x01, 0xf9, 0x56, 0xaa } };

/* Namespace that lists the commands of the folder in view */
static const WCHAR c_szCommandFolder[] = L"shell:::{437ff9c0-a07f-4fa0-af80-84b6c6440a16}";
static const WCHAR c_szFolderBandInner[] = L"FolderBandModuleInner";
static const WCHAR c_szReadingPaneVisible[] = L"ReadingPaneSizer_Visible";

DirectUI::IClassInfo *Button3d::Class = NULL;
DirectUI::IClassInfo *CCommandModuleInner::Class = NULL;

static const DirectUI::_StaticValue c_svIntOne = { (int)DirectUI::ValueType::Int, 0, -1, 1 };
static DirectUI::Value *WINAPI svIntOne() { return (DirectUI::Value *)&c_svIntOne; }

static DirectUI::PropCapability s_pcInt = { DirectUI::ValueType::Int, 0, UINT_MAX };
static DirectUI::PropCapability s_pcBool = { DirectUI::ValueType::Bool, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidPressedPaddingShift;
static DirectUI::PropertyInfoData s_pidEvaluatingAsyncState;

static const DirectUI::PropertyInfo s_piPressedPaddingShift =
    { L"PressedPaddingShift", 6, 0x20001, &s_pcInt, NULL, svIntOne, &s_pidPressedPaddingShift };
static const DirectUI::PropertyInfo s_piEvaluatingAsyncState =
    { L"EvaluatingAsyncState", 6, 0x20001, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse,
      &s_pidEvaluatingAsyncState };

static const DirectUI::PropertyInfo *const c_rgpiButton3d[] =
{
    &s_piPressedPaddingShift,
    &s_piEvaluatingAsyncState,
};

const DirectUI::PropertyInfo *const Button3d::PressedPaddingShiftProp = &s_piPressedPaddingShift;
const DirectUI::PropertyInfo *const Button3d::EvaluatingAsyncStateProp = &s_piEvaluatingAsyncState;

/* Button3d */

HRESULT
WINAPI
Button3d::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    Button3d *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<Button3d>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->DirectUI::Button::Initialize(0, peParent, pdwDeferCookie);
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
Button3d::Register()
{
    return DirectUI::ClassInfo<Button3d, DirectUI::Button, DirectUI::StandardCreator<Button3d>>::Register(
        DUIFRAME_HINST, L"Button3d", c_rgpiButton3d, _countof(c_rgpiButton3d), false);
}

/* While pressed the content moves down and right by PressedPaddingShift */
void
Button3d::OnPropertyChanged(
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Button::PressedProp() && (int)(ppi->Flags & 3) == iIndex)
    {
        DirectUI::Value *pvPadding;
        DirectUI::Value *pvShift;
        RECT rcPadding;
        int iShift;

        rcPadding = *GetPadding(&pvPadding);
        pvShift = GetValue(PressedPaddingShiftProp, PROPERTY_STORE_SPECIFIED, NULL);
        iShift = pvShift->GetInt();
        pvShift->Release();

        if (!pvNew->GetBool())
            iShift = -iShift;
        SetPadding(rcPadding.left + iShift, rcPadding.top + iShift, rcPadding.right - iShift,
                   rcPadding.bottom - iShift);
        pvPadding->Release();
    }
    DirectUI::Button::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

HRESULT
Button3d::SetEvaluatingAsyncState(
    _In_ bool fEvaluating)
{
    DirectUI::Value *pv = DirectUI::Value::CreateBool(fEvaluating);
    HRESULT hr;

    if (!pv)
        return E_OUTOFMEMORY;

    hr = SetValue(EvaluatingAsyncStateProp, PROPERTY_STORE_LOCAL, pv);
    pv->Release();
    return hr;
}

/* Gives an element a small icon as its content; the icon belongs to the element afterwards */
static
HRESULT
SetElementIcon(
    _In_ DirectUI::Element *pe,
    _In_ HICON hicon)
{
    DirectUI::Value *pv = DirectUI::Value::CreateGraphic(hicon, false, false, false);
    HRESULT hr;

    if (!pv)
    {
        DestroyIcon(hicon);
        return E_OUTOFMEMORY;
    }

    hr = pe->SetValue(DirectUI::Element::ContentProp, PROPERTY_STORE_LOCAL, pv);
    pv->Release();
    return hr;
}

/*
 * Whether a pane may show and whether it is open. Bit 2 means the pane is unavailable;
 * otherwise bit 4 says the state is known and bit 8 that it is open.
 */
static
HRESULT
CanShowPane(
    _In_ PCWSTR pszVisibleProp,
    _In_opt_ const GUID *ppolid,
    _In_ REFEXPLORERPANE ep,
    _In_opt_ IUnknown *punkSite,
    _Out_ DWORD *pdwState)
{
    EXPLORERPANESTATE eps;
    IPropertyBag *ppb;

    *pdwState = 2;
    IUnknown_GetExplorerPaneState(punkSite, ep, &eps);
    if (eps & EPS_FORCED)
        return S_OK;

    if (ppolid)
    {
        DWORD dwPolicy = 0;
        DWORD cbPolicy = sizeof(dwPolicy);

        if (SUCCEEDED(SHWindowsPolicyGetValue(*ppolid, &dwPolicy, &cbPolicy)) && dwPolicy)
            return S_OK;
    }

    if (SUCCEEDED(IUnknown_QueryService(punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        BOOL fVisible;

        if (SUCCEEDED(SHPropertyBag_ReadBOOL(ppb, pszVisibleProp, &fVisible)))
            *pdwState = fVisible ? (4 | 8) : 4;
        ppb->Release();
    }
    return S_OK;
}

/* CCommandModuleInner */

HRESULT
WINAPI
CCommandModuleInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Element *peCreated;
    DirectUI::Layout *pLayout = NULL;
    CCommandModuleInner *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CCommandModuleInner>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(0);
    if (SUCCEEDED(hr))
        hr = DirectUI::ShellBorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        hr = pe->SetLayout(pLayout);

    /* The band markup fills in this element: background, overflow, preview and help buttons */
    if (SUCCEEDED(hr))
    {
        hr = DUI_CreateElementFromResource(DUIFRAME_HINST, IDR_UIFILE_FOLDERBAND, L"FolderBand", pe, NULL, NULL,
                                           &peCreated, NULL);
    }

    if (SUCCEEDED(hr))
    {
        pe->_hwndWorker = SHCreateWorkerWindowW(s_WndProc, NULL, 0, 0, NULL, (LONG_PTR)pe);
        if (!pe->_hwndWorker)
            hr = E_FAIL;
    }

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
CCommandModuleInner::Register()
{
    return DirectUI::ClassInfo<CCommandModuleInner, CElementWithSite,
                               DirectUI::StandardCreator<CCommandModuleInner>>::Register(
        DUIFRAME_HINST, c_szFolderBandInner, NULL, 0, false);
}

STDMETHODIMP
CCommandModuleInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CCommandModuleInner, IFrameNotificationClient),
        QITABENT(CCommandModuleInner, IFrameShellViewClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

STDMETHODIMP
CCommandModuleInner::SetSite(
    IUnknown *punkSite)
{
    IUnknown_Set(&_punkSite, punkSite);
    return S_OK;
}

void
CCommandModuleInner::OnDestroy()
{
    DirectUI::Element::OnDestroy();
    if (_hwndWorker)
    {
        DestroyWindow(_hwndWorker);
        _hwndWorker = NULL;
    }
    _EndRefreshTracking();
    IUnknown_SafeReleaseAndNullPtr(&_pte);
}

LRESULT
CALLBACK
CCommandModuleInner::s_WndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    if (uMsg == WM_TIMER && wParam == IDT_REFRESHBUTTONS)
    {
        CCommandModuleInner *pThis = (CCommandModuleInner *)GetWindowLongPtrW(hwnd, 0);

        KillTimer(hwnd, IDT_REFRESHBUTTONS);
        if (pThis)
        {
            pThis->_RefreshButtons();
            pThis->_EndRefreshTracking();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void
CCommandModuleInner::_EndRefreshTracking()
{
    if (!_fRefreshPending)
        return;

    _fRefreshPending = FALSE;
    if (_pte)
        _pte->OperationEvent(c_szFolderBandInner, (TRACKOPERATIONTYPE)0, TOE_END);
}

/*
 * The buttons come from the command folder of the view. Building them needs that folder;
 * without it the band keeps only its fixed buttons.
 */
void
CCommandModuleInner::_RefreshButtons()
{
    PIDLIST_ABSOLUTE pidl;
    IShellFolder *psf;

    _fRefreshedOnce = TRUE;
    if (FAILED(SHParseDisplayName(c_szCommandFolder, NULL, &pidl, 0, NULL)))
        return;

    if (SUCCEEDED(SHBindToObject(NULL, pidl, NULL, IID_PPV_ARGS(&psf))))
    {
        /* TODO: per command buttons (split buttons and the view control) arrive with the command folder */
        psf->Release();
    }
    ILFree(pidl);
}

/* Schedules a rebuild unless the view reports the same settled state as last time */
void
CCommandModuleInner::_InitRefreshButtons(
    _In_ BOOL fDelay)
{
    IFolderView *pfv;
    ULONG dwDeferCookie = 0;
    BOOL fRefresh = TRUE;

    StartDefer(&dwDeferCookie);
    RemoveLocalValue(DirectUI::Element::HeightProp);
    RemoveLocalValue(DirectUI::Element::LayoutPosProp);

    if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv))))
    {
        IViewStatus *pvs;
        int cSelected;

        if (SUCCEEDED(pfv->ItemCount(SVGIO_SELECTION, &cSelected)) && !cSelected &&
            SUCCEEDED(pfv->QueryInterface(IID_IViewStatus, (void **)&pvs)))
        {
            DEFVIEWSTATUS dvs;
            UINT uOperation;

            if (SUCCEEDED(pvs->GetStatus(&dvs)) && (dvs & DVS_INCREMENTALFILLING) &&
                SUCCEEDED(pvs->GetOperationID(&uOperation)))
            {
                if (uOperation == _uViewOperation)
                    fRefresh = FALSE;
                else
                    _uViewOperation = uOperation;
            }
            pvs->Release();
        }
        pfv->Release();
    }

    if (fRefresh)
    {
        if (!fDelay)
            SetTimer(_hwndWorker, IDT_REFRESHBUTTONS, REFRESH_DELAY, NULL);
        else if (!_fRefreshPending)
            PostMessageW(_hwndWorker, WM_TIMER, IDT_REFRESHBUTTONS, 0);
        else
            SetTimer(_hwndWorker, IDT_REFRESHBUTTONS, _fRefreshedOnce ? REFRESH_DELAY : REFRESH_DELAY_FIRST, NULL);

        if (!_fRefreshPending)
        {
            _fRefreshPending = TRUE;
            if (!_pte)
                IUnknown_QueryService(_punkSite, IID_ITrackEvents, IID_PPV_ARGS(&_pte));
            if (_pte)
                _pte->OperationEvent(c_szFolderBandInner, (TRACKOPERATIONTYPE)0, TOE_START);
        }
    }
    else if (_fRefreshPending && fDelay && !_fRefreshedOnce)
    {
        SetTimer(_hwndWorker, IDT_REFRESHBUTTONS, 0, NULL);
    }

    if (dwDeferCookie)
        EndDefer(dwDeferCookie);
}

/* The band goes away when the layout or the browser hides it, and comes back otherwise */
void
CCommandModuleInner::_DeferRefreshButtons(
    _In_ BOOL fDelay)
{
    BOOL fWasHidden = _fNoFolderBand;
    IPropertyBag *ppb;

    _fNoFolderBand = FALSE;
    if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_PerLayoutPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        SHPropertyBag_ReadBOOL(ppb, L"NoFolderBand", &_fNoFolderBand);
        ppb->Release();
    }

    if (!_fNoFolderBand)
    {
        EXPLORERPANESTATE eps = EPS_DONTCARE;
        EXPLORERPANE ep = EP_Commands;

        IUnknown_GetExplorerPaneState(_punkSite, &ep, &eps);
        _fNoFolderBand = (eps & EPS_DEFAULT_OFF) != 0;
    }

    if (!_fNoFolderBand)
    {
        _InitRefreshButtons(fDelay);
    }
    else if (!fWasHidden)
    {
        ULONG dwDeferCookie = 0;

        KillTimer(_hwndWorker, IDT_REFRESHBUTTONS);
        _EndRefreshTracking();

        StartDefer(&dwDeferCookie);
        SetHeight(0);
        SetLayoutPos(ROSUI_LP_NONE);
        if (dwDeferCookie)
            EndDefer(dwDeferCookie);
    }
    _UpdatePreviewButton();
}

/* The preview button shows the state of the reading pane, and goes when the pane cannot show */
void
CCommandModuleInner::_UpdatePreviewButton()
{
    DirectUI::Element *pePreview = FindDescendent(StrToID(L"PreviewButton"));
    EXPLORERPANE ep = EP_PreviewPane;
    IPropertyBag *ppb;
    DWORD dwState;

    if (!pePreview)
        return;

    if (FAILED(CanShowPane(c_szReadingPaneVisible, &c_polidNoReadingPane, &ep, _punkSite, &dwState)) ||
        (dwState & 2))
    {
        pePreview->SetVisible(false);
        pePreview->SetEnabled(false);
        pePreview->SetLayoutPos(ROSUI_LP_NONE);
        return;
    }

    pePreview->SetVisible(true);
    pePreview->SetEnabled(true);
    pePreview->SetLayoutPos(ROSUI_LP_RIGHT);

    if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        BOOL fVisible = PropertyBag_ReadBOOLDefault(ppb, c_szReadingPaneVisible, TRUE);
        UINT uOffset = fVisible ? 1 : 0;
        WCHAR szDesc[200];
        HICON hicon;

        if (SUCCEEDED(LoadIconWithScaleDown(DUIFRAME_HINST, MAKEINTRESOURCEW(IDI_PREVIEWPANE_SHOW + uOffset),
                                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                            &hicon)))
        {
            SetElementIcon(pePreview, hicon);
        }

        if (LoadStringW(DUIFRAME_HINST, IDS_PREVIEWPANE_SHOW + uOffset, szDesc, _countof(szDesc)))
            pePreview->SetAccDesc(szDesc);

        if (fVisible)
            pePreview->SetAccState(GetAccState() | STATE_SYSTEM_PRESSED);
        else
            pePreview->SetAccState(GetAccState() & ~STATE_SYSTEM_PRESSED);
        ppb->Release();
    }
}

void
CCommandModuleInner::OnEvent(
    DirectUI::Event *pev)
{
    if (pev->nStage == GMF_BUBBLED && pev->uidType.m_Value == DirectUI::Button::Click().m_Value &&
        pev->peTarget && pev->peTarget->GetID())
    {
        ATOM atomID = pev->peTarget->GetID();

        if (atomID == StrToID(L"HelpButton"))
        {
            IShellBrowser *psb;
            HWND hwndBrowser = NULL;

            if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&psb))))
            {
                psb->GetWindow(&hwndBrowser);
                psb->Release();
            }
            PostMessageW(hwndBrowser, WM_COMMAND, FCIDM_HELPCONTENTS, 0);
            pev->fHandled = true;
        }
        else if (atomID == StrToID(L"PreviewButton"))
        {
            IPropertyBag *ppb;

            if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
            {
                BOOL fVisible = PropertyBag_ReadBOOLDefault(ppb, c_szReadingPaneVisible, TRUE);

                SHPropertyBag_WriteBOOL(ppb, c_szReadingPaneVisible, !fVisible);
                ppb->Release();
            }
            pev->fHandled = true;
        }
        else if (atomID == StrToID(L"OverflowButton"))
        {
            /* The overflow menu lists commands of the command folder that did not fit */
            pev->fHandled = true;
        }
    }

    if (!pev->fHandled)
        DirectUI::Element::OnEvent(pev);
}

STDMETHODIMP
CCommandModuleInner::LayoutInitialized()
{
    _fRefreshedOnce = FALSE;
    _DeferRefreshButtons(FALSE);
    return S_OK;
}

STDMETHODIMP
CCommandModuleInner::Notify(
    PCWSTR pszPropertyName)
{
    if (!StrCmpICW(pszPropertyName, L"NoFolderBand") || !StrCmpICW(pszPropertyName, L"SettingsChanged"))
        _DeferRefreshButtons(FALSE);
    else if (!StrCmpICW(pszPropertyName, c_szReadingPaneVisible))
        _UpdatePreviewButton();
    return S_OK;
}

STDMETHODIMP
CCommandModuleInner::OnNavigateAway()
{
    _uViewOperation = (UINT)-1;
    return S_OK;
}

STDMETHODIMP
CCommandModuleInner::OnSelectionChanged()
{
    _uViewOperation = (UINT)-1;
    _DeferRefreshButtons(FALSE);
    return S_OK;
}

STDMETHODIMP
CCommandModuleInner::OnContentsChanged()
{
    _DeferRefreshButtons(TRUE);
    return S_OK;
}

STDMETHODIMP
CCommandModuleInner::OnFolderChanged()
{
    _DeferRefreshButtons(FALSE);
    return S_OK;
}
