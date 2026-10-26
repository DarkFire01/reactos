/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Commands described by the Explorer CommandStore and the enumerators that list them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

static const WCHAR c_szCommandStore[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore";
static const WCHAR c_szImpliedSelectionModel[] = L"ImpliedSelectionModel";
static const WCHAR c_szSubCommands[] = L"SubCommands";
static const WCHAR c_szPaneVisibleProperty[] = L"PaneVisibleProperty";
static const WCHAR c_szCommandStateSync[] = L"CommandStateSync";

/* System.ItemUrl and System.ParsingPath */
static const PROPERTYKEY c_pkeyItemUrl =
    { { 0x49691c90, 0x7e17, 0x101a, { 0xa9, 0x1c, 0x08, 0x00, 0x2b, 0x2e, 0xcd, 0xa9 } }, 9 };
static const PROPERTYKEY c_pkeyParsingPath =
    { { 0x28636aa6, 0x953d, 0x11d2, { 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0 } }, 30 };

/* Verb filters handed to the selection's context menu when looking for a verb */
#define VERBQUERY_STATICONLY    0x10
#define VERBQUERY_DEFAULT       0x400

static
int
CALLBACK
FreeNameCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    CoTaskMemFree(p);
    return TRUE;
}

static
int
CALLBACK
ReleaseEnumCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    ((IUnknown *)p)->Release();
    return TRUE;
}

/* Folder type descriptions are not implemented in shell32 yet, so a type has no known parent */
static
HRESULT
_GetOrCreateFolderTypeDescription(
    _In_ REFGUID rguidFolderType,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    *ppv = NULL;
    return E_NOTIMPL;
}

/* URL helpers for the activation filters */

static
BOOL
Key_AcceptsUrl(
    _In_ HKEY hkey,
    _In_ PCWSTR pszUrl,
    _In_ PCWSTR pszSubKey)
{
    WCHAR szScheme[MAX_PATH];
    DWORD cchScheme = _countof(szScheme);

    if (SHGetValueW(hkey, pszSubKey, L"*", NULL, NULL, NULL) == ERROR_SUCCESS)
        return TRUE;

    return SUCCEEDED(UrlGetPartW(pszUrl, szScheme, &cchScheme, URL_PART_SCHEME, 0)) &&
           SHGetValueW(hkey, pszSubKey, szScheme, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static
BOOL
Class_AcceptsUrl(
    _In_ PCWSTR pszUrl,
    _In_ PCWSTR pszClassKey)
{
    WCHAR szSubKey[MAX_PATH];

    if (FAILED(StringCchPrintfW(szSubKey, _countof(szSubKey), L"%s\\SupportedProtocols", pszClassKey)))
        return FALSE;
    return Key_AcceptsUrl(HKEY_CLASSES_ROOT, pszUrl, szSubKey);
}

/* Copies the invoke parameters of a menu invocation into a delegate */
static
HRESULT
InitCommandExec(
    _In_ const CMINVOKECOMMANDINFOEX *pici,
    _In_ IExecuteCommand *pexec)
{
    HRESULT hr = pexec->SetShowWindow(pici->nShow);

    if (SUCCEEDED(hr) && (pici->fMask & (CMIC_MASK_CONTROL_DOWN | CMIC_MASK_SHIFT_DOWN)))
    {
        DWORD grfKeyState = (pici->fMask & CMIC_MASK_SHIFT_DOWN) ? MK_SHIFT : 0;

        if (pici->fMask & CMIC_MASK_CONTROL_DOWN)
            grfKeyState |= MK_CONTROL;
        hr = pexec->SetKeyState(grfKeyState);
    }
    if (SUCCEEDED(hr) && (pici->fMask & CMIC_MASK_PTINVOKE))
        hr = pexec->SetPosition(pici->ptInvoke);
    if (SUCCEEDED(hr) && pici->lpDirectoryW)
        hr = pexec->SetDirectory(pici->lpDirectoryW);
    if (SUCCEEDED(hr) && pici->lpParametersW)
        hr = pexec->SetParameters(pici->lpParametersW);
    if (SUCCEEDED(hr))
        hr = pexec->SetNoShowUI(pici->fMask & CMIC_MASK_FLAG_NO_UI);
    return hr;
}

/* CRegDataDrivenCommand */

CRegDataDrivenCommand::CRegDataDrivenCommand() :
    _cRef(1)
{
}

CRegDataDrivenCommand::~CRegDataDrivenCommand()
{
    if (_hkeyCommand)
        RegCloseKey(_hkeyCommand);
    if (_hkeyStore)
        RegCloseKey(_hkeyStore);
    if (_pecHandler)
        _pecHandler->Release();
    if (_psiaImplied)
        _psiaImplied->Release();
    if (_psiaSelection)
        _psiaSelection->Release();
    CoTaskMemFree(_pszName);
}

HRESULT
WINAPI
CRegDataDrivenCommand::CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CRegDataDrivenCommand *pCommand;
    HRESULT hr;

    *ppv = NULL;
    pCommand = new CRegDataDrivenCommand();
    if (!pCommand)
        return E_OUTOFMEMORY;

    hr = pCommand->QueryInterface(riid, ppv);
    pCommand->Release();
    return hr;
}

HRESULT
WINAPI
GetRegDataDrivenCommand(
    _In_ HKEY hkeyStore,
    _In_ PCWSTR pszName,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CRegDataDrivenCommand *pCommand;
    HRESULT hr;

    *ppv = NULL;
    pCommand = new CRegDataDrivenCommand();
    if (!pCommand)
        return E_OUTOFMEMORY;

    hr = pCommand->Initialize(hkeyStore, pszName);
    if (SUCCEEDED(hr))
        hr = pCommand->QueryInterface(riid, ppv);
    pCommand->Release();
    return hr;
}

/* Command by its name in the machine wide command store */
HRESULT
WINAPI
GetCanonicalCommand(
    _In_ PCWSTR pszName,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    LSTATUS lError;
    HKEY hkey;
    HRESULT hr;

    *ppv = NULL;
    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_szCommandStore, 0, KEY_READ, &hkey);
    hr = HRESULT_FROM_WIN32(lError);
    if (SUCCEEDED(hr))
    {
        hr = GetRegDataDrivenCommand(hkey, pszName, riid, ppv);
        RegCloseKey(hkey);
    }
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CRegDataDrivenCommand, IObjectWithSite),
        QITABENT(CRegDataDrivenCommand, IObjectWithSelection),
        QITABENT(CRegDataDrivenCommand, IExplorerCommand),
        QITABENT(CRegDataDrivenCommand, IRegDataDrivenCommand),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CRegDataDrivenCommand::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CRegDataDrivenCommand::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

HRESULT
CRegDataDrivenCommand::Initialize(
    _In_ HKEY hkeyStore,
    _In_ PCWSTR pszName)
{
    WCHAR szSubKey[MAX_PATH];
    PCWSTR pszKeyName = pszName;
    LSTATUS lError;
    GUID clsidHandler;
    HRESULT hr;

    hr = SHStrDupW(pszName, &_pszName);
    if (FAILED(hr))
        return hr;

    lError = RegOpenKeyExW(hkeyStore, NULL, 0, KEY_READ, &_hkeyStore);
    hr = HRESULT_FROM_WIN32(lError);
    if (FAILED(hr))
        return hr;

    /* Menus list a separator as "|" */
    if (!StrCmpICW(pszKeyName, L"|"))
        pszKeyName = L"Windows.separator";

    hr = StringCchPrintfW(szSubKey, _countof(szSubKey), L"%ws\\%ws", L"Shell", pszKeyName);
    if (FAILED(hr))
        return hr;

    lError = RegOpenKeyExW(_hkeyStore, szSubKey, 0, KEY_READ, &_hkeyCommand);
    hr = HRESULT_FROM_WIN32(lError);
    if (FAILED(hr))
        return hr;

    /* A handler object answers every IExplorerCommand call when the entry names one */
    if (SUCCEEDED(SHRegGetGUID(_hkeyCommand, NULL, L"ExplorerCommandHandler", &clsidHandler)))
    {
        hr = SHCoCreateInstance(NULL, &clsidHandler, NULL, IID_PPV_ARGS(&_pecHandler));
        if (FAILED(hr))
            return hr;

        if (_pecHandler)
        {
            hr = IInitializeCommand_InitializeWithHKEY(_pecHandler, _pszName, _hkeyCommand);
            IUnknown_SetSelection(_pecHandler, _GetImpliedSelection(_psiaSelection));
        }
    }

    if (FAILED(hr))
        return hr;

    _fDelegateIsCommandKey = TRUE;
    _fAcceptsUrls = FALSE;
    if (SUCCEEDED(SHRegGetStringExW(_hkeyCommand, L"command", L"DelegateExecute", SRRF_RT_REG_SZ, _szDelegate,
                                    _countof(_szDelegate))))
    {
        _fAcceptsUrls = TRUE;
        return hr;
    }

    if (SUCCEEDED(SHRegGetStringExW(_hkeyCommand, L"DropTarget", L"CLSID", SRRF_RT_REG_SZ, _szDelegate,
                                    _countof(_szDelegate))))
    {
        if (SUCCEEDED(StringCchPrintfW(szSubKey, _countof(szSubKey), L"CLSID\\%s", _szDelegate)))
            _fAcceptsUrls = Class_AcceptsUrl(L"*", szSubKey);
    }
    else
    {
        _fDelegateIsCommandKey = FALSE;
        if (SUCCEEDED(StringCchPrintfW(szSubKey, _countof(szSubKey), L"%s\\SupportedProtocols", L"command")))
            _fAcceptsUrls = Key_AcceptsUrl(_hkeyCommand, L"*", szSubKey);
    }
    return hr;
}

/* Items the command works on: the ones its implied selection model names, else the selection */
IShellItemArray *
CRegDataDrivenCommand::_GetImpliedSelection(
    _In_opt_ IShellItemArray *psia)
{
    DWORD dwModel;

    if (_psiaImplied)
        return _psiaImplied;

    if (SUCCEEDED(SHRegGetDWORDW(_hkeyCommand, NULL, c_szImpliedSelectionModel, &dwModel)) &&
        SUCCEEDED(SHGetItemArrayOfSelection(_punkSite, psia, dwModel, &_psiaImplied)))
    {
        return _psiaImplied;
    }
    return psia;
}

/* Commands that list the verbs of another registry key as their sub commands */
BOOL
CRegDataDrivenCommand::_HasExtendedSubCommands(
    _Out_opt_ HKEY *phkey)
{
    WCHAR szKey[MAX_PATH];
    WCHAR szShellKey[MAX_PATH];
    BOOL fHas = FALSE;
    HKEY hkeyShell;

    if (phkey)
        *phkey = NULL;

    if (FAILED(SHRegGetStringExW(_hkeyCommand, NULL, L"ExtendedSubCommandsKey", SRRF_RT_REG_SZ, szKey,
                                 _countof(szKey))))
    {
        return FALSE;
    }

    if (SUCCEEDED(StringCchPrintfW(szShellKey, _countof(szShellKey), L"%ws\\%ws", szKey, L"Shell")) &&
        RegOpenKeyExW(HKEY_CLASSES_ROOT, szShellKey, 0, KEY_ENUMERATE_SUB_KEYS, &hkeyShell) == ERROR_SUCCESS)
    {
        WCHAR szVerb[80];

        if (RegEnumKeyW(hkeyShell, 0, szVerb, _countof(szVerb)) == ERROR_SUCCESS)
            fHas = TRUE;
        RegCloseKey(hkeyShell);
    }

    if (phkey && fHas && RegOpenKeyExW(HKEY_CLASSES_ROOT, szKey, 0, KEY_READ, phkey) != ERROR_SUCCESS)
        fHas = FALSE;
    return fHas;
}

STDMETHODIMP
CRegDataDrivenCommand::SetSite(
    IUnknown *punkSite)
{
    if (_pecHandler)
        IUnknown_SetSite(_pecHandler, punkSite);
    IUnknown_Set(&_punkSite, punkSite);
    return S_OK;
}

STDMETHODIMP
CRegDataDrivenCommand::SetSelection(
    IShellItemArray *psia)
{
    if (_pecHandler)
        IUnknown_SetSelection(_pecHandler, _GetImpliedSelection(psia));
    IUnknown_Set((IUnknown **)&_psiaSelection, psia);
    return S_OK;
}

STDMETHODIMP
CRegDataDrivenCommand::GetSelection(
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (!_psiaSelection)
        return E_NOINTERFACE;
    return _psiaSelection->QueryInterface(riid, ppv);
}

STDMETHODIMP
CRegDataDrivenCommand::GetTitle(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszName)
{
    WCHAR szSource[MAX_PATH];
    WCHAR szTitle[80];
    HRESULT hr;

    *ppszName = NULL;
    if (_pecHandler)
        return _pecHandler->GetTitle(_GetImpliedSelection(psiItemArray), ppszName);

    hr = SHRegGetStringExW(_hkeyCommand, NULL, L"MUIVerb", SRRF_RT_REG_SZ, szSource, _countof(szSource));
    if (FAILED(hr))
        hr = SHRegGetStringExW(_hkeyCommand, NULL, NULL, SRRF_RT_REG_SZ, szSource, _countof(szSource));
    if (SUCCEEDED(hr))
        hr = SHLoadIndirectString(szSource, szTitle, _countof(szTitle), NULL);
    if (SUCCEEDED(hr))
        hr = SHStrDupW(szTitle, ppszName);
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::GetIcon(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszIcon)
{
    WCHAR szIcon[MAX_PATH];
    HRESULT hr;

    *ppszIcon = NULL;
    if (_pecHandler)
        return _pecHandler->GetIcon(_GetImpliedSelection(psiItemArray), ppszIcon);

    hr = SHRegGetStringExW(_hkeyCommand, NULL, L"Icon", SRRF_RT_REG_SZ, szIcon, _countof(szIcon));
    if (SUCCEEDED(hr))
        hr = SHStrDupW(szIcon, ppszIcon);
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::GetToolTip(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszInfotip)
{
    WCHAR szSource[MAX_PATH];
    WCHAR szToolTip[1024];
    HRESULT hr;

    *ppszInfotip = NULL;
    if (_pecHandler)
        return _pecHandler->GetToolTip(_GetImpliedSelection(psiItemArray), ppszInfotip);

    hr = SHRegGetStringExW(_hkeyCommand, NULL, L"Description", SRRF_RT_REG_SZ, szSource, _countof(szSource));
    if (SUCCEEDED(hr))
        hr = SHLoadIndirectString(szSource, szToolTip, _countof(szToolTip), NULL);
    if (SUCCEEDED(hr))
        hr = SHStrDupW(szToolTip, ppszInfotip);
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::GetCanonicalName(
    GUID *pguidCommandName)
{
    if (_pecHandler)
        return _pecHandler->GetCanonicalName(pguidCommandName);
    return SHRegGetGUID(_hkeyCommand, NULL, L"CanonicalName", pguidCommandName);
}

/* Hidden unless the selection's attributes match AttributeMask and AttributeValue */
HRESULT
CRegDataDrivenCommand::_CheckAttributes(
    _In_opt_ IShellItemArray *psia,
    _Out_ EXPCMDSTATE *pState)
{
    DWORD dwModel;
    DWORD dwMask;
    DWORD dwValue;
    IShellItemArray *psiaTarget = NULL;
    SFGAOF sfgao = 0;
    HRESULT hr = S_FALSE;

    if (FAILED(SHRegGetDWORDW(_hkeyCommand, NULL, c_szImpliedSelectionModel, &dwModel)) ||
        FAILED(SHRegGetDWORDW(_hkeyCommand, NULL, L"AttributeMask", &dwMask)) ||
        FAILED(SHRegGetDWORDW(_hkeyCommand, NULL, L"AttributeValue", &dwValue)))
    {
        return hr;
    }

    hr = SHGetItemArrayOfSelection(_punkSite, psia, dwModel, &psiaTarget);
    if (SUCCEEDED(hr))
    {
        IShellItem *psi = NULL;

        /* The first item decides, as it does for the context menu */
        hr = psiaTarget->GetItemAt(0, &psi);
        if (SUCCEEDED(hr))
            hr = psi->GetAttributes(dwMask, &sfgao);
        if (psi)
            psi->Release();
    }

    *pState = (SUCCEEDED(hr) && dwValue == sfgao) ? ECS_ENABLED : ECS_HIDDEN;
    if (psiaTarget)
        psiaTarget->Release();
    return hr;
}

/* Hidden unless the selection's context menu carries the verb the command is named after */
HRESULT
CRegDataDrivenCommand::_CheckIsVerbOnContextMenu(
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    BOOL fStaticOnly = SHRegValueExists(_hkeyCommand, NULL, L"StaticVerbOnly");
    BOOL fInMenu = FALSE;
    HRESULT hr = S_FALSE;

    IUnknown *punkCommandSite;

    if (!fStaticOnly)
        fInMenu = SHRegValueExists(_hkeyCommand, NULL, L"IsInContextMenu");
    if (!(fStaticOnly || fInMenu))
        return hr;

    /* Only the command bar's own site runs the menu check */
    if (FAILED(IUnknown_QueryService(_punkSite, SID_CommandModuleSite, IID_PPV_ARGS(&punkCommandSite))))
        return hr;
    punkCommandSite->Release();

    if (!fOkToBeSlow && fInMenu)
    {
        *pState = ECS_DISABLED;
        return E_PENDING;
    }

    PCWSTR pszExtension = PathFindExtensionW(_pszName);
    PCWSTR pszVerb = (pszExtension && *pszExtension) ? pszExtension + 1 : _pszName;
    CHAR szVerb[80];
    BOOL fAvailable = FALSE;
    DWORD dwModel = SELMODEL_NONE;

    hr = E_FAIL;
    if (SHUnicodeToAnsi(pszVerb, szVerb, _countof(szVerb)))
    {
        BOOL fAnyState = SHRegValueExists(_hkeyCommand, NULL, L"EnableIfContextMenuVerbExists");

        SHRegGetDWORDW(_hkeyCommand, NULL, c_szImpliedSelectionModel, &dwModel);
        hr = SHIsVerbAvailableOnSelection(_punkSite, psia, dwModel,
                                          (fStaticOnly ? VERBQUERY_STATICONLY : 0) | VERBQUERY_DEFAULT,
                                          fAnyState, szVerb, &fAvailable);
    }

    *pState = (SUCCEEDED(hr) && fAvailable) ? ECS_ENABLED : ECS_HIDDEN;
    return hr;
}

/* Pane toggles show a check mark for the pane's visibility */
HRESULT
CRegDataDrivenCommand::_CheckTogglePane(
    _Out_ EXPCMDSTATE *pState)
{
    WCHAR szProperty[MAX_PATH];
    GUID guidPolicy = GUID_NULL;
    GUID guidPane;

    if (FAILED(SHRegGetStringExW(_hkeyCommand, NULL, c_szPaneVisibleProperty, SRRF_RT_REG_SZ, szProperty,
                                 _countof(szProperty))) ||
        FAILED(SHRegGetGUID(_hkeyCommand, NULL, L"PaneID", &guidPane)))
    {
        return S_FALSE;
    }

    SHRegGetGUID(_hkeyCommand, NULL, L"PolicyID", &guidPolicy);
    return CanShowPaneAux(szProperty, guidPolicy, &guidPane, SID_PerBrowserPropertyBag, _punkSite,
                          (DWORD *)pState);
}

/* AppliesTo holds a query the selection must match; ReactOS has no query engine to run it */
HRESULT
CRegDataDrivenCommand::_CheckEnabledStateFromAQSQuery(
    _In_opt_ IShellItemArray *psia,
    _Out_ EXPCMDSTATE *pState)
{
    WCHAR szQuery[MAX_PATH];

    if (FAILED(SHRegGetStringExW(_hkeyCommand, NULL, L"AppliesTo", SRRF_RT_REG_SZ, szQuery, _countof(szQuery))))
        return S_FALSE;

    *pState = ECS_HIDDEN;
    return S_OK;
}

HRESULT
CRegDataDrivenCommand::_CheckCommandStateHandler(
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow,
    _Out_ EXPCMDSTATE *pState)
{
    IExplorerCommandState *pecs = NULL;
    GUID clsidState;
    HRESULT hr = S_FALSE;

    if (FAILED(SHRegGetGUID(_hkeyCommand, NULL, L"CommandStateHandler", &clsidState)))
        return hr;

    *pState = ECS_DISABLED;

    /* Handlers that may be slow run on a background thread unless the caller can wait */
    if (!SHRegValueExists(_hkeyCommand, NULL, c_szCommandStateSync) && !fOkToBeSlow)
        return E_PENDING;

    hr = SHCoCreateInstance(NULL, &clsidState, NULL, IID_PPV_ARGS(&pecs));
    if (SUCCEEDED(hr))
    {
        IUnknown_SetSite(pecs, _punkSite);
        hr = IInitializeCommand_InitializeWithHKEY(pecs, _pszName, _hkeyCommand);
        if (SUCCEEDED(hr))
            hr = pecs->GetState(_GetImpliedSelection(psia), fOkToBeSlow, pState);
        IUnknown_SetSite(pecs, NULL);
    }
    if (pecs)
        pecs->Release();
    return hr;
}

/* Commands that refuse read only items, internet items or large downloads */
HRESULT
CRegDataDrivenCommand::_CheckActivationFilters(
    _In_opt_ IShellItemArray *psia,
    _Out_ EXPCMDSTATE *pState)
{
    BOOL fReadWrite = SHRegValueExists(_hkeyCommand, NULL, L"ReadWriteRequired");
    BOOL fNoDownload = SHRegValueExists(_hkeyCommand, NULL, L"DownloadInvokeDisabled");
    DWORD cbMaxDownload = 0;
    DWORD cItems = 0;
    HRESULT hr = S_FALSE;

    if (!fReadWrite && !fNoDownload &&
        !(SUCCEEDED(SHRegGetDWORDW(_hkeyCommand, NULL, L"MaxDownloadFileSize", &cbMaxDownload)) && cbMaxDownload))
    {
        return hr;
    }

    if (!psia)
        return hr;

    hr = IShellItemArray_GetEstimatedCount(psia, &cItems);
    cItems = min(cItems, 16);

    for (DWORD i = 0; i < cItems && SUCCEEDED(hr); i++)
    {
        IShellItem2 *psi2 = NULL;
        IShellItem *psi;
        BOOL fBlocked = FALSE;

        hr = psia->GetItemAt(i, &psi);
        if (SUCCEEDED(hr))
        {
            hr = psi->QueryInterface(IID_PPV_ARGS(&psi2));
            psi->Release();
        }

        if (SUCCEEDED(hr))
        {
            SFGAOF sfgao;

            if (fReadWrite && SUCCEEDED(psi2->GetAttributes(SFGAO_READONLY, &sfgao)) && sfgao == SFGAO_READONLY)
            {
                fBlocked = TRUE;
            }
            else if (!_fAcceptsUrls)
            {
                PWSTR pszUrl = NULL;

                hr = psi2->GetString(c_pkeyItemUrl, &pszUrl);
                if (FAILED(hr))
                    hr = psi2->GetString(c_pkeyParsingPath, &pszUrl);

                if (SUCCEEDED(hr) && UrlIsW(pszUrl, URLIS_URL))
                {
                    if (fNoDownload)
                    {
                        WCHAR szKey[MAX_PATH];

                        if (_fDelegateIsCommandKey)
                        {
                            if (SUCCEEDED(StringCchPrintfW(szKey, _countof(szKey), L"CLSID\\%s", _szDelegate)))
                                fBlocked = !Class_AcceptsUrl(pszUrl, szKey);
                            else
                                fBlocked = TRUE;
                        }
                        else if (SUCCEEDED(StringCchPrintfW(szKey, _countof(szKey), L"%s\\SupportedProtocols",
                                                            L"command")))
                        {
                            fBlocked = !Key_AcceptsUrl(_hkeyCommand, pszUrl, szKey);
                        }
                        else
                        {
                            fBlocked = TRUE;
                        }
                    }
                    else if (cbMaxDownload)
                    {
                        ULONGLONG ullSize;

                        hr = psi2->GetUInt64(PKEY_Size, &ullSize);
                        if (SUCCEEDED(hr))
                            fBlocked = (ullSize >> 10) > cbMaxDownload;
                    }
                }
                CoTaskMemFree(pszUrl);
            }
        }

        if (FAILED(hr) || fBlocked)
        {
            *pState = ECS_DISABLED;
            if (psi2)
                psi2->Release();
            return hr;
        }
        psi2->Release();
    }
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::GetState(
    IShellItemArray *psiItemArray,
    BOOL fOkToBeSlow,
    EXPCMDSTATE *pCmdState)
{
    HRESULT hr = S_OK;

    if (_pecHandler)
    {
        if (!SHRegValueExists(_hkeyCommand, NULL, c_szCommandStateSync) && !fOkToBeSlow)
        {
            *pCmdState = ECS_DISABLED;
            hr = E_PENDING;
        }
        else
        {
            hr = _pecHandler->GetState(_GetImpliedSelection(psiItemArray), fOkToBeSlow, pCmdState);
        }
    }
    else
    {
        *pCmdState = ECS_ENABLED;

        /* Server only commands */
        OSVERSIONINFOEXW osvi = { sizeof(osvi) };
        if (SHRegValueExists(_hkeyCommand, NULL, L"ClientOnly") && GetVersionExW((OSVERSIONINFOW *)&osvi) &&
            (osvi.wProductType == VER_NT_SERVER || osvi.wProductType == VER_NT_DOMAIN_CONTROLLER))
        {
            *pCmdState = ECS_HIDDEN;
        }
        else
        {
            /* Each check runs only while the command is still visible and enabled */
            hr = _CheckAttributes(psiItemArray, pCmdState);
            if (SUCCEEDED(hr) && !(*pCmdState & (ECS_DISABLED | ECS_HIDDEN)))
                hr = _CheckIsVerbOnContextMenu(psiItemArray, fOkToBeSlow, pCmdState);
            if (SUCCEEDED(hr) && !(*pCmdState & (ECS_DISABLED | ECS_HIDDEN)))
                hr = _CheckTogglePane(pCmdState);
            if (SUCCEEDED(hr) && !(*pCmdState & (ECS_DISABLED | ECS_HIDDEN)))
                hr = _CheckEnabledStateFromAQSQuery(psiItemArray, pCmdState);
            if (SUCCEEDED(hr) && !(*pCmdState & (ECS_DISABLED | ECS_HIDDEN)))
                hr = _CheckCommandStateHandler(psiItemArray, fOkToBeSlow, pCmdState);
            if (SUCCEEDED(hr) && !(*pCmdState & (ECS_DISABLED | ECS_HIDDEN)))
                hr = _CheckActivationFilters(psiItemArray, pCmdState);
        }
    }

    if ((*pCmdState & ECS_HIDDEN) && SHRegValueExists(_hkeyCommand, NULL, L"ShowAsDisabledIfHidden"))
    {
        *pCmdState &= ~ECS_HIDDEN;
        *pCmdState |= ECS_DISABLED;
    }
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::GetFlags(
    EXPCMDFLAGS *pFlags)
{
    WCHAR szShield[MAX_PATH];
    DWORD dwFlags;

    *pFlags = ECF_DEFAULT;
    if (_pecHandler)
        return _pecHandler->GetFlags(pFlags);

    if (!StrCmpICW(_pszName, L"|"))
    {
        *pFlags = ECF_ISSEPARATOR;
        return S_OK;
    }

    if (SUCCEEDED(SHRegGetDWORDW(_hkeyCommand, NULL, L"CommandFlags", &dwFlags)))
        *pFlags |= (EXPCMDFLAGS)dwFlags;
    if (SHRegValueExists(_hkeyCommand, NULL, c_szSubCommands) || _HasExtendedSubCommands(NULL))
        *pFlags |= ECF_HASSUBCOMMANDS;

    /* An empty HasLUAShield always shows the shield; a query needs a matching selection */
    if (SUCCEEDED(SHRegGetStringExW(_hkeyCommand, NULL, L"HasLUAShield", SRRF_RT_REG_SZ, szShield,
                                    _countof(szShield))) &&
        !szShield[0])
    {
        *pFlags |= ECF_HASLUASHIELD;
    }
    return S_OK;
}

STDMETHODIMP
CRegDataDrivenCommand::EnumSubCommands(
    IEnumExplorerCommand **ppEnum)
{
    IEnumExplorerCommand *penumList = NULL;
    IEnumExplorerCommand *penumExtended = NULL;
    HKEY hkeyExtended;
    HRESULT hr = S_OK;

    *ppEnum = NULL;
    if (_pecHandler)
        return _pecHandler->EnumSubCommands(ppEnum);

    if (SHRegValueExists(_hkeyCommand, NULL, c_szSubCommands))
        hr = GetExplorerCommandEnumeratorForRegKey(_hkeyCommand, IID_PPV_ARGS(&penumList));

    if (SUCCEEDED(hr) && _HasExtendedSubCommands(&hkeyExtended))
    {
        hr = GetExplorerCommandEnumeratorForRegKey(hkeyExtended, IID_PPV_ARGS(&penumExtended));
        RegCloseKey(hkeyExtended);
    }

    if (SUCCEEDED(hr))
    {
        if (penumList && penumExtended)
            hr = CEnumExplorerCommandAggregator::CreateInstance(penumList, penumExtended, IID_PPV_ARGS(ppEnum));
        else if (penumList)
            hr = penumList->QueryInterface(IID_PPV_ARGS(ppEnum));
        else if (penumExtended)
            hr = penumExtended->QueryInterface(IID_PPV_ARGS(ppEnum));
        else
            hr = E_FAIL;
    }

    if (penumExtended)
        penumExtended->Release();
    if (penumList)
        penumList->Release();
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::SetKeyAndName(
    HKEY hkey,
    PCWSTR pszName)
{
    return Initialize(hkey, pszName);
}

STDMETHODIMP
CRegDataDrivenCommand::GetKeyAndName(
    HKEY *phkey,
    PWSTR *ppszName)
{
    LSTATUS lError;
    HRESULT hr;

    *ppszName = NULL;
    lError = RegOpenKeyExW(_hkeyStore, NULL, 0, KEY_READ, phkey);
    hr = HRESULT_FROM_WIN32(lError);
    if (SUCCEEDED(hr))
        hr = SHStrDupW(_pszName, ppszName);

    if (FAILED(hr) && *phkey)
    {
        RegCloseKey(*phkey);
        *phkey = NULL;
    }
    return hr;
}

/* Position is "Last", or "Before"/"After" the command named by PositionCompare */
STDMETHODIMP
CRegDataDrivenCommand::GetPosition(
    COMMANDPOSITION *pPosition,
    GUID *pguidRelative)
{
    WCHAR szPosition[40];
    HRESULT hr;

    *pPosition = CMDPOSITION_DEFAULT;
    *pguidRelative = GUID_NULL;

    hr = SHRegGetStringExW(_hkeyCommand, NULL, L"Position", SRRF_RT_REG_SZ, szPosition, _countof(szPosition));
    if (FAILED(hr))
        return hr;

    if (!StrCmpICW(szPosition, L"Last"))
    {
        *pPosition = CMDPOSITION_LAST;
        return hr;
    }

    if (!StrCmpICW(szPosition, L"Before"))
        *pPosition = CMDPOSITION_BEFORE;
    else if (!StrCmpICW(szPosition, L"After"))
        *pPosition = CMDPOSITION_AFTER;

    if (*pPosition == CMDPOSITION_BEFORE || *pPosition == CMDPOSITION_AFTER)
        hr = SHRegGetGUID(_hkeyCommand, NULL, L"PositionCompare", pguidRelative);
    return hr;
}

STDMETHODIMP
CRegDataDrivenCommand::InvokeFromContextMenu(
    const CMINVOKECOMMANDINFOEX *pici,
    IShellItemArray *psia)
{
    return _Invoke(pici, psia, NULL);
}

STDMETHODIMP
CRegDataDrivenCommand::Invoke(
    IShellItemArray *psiItemArray,
    IBindCtx *pbc)
{
    return _Invoke(NULL, psiItemArray, pbc);
}

/* The verb named after the command, run on the context menu of the selection */
HRESULT
CRegDataDrivenCommand::_TryInvokeOnSelection(
    _In_opt_ IShellItemArray *psia,
    _Out_ BOOL *pfHandled)
{
    DWORD fOnSelection;
    DWORD dwModel;
    HRESULT hr = E_FAIL;

    *pfHandled = FALSE;
    if (FAILED(SHRegGetDWORDW(_hkeyCommand, NULL, L"InvokeCommandOnSelection", &fOnSelection)) ||
        FAILED(SHRegGetDWORDW(_hkeyCommand, NULL, c_szImpliedSelectionModel, &dwModel)))
    {
        return hr;
    }

    PCWSTR pszExtension = PathFindExtensionW(_pszName);
    PCWSTR pszVerb = (pszExtension && *pszExtension) ? pszExtension + 1 : _pszName;
    CHAR szVerb[80];

    if (SHUnicodeToAnsi(pszVerb, szVerb, _countof(szVerb)))
        hr = SHInvokeCommandOnSelection(_punkSite, fOnSelection ? psia : NULL, dwModel, szVerb);
    *pfHandled = TRUE;
    return hr;
}

HRESULT
CRegDataDrivenCommand::_TryOpenControlPanel(
    _Out_ BOOL *pfHandled)
{
    WCHAR szName[MAX_PATH] = L"";
    WCHAR szPage[MAX_PATH] = L"";
    IOpenControlPanel *pocp = NULL;
    HRESULT hr = E_FAIL;

    *pfHandled = FALSE;
    SHRegGetStringExW(_hkeyCommand, NULL, L"OpenControlPanel", SRRF_RT_REG_SZ, szName, _countof(szName));
    SHRegGetStringExW(_hkeyCommand, NULL, L"OpenControlPanelPage", SRRF_RT_REG_SZ, szPage, _countof(szPage));
    if (!szName[0] && !szPage[0])
        return hr;

    hr = CoCreateInstance(CLSID_OpenControlPanel, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER |
                          CLSCTX_REMOTE_SERVER, IID_PPV_ARGS(&pocp));
    if (SUCCEEDED(hr))
        hr = pocp->Open(szName[0] ? szName : NULL, szPage[0] ? szPage : NULL, _punkSite);
    if (pocp)
        pocp->Release();
    *pfHandled = TRUE;
    return hr;
}

/* Pane commands flip the pane's visibility property in the browser's property bag */
HRESULT
CRegDataDrivenCommand::_TryPane(
    _Out_ BOOL *pfHandled)
{
    WCHAR szProperty[MAX_PATH];
    IPropertyBag *ppb = NULL;
    HRESULT hr;

    *pfHandled = FALSE;
    hr = SHRegGetStringExW(_hkeyCommand, NULL, c_szPaneVisibleProperty, SRRF_RT_REG_SZ, szProperty,
                           _countof(szProperty));
    if (FAILED(hr))
        return hr;

    hr = IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb));
    if (SUCCEEDED(hr))
    {
        BOOL fVisible = PropertyBag_ReadBOOLDefault(ppb, szProperty, TRUE);

        hr = SHPropertyBag_WriteBOOL(ppb, szProperty, !fVisible);
    }
    if (ppb)
        ppb->Release();
    *pfHandled = TRUE;
    return hr;
}

HRESULT
CRegDataDrivenCommand::_Invoke(
    _In_opt_ const CMINVOKECOMMANDINFOEX *pici,
    _In_opt_ IShellItemArray *psia,
    _In_opt_ IBindCtx *pbc)
{
    IExecuteCommand *pexec = NULL;
    GUID clsidDelegate;
    HRESULT hr;

    if (_pecHandler)
        return _pecHandler->Invoke(_GetImpliedSelection(psia), pbc);

    /* The command store verb's DelegateExecute object does the work when there is one */
    hr = SHRegGetGUID(_hkeyCommand, L"command", L"DelegateExecute", &clsidDelegate);
    if (SUCCEEDED(hr))
        hr = SHCoCreateInstance(NULL, &clsidDelegate, NULL, IID_PPV_ARGS(&pexec));
    if (SUCCEEDED(hr))
    {
        IUnknown_SetSite(pexec, _punkSite);
        IUnknown_SetSelection(pexec, _GetImpliedSelection(psia));
        if (pici)
            hr = InitCommandExec(pici, pexec);
        else
            hr = pexec->SetShowWindow(SW_SHOW);
        if (SUCCEEDED(hr))
            hr = pexec->Execute();
        IUnknown_SetSite(pexec, NULL);
    }
    if (pexec)
        pexec->Release();

    if (FAILED(hr))
    {
        BOOL fHandled;
        HRESULT hrTry;

        hrTry = _TryInvokeOnSelection(psia, &fHandled);
        if (!fHandled)
            hrTry = _TryOpenControlPanel(&fHandled);
        if (!fHandled)
            hrTry = _TryPane(&fHandled);
        if (fHandled)
            hr = hrTry;
    }
    return hr;
}

/* CExplorerCommandEnumerator */

CExplorerCommandEnumerator::CExplorerCommandEnumerator() :
    _cRef(1)
{
}

CExplorerCommandEnumerator::~CExplorerCommandEnumerator()
{
    if (_hkey)
        RegCloseKey(_hkey);
    if (_hkeyShell)
        RegCloseKey(_hkeyShell);
    if (_hdpaNames)
        DPA_DestroyCallback(_hdpaNames, FreeNameCB, NULL);
}

HRESULT
WINAPI
CExplorerCommandEnumerator::CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CExplorerCommandEnumerator *penum;
    HRESULT hr;

    *ppv = NULL;
    penum = new CExplorerCommandEnumerator();
    if (!penum)
        return E_OUTOFMEMORY;

    hr = penum->QueryInterface(riid, ppv);
    penum->Release();
    return hr;
}

HRESULT
WINAPI
GetExplorerCommandEnumeratorForRegKey(
    _In_ HKEY hkey,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IObjectWithRegistryKey *powrk;
    HRESULT hr;

    *ppv = NULL;
    hr = CExplorerCommandEnumerator::CreateInstance(IID_PPV_ARGS(&powrk));
    if (SUCCEEDED(hr))
    {
        hr = powrk->SetKey(hkey);
        if (SUCCEEDED(hr))
            hr = powrk->QueryInterface(riid, ppv);
        powrk->Release();
    }
    return hr;
}

STDMETHODIMP
CExplorerCommandEnumerator::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CExplorerCommandEnumerator, IEnumExplorerCommand),
        QITABENT(CExplorerCommandEnumerator, IObjectWithRegistryKey),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CExplorerCommandEnumerator::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CExplorerCommandEnumerator::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CExplorerCommandEnumerator::SetKey(
    HKEY hkey)
{
    LSTATUS lError;
    HRESULT hr;

    if (_hkey)
        RegCloseKey(_hkey);
    _hkey = NULL;

    lError = RegOpenKeyExW(hkey, NULL, 0, KEY_READ, &_hkey);
    hr = HRESULT_FROM_WIN32(lError);
    if (SUCCEEDED(hr))
        hr = _Initialize();
    return hr;
}

STDMETHODIMP
CExplorerCommandEnumerator::GetKey(
    HKEY *phkey)
{
    *phkey = NULL;
    if (!_hkey)
        return E_FAIL;
    return HRESULT_FROM_WIN32(RegOpenKeyExW(_hkey, NULL, 0, KEY_READ, phkey));
}

/* The key's default value or SubCommands lists command store names; else its Shell subkey has verbs */
HRESULT
CExplorerCommandEnumerator::_Initialize()
{
    PWSTR pszList = NULL;
    HRESULT hr = S_OK;

    if (FAILED(SHRegAllocString(_hkey, NULL, NULL, &pszList)) &&
        FAILED(SHRegAllocString(_hkey, NULL, c_szSubCommands, &pszList)))
    {
        RegOpenKeyExW(_hkey, L"Shell", 0, KEY_READ, &_hkeyShell);
        CoTaskMemFree(pszList);
        return hr;
    }

    if (!pszList[0])
    {
        RegOpenKeyExW(_hkey, L"Shell", 0, KEY_READ, &_hkeyShell);
        CoTaskMemFree(pszList);
        return hr;
    }

    _hdpaNames = DPA_Create(1);
    if (!_hdpaNames)
    {
        CoTaskMemFree(pszList);
        return E_OUTOFMEMORY;
    }

    PWSTR pszNext = pszList;

    hr = E_FAIL;
    do
    {
        PWSTR pszName = pszNext;

        while (*pszName == L' ')
            pszName++;
        PWSTR pszSeparator = StrChrW(pszName, L';');
        PWSTR pszCopy;

        if (pszSeparator)
            *pszSeparator = UNICODE_NULL;

        if (*pszName)
        {
            hr = SHStrDupW(pszName, &pszCopy);
            if (SUCCEEDED(hr))
            {
                if (DPA_AppendPtr(_hdpaNames, pszCopy) == -1)
                {
                    CoTaskMemFree(pszCopy);
                    hr = E_OUTOFMEMORY;
                }
            }
        }

        if (!pszSeparator)
            break;
        pszNext = pszSeparator + 1;
    } while (*pszNext && SUCCEEDED(hr));

    CoTaskMemFree(pszList);
    return hr;
}

STDMETHODIMP
CExplorerCommandEnumerator::Next(
    ULONG celt,
    IExplorerCommand **pUICommand,
    ULONG *pceltFetched)
{
    ULONG cFetched = 0;

    *pUICommand = NULL;
    if (!celt)
    {
        if (pceltFetched)
            *pceltFetched = 0;
        return S_OK;
    }

    if (_hdpaNames)
    {
        /* Listed names come from the command store; the list ends at a name it does not know */
        if (_iNext < (UINT)DPA_GetPtrCount(_hdpaNames))
        {
            PCWSTR pszName = (PCWSTR)DPA_FastGetPtr(_hdpaNames, _iNext);

            if (SUCCEEDED(GetCanonicalCommand(pszName, IID_PPV_ARGS(pUICommand))))
                cFetched = 1;
            _iNext++;
        }
    }
    else if (_hkeyShell)
    {
        WCHAR szVerb[MAX_PATH];

        if (RegEnumKeyW(_hkeyShell, _iNext, szVerb, _countof(szVerb)) == ERROR_SUCCESS)
        {
            if (SUCCEEDED(GetRegDataDrivenCommand(_hkey, szVerb, IID_PPV_ARGS(pUICommand))))
                cFetched = 1;
            _iNext++;
        }
    }
    else
    {
        /* Vista style task lists: numbered groups, then the unordered group */
        WCHAR szGroup[MAX_PATH];
        HKEY hkeyGroup = NULL;
        DWORD iTask = 0;

        if (SUCCEEDED(StringCchPrintfW(szGroup, _countof(szGroup), L"%d", _iNext)))
        {
            if (RegOpenKeyExW(_hkey, szGroup, 0, KEY_READ, &hkeyGroup) == ERROR_SUCCESS)
            {
                _iNext++;
            }
            else
            {
                StringCchCopyW(szGroup, _countof(szGroup), L"Unordered");
                RegOpenKeyExW(_hkey, szGroup, 0, KEY_READ, &hkeyGroup);
                iTask = _iUnordered++;
            }
        }

        if (hkeyGroup)
        {
            WCHAR szTask[MAX_PATH];
            DWORD cchTask = _countof(szTask);

            /*
             * Each group names a Vista task: Print, Delete or a task id. Those come from the web
             * view task definitions, which shell32 does not have yet, so the group adds nothing.
             */
            RegEnumKeyExW(hkeyGroup, iTask, szTask, &cchTask, NULL, NULL, NULL, NULL);
            RegCloseKey(hkeyGroup);
        }
    }

    if (pceltFetched)
        *pceltFetched = cFetched;
    return (celt == cFetched) ? S_OK : S_FALSE;
}

STDMETHODIMP
CExplorerCommandEnumerator::Skip(
    ULONG celt)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CExplorerCommandEnumerator::Reset()
{
    _iNext = 0;
    _iUnordered = 0;
    return S_OK;
}

STDMETHODIMP
CExplorerCommandEnumerator::Clone(
    IEnumExplorerCommand **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

/* CEnumExplorerCommandAggregator */

CEnumExplorerCommandAggregator::CEnumExplorerCommandAggregator() :
    _cRef(1)
{
}

CEnumExplorerCommandAggregator::~CEnumExplorerCommandAggregator()
{
    if (_hdpaEnums)
        DPA_DestroyCallback(_hdpaEnums, ReleaseEnumCB, NULL);
}

HRESULT
WINAPI
CEnumExplorerCommandAggregator::CreateInstance(
    _In_ IEnumExplorerCommand *penumFirst,
    _In_ IEnumExplorerCommand *penumSecond,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CEnumExplorerCommandAggregator *penum;
    HRESULT hr = E_OUTOFMEMORY;

    *ppv = NULL;
    penum = new CEnumExplorerCommandAggregator();
    if (!penum)
        return hr;

    penum->_hdpaEnums = DPA_Create(2);
    if (penum->_hdpaEnums && DPA_AppendPtr(penum->_hdpaEnums, penumFirst) != -1)
    {
        penumFirst->AddRef();
        if (DPA_AppendPtr(penum->_hdpaEnums, penumSecond) != -1)
        {
            penumSecond->AddRef();
            hr = penum->QueryInterface(riid, ppv);
        }
    }
    penum->Release();
    return hr;
}

STDMETHODIMP
CEnumExplorerCommandAggregator::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CEnumExplorerCommandAggregator, IEnumExplorerCommand),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CEnumExplorerCommandAggregator::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CEnumExplorerCommandAggregator::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CEnumExplorerCommandAggregator::Next(
    ULONG celt,
    IExplorerCommand **pUICommand,
    ULONG *pceltFetched)
{
    ULONG cFetched = 0;

    while (cFetched < celt && _iCurrent < (UINT)DPA_GetPtrCount(_hdpaEnums))
    {
        IEnumExplorerCommand *penum = (IEnumExplorerCommand *)DPA_FastGetPtr(_hdpaEnums, _iCurrent);
        ULONG cThis = 0;

        if (penum->Next(celt - cFetched, pUICommand + cFetched, &cThis) != S_OK)
            _iCurrent++;
        cFetched += cThis;
    }

    if (pceltFetched)
        *pceltFetched = cFetched;
    return (cFetched == celt) ? S_OK : S_FALSE;
}

STDMETHODIMP
CEnumExplorerCommandAggregator::Skip(
    ULONG celt)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CEnumExplorerCommandAggregator::Reset()
{
    for (int i = 0; i < DPA_GetPtrCount(_hdpaEnums); i++)
        ((IEnumExplorerCommand *)DPA_FastGetPtr(_hdpaEnums, i))->Reset();
    _iCurrent = 0;
    return S_OK;
}

STDMETHODIMP
CEnumExplorerCommandAggregator::Clone(
    IEnumExplorerCommand **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

/* CFolderTypeCommandProvider */

CFolderTypeCommandProvider::CFolderTypeCommandProvider(
    _In_ REFGUID rguidFolderType) :
    _cRef(1),
    _guidFolderType(rguidFolderType)
{
}

STDMETHODIMP
CFolderTypeCommandProvider::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFolderTypeCommandProvider, IExplorerCommandProvider),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CFolderTypeCommandProvider::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CFolderTypeCommandProvider::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* Walks up the folder type's parents until one has a task list for the current selection state */
STDMETHODIMP
CFolderTypeCommandProvider::GetCommands(
    IUnknown *punkSite,
    REFIID riid,
    void **ppv)
{
    IShellItemArray *psia = NULL;
    GUID guidType = _guidFolderType;
    PCWSTR pszTasks;
    HKEY hkey = NULL;
    BOOL fFound = FALSE;
    HRESULT hr;

    *ppv = NULL;
    SHGetItemArrayOfSelection(punkSite, NULL, SELMODEL_PREFERSELECTION, &psia);
    pszTasks = (GetSelectionStateFromItemArray(psia) == SELSTATE_NONE) ? L"TasksNoItemsSelected" :
                                                                          L"TasksItemsSelected";

    do
    {
        WCHAR szType[39];
        WCHAR szKey[MAX_PATH];
        IFolderTypeDescription *pftd;

        SHStringFromGUIDW(guidType, szType, _countof(szType));
        hr = StringCchPrintfW(szKey, _countof(szKey),
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes\\%ws\\%ws",
                              szType, pszTasks);
        if (FAILED(hr))
            break;

        hr = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ, &hkey));
        if (SUCCEEDED(hr))
        {
            fFound = TRUE;
            break;
        }

        hr = _GetOrCreateFolderTypeDescription(guidType, IID_PPV_ARGS(&pftd));
        if (SUCCEEDED(hr))
        {
            hr = pftd->GetParentFolderType(&guidType);
            pftd->Release();
        }
    } while (SUCCEEDED(hr));

    if (fFound)
    {
        hr = GetExplorerCommandEnumeratorForRegKey(hkey, riid, ppv);
        RegCloseKey(hkey);
    }

    if (psia)
        psia->Release();
    return hr;
}

STDMETHODIMP
CFolderTypeCommandProvider::GetCommand(
    REFGUID rguidCommandId,
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    return E_NOTIMPL;
}
