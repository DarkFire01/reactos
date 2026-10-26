/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     State and invoke handlers of the built-in command store commands
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* String ids of the folder options command */
#define IDS_FOLDEROPTIONS               31447
#define IDS_FOLDERANDSEARCHOPTIONS      31448
#define IDS_FOLDEROPTIONS_TIP           31449

/* CGID_Explorer command that opens the folder options */
#define SBCMDID_FOLDEROPTIONS           6

/* IShellFolderView::Select flag */
#ifndef SFVS_SELECT_ALLITEMS
#define SFVS_SELECT_ALLITEMS            0x1
#endif

/* CGID_PrivCITCommands command that re-reads the classic menu setting */
#define CITIDM_MENUSETTINGSCHANGED      15

/*
 * Copies of GUIDs that live in both the ModernUI and the ReactOS GUID libraries; shell32 links
 * the ReactOS one, whose IExplorerToolbar is not the one ExplorerFrame offers.
 */
static const GUID c_sidExplorerToolbar =
    { 0xf80c2137, 0x5829, 0x4ce9, { 0x9f, 0x81, 0xa9, 0x5e, 0x15, 0x9d, 0xd8, 0xd5 } };
static const GUID c_guidPrivCITCommands =
    { 0x67077b95, 0x4f9d, 0x11d0, { 0xb8, 0x84, 0x00, 0xaa, 0x00, 0xb6, 0x01, 0x04 } };
static const IID c_iidObjectWithAssociationElement =
    { 0xe157c3a1, 0xa532, 0x4de2, { 0x94, 0x80, 0x14, 0x52, 0xb7, 0x42, 0x6e, 0xee } };

static const WCHAR c_szAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

/* Undo and redo need the shell's undo manager, which shell32 does not have yet */
static
BOOL
CanDo(
    _In_ BOOL fUndo)
{
    UNREFERENCED_PARAMETER(fUndo);
    return FALSE;
}

/* The location the view shows, as an item */
static
HRESULT
IUnknown_QueryCurrentItem(
    _In_opt_ IUnknown *punkSite,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IFolderView *pfv;
    HRESULT hr;

    *ppv = NULL;
    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        hr = pfv->GetFolder(riid, ppv);
        pfv->Release();
    }
    return hr;
}

/*
 * A new folder needs a writable storage. Windows asks the folder for a transfer destination
 * and the storage attribute; ReactOS folders have no transfer destination and drive roots do
 * not report storage, so a file system folder that is not read only qualifies.
 */
static
BOOL
CanDoNewFolderForItem(
    _In_ IShellItem *psi)
{
    const SFGAOF sfgaoWanted = SFGAO_FOLDER | SFGAO_FILESYSTEM;
    SFGAOF sfgao = 0;

    if (FAILED(psi->GetAttributes(sfgaoWanted | SFGAO_READONLY, &sfgao)))
        return FALSE;
    return (sfgao & (sfgaoWanted | SFGAO_READONLY)) == sfgaoWanted;
}

static
BOOL
CanDoNewFolderForItemFromSite(
    _In_opt_ IUnknown *punkSite)
{
    IShellItem *psi;
    BOOL fCan = FALSE;

    if (SUCCEEDED(IUnknown_QueryCurrentItem(punkSite, IID_PPV_ARGS(&psi))))
    {
        fCan = CanDoNewFolderForItem(psi);
        psi->Release();
    }
    return fCan;
}

static
DWORD
DataObj_GetDWORD(
    _In_ IDataObject *pdtobj,
    _In_ CLIPFORMAT cf,
    _In_ DWORD dwDefault)
{
    FORMATETC fmte = { cf, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    DWORD dwValue = dwDefault;

    if (SUCCEEDED(pdtobj->GetData(&fmte, &medium)))
    {
        DWORD *pdw = (DWORD *)GlobalLock(medium.hGlobal);

        if (pdw)
        {
            dwValue = *pdw;
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    return dwValue;
}

/* Whether the drop target takes what the clipboard holds as a copy or a move */
static
BOOL
Def_IsPasteAvailable(
    _In_ IDropTarget *pdt,
    _Out_ DWORD *pdwEffect)
{
    IDataObject *pdtobj;
    BOOL fAvailable = FALSE;

    *pdwEffect = 0;
    if (CountClipboardFormats() <= 0 || FAILED(OleGetClipboard(&pdtobj)))
        return FALSE;

    CLIPFORMAT cfDropEffect = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    DWORD dwPreferred = DataObj_GetDWORD(pdtobj, cfDropEffect, DROPEFFECT_COPY | DROPEFFECT_LINK);
    DWORD dwEffect = dwPreferred & (DROPEFFECT_COPY | DROPEFFECT_MOVE);
    POINTL pt = { 0, 0 };

    if (dwEffect)
    {
        if (FAILED(pdt->DragEnter(pdtobj, MK_RBUTTON, pt, &dwEffect)))
            dwEffect = 0;
        else
            pdt->DragLeave();
    }

    if (dwPreferred & DROPEFFECT_LINK)
    {
        DWORD dwLink = DROPEFFECT_LINK;

        if (SUCCEEDED(pdt->DragEnter(pdtobj, MK_RBUTTON, pt, &dwLink)))
        {
            pdt->DragLeave();
            dwEffect |= dwLink;
        }
    }

    *pdwEffect = dwEffect;
    fAvailable = (dwEffect & (DROPEFFECT_COPY | DROPEFFECT_MOVE)) != 0;
    pdtobj->Release();
    return fAvailable;
}

static
HRESULT
GetClassicMenusState(
    _In_opt_ IUnknown *punkSite,
    _Out_ BOOL *pfShown)
{
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);

    *pfShown = SHGetValueW(HKEY_CURRENT_USER, c_szAdvancedKey, L"AlwaysShowMenus", NULL, &dwValue, &cbValue) ==
                   ERROR_SUCCESS &&
               dwValue;
    return S_OK;
}

static
BOOL
IsSearchEnabled(VOID)
{
    static int s_iSearchEnabled = 0;

    if (!s_iSearchEnabled)
    {
        OSVERSIONINFOEXW osvi = { sizeof(osvi) };
        HKEY hkey;

        if ((GetVersionExW((OSVERSIONINFOW *)&osvi) &&
             (osvi.wProductType == VER_NT_SERVER || osvi.wProductType == VER_NT_DOMAIN_CONTROLLER)) ||
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows Search", 0, KEY_QUERY_VALUE, &hkey) !=
                ERROR_SUCCESS)
        {
            s_iSearchEnabled = 1;
        }
        else
        {
            WCHAR szVersion[MAX_PATH];
            DWORD cbVersion = sizeof(szVersion);

            s_iSearchEnabled = (RegGetValueW(hkey, NULL, L"CurrentVersion", RRF_RT_ANY, NULL, szVersion,
                                             &cbVersion) != ERROR_SUCCESS) + 1;
            RegCloseKey(hkey);
        }
    }
    return s_iSearchEnabled == 1;
}

/* State handlers */

static
HRESULT
WINAPI
_CanNewFolder(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    *pState = CanDoNewFolderForItemFromSite(punkSite) ? ECS_ENABLED : ECS_HIDDEN;
    return S_OK;
}

/* Sending mail needs a mail client registered for the Send To menu; ReactOS has none */
static
HRESULT
WINAPI
_CanEmail(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    *pState = ECS_HIDDEN;
    return S_OK;
}

/* Burning needs a disc recorder that the shell's CD burning support reports; ReactOS has no such support */
static
HRESULT
WINAPI
_CanSendToCD(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    *pState = ECS_HIDDEN;
    return S_OK;
}

/* Libraries are not in ReactOS yet, so there is nothing to include folders in */
static
HRESULT
WINAPI
_CanIncludeInLibrary(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    *pState = ECS_HIDDEN;
    return S_OK;
}

static
HRESULT
WINAPI
_CanPaste(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    IFolderView *pfv = NULL;

    *pState = ECS_DISABLED;
    if (SUCCEEDED(IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv))))
    {
        IShellFolder *psf = NULL;

        if (SUCCEEDED(pfv->GetFolder(IID_PPV_ARGS(&psf))))
        {
            IDropTarget *pdt = NULL;

            if (SUCCEEDED(psf->CreateViewObject(SHOwnerWindowFromSite(punkSite), IID_PPV_ARGS(&pdt))))
            {
                DWORD dwEffect;

                *pState = (pdt && Def_IsPasteAvailable(pdt, &dwEffect)) ? ECS_ENABLED : ECS_DISABLED;
            }
            if (pdt)
                pdt->Release();
            psf->Release();
        }
        pfv->Release();
    }
    return S_OK;
}

static
HRESULT
WINAPI
_CanUndo(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    *pState = CanDo(TRUE) ? ECS_ENABLED : ECS_DISABLED;
    return S_OK;
}

static
HRESULT
WINAPI
_CanRedo(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    *pState = CanDo(FALSE) ? ECS_ENABLED : ECS_DISABLED;
    return S_OK;
}

/* Views that only allow one selected item cannot select all; a view still filling waits */
static
HRESULT
WINAPI
_CanSelectAll(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    IShellView *psv = NULL;
    FOLDERSETTINGS fs;

    *pState = ECS_DISABLED;
    if (SUCCEEDED(IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&psv))) &&
        SUCCEEDED(psv->GetCurrentInfo(&fs)) && !(fs.fFlags & FWF_SINGLESEL))
    {
        IViewStatus *pvs;

        if (FAILED(psv->QueryInterface(IID_IViewStatus, (void **)&pvs)))
        {
            *pState = ECS_ENABLED;
        }
        else
        {
            DEFVIEWSTATUS dvs;

            if (SUCCEEDED(pvs->GetStatus(&dvs)) && !(dvs & DVS_INCREMENTALFILLING))
                *pState = ECS_ENABLED;
            pvs->Release();
        }
    }
    if (psv)
        psv->Release();
    return S_OK;
}

/* The menu bar toggle is checked while the menu bar always shows */
static
HRESULT
WINAPI
_CanToggleClassicMenus(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    DWORD dwPolicy = 0;
    DWORD cbPolicy = sizeof(dwPolicy);
    IOleCommandTarget *pct = NULL;

    if (SUCCEEDED(SHWindowsPolicyGetValue(POLID_AlwaysShowClassicMenu, &dwPolicy, &cbPolicy)) && dwPolicy)
    {
        *pState = ECS_HIDDEN;
        return S_OK;
    }

    *pState = ECS_CHECKBOX | ECS_DISABLED;
    if (SUCCEEDED(IUnknown_QueryService(punkSite, c_sidExplorerToolbar, IID_PPV_ARGS(&pct))))
    {
        BOOL fShown;

        *pState = ECS_CHECKBOX;
        if (SUCCEEDED(GetClassicMenusState(punkSite, &fShown)) && fShown)
            *pState |= ECS_CHECKED;
    }
    if (pct)
        pct->Release();
    return S_OK;
}

static
HRESULT
WINAPI
_CanCloseWindow(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    IWebBrowserApp *pwba = NULL;

    *pState = ECS_DISABLED;
    if (SUCCEEDED(IUnknown_QueryServiceForWebBrowserApp(punkSite, IID_PPV_ARGS(&pwba))))
        *pState = ECS_ENABLED;
    if (pwba)
        pwba->Release();
    return S_OK;
}

/* Invoke handlers */

static
HRESULT
WINAPI
_OnRename(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    IFolderView2 *pfv2;
    HRESULT hr;

    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv2));
    if (SUCCEEDED(hr))
    {
        hr = pfv2->DoRename();
        pfv2->Release();
    }
    return hr;
}

static
HRESULT
WINAPI
_OnUndo(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    return CanDo(TRUE) ? S_OK : E_NOTIMPL;
}

static
HRESULT
WINAPI
_OnRedo(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    return CanDo(FALSE) ? S_OK : E_NOTIMPL;
}

static
HRESULT
WINAPI
_OnSelectAll(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    IShellFolderView *psfv = NULL;
    HRESULT hr;

    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_IShellFolderView, (void **)&psfv);
    if (SUCCEEDED(hr))
        hr = psfv->Select(SFVS_SELECT_ALLITEMS);
    if (psfv)
        psfv->Release();
    return hr;
}

/* Flips the always shown menu bar setting and tells the browser to read it again */
static
HRESULT
WINAPI
_OnToggleClassicMenus(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    BOOL fShown;
    DWORD dwShow;

    GetClassicMenusState(punkSite, &fShown);
    dwShow = !fShown;
    if (SHSetValueW(HKEY_CURRENT_USER, c_szAdvancedKey, L"AlwaysShowMenus", REG_DWORD, &dwShow,
                    sizeof(dwShow)) != ERROR_SUCCESS)
    {
        return E_FAIL;
    }
    return IUnknown_QueryServiceExec(punkSite, c_sidExplorerToolbar, &c_guidPrivCITCommands,
                                     CITIDM_MENUSETTINGSCHANGED, OLECMDEXECOPT_PROMPTUSER, NULL, NULL);
}

static
HRESULT
WINAPI
_OnCloseWindow(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    IWebBrowserApp *pwba = NULL;
    HRESULT hr;

    hr = IUnknown_QueryServiceForWebBrowserApp(punkSite, IID_PPV_ARGS(&pwba));
    if (SUCCEEDED(hr))
        hr = pwba->Quit();
    if (pwba)
        pwba->Release();
    return hr;
}

/* Commands the common state handler answers for, by command store name */
static const struct
{
    PCWSTR pszName;
    PFNCOMMANDSTATE pfnState;
} c_rgCommonCommandStates[] =
{
    { L"Windows.newfolder", _CanNewFolder },
    { L"Windows.email", _CanEmail },
    { L"Windows.burn", _CanSendToCD },
    { L"Windows.includeinlibrary", _CanIncludeInLibrary },
    { L"Windows.paste", _CanPaste },
    { L"Windows.undo", _CanUndo },
    { L"Windows.redo", _CanRedo },
    { L"Windows.selectall", _CanSelectAll },
    { L"Windows.menubar", _CanToggleClassicMenus },
    { L"Windows.closewindow", _CanCloseWindow },
};

/* CCommonCommandState */

CCommonCommandState::CCommonCommandState() :
    _cRef(1)
{
}

CCommonCommandState::~CCommonCommandState()
{
    CoTaskMemFree(_pszCommandName);
    IUnknown_SafeReleaseAndNullPtr(&_ppb);
}

HRESULT
WINAPI
CCommonCommandState::CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CCommonCommandState *pState;
    HRESULT hr;

    *ppv = NULL;
    pState = new CCommonCommandState();
    if (!pState)
        return E_OUTOFMEMORY;

    hr = pState->QueryInterface(riid, ppv);
    pState->Release();
    return hr;
}

STDMETHODIMP
CCommonCommandState::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CCommonCommandState, IObjectWithSite),
        QITABENT(CCommonCommandState, IExplorerCommandState),
        QITABENT(CCommonCommandState, IInitializeCommand),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CCommonCommandState::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CCommonCommandState::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CCommonCommandState::Initialize(
    LPCWSTR pszCommandName,
    IPropertyBag *ppb)
{
    CoTaskMemFree(_pszCommandName);
    _pszCommandName = NULL;
    IUnknown_Set((IUnknown **)&_ppb, ppb);
    return SHStrDupW(pszCommandName, &_pszCommandName);
}

STDMETHODIMP
CCommonCommandState::GetState(
    IShellItemArray *psiItemArray,
    BOOL fOkToBeSlow,
    EXPCMDSTATE *pCmdState)
{
    *pCmdState = ECS_DISABLED;
    if (!_pszCommandName)
        return E_UNEXPECTED;

    for (UINT i = 0; i < _countof(c_rgCommonCommandStates); i++)
    {
        if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, _pszCommandName, -1,
                           c_rgCommonCommandStates[i].pszName, -1) == CSTR_EQUAL)
        {
            return c_rgCommonCommandStates[i].pfnState(_punkSite, psiItemArray, fOkToBeSlow, pCmdState);
        }
    }
    return E_UNEXPECTED;
}

/* CExecuteCommon */

CExecuteCommon::CExecuteCommon(
    _In_ PFNCOMMANDEXECUTE pfnExecute) :
    _cRef(1),
    _pfnExecute(pfnExecute)
{
}

CExecuteCommon::~CExecuteCommon()
{
    CoTaskMemFree(_pszParameters);
    CoTaskMemFree(_pszDirectory);
    IUnknown_SafeReleaseAndNullPtr(&_pae);
    IUnknown_SafeReleaseAndNullPtr(&_psia);
}

HRESULT
WINAPI
CExecuteCommon::s_CreateInstance(
    _In_ PFNCOMMANDEXECUTE pfnExecute,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CExecuteCommon *pExecute;
    HRESULT hr;

    *ppv = NULL;
    pExecute = new CExecuteCommon(pfnExecute);
    if (!pExecute)
        return E_OUTOFMEMORY;

    hr = pExecute->QueryInterface(riid, ppv);
    pExecute->Release();
    return hr;
}

STDMETHODIMP
CExecuteCommon::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CExecuteCommon, IExecuteCommand),
        { &c_iidObjectWithAssociationElement, OFFSETOFCLASS(IObjectWithAssociationElement, CExecuteCommon) },
        QITABENT(CExecuteCommon, IObjectWithSite),
        QITABENT(CExecuteCommon, IObjectWithSelection),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CExecuteCommon::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CExecuteCommon::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CExecuteCommon::SetKeyState(
    DWORD grfKeyState)
{
    _grfKeyState = grfKeyState;
    return S_OK;
}

STDMETHODIMP
CExecuteCommon::SetParameters(
    LPCWSTR pszParameters)
{
    CoTaskMemFree(_pszParameters);
    _pszParameters = NULL;
    return SHStrDupW(pszParameters, &_pszParameters);
}

STDMETHODIMP
CExecuteCommon::SetPosition(
    POINT pt)
{
    _pt = pt;
    return S_OK;
}

STDMETHODIMP
CExecuteCommon::SetShowWindow(
    int nShow)
{
    _nShow = nShow;
    return S_OK;
}

STDMETHODIMP
CExecuteCommon::SetNoShowUI(
    BOOL fNoShowUI)
{
    _fNoShowUI = fNoShowUI;
    return S_OK;
}

STDMETHODIMP
CExecuteCommon::SetDirectory(
    LPCWSTR pszDirectory)
{
    CoTaskMemFree(_pszDirectory);
    _pszDirectory = NULL;
    return SHStrDupW(pszDirectory, &_pszDirectory);
}

STDMETHODIMP
CExecuteCommon::Execute()
{
    return _pfnExecute(_punkSite, _psia, NULL);
}

STDMETHODIMP
CExecuteCommon::SetAssocElement(
    IAssociationElement *pae)
{
    IUnknown_Set((IUnknown **)&_pae, pae);
    return S_OK;
}

STDMETHODIMP
CExecuteCommon::GetAssocElement(
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (!_pae)
        return E_NOINTERFACE;
    return _pae->QueryInterface(riid, ppv);
}

STDMETHODIMP
CExecuteCommon::SetSelection(
    IShellItemArray *psia)
{
    IUnknown_Set((IUnknown **)&_psia, psia);
    return S_OK;
}

STDMETHODIMP
CExecuteCommon::GetSelection(
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (!_psia)
        return E_NOINTERFACE;
    return _psia->QueryInterface(riid, ppv);
}

/* CFolderOptionsCommand */

CFolderOptionsCommand::CFolderOptionsCommand() :
    _cRef(1)
{
}

HRESULT
WINAPI
CFolderOptionsCommand::CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CFolderOptionsCommand *pCommand;
    HRESULT hr;

    *ppv = NULL;
    pCommand = new CFolderOptionsCommand();
    if (!pCommand)
        return E_OUTOFMEMORY;

    hr = pCommand->QueryInterface(riid, ppv);
    pCommand->Release();
    return hr;
}

STDMETHODIMP
CFolderOptionsCommand::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFolderOptionsCommand, IObjectWithSite),
        QITABENT(CFolderOptionsCommand, IExplorerCommand),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CFolderOptionsCommand::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CFolderOptionsCommand::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

static
HRESULT
LoadStringAlloc(
    _In_ UINT idString,
    _Outptr_ PWSTR *ppsz)
{
    WCHAR szText[MAX_PATH];

    *ppsz = NULL;
    if (!LoadStringW(DUIFRAME_HINST, idString, szText, _countof(szText)))
        return HRESULT_FROM_WIN32(GetLastError());
    return SHStrDupW(szText, ppsz);
}

/* With search options to set, the title says so */
STDMETHODIMP
CFolderOptionsCommand::GetTitle(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszName)
{
    return LoadStringAlloc(IsSearchEnabled() ? IDS_FOLDERANDSEARCHOPTIONS : IDS_FOLDEROPTIONS, ppszName);
}

STDMETHODIMP
CFolderOptionsCommand::GetIcon(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszIcon)
{
    *ppszIcon = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CFolderOptionsCommand::GetToolTip(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszInfotip)
{
    return LoadStringAlloc(IDS_FOLDEROPTIONS_TIP, ppszInfotip);
}

STDMETHODIMP
CFolderOptionsCommand::GetCanonicalName(
    GUID *pguidCommandName)
{
    *pguidCommandName = UICID_ExplorerOptions;
    return S_OK;
}

/* Enabled when the browser has folder options to show */
STDMETHODIMP
CFolderOptionsCommand::GetState(
    IShellItemArray *psiItemArray,
    BOOL fOkToBeSlow,
    EXPCMDSTATE *pCmdState)
{
    IOleCommandTarget *pct;

    *pCmdState = ECS_DISABLED;
    if (!SHRestricted(REST_NOFOLDEROPTIONS) &&
        SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IShellBrowser, IID_PPV_ARGS(&pct))))
    {
        OLECMD cmd = { SBCMDID_FOLDEROPTIONS, 0 };

        if (SUCCEEDED(pct->QueryStatus(&CGID_Explorer, 1, &cmd, NULL)) && (cmd.cmdf & OLECMDF_ENABLED))
            *pCmdState = ECS_ENABLED;
        pct->Release();
    }
    return S_OK;
}

STDMETHODIMP
CFolderOptionsCommand::Invoke(
    IShellItemArray *psiItemArray,
    IBindCtx *pbc)
{
    return IUnknown_QueryServiceExec(_punkSite, IID_IShellBrowser, &CGID_Explorer, SBCMDID_FOLDEROPTIONS, 0, NULL,
                                     NULL);
}

STDMETHODIMP
CFolderOptionsCommand::GetFlags(
    EXPCMDFLAGS *pFlags)
{
    *pFlags = ECF_DEFAULT;
    return S_OK;
}

STDMETHODIMP
CFolderOptionsCommand::EnumSubCommands(
    IEnumExplorerCommand **ppEnum)
{
    *ppEnum = NULL;
    return E_NOTIMPL;
}

/* Class objects of the command bar */

HRESULT
WINAPI
CommandBar_CreateInstance(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    *ppv = NULL;
    if (IsEqualCLSID(rclsid, CLSID_CommandFolder))
        return CCommandFolder::CreateInstance(riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_CommonCommandState))
        return CCommonCommandState::CreateInstance(riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_FolderOptionsCommand))
        return CFolderOptionsCommand::CreateInstance(riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_SelectAllExecute))
        return CExecuteCommon::s_CreateInstance(_OnSelectAll, riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_RenameExecute))
        return CExecuteCommon::s_CreateInstance(_OnRename, riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_CloseWindowExecute))
        return CExecuteCommon::s_CreateInstance(_OnCloseWindow, riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_MenuBarExecute))
        return CExecuteCommon::s_CreateInstance(_OnToggleClassicMenus, riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_UndoExecute))
        return CExecuteCommon::s_CreateInstance(_OnUndo, riid, ppv);
    if (IsEqualCLSID(rclsid, CLSID_RedoExecute))
        return CExecuteCommon::s_CreateInstance(_OnRedo, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}
