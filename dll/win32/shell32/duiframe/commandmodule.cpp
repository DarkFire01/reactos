/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Command bar module: the folder band, its command buttons and their sites
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define PROPERTY_STORE_LOCAL     1
#define PROPERTY_STORE_SPECIFIED 2

/* UIFILEs: the band, then the split button, the plain button and the view control markup */
#define IDR_UIFILE_FOLDERBAND       32
#define IDR_UIFILE_SPLITBUTTON      43
#define IDR_UIFILE_TASKBUTTON       44
#define IDR_UIFILE_VIEWCONTROL      45

/* Preview pane button icons and descriptions; the second of each pair is for an open pane */
#define IDI_PREVIEWPANE_SHOW    16757
#define IDS_PREVIEWPANE_SHOW    50209

/* Browser command that opens help */
#define FCIDM_HELPCONTENTS      0x7091

/* Worker window timer that rebuilds the command buttons */
#define IDT_REFRESHBUTTONS      1
#define REFRESH_DELAY_FIRST     20
#define REFRESH_DELAY           100

/* Layout position that takes an element out of the layout */
#define LAYOUTPOS_NONE          (-3)

/* Explorer pane state bit for a pane that is off by default */
#define EPS_DEFAULT_OFF_BIT     0x2

/* Fixed buttons of the band ahead of the command buttons: overflow, help and preview */
#define FIXED_BUTTON_COUNT      3

static const WCHAR c_szFolderBandInner[] = L"FolderBandModuleInner";
static const WCHAR c_szReadingPaneVisible[] = L"ReadingPaneSizer_Visible";

DirectUI::IClassInfo *Button3d::Class = NULL;
DirectUI::IClassInfo *CCommandModuleInner::Class = NULL;

static const DirectUI::_StaticValue c_svIntOne = { (int)DirectUI::ValueType::Int, 0, -1, 1 };
static DirectUI::Value *WINAPI svIntOne() { return (DirectUI::Value *)&c_svIntOne; }

static DirectUI::PropCapability s_pcInt = { DirectUI::ValueType::Int, 0, UINT_MAX };
static DirectUI::PropCapability s_pcBool = { DirectUI::ValueType::Bool, 0, UINT_MAX };
static DirectUI::PropCapability s_pcString = { DirectUI::ValueType::String, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidPressedPaddingShift;
static DirectUI::PropertyInfoData s_pidEvaluatingAsyncState;
static DirectUI::PropertyInfoData s_pidSplitButtonSheet;
static DirectUI::PropertyInfoData s_pidTaskButtonSheet;

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

/* Style sheets the command module's buttons use; control panel layouts name their own */
static const DirectUI::PropertyInfo s_piSplitButtonSheet =
    { L"SplitButtonSheet", 2, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidSplitButtonSheet };
static const DirectUI::PropertyInfo s_piTaskButtonSheet =
    { L"TaskButtonSheet", 2, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidTaskButtonSheet };

const DirectUI::PropertyInfo *const c_rgpiCommandModule[] =
{
    &s_piSplitButtonSheet,
    &s_piTaskButtonSheet,
};
const UINT c_cpiCommandModule = _countof(c_rgpiCommandModule);

/* One parser per command element UIFILE, kept for the module's lifetime */
typedef struct _COMMANDPARSER
{
    UINT uID;
    DirectUI::DUIXmlParser *pParser;
    CCommandModuleInner *pModule;
} COMMANDPARSER;

/* A command shown, or about to be shown, on the band */
typedef struct _COMMANDBUTTON
{
    PITEMID_CHILD pidl;
    GUID guidCanonical;
    DirectUI::Element *pe;
} COMMANDBUTTON;

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


/* CCommandModuleSite */

CCommandModuleSite::CCommandModuleSite() :
    _cRef(1)
{
}

CCommandModuleSite::~CCommandModuleSite()
{
    IUnknown_SafeReleaseAndNullPtr(&_ppbCommands);
}

STDMETHODIMP
CCommandModuleSite::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CCommandModuleSite, IObjectWithSite),
        QITABENT(CCommandModuleSite, IServiceProvider),
        QITABENT(CCommandModuleSite, IOleWindow),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CCommandModuleSite::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CCommandModuleSite::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* Commands of one refresh share a property bag; everything else goes to the frame */
STDMETHODIMP
CCommandModuleSite::QueryService(
    REFGUID guidService,
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (IsEqualGUID(guidService, SID_CommandsPropertyBag))
    {
        if (!_ppbCommands)
        {
            HRESULT hr = SHCreatePropertyBagOnMemory(STGM_READWRITE, IID_PPV_ARGS(&_ppbCommands));

            if (FAILED(hr))
                return hr;
        }
        return _ppbCommands->QueryInterface(riid, ppv);
    }

    if (IsEqualGUID(guidService, SID_CommandModuleSite))
        return QueryInterface(riid, ppv);

    return IUnknown_QueryService(_punkSite, guidService, riid, ppv);
}

STDMETHODIMP
CCommandModuleSite::GetWindow(
    HWND *phwnd)
{
    return IUnknown_GetWindow(_punkSite, phwnd);
}

VOID
CCommandModuleSite::ResetCommandsPropertyBag()
{
    IUnknown_SafeReleaseAndNullPtr(&_ppbCommands);
}

/* CCommandButtonSite */

CCommandButtonSite::CCommandButtonSite() :
    _cRef(1)
{
}

CCommandButtonSite::~CCommandButtonSite()
{
}

HRESULT
WINAPI
CCommandButtonSite::s_CreateInstance(
    _In_ IUnknown *punkSite,
    _In_opt_ HWND hwnd,
    _In_ const RECT *prc,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CCommandButtonSite *pSite;
    HRESULT hr;

    *ppv = NULL;
    pSite = new CCommandButtonSite();
    if (!pSite)
        return E_OUTOFMEMORY;

    IUnknown_Set(&pSite->_punkSite, punkSite);
    pSite->_hwnd = hwnd;
    pSite->_rc = *prc;

    hr = pSite->QueryInterface(riid, ppv);
    pSite->Release();
    return hr;
}

STDMETHODIMP
CCommandButtonSite::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CCommandButtonSite, IObjectWithSite),
        QITABENT(CCommandButtonSite, IServiceProvider),
        QITABENTMULTI(CCommandButtonSite, IOleWindow, ICommandButtonInfo),
        QITABENT(CCommandButtonSite, ICommandButtonInfo),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CCommandButtonSite::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CCommandButtonSite::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CCommandButtonSite::QueryService(
    REFGUID guidService,
    REFIID riid,
    void **ppv)
{
    if (IsEqualGUID(guidService, IID_ICommandButtonInfo))
        return QueryInterface(riid, ppv);
    return IUnknown_QueryService(_punkSite, guidService, riid, ppv);
}

STDMETHODIMP
CCommandButtonSite::GetWindow(
    HWND *phwnd)
{
    *phwnd = _hwnd;
    return _hwnd ? S_OK : E_FAIL;
}

STDMETHODIMP
CCommandButtonSite::GetRect(
    RECT *prc)
{
    *prc = _rc;
    return S_OK;
}

/* CGetCommandStateTask */

CGetCommandStateTask::CGetCommandStateTask()
{
}

CGetCommandStateTask::~CGetCommandStateTask()
{
    CoTaskMemFree(_pidl);
    CoTaskMemFree(_pidlFolder);
    CoTaskMemFree(_pidlView);
    if (_hkeyStore)
        RegCloseKey(_hkeyStore);
    CoTaskMemFree(_pszName);

    if (_pstmSite)
    {
        IUnknown *punk = NULL;

        CoGetInterfaceAndReleaseStream(_pstmSite, IID_PPV_ARGS(&punk));
        if (punk)
            punk->Release();
    }
    if (_pstmSelection)
    {
        IShellItemArray *psia = NULL;

        CoGetInterfaceAndReleaseStream(_pstmSelection, IID_PPV_ARGS(&psia));
        if (psia)
            psia->Release();
    }
}

HRESULT
WINAPI
CGetCommandStateTask::CreateInstance(
    _In_ PCUITEMID_CHILD pidl,
    _In_ IExplorerCommand *pec,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IUnknown *punkSite,
    _In_ REFGUID rguidCanonical,
    _In_ HWND hwndNotify,
    _In_ UINT uRefreshID,
    _COM_Outptr_ IFrameWorkItem **ppfwi)
{
    CGetCommandStateTask *pTask;
    HRESULT hr;

    *ppfwi = NULL;
    pTask = new CGetCommandStateTask();
    if (!pTask)
        return E_OUTOFMEMORY;

    hr = pTask->_SetData(pidl, pec, psia, punkSite, rguidCanonical, hwndNotify, uRefreshID);
    if (SUCCEEDED(hr))
        hr = pTask->QueryInterface(IID_PPV_ARGS(ppfwi));
    pTask->Release();
    return hr;
}

/* Everything the worker thread needs to build the command again: its registry entry or its folder */
HRESULT
CGetCommandStateTask::_SetData(
    _In_ PCUITEMID_CHILD pidl,
    _In_ IExplorerCommand *pec,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IUnknown *punkSite,
    _In_ REFGUID rguidCanonical,
    _In_ HWND hwndNotify,
    _In_ UINT uRefreshID)
{
    IRegDataDrivenCommand *prddc;
    IFolderView *pfv;
    HRESULT hr = S_OK;

    _guidCanonical = rguidCanonical;
    _hwndNotify = hwndNotify;
    _uRefreshID = uRefreshID;

    if (punkSite)
        hr = CoMarshalInterThreadInterfaceInStream(IID_IUnknown, punkSite, &_pstmSite);
    if (FAILED(hr))
        return hr;

    if (SUCCEEDED(pec->QueryInterface(IID_PPV_ARGS(&prddc))))
    {
        hr = prddc->GetKeyAndName(&_hkeyStore, &_pszName);
        prddc->Release();
    }

    if (SUCCEEDED(hr))
        hr = SHILCloneFirst(pidl, &_pidl);
    if (SUCCEEDED(hr))
    {
        hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
        if (SUCCEEDED(hr))
        {
            hr = SHGetIDListFromObject(pfv, &_pidlView);
            if (SUCCEEDED(hr) && psia)
                hr = CoMarshalInterThreadInterfaceInStream(IID_IShellItemArray, psia, &_pstmSelection);
            pfv->Release();
        }
    }
    return hr;
}

/* Works out the state with time to spare and posts it to the command module */
STDMETHODIMP
CGetCommandStateTask::DoWork()
{
    COMMANDBUTTONDATA *pData;
    IShellItemArray *psia = NULL;
    IUnknown *punkSite = NULL;
    IExplorerCommand *pec = NULL;
    HRESULT hr = S_OK;

    pData = (COMMANDBUTTONDATA *)LocalAlloc(LPTR, sizeof(*pData));
    if (!pData)
        return E_OUTOFMEMORY;

    if (_pstmSelection)
    {
        hr = CoGetInterfaceAndReleaseStream(_pstmSelection, IID_PPV_ARGS(&psia));
        _pstmSelection = NULL;
    }
    if (SUCCEEDED(hr) && _pstmSite)
    {
        hr = CoGetInterfaceAndReleaseStream(_pstmSite, IID_PPV_ARGS(&punkSite));
        _pstmSite = NULL;
    }

    if (SUCCEEDED(hr))
    {
        if (_hkeyStore)
        {
            hr = GetRegDataDrivenCommand(_hkeyStore, _pszName, IID_PPV_ARGS(&pec));
        }
        else
        {
            IShellFolder *psfView = NULL;

            /* Other commands come back through a command folder sited on a view of the same folder */
            hr = SHBindToObject(NULL, _pidlView, NULL, IID_PPV_ARGS(&psfView));
            if (SUCCEEDED(hr))
            {
                IShellView *psv = NULL;

                hr = psfView->CreateViewObject(NULL, IID_PPV_ARGS(&psv));
                if (SUCCEEDED(hr))
                {
                    IShellFolder *psfCommands = NULL;

                    hr = _GetCommandFolder(&psfCommands);
                    if (SUCCEEDED(hr))
                    {
                        IShellFolder *psfParent = NULL;

                        IUnknown_SetSite(psfCommands, psv);
                        if (_pidlFolder)
                        {
                            hr = psfCommands->BindToObject(_pidlFolder, NULL, IID_PPV_ARGS(&psfParent));
                        }
                        else
                        {
                            psfParent = psfCommands;
                            psfParent->AddRef();
                        }
                        if (SUCCEEDED(hr))
                            hr = psfParent->BindToObject(_pidl, NULL, IID_PPV_ARGS(&pec));
                        IUnknown_SetSite(psfCommands, NULL);
                        if (psfParent)
                            psfParent->Release();
                        psfCommands->Release();
                    }
                    psv->Release();
                }
                psfView->Release();
            }
        }

        if (SUCCEEDED(hr))
            hr = CommandButtonData_Init(pData, pec, punkSite, psia, TRUE);
        if (pec)
            pec->Release();
    }

    /* A command that fails to say goes away */
    if (FAILED(hr))
    {
        pData->guidCanonical = _guidCanonical;
        pData->dwState = ECS_HIDDEN;
    }

    if (!PostMessageW(_hwndNotify, CMWM_COMMANDSTATE, (WPARAM)pData, _uRefreshID))
    {
        CommandButtonData_Free(pData);
        LocalFree(pData);
    }

    if (punkSite)
        punkSite->Release();
    if (psia)
        psia->Release();
    return E_FAIL;
}

/* CCommandModuleInner */

HRESULT
WINAPI
CCommandModuleInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Element *peBand;
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
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    pe->_pSite = new CCommandModuleSite();
    hr = pe->_pSite ? S_OK : E_OUTOFMEMORY;

    /* The band markup becomes this element: background, overflow, preview and help buttons */
    if (SUCCEEDED(hr))
    {
        hr = DUI_CreateElementFromResource(DUIFRAME_HINST, IDR_UIFILE_FOLDERBAND, L"FolderBand", pe, NULL, NULL,
                                           &peBand, NULL);
    }

    /* Windows lays the band out with an overflow layout; ReactUI has none, so the markup's border layout stays */
    if (SUCCEEDED(hr))
    {
        pe->_hwndWorker = SHCreateWorkerWindowW(s_WndProc, HWND_MESSAGE, 0, 0, NULL, (LONG_PTR)pe);
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
    IUnknown_SetSite(static_cast<IObjectWithSite *>(_pSite), punkSite);
    IUnknown_Set(&_punkSite, punkSite);
    return S_OK;
}

IServiceProvider *
CCommandModuleInner::_GetCommandSite()
{
    return _pSite ? static_cast<IServiceProvider *>(_pSite) : NULL;
}

static
int
CALLBACK
DestroyParserCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    COMMANDPARSER *pEntry = (COMMANDPARSER *)p;

    if (pEntry->pParser)
        pEntry->pParser->Destroy();
    LocalFree(pEntry);
    return TRUE;
}

static
int
CALLBACK
FreeButtonCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    COMMANDBUTTON *pButton = (COMMANDBUTTON *)p;

    CoTaskMemFree(pButton->pidl);
    LocalFree(pButton);
    return TRUE;
}

void
CCommandModuleInner::OnDestroy()
{
    if (_hdpaParsers)
    {
        DPA_DestroyCallback(_hdpaParsers, DestroyParserCB, NULL);
        _hdpaParsers = NULL;
    }

    DirectUI::Element::OnDestroy();
    if (_hwndWorker)
    {
        DestroyWindow(_hwndWorker);
        _hwndWorker = NULL;
    }

    _EndRefreshTracking();
    IUnknown_SafeReleaseAndNullPtr(&_pte);

    if (_hdpaButtons)
    {
        DPA_DestroyCallback(_hdpaButtons, FreeButtonCB, NULL);
        _hdpaButtons = NULL;
    }
    if (_pSite)
    {
        _pSite->Release();
        _pSite = NULL;
    }
    LocalFree(_pszTaskButtonSheet);
    _pszTaskButtonSheet = NULL;
    LocalFree(_pszSplitButtonSheet);
    _pszSplitButtonSheet = NULL;
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

LRESULT
CALLBACK
CCommandModuleInner::s_WndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    CCommandModuleInner *pThis = (CCommandModuleInner *)GetWindowLongPtrW(hwnd, 0);

    switch (uMsg)
    {
        case WM_TIMER:
            if (wParam == IDT_REFRESHBUTTONS && pThis)
            {
                KillTimer(hwnd, IDT_REFRESHBUTTONS);
                pThis->_RefreshButtons();
                pThis->_EndRefreshTracking();
            }
            return 0;

        case CMWM_SHOWFOLDERMENU:
        {
            FOLDERMENUREQUEST *pRequest = (FOLDERMENUREQUEST *)wParam;

            if (!pRequest || !pThis)
                return 0;

            /* The menu loop runs here; the owner is known while it is up */
            pThis->_peMenuOwner = pRequest->peOwner;
            ShowFolderMenu(pRequest->psf, pRequest->punkSite, &pRequest->rcExclude, pRequest->cSkip,
                           pRequest->fOverflow);
            PostMessageW(hwnd, CMWM_FOLDERMENUDONE, 0, (LPARAM)pRequest->peOwner);

            if (pRequest->punkSite)
                pRequest->punkSite->Release();
            if (pRequest->psf)
                pRequest->psf->Release();
            LocalFree(pRequest);
            return 0;
        }

        case CMWM_INVOKECOMMAND:
        {
            COMMANDINVOKEREQUEST *pRequest = (COMMANDINVOKEREQUEST *)wParam;

            if (!pRequest)
                return 0;

            SHInvokeCommandWithFlagsAndSite(NULL, pRequest->punkSite, pRequest->psf, pRequest->pidl, 0, NULL);
            CoTaskMemFree(pRequest->pidl);
            if (pRequest->punkSite)
                pRequest->punkSite->Release();
            if (pRequest->psf)
                pRequest->psf->Release();
            LocalFree(pRequest);
            return 0;
        }

        case CMWM_FOLDERMENUDONE:
            if (pThis)
            {
                if ((DirectUI::Element *)lParam == pThis->_peMenuOwner)
                    pThis->_peMenuOwner = NULL;
                if (!lParam)
                    pThis->_fOverflowMenuUp = FALSE;
            }
            return 0;

        case CMWM_COMMANDSTATE:
        {
            COMMANDBUTTONDATA *pData = (COMMANDBUTTONDATA *)wParam;

            if (!pData)
                return 0;

            if (pThis)
                pThis->_InsertBackgroundTaskButton(pData, (UINT)lParam);
            CommandButtonData_Free(pData);
            LocalFree(pData);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* The button style sheets come from the outer module, else the shell style's defaults */
HRESULT
CCommandModuleInner::_GetButtonStyles()
{
    DirectUI::Element *peModule = GetParent();
    HRESULT hr;

    hr = _AllocSheetName(peModule, &s_piSplitButtonSheet, L"SplitButtonStyle", &_pszSplitButtonSheet);
    if (SUCCEEDED(hr))
        hr = _AllocSheetName(peModule, &s_piTaskButtonSheet, L"TaskButtonStyle", &_pszTaskButtonSheet);
    return hr;
}

HRESULT
CCommandModuleInner::_AllocSheetName(
    _In_opt_ DirectUI::Element *peModule,
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ PCWSTR pszDefault,
    _Outptr_ PWSTR *ppszSheet)
{
    DirectUI::Value *pv = NULL;
    PCWSTR pszSheet = pszDefault;
    HRESULT hr;
    size_t cch;

    *ppszSheet = NULL;
    if (peModule && peModule->GetClassInfoW()->IsValidProperty(ppi))
    {
        pv = peModule->GetValue(ppi, PROPERTY_STORE_LOCAL, NULL);
        if (pv && pv != DirectUI::Value::GetUnset() && pv->GetString())
            pszSheet = pv->GetString();
    }

    cch = wcslen(pszSheet) + 1;
    *ppszSheet = (PWSTR)LocalAlloc(LMEM_FIXED, cch * sizeof(WCHAR));
    hr = *ppszSheet ? StringCchCopyW(*ppszSheet, cch, pszSheet) : E_OUTOFMEMORY;

    if (pv)
        pv->Release();
    return hr;
}

/* The command element markup names placeholder sheets that stand for the module's button styles */
DirectUI::Value *
WINAPI
CCommandModuleInner::s_GetSheetCallback(
    _In_ const wchar_t *pszSheetID,
    _In_opt_ void *pvContext)
{
    COMMANDPARSER *pEntry = (COMMANDPARSER *)pvContext;
    PCWSTR pszSheet = pszSheetID;

    if (pEntry)
    {
        if (!StrCmpICW(L"SplitButtonStylePlaceholder", pszSheetID))
            pszSheet = pEntry->pModule->_pszSplitButtonSheet;
        else if (!StrCmpICW(L"TaskButtonStylePlaceholder", pszSheetID))
            pszSheet = pEntry->pModule->_pszTaskButtonSheet;
    }
    return DUI_GetSheetCB(pszSheet, NULL);
}

HRESULT
CCommandModuleInner::_GetCommandElementParser(
    _In_ UINT uID,
    _Outptr_ DirectUI::DUIXmlParser **ppParser)
{
    COMMANDPARSER *pEntry;
    HRESULT hr = S_OK;

    *ppParser = NULL;
    if (!_hdpaParsers)
    {
        _hdpaParsers = DPA_Create(1);
        if (!_hdpaParsers)
            return E_OUTOFMEMORY;
    }

    for (int i = 0; i < DPA_GetPtrCount(_hdpaParsers); i++)
    {
        pEntry = (COMMANDPARSER *)DPA_FastGetPtr(_hdpaParsers, i);
        if (pEntry->uID == uID)
        {
            *ppParser = pEntry->pParser;
            return S_OK;
        }
    }

    pEntry = (COMMANDPARSER *)LocalAlloc(LPTR, sizeof(*pEntry));
    if (!pEntry)
        return E_OUTOFMEMORY;

    if (!_pszSplitButtonSheet)
        hr = _GetButtonStyles();

    if (SUCCEEDED(hr))
    {
        pEntry->uID = uID;
        pEntry->pModule = this;
        hr = DUI_CreateParserWithCallbackFromResource(DUIFRAME_HINST, uID, &pEntry->pParser, s_GetSheetCallback,
                                                      pEntry);
    }

    if (SUCCEEDED(hr) && DPA_AppendPtr(_hdpaParsers, pEntry) == -1)
        hr = E_OUTOFMEMORY;

    if (SUCCEEDED(hr))
    {
        *ppParser = pEntry->pParser;
    }
    else
    {
        if (pEntry->pParser)
            pEntry->pParser->Destroy();
        LocalFree(pEntry);
    }
    return hr;
}

/* Commands with a split part use the split button markup; the others the plain button markup */
HRESULT
CCommandModuleInner::_CreateSplitButton(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_ const COMMANDBUTTONDATA *pData,
    _In_ DirectUI::Element *peParent,
    _Outptr_ DirectUI::Element **ppe)
{
    BOOL fSplit = (pData->dwFlags & ECF_HASSPLITBUTTON) != 0;
    DirectUI::DUIXmlParser *pParser;
    DirectUI::Element *peButton;
    DirectUI::Element *peCreated;
    HRESULT hr;

    *ppe = NULL;
    hr = _GetCommandElementParser(fSplit ? IDR_UIFILE_SPLITBUTTON : IDR_UIFILE_TASKBUTTON, &pParser);
    if (FAILED(hr))
        return hr;

    hr = CSplitButton::Create(peParent, NULL, &peButton, fSplit);
    if (FAILED(hr))
        return hr;

    CSplitButton *psb = static_cast<CSplitButton *>(peButton);

    hr = pParser->CreateElement(L"CommandElement", peButton, NULL, NULL, &peCreated);
    if (SUCCEEDED(hr))
    {
        IUnknown_SetSite(static_cast<IObjectWithSite *>(psb), _punkSite);
        hr = psb->SetItem(psf, pidl, pData, _GetCommandSite());
        if (FAILED(hr))
            IUnknown_SetSite(static_cast<IObjectWithSite *>(psb), NULL);
    }

    if (FAILED(hr))
    {
        peButton->Destroy(true);
        return hr;
    }

    *ppe = peButton;
    return S_OK;
}

/* Elements of their own, such as the view control, come straight from their markup */
HRESULT
CCommandModuleInner::_CreateCustomElement(
    _In_ UINT uID,
    _In_ DirectUI::Element *peParent,
    _Outptr_ DirectUI::Element **ppe)
{
    DirectUI::DUIXmlParser *pParser;
    DirectUI::Element *pe;
    HRESULT hr;

    *ppe = NULL;
    hr = _GetCommandElementParser(uID, &pParser);
    if (SUCCEEDED(hr))
        hr = pParser->CreateElement(L"CommandElement", NULL, peParent, NULL, &pe);
    if (SUCCEEDED(hr))
    {
        IUnknown *punk = CElementWithIUnknown::GetUnknownFromElement(pe);

        if (punk)
            IUnknown_SetSite(punk, _punkSite);
        *ppe = pe;
    }
    return hr;
}

HRESULT
CCommandModuleInner::_CreateButton(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_ const COMMANDBUTTONDATA *pData,
    _In_ DirectUI::Element *peParent,
    _Outptr_ DirectUI::Element **ppe)
{
    if (IsEqualGUID(pData->guidCanonical, UICID_ViewControl))
        return _CreateCustomElement(IDR_UIFILE_VIEWCONTROL, peParent, ppe);
    return _CreateSplitButton(psf, pidl, pData, peParent, ppe);
}

/* Right side commands go right after the preview pane button */
HRESULT
CCommandModuleInner::_InsertButtonToBand(
    _In_ DirectUI::Element *pe)
{
    DirectUI::Element *pePreview = FindDescendent(StrToID(L"PreviewButton"));
    DirectUI::Value *pvChildren = NULL;
    HRESULT hr = E_FAIL;

    if (!pePreview)
        return hr;

    DirectUI::DynamicArray<DirectUI::Element *, 0> *pChildren = GetChildren(&pvChildren);

    if (pChildren)
    {
        UINT iPreview = (UINT)-1;

        for (UINT i = 0; i < pChildren->GetSize(); i++)
        {
            if (pChildren->GetItem(i) == pePreview)
            {
                iPreview = i;
                break;
            }
        }

        if (iPreview != (UINT)-1)
        {
            hr = Insert(pe, iPreview + 1);
            if (SUCCEEDED(hr))
                hr = pe->SetLayoutPos(ROSUI_LP_RIGHT);
        }
    }
    if (pvChildren)
        pvChildren->Release();
    return hr;
}

HRESULT
CCommandModuleInner::_AddTrackedElement(
    _In_ PCUITEMID_CHILD pidl,
    _In_ REFGUID rguidCanonical,
    _In_opt_ DirectUI::Element *pe,
    _In_ int iButton)
{
    COMMANDBUTTON *pButton;
    HRESULT hr;

    if (!_hdpaButtons)
    {
        _hdpaButtons = DPA_Create(4);
        if (!_hdpaButtons)
            return E_OUTOFMEMORY;
    }

    pButton = (COMMANDBUTTON *)LocalAlloc(LPTR, sizeof(*pButton));
    if (!pButton)
        return E_OUTOFMEMORY;

    hr = SHILCloneFirst(pidl, &pButton->pidl);
    if (SUCCEEDED(hr))
    {
        pButton->guidCanonical = rguidCanonical;
        pButton->pe = pe;
        if (DPA_InsertPtr(_hdpaButtons, iButton, pButton) == -1)
            hr = E_OUTOFMEMORY;
    }

    if (FAILED(hr))
        FreeButtonCB(pButton, NULL);
    return hr;
}

int
CCommandModuleInner::_GetButtonCount()
{
    return _hdpaButtons ? DPA_GetPtrCount(_hdpaButtons) : 0;
}

void
CCommandModuleInner::_DestroyButton(
    _In_ int iButton)
{
    COMMANDBUTTON *pButton;

    if (!_hdpaButtons)
        return;

    pButton = (COMMANDBUTTON *)DPA_DeletePtr(_hdpaButtons, iButton);
    if (!pButton)
        return;

    if (pButton->pe)
    {
        IUnknown *punk = CElementWithIUnknown::GetUnknownFromElement(pButton->pe);

        if (punk)
            IUnknown_SetSite(punk, NULL);
        pButton->pe->Destroy(false);
    }
    FreeButtonCB(pButton, NULL);
}

void
CCommandModuleInner::_DestroyButtons()
{
    for (int i = _GetButtonCount() - 1; i >= 0; i -= 1)
        _DestroyButton(i);
}

/* Shown buttons, buttons and left side buttons; a button shows when it got some room */
void
CCommandModuleInner::_GetDisplayedAndTotalCommands(
    _Out_opt_ int *pcDisplayed,
    _Out_opt_ int *pcTotal,
    _Out_opt_ int *pcLeftSide)
{
    int cDisplayed = 0;
    int cTotal = 0;
    int cLeftSide = 0;

    for (int i = 0; i < _GetButtonCount(); i++)
    {
        COMMANDBUTTON *pButton = (COMMANDBUTTON *)DPA_FastGetPtr(_hdpaButtons, i);
        DirectUI::Value *pvExtent;

        if (!pButton->pe)
            continue;

        if (!IsRightSideCommand(pButton->guidCanonical))
            cLeftSide++;

        pvExtent = pButton->pe->GetValue(DirectUI::Element::ExtentProp, PROPERTY_STORE_SPECIFIED, NULL);
        if (pvExtent)
        {
            const SIZE *psize = pvExtent->GetSize();

            if (psize && psize->cx > 0 && psize->cy > 0)
                cDisplayed++;
            pvExtent->Release();
        }
        cTotal++;
    }

    if (pcDisplayed)
        *pcDisplayed = cDisplayed;
    if (pcTotal)
        *pcTotal = cTotal;
    if (pcLeftSide)
        *pcLeftSide = cLeftSide;
}

/*
 * One command of a refresh. Buttons ahead of it that no longer match go away; a matching
 * button is updated in place. A command that is slow to say its state gets a placeholder
 * while a task works it out.
 */
HRESULT
CCommandModuleInner::_UpdateButton(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_opt_ IShellItemArray *psia,
    _Inout_ int *piButton,
    _Inout_ BOOL *pfCreated,
    _Inout_ BOOL *pfDestroyed)
{
    COMMANDBUTTONDATA data = { 0 };
    IExplorerCommand *pec;
    HRESULT hr;

    hr = psf->BindToObject(pidl, NULL, IID_PPV_ARGS(&pec));
    if (FAILED(hr))
        return hr;

    hr = CommandButtonData_Init(&data, pec, _GetCommandSite(), psia, FALSE);
    if (SUCCEEDED(hr) && !(data.dwState & ECS_HIDDEN))
    {
        BOOL fDone = FALSE;

        while (*piButton < _GetButtonCount())
        {
            COMMANDBUTTON *pButton = (COMMANDBUTTON *)DPA_FastGetPtr(_hdpaButtons, *piButton);
            HRESULT hrCompare = psf->CompareIDs(SHCIDS_CANONICALONLY, pidl, pButton->pidl);

            if (FAILED(hrCompare) || (short)HRESULT_CODE(hrCompare) > 0)
            {
                _DestroyButton(*piButton);
                *pfDestroyed = TRUE;
                continue;
            }

            /* A placeholder of a state that never came back is rebuilt */
            if (!pButton->pe)
            {
                _DestroyButton(*piButton);
                *pfDestroyed = TRUE;
                break;
            }

            CSplitButton *psb = element_cast_CSplitButton(pButton->pe);

            if (!psb || ((psb->IsSplit() != FALSE) == ((data.dwFlags & ECF_HASSPLITBUTTON) == ECF_HASSPLITBUTTON) &&
                         SUCCEEDED(hr = psb->SetItem(psf, pidl, &data, _GetCommandSite()))))
            {
                /* Once a button was added ahead of it, it moves to keep the order */
                if (*pfCreated)
                {
                    Remove(pButton->pe);
                    if (IsRightSideCommand(data.guidCanonical))
                        hr = _InsertButtonToBand(pButton->pe);
                    else
                        hr = Add(pButton->pe);
                }
                fDone = TRUE;
                break;
            }

            _DestroyButton(*piButton);
            *pfDestroyed = TRUE;
            break;
        }

        if (!fDone)
        {
            DirectUI::Element *pe;

            if (SUCCEEDED(_CreateButton(psf, pidl, &data, this, &pe)))
            {
                if (FAILED(_AddTrackedElement(pidl, data.guidCanonical, pe, *piButton)))
                {
                    pe->Destroy(true);
                }
                else
                {
                    HRESULT hrAdd = IsRightSideCommand(data.guidCanonical) ? _InsertButtonToBand(pe) : Add(pe);

                    if (FAILED(hrAdd))
                        _DestroyButton(*piButton);
                }
            }
            *pfCreated = TRUE;
        }
        (*piButton)++;
    }
    else if (hr == E_PENDING)
    {
        IFrameTaskManager *pftm = NULL;
        IObjectProvider *pop;

        if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFrameManager, IID_PPV_ARGS(&pop))))
        {
            pop->QueryObject(OID_FrameTaskManager, IID_PPV_ARGS(&pftm));
            pop->Release();
        }

        if (pftm)
        {
            IFrameWorkItem *pfwi;

            if (SUCCEEDED(CGetCommandStateTask::CreateInstance(pidl, pec, psia, _GetCommandSite(),
                                                               data.guidCanonical, _hwndWorker, _uRefreshID,
                                                               &pfwi)))
            {
                BOOL fPlaced = FALSE;

                /* The button already there waits greyed out for the new state */
                if (!*pfCreated && !*pfDestroyed && *piButton < _GetButtonCount())
                {
                    COMMANDBUTTON *pButton = (COMMANDBUTTON *)DPA_FastGetPtr(_hdpaButtons, *piButton);

                    if (psf->CompareIDs(SHCIDS_CANONICALONLY, pidl, pButton->pidl) == S_OK)
                    {
                        CSplitButton *psb = element_cast_CSplitButton(pButton->pe);

                        if (psb)
                        {
                            psb->SetEnabled(false);
                            psb->SetEvaluatingAsyncState(true);
                            fPlaced = TRUE;
                        }
                        else
                        {
                            _DestroyButton(*piButton);
                        }
                    }
                }

                if (!fPlaced)
                    _AddTrackedElement(pidl, data.guidCanonical, NULL, *piButton);

                (*piButton)++;
                hr = pftm->StartWorkItem(pfwi, TOID_DVGetCommandState, 0);
                pfwi->Release();
            }
            pftm->Release();
        }
    }

    pec->Release();
    CommandButtonData_Free(&data);
    return hr;
}

HRESULT
CCommandModuleInner::_CreateButtons()
{
    IShellFolder *psf;

    if (FAILED(_GetCommandFolder(&psf)))
        return S_OK;

    IShellItemArray *psia = NULL;
    IEnumIDList *penum;
    HRESULT hr;

    hr = GetSelectionFromSite(_punkSite, &psia);
    if ((SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) &&
        psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum) == S_OK)
    {
        PITEMID_CHILD pidl;
        BOOL fCreated = FALSE;
        BOOL fDestroyed = FALSE;
        int iButton = 0;

        _uRefreshID++;
        IUnknown_SetSite(penum, _GetCommandSite());
        while (penum->Next(1, &pidl, NULL) == S_OK)
        {
            _UpdateButton(psf, pidl, psia, &iButton, &fCreated, &fDestroyed);
            CoTaskMemFree(pidl);
        }

        /* Whatever is left over was not listed this time */
        for (int i = _GetButtonCount() - 1; i >= iButton; i -= 1)
            _DestroyButton(i);

        IUnknown_SetSite(penum, NULL);
        penum->Release();
    }

    if (psia)
        psia->Release();
    psf->Release();
    return S_OK;
}

HRESULT
CCommandModuleInner::_RefreshButtons()
{
    ULONG dwDeferCookie = 0;

    if (_fNoFolderBand)
        return S_OK;

    _fRefreshedOnce = TRUE;
    StartDefer(&dwDeferCookie);

    /* Commands of the previous refresh do not share their bag with the next ones */
    if (_pSite)
        _pSite->ResetCommandsPropertyBag();

    _CreateButtons();
    RemoveLocalValue(DirectUI::Element::HeightProp);
    RemoveLocalValue(DirectUI::Element::LayoutPosProp);
    if (dwDeferCookie)
        EndDefer(dwDeferCookie);
    return S_OK;
}

/* A state a task worked out: the placeholder becomes a button, or goes when the command is hidden */
void
CCommandModuleInner::_InsertBackgroundTaskButton(
    _In_ const COMMANDBUTTONDATA *pData,
    _In_ UINT uRefreshID)
{
    IShellFolder *psf;
    int cTotal;
    int cLeftSide;
    int iButton;
    UINT iInsert;

    if (uRefreshID != _uRefreshID || FAILED(_GetCommandFolder(&psf)))
        return;

    IUnknown_SetSite(psf, _GetCommandSite());
    _GetDisplayedAndTotalCommands(NULL, &cTotal, &cLeftSide);

    /* Children: the fixed buttons, the right side buttons, then the left side buttons in order */
    iInsert = cTotal - cLeftSide + FIXED_BUTTON_COUNT;
    for (iButton = 0; iButton < _GetButtonCount(); iButton++)
    {
        COMMANDBUTTON *pButton = (COMMANDBUTTON *)DPA_FastGetPtr(_hdpaButtons, iButton);

        if (IsEqualGUID(pButton->guidCanonical, pData->guidCanonical))
            break;
        if (pButton->pe)
            iInsert++;
    }

    if (iButton < _GetButtonCount())
    {
        COMMANDBUTTON *pButton = (COMMANDBUTTON *)DPA_FastGetPtr(_hdpaButtons, iButton);

        if (pData->dwState & ECS_HIDDEN)
        {
            _DestroyButton(iButton);
        }
        else
        {
            CSplitButton *psb = element_cast_CSplitButton(pButton->pe);
            DirectUI::Element *pe;

            if (psb)
            {
                if (SUCCEEDED(psb->SetItem(psf, pButton->pidl, pData, _GetCommandSite())))
                    psb->SetEvaluatingAsyncState(false);
                else
                    _DestroyButton(iButton);
            }
            else if (SUCCEEDED(_CreateButton(psf, pButton->pidl, pData, this, &pe)))
            {
                pButton->pe = pe;
                if (FAILED(Insert(pe, iInsert)))
                    _DestroyButton(iButton);
            }
        }
    }

    IUnknown_SetSite(psf, NULL);
    psf->Release();
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
        _DestroyButtons();

        StartDefer(&dwDeferCookie);
        SetHeight(0);
        SetLayoutPos(LAYOUTPOS_NONE);
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

    if (FAILED(CanShowPaneAux(c_szReadingPaneVisible, POLID_NoReadingPane, &ep, SID_PerBrowserPropertyBag,
                              _punkSite, &dwState)) ||
        (dwState & ECS_HIDDEN))
    {
        pePreview->SetVisible(false);
        pePreview->SetEnabled(false);
        pePreview->SetLayoutPos(LAYOUTPOS_NONE);
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
            DUI_SetElementIcon(pePreview, hicon);
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

/* The overflow menu lists the commands the band had no room for */
HRESULT
CCommandModuleInner::_ShowOverflowMenu(
    _In_ DirectUI::Element *peButton)
{
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    hr = SHParseDisplayName(COMMANDFOLDER_PARSENAME, NULL, &pidl, 0, NULL);
    if (FAILED(hr) || _fOverflowMenuUp)
    {
        if (SUCCEEDED(hr))
            CoTaskMemFree(pidl);
        return hr;
    }

    IShellFolder *psf;

    _fOverflowMenuUp = TRUE;
    hr = SHBindToObject(NULL, pidl, NULL, IID_PPV_ARGS(&psf));
    if (SUCCEEDED(hr))
    {
        FOLDERMENUREQUEST *pRequest = (FOLDERMENUREQUEST *)LocalAlloc(LPTR, sizeof(*pRequest));

        hr = pRequest ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
            hr = DUI_GetElementScreenBounds(peButton, &pRequest->rcExclude);
        if (SUCCEEDED(hr))
        {
            IUnknown *punkButtonSite;
            HWND hwndRoot = NULL;

            DUI_GetElementRootHWND(this, &hwndRoot);
            hr = CCommandButtonSite::s_CreateInstance(_GetCommandSite(), hwndRoot, &pRequest->rcExclude,
                                                      IID_PPV_ARGS(&punkButtonSite));
            if (SUCCEEDED(hr))
            {
                int cDisplayed = 0;

                pRequest->psf = psf;
                psf->AddRef();
                pRequest->punkSite = punkButtonSite;
                pRequest->fOverflow = TRUE;
                _GetDisplayedAndTotalCommands(&cDisplayed, NULL, NULL);
                pRequest->cSkip = cDisplayed;

                if (!PostMessageW(_hwndWorker, CMWM_SHOWFOLDERMENU, (WPARAM)pRequest, 0))
                {
                    hr = E_FAIL;
                    punkButtonSite->Release();
                    pRequest->punkSite = NULL;
                }
            }
        }

        if (FAILED(hr) && pRequest)
        {
            if (pRequest->psf)
                pRequest->psf->Release();
            LocalFree(pRequest);
        }
        psf->Release();
    }

    if (FAILED(hr))
        _fOverflowMenuUp = FALSE;
    CoTaskMemFree(pidl);
    return hr;
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
            _ShowOverflowMenu(FindDescendent(atomID));
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
