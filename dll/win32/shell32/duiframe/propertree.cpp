/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Navigation pane module: the namespace tree host and the module's inner element
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define PROPERTY_STORE_SPECIFIED 1
#define PROPERTY_STORE_COMPUTED  2

/* UIFILE holding the "ProperTree" and "ProperTreeInner" markup */
#define IDR_UIFILE_PROPERTREE   52

/* Shell32 accelerators of the tree and the nav pane's null space menu */
#define IDA_NAVPANE_TREE        5
#define IDM_NAVPANE_NULLSPACE   275

/* String ids */
#define IDS_NAVPANE_NAVIGATE    0x8006

/* Command the frame sends to bring the current folder into view in the tree */
#define FCIDM_NAVPANE_SHOWCURRENT 0xA026

/* Logical sizes of the links expando, in 96 DPI pixels */
#define EXPANDO_GROW_SLACK      46
#define EXPANDO_MIN_PANE        174
#define EXPANDO_FULL_PANE       242
#define EXPANDO_TREE_RESERVE    122
#define EXPANDO_MAX_SIZE        120

/* Width the file save dialog keeps for its nav pane, logical pixels */
#define NAVPANE_SAVEDIALOG_SIZE 76

/* ICommDlgBrowser2::GetViewFlags bit of the save dialog */
#ifndef CDB2GVF_ISFILESAVE
#define CDB2GVF_ISFILESAVE      0x2
#endif

/* Text colors of compressed and encrypted items when the user asked for them */
#define NAVPANE_COMPRESSED_COLOR RGB(0, 0, 255)
#define NAVPANE_ENCRYPTED_COLOR  RGB(19, 147, 13)

/* Version tag of the expanded state stream */
#define EXPANDEDSTATE_SIGNATURE 0x31535058

static const WCHAR c_szExplorerAdvanced[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static const WCHAR c_szNavPaneKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Modules\\NavPane\\";
static const WCHAR c_szCommonPlacesKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Modules\\CommonPlaces\\";
static const WCHAR c_szFavoritesItem[] = L"shell:::{323CA680-C24D-4099-B94D-446DD2D7249E}";
static const WCHAR c_szExpandoSize[] = L"ProperTreeExpandoSizer_Size";
static const WCHAR c_szExpandoMaxxedOut[] = L"ProperTreeExpandoSizer_MaxxedOut";
static const WCHAR c_szExpandoPrevSize[] = L"NavPane_ExpandoPrevSize";
static const WCHAR c_szAppLocationsExpanded[] = L"NavPane_IsAppLocationsExpanded";

/* Folder that lists the users of a homegroup */
static const CLSID c_clsidHomeGroupUsers =
    { 0xb4fb3f98, 0xc1ea, 0x428d, { 0xa7, 0x8a, 0xd1, 0xf5, 0x65, 0x9c, 0xba, 0x93 } };

static const ITEMIDLIST c_idlDesktopRoot = { { 0 } };

/* Settings of the module's own bag that survive between windows */
static const PCWSTR c_rgpszNavPaneSettings[] =
{
    L"NavPane_ExpandoPrevSize",
    L"NavPane_FirstRun",
    L"NavPane_CFD_FirstRun",
    L"NavPane_ShowLibraryPane",
    L"NavPane_IsAppLocationsExpanded",
};

DirectUI::IClassInfo *CProperTreeModuleInner::Class = NULL;
DirectUI::IClassInfo *CProperTreeHost::Class = NULL;

static DirectUI::PropCapability s_pcInt = { DirectUI::ValueType::Int, 0, UINT_MAX };
static DirectUI::PropCapability s_pcBool = { DirectUI::ValueType::Bool, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidTopMargin;
static DirectUI::PropertyInfoData s_pidInitialExpand;

static const DirectUI::PropertyInfo s_piTopMargin =
    { L"TopMargin", 6, 0, &s_pcInt, NULL, DirectUI::Value::GetIntZero, &s_pidTopMargin };
static const DirectUI::PropertyInfo s_piInitialExpand =
    { L"InitialExpand", 2, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse, &s_pidInitialExpand };

static const DirectUI::PropertyInfo *const c_rgpiProperTreeHost[] =
{
    &s_piTopMargin,
    &s_piInitialExpand,
};

const DirectUI::PropertyInfo *const CProperTreeHost::TopMarginProp = &s_piTopMargin;
const DirectUI::PropertyInfo *const CProperTreeHost::InitialExpandProp = &s_piInitialExpand;

/* Nav pane options */

static
BOOL
NavPane_GetOption(
    _In_ PCWSTR pszValue)
{
    return SHRegGetBoolUSValueW(c_szExplorerAdvanced, pszValue, FALSE, FALSE);
}

static
BOOL
ShouldNavPaneShowAllFolders(VOID)
{
    return NavPane_GetOption(L"NavPaneShowAllFolders");
}

static
BOOL
ShouldNavPaneExpandToCurrentFolder(VOID)
{
    return NavPane_GetOption(L"NavPaneExpandToCurrentFolder");
}

/* Every open browser with a nav pane hears about a changed option */
static
VOID
NavPane_BroadcastSetting(
    _In_ int iSetting)
{
    IShellWindows *psw;
    IUnknown *punkEnum;
    IEnumVARIANT *pev;
    VARIANT var;

    if (FAILED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&psw))))
    {
        return;
    }

    if (SUCCEEDED(psw->_NewEnum(&punkEnum)))
    {
        if (SUCCEEDED(punkEnum->QueryInterface(IID_PPV_ARGS(&pev))))
        {
            VariantInit(&var);
            while (pev->Next(1, &var, NULL) == S_OK)
            {
                IUpdateNavPaneSettings *punps;

                if (V_VT(&var) == VT_DISPATCH &&
                    SUCCEEDED(IUnknown_QueryService(V_DISPATCH(&var), IID_INameSpaceTreeControl,
                                                    IID_PPV_ARGS(&punps))))
                {
                    punps->UpdateSetting(iSetting);
                    punps->Release();
                }
                VariantClear(&var);
            }
            pev->Release();
        }
        punkEnum->Release();
    }
    psw->Release();
}

static
HRESULT
NavPane_SetOption(
    _In_ PCWSTR pszValue,
    _In_ BOOL fValue,
    _In_ int iSetting)
{
    DWORD dwValue = fValue ? 1 : 0;
    LSTATUS lStatus;

    lStatus = SHSetValueW(HKEY_CURRENT_USER, c_szExplorerAdvanced, pszValue, REG_DWORD, &dwValue, sizeof(dwValue));
    if (lStatus != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(lStatus);

    NavPane_BroadcastSetting(iSetting);
    return S_OK;
}

/* Small bag persistence: only the settings the module knows about are kept */
static
HRESULT
NavPane_LoadSettings(
    _In_ IStream *pstm,
    _In_ IPropertyBag *ppb)
{
    DWORD dwSignature;
    HRESULT hr;
    SIZE_T i;

    hr = IStream_Read(pstm, &dwSignature, sizeof(dwSignature));
    if (FAILED(hr) || dwSignature != EXPANDEDSTATE_SIGNATURE)
        return E_FAIL;

    for (i = 0; i < _countof(c_rgpszNavPaneSettings) && SUCCEEDED(hr); i++)
    {
        LONG lValue;

        hr = IStream_Read(pstm, &lValue, sizeof(lValue));
        if (SUCCEEDED(hr) && lValue != LONG_MIN)
            SHPropertyBag_WriteInt(ppb, c_rgpszNavPaneSettings[i], lValue);
    }
    return hr;
}

static
HRESULT
NavPane_SaveSettings(
    _In_ IStream *pstm,
    _In_ IPropertyBag *ppb)
{
    DWORD dwSignature = EXPANDEDSTATE_SIGNATURE;
    HRESULT hr;
    SIZE_T i;

    hr = IStream_Write(pstm, &dwSignature, sizeof(dwSignature));
    for (i = 0; i < _countof(c_rgpszNavPaneSettings) && SUCCEEDED(hr); i++)
    {
        int iValue;
        LONG lValue = LONG_MIN;

        if (SUCCEEDED(SHPropertyBag_ReadInt(ppb, c_rgpszNavPaneSettings[i], &iValue)))
            lValue = iValue;
        hr = IStream_Write(pstm, &lValue, sizeof(lValue));
    }
    return hr;
}

/* Remembered expansion of the tree's top items, kept in the user's NavPane key */
class CNavPaneExpandedState : public CDUIFrameZeroAlloc
{
public:
    ~CNavPaneExpandedState()
    {
        if (_hdsa)
            DSA_DestroyCallback(_hdsa, s_FreeEntry, NULL);
    }

    BOOL
    Init()
    {
        _hdsa = DSA_Create(sizeof(ENTRY), 4);
        return _hdsa != NULL;
    }

    int
    GetCount()
    {
        return DSA_GetItemCount(_hdsa);
    }

    IShellItem *
    GetItem(
        _In_ int i)
    {
        return ((ENTRY *)DSA_GetItemPtr(_hdsa, i))->psi;
    }

    void
    SetExpanded(
        _In_ int i,
        _In_ BOOL fExpanded)
    {
        ((ENTRY *)DSA_GetItemPtr(_hdsa, i))->fExpanded = fExpanded;
    }

    /* Index of the item, or -1; the stored state comes back in *pfExpanded */
    int
    Find(
        _In_ IShellItem *psi,
        _Out_ BOOL *pfExpanded)
    {
        int cItems = GetCount();

        *pfExpanded = FALSE;
        for (int i = 0; i < cItems; i++)
        {
            ENTRY *pEntry = (ENTRY *)DSA_GetItemPtr(_hdsa, i);
            int iOrder;

            if (SUCCEEDED(pEntry->psi->Compare(psi, SICHINT_CANONICAL, &iOrder)) && !iOrder)
            {
                *pfExpanded = pEntry->fExpanded;
                return i;
            }
        }
        return -1;
    }

    HRESULT
    Add(
        _In_ IShellItem *psi,
        _In_ BOOL fExpanded)
    {
        ENTRY entry = { psi, fExpanded };

        if (DSA_AppendItem(_hdsa, &entry) == -1)
            return E_OUTOFMEMORY;
        psi->AddRef();
        return S_OK;
    }

    HRESULT
    Load(
        _In_ IStream *pstm)
    {
        DWORD dwSignature;
        DWORD cItems;
        HRESULT hr;

        hr = IStream_Read(pstm, &dwSignature, sizeof(dwSignature));
        if (SUCCEEDED(hr) && dwSignature != EXPANDEDSTATE_SIGNATURE)
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            hr = IStream_Read(pstm, &cItems, sizeof(cItems));

        for (DWORD i = 0; SUCCEEDED(hr) && i < cItems; i++)
        {
            PIDLIST_RELATIVE pidl = NULL;
            BOOL fExpanded;

            hr = IStream_Read(pstm, &fExpanded, sizeof(fExpanded));
            if (SUCCEEDED(hr))
                hr = ILLoadFromStream(pstm, &pidl);
            if (SUCCEEDED(hr))
            {
                IShellItem *psi;

                /* Items that went away since the last save are dropped */
                if (SUCCEEDED(SHCreateItemFromIDList((PCIDLIST_ABSOLUTE)pidl, IID_PPV_ARGS(&psi))))
                {
                    hr = Add(psi, fExpanded);
                    psi->Release();
                }
                ILFree(pidl);
            }
        }
        return hr;
    }

    HRESULT
    Save(
        _In_ IStream *pstm)
    {
        DWORD dwSignature = EXPANDEDSTATE_SIGNATURE;
        DWORD cItems = GetCount();
        HRESULT hr;

        hr = IStream_Write(pstm, &dwSignature, sizeof(dwSignature));
        if (SUCCEEDED(hr))
            hr = IStream_Write(pstm, &cItems, sizeof(cItems));

        for (DWORD i = 0; SUCCEEDED(hr) && i < cItems; i++)
        {
            ENTRY *pEntry = (ENTRY *)DSA_GetItemPtr(_hdsa, i);
            PIDLIST_ABSOLUTE pidl;

            hr = SHGetIDListFromObject(pEntry->psi, &pidl);
            if (SUCCEEDED(hr))
            {
                hr = IStream_Write(pstm, &pEntry->fExpanded, sizeof(pEntry->fExpanded));
                if (SUCCEEDED(hr))
                    hr = ILSaveToStream(pstm, pidl);
                ILFree(pidl);
            }
        }
        return hr;
    }

private:
    typedef struct _ENTRY
    {
        IShellItem *psi;
        BOOL fExpanded;
    } ENTRY;

    static
    INT
    CALLBACK
    s_FreeEntry(
        _In_ void *pv,
        _In_opt_ void *pvData)
    {
        ((ENTRY *)pv)->psi->Release();
        return TRUE;
    }

    HDSA _hdsa;
};

/* Only shortcuts show up under Favorites */
class CNavPaneFavoritesFilter : public IShellItemFilter, public CDUIFrameZeroAlloc
{
public:
    CNavPaneFavoritesFilter() : _cRef(1) {}

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CNavPaneFavoritesFilter, IShellItemFilter),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        ULONG cRef = InterlockedDecrement(&_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    STDMETHODIMP
    IncludeItem(
        IShellItem *psi) override
    {
        SFGAOF sfgao;

        return psi->GetAttributes(SFGAO_LINK, &sfgao);
    }

    STDMETHODIMP
    GetEnumFlagsForItem(
        IShellItem *psi,
        SHCONTF *pgrfFlags) override
    {
        return E_NOTIMPL;
    }

private:
    LONG _cRef;
};

/* Context menu of the empty area below the tree: the two nav pane options */
class CNavPaneNullSpaceMenu : public IContextMenu, public CDUIFrameZeroAlloc
{
public:
    CNavPaneNullSpaceMenu() : _cRef(1) {}

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CNavPaneNullSpaceMenu, IContextMenu),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        ULONG cRef = InterlockedDecrement(&_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    STDMETHODIMP
    QueryContextMenu(
        HMENU hmenu,
        UINT indexMenu,
        UINT idCmdFirst,
        UINT idCmdLast,
        UINT uFlags) override
    {
        HMENU hmenuOptions;
        UINT idMax = idCmdFirst;

        hmenuOptions = LoadMenuW(DUIFRAME_HINST, MAKEINTRESOURCEW(IDM_NAVPANE_NULLSPACE));
        if (hmenuOptions)
        {
            HMENU hmenuPopup = GetSubMenu(hmenuOptions, 0);

            if (hmenuPopup)
            {
                CheckMenuItem(hmenuPopup, NULLSPACE_SHOWALLFOLDERS,
                              MF_BYCOMMAND | (ShouldNavPaneShowAllFolders() ? MF_CHECKED : MF_UNCHECKED));
                CheckMenuItem(hmenuPopup, NULLSPACE_EXPANDTOCURRENT,
                              MF_BYCOMMAND | (ShouldNavPaneExpandToCurrentFolder() ? MF_CHECKED : MF_UNCHECKED));
                idMax = Shell_MergeMenus(hmenu, hmenuPopup, indexMenu, idCmdFirst, idCmdLast,
                                         MM_SUBMENUSHAVEIDS | MM_DONTREMOVESEPS);
            }
            DestroyMenu(hmenuOptions);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(idMax - idCmdFirst));
    }

    STDMETHODIMP
    InvokeCommand(
        LPCMINVOKECOMMANDINFO pici) override
    {
        UINT idCmd;
        HRESULT hr;

        hr = _MapVerb(pici->lpVerb, &idCmd);
        if (FAILED(hr))
            return hr;

        if (idCmd == NULLSPACE_SHOWALLFOLDERS)
        {
            hr = NavPane_SetOption(L"NavPaneShowAllFolders", !ShouldNavPaneShowAllFolders(),
                                   NAVPANE_SETTING_SHOWALLFOLDERS);
            if (SUCCEEDED(hr) && _pHost)
                hr = _pHost->UpdateSetting(NAVPANE_SETTING_SHOWALLFOLDERS);
        }
        else if (idCmd == NULLSPACE_EXPANDTOCURRENT)
        {
            hr = NavPane_SetOption(L"NavPaneExpandToCurrentFolder", !ShouldNavPaneExpandToCurrentFolder(),
                                   NAVPANE_SETTING_EXPANDTOCURRENT);
            if (SUCCEEDED(hr) && _pHost)
                hr = _pHost->UpdateSetting(NAVPANE_SETTING_EXPANDTOCURRENT);
        }
        return hr;
    }

    STDMETHODIMP
    GetCommandString(
        UINT_PTR idCmd,
        UINT uType,
        UINT *pReserved,
        LPSTR pszName,
        UINT cchMax) override
    {
        PCWSTR pszVerb;

        if (uType != GCS_VERBW && uType != GCS_VERBA)
            return E_NOTIMPL;

        if (idCmd == NULLSPACE_SHOWALLFOLDERS)
            pszVerb = c_szVerbShowAll;
        else if (idCmd == NULLSPACE_EXPANDTOCURRENT)
            pszVerb = c_szVerbSync;
        else
            return E_INVALIDARG;

        if (uType == GCS_VERBW)
            return StringCchCopyW((PWSTR)pszName, cchMax, pszVerb);
        return SHUnicodeToAnsi(pszVerb, pszName, cchMax) ? S_OK : E_FAIL;
    }

    /* Set only when the host must refresh itself because no browser will */
    CProperTreeHost *_pHost;

private:
    enum
    {
        NULLSPACE_SHOWALLFOLDERS = 0,
        NULLSPACE_EXPANDTOCURRENT = 1,
    };

    static const WCHAR c_szVerbShowAll[];
    static const WCHAR c_szVerbSync[];

    HRESULT
    _MapVerb(
        _In_ LPCSTR pszVerb,
        _Out_ UINT *pidCmd)
    {
        WCHAR szVerb[64];

        if (IS_INTRESOURCE(pszVerb))
        {
            *pidCmd = LOWORD(pszVerb);
            return S_OK;
        }

        SHAnsiToUnicode(pszVerb, szVerb, _countof(szVerb));
        if (!StrCmpIW(szVerb, c_szVerbShowAll))
            *pidCmd = NULLSPACE_SHOWALLFOLDERS;
        else if (!StrCmpIW(szVerb, c_szVerbSync))
            *pidCmd = NULLSPACE_EXPANDTOCURRENT;
        else
            return E_INVALIDARG;
        return S_OK;
    }

    LONG _cRef;
};

const WCHAR CNavPaneNullSpaceMenu::c_szVerbShowAll[] = L"showAllFolders";
const WCHAR CNavPaneNullSpaceMenu::c_szVerbSync[] = L"sync";

/* The item a shortcut points at */
static
HRESULT
GetLinkTargetItem(
    _In_ IShellItem *psi,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IShellLinkW *psl;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    *ppv = NULL;
    hr = psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARGS(&psl));
    if (FAILED(hr))
        return hr;

    hr = psl->GetIDList(&pidl);
    if (hr == S_OK)
    {
        hr = SHCreateItemFromIDList(pidl, riid, ppv);
        ILFree(pidl);
    }
    else if (SUCCEEDED(hr))
    {
        hr = E_FAIL;
    }
    psl->Release();
    return hr;
}

/* TRUE when psiChild lies somewhere below psiParent */
static
BOOL
IsItemDescendantOf(
    _In_ IShellItem *psiParent,
    _In_ IShellItem *psiChild)
{
    PIDLIST_ABSOLUTE pidlParent;
    PIDLIST_ABSOLUTE pidlChild;
    BOOL fDescendant = FALSE;

    if (SUCCEEDED(SHGetIDListFromObject(psiParent, &pidlParent)))
    {
        if (SUCCEEDED(SHGetIDListFromObject(psiChild, &pidlChild)))
        {
            fDescendant = !ILIsEqual(pidlParent, pidlChild) && ILIsParent(pidlParent, pidlChild, FALSE);
            ILFree(pidlChild);
        }
        ILFree(pidlParent);
    }
    return fDescendant;
}

/* Runs the default verb of an item the browser cannot show itself */
static
HRESULT
InvokeDefaultCommandOnItem(
    _In_opt_ HWND hwnd,
    _In_opt_ IUnknown *punkSite,
    _In_ IShellItem *psi)
{
    IContextMenu *pcm;
    HRESULT hr;

    hr = psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARGS(&pcm));
    if (FAILED(hr))
        return hr;

    HMENU hmenu = CreatePopupMenu();
    if (hmenu)
    {
        IUnknown_SetSite(pcm, punkSite);
        hr = pcm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_DEFAULTONLY);
        if (SUCCEEDED(hr))
        {
            UINT idCmd = GetMenuDefaultItem(hmenu, FALSE, 0);
            CMINVOKECOMMANDINFO ici = { sizeof(ici) };

            ici.hwnd = hwnd;
            ici.nShow = SW_NORMAL;
            ici.lpVerb = (idCmd != (UINT)-1) ? MAKEINTRESOURCEA(idCmd - 1) : NULL;
            hr = ici.lpVerb ? pcm->InvokeCommand(&ici) : E_FAIL;
        }
        IUnknown_SetSite(pcm, NULL);
        DestroyMenu(hmenu);
    }
    else
    {
        hr = E_OUTOFMEMORY;
    }
    pcm->Release();
    return hr;
}

/* Label edit boxes keep the clipboard, selection and caret keys for themselves */
static
HRESULT
EditBox_TranslateAccelerator(
    _In_ MSG *pmsg)
{
    BOOL fTranslate;

    if (pmsg->message == WM_CHAR)
    {
        fTranslate = TRUE;
    }
    else if (pmsg->message != WM_KEYDOWN && pmsg->message != WM_KEYUP)
    {
        return S_FALSE;
    }
    else if (pmsg->wParam == VK_TAB)
    {
        return S_FALSE;
    }
    else if (GetKeyState(VK_CONTROL) >= 0)
    {
        fTranslate = !(pmsg->wParam == VK_F5 || pmsg->wParam == VK_F6 || pmsg->wParam == VK_F11);
    }
    else
    {
        switch (pmsg->wParam)
        {
            case 'A':
            case 'C':
            case 'V':
            case 'X':
            case 'Z':
            case VK_F4:
                fTranslate = TRUE;
                break;

            default:
                fTranslate = (pmsg->wParam >= VK_END && pmsg->wParam <= VK_DOWN) ||
                             pmsg->wParam == VK_INSERT || pmsg->wParam == VK_DELETE;
                break;
        }
    }

    if (!fTranslate)
        return S_FALSE;

    TranslateMessage(pmsg);
    DispatchMessageW(pmsg);
    return S_OK;
}

/* CProperTreeModuleInner */

HRESULT
WINAPI
CProperTreeModuleInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Element *peCreated;
    DirectUI::Layout *pLayout = NULL;
    CProperTreeModuleInner *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CProperTreeModuleInner>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(0);
    if (SUCCEEDED(hr))
        hr = DirectUI::ShellBorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        hr = pe->SetLayout(pLayout);

    /* Both markup pieces fill in this element: the outer look first, then the tree and its sizer */
    if (SUCCEEDED(hr))
    {
        hr = DUI_CreateElementFromResource(DUIFRAME_HINST, IDR_UIFILE_PROPERTREE, L"ProperTree", pe, NULL, NULL,
                                           &peCreated, NULL);
    }
    if (SUCCEEDED(hr))
    {
        hr = DUI_CreateElementFromResource(DUIFRAME_HINST, IDR_UIFILE_PROPERTREE, L"ProperTreeInner", pe, NULL, NULL,
                                           &peCreated, NULL);
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
CProperTreeModuleInner::Register()
{
    return DirectUI::ClassInfo<CProperTreeModuleInner, CElementWithSite,
                               DirectUI::StandardCreator<CProperTreeModuleInner>>::Register(
        DUIFRAME_HINST, L"ProperTreeModuleInner", NULL, 0, false);
}

STDMETHODIMP
CProperTreeModuleInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CProperTreeModuleInner, IFrameNotificationClient),
        QITABENT(CProperTreeModuleInner, IInputObject),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

/*
 * Opens or closes the links expando. Opening restores the last size when it still fits,
 * otherwise picks one from the pane height; closing remembers the size for next time.
 */
void
CProperTreeModuleInner::_ExpandFolders(
    _In_ BOOL fExpand)
{
    int cyPrev = -1;

    if (!_pseExpando || !_ppbBrowser)
        return;

    if (_ppbSettings)
        SHPropertyBag_ReadInt(_ppbSettings, c_szExpandoPrevSize, &cyPrev);

    if (fExpand)
    {
        DirectUI::Value *pv = GetValue(DirectUI::Element::ExtentProp, PROPERTY_STORE_SPECIFIED, NULL);
        int cyPane = pv->GetSize()->cy;
        int cySize;

        pv->Release();
        SHPhysicalToLogicalDPI(&cyPane, NULL);

        if (cyPrev >= 0 && cyPane - cyPrev >= EXPANDO_GROW_SLACK)
            cySize = cyPrev;
        else if (cyPane < EXPANDO_MIN_PANE)
            cySize = 0;
        else if (cyPane < EXPANDO_FULL_PANE)
            cySize = cyPane - EXPANDO_TREE_RESERVE;
        else
            cySize = EXPANDO_MAX_SIZE;

        SHPropertyBag_WriteInt(_ppbBrowser, c_szExpandoSize, cySize);
    }
    else if (_ppbSettings)
    {
        int cySize = 0;

        SHPropertyBag_ReadInt(_ppbBrowser, c_szExpandoSize, &cySize);
        SHPropertyBag_WriteInt(_ppbSettings, c_szExpandoPrevSize, cySize);
    }

    SHPropertyBag_WriteBOOL(_ppbBrowser, c_szExpandoMaxxedOut, !fExpand);
}

/* Shows the header button that matches the expando: collapse while it is fully open */
void
CProperTreeModuleInner::_UpdateButton()
{
    BOOL fMaxxedOut = PropertyBag_ReadBOOLDefault(_ppbBrowser, c_szExpandoMaxxedOut, FALSE);

    if (!fMaxxedOut && _pseExpando && _ppbBrowser)
    {
        int cySize = 0;

        if (SUCCEEDED(SHPropertyBag_ReadInt(_ppbBrowser, c_szExpandoSize, &cySize)) &&
            cySize >= _pseExpando->_ComputeBoundedSize(INT_MAX))
        {
            fMaxxedOut = TRUE;
        }
    }

    if (_peHeaderExpand && _peHeaderCollapse)
    {
        _peHeaderExpand->SetLayoutPos(fMaxxedOut ? ROSUI_LP_TOP : ROSUI_LP_NONE);
        _peHeaderCollapse->SetLayoutPos(fMaxxedOut ? ROSUI_LP_NONE : ROSUI_LP_TOP);
    }
}

void
CProperTreeModuleInner::OnHosted(
    DirectUI::Element *peNewHost)
{
    DirectUI::Element::OnHosted(peNewHost);
    if (_fExpandPending)
    {
        _ExpandFolders(TRUE);
        _fExpandPending = FALSE;
    }
}

void
CProperTreeModuleInner::OnPropertyChanged(
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    /* The first real size decides how far a pending expando may open */
    if (ppi == DirectUI::Element::ExtentProp() && (int)(ppi->Flags & 3) == iIndex && _fExpandPending)
    {
        _ExpandFolders(TRUE);
        _fExpandPending = FALSE;
    }
    DirectUI::Element::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

void
CProperTreeModuleInner::OnListenedPropertyChanged(
    DirectUI::Element *peFrom,
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (_ppbBrowser && peFrom == this && !pvOld->IsEqual(pvNew) &&
        ppi == DirectUI::Element::ExtentProp() && (int)(ppi->Flags & 3) == iIndex)
    {
        _UpdateButton();
    }
}

STDMETHODIMP
CProperTreeModuleInner::LayoutInitialized()
{
    if (!_fLayoutInitialized && _ppbBrowser)
    {
        BOOL fInExplorerBrowser;

        _fLayoutInitialized = TRUE;
        fInExplorerBrowser = PropertyBag_ReadBOOLDefault(_ppbBrowser, L"InExplorerBrowser", FALSE);
        _fExpandPending = fInExplorerBrowser;

        if (_pHost)
        {
            DirectUI::Value *pv;

            pv = DirectUI::Value::CreateBool(!!PropertyBag_ReadBOOLDefault(_ppbBrowser, L"ExpandInitialNav", FALSE));
            if (pv)
            {
                _pHost->SetValue(CProperTreeHost::InitialExpandProp, 1, pv);
                pv->Release();
            }
        }
    }

    if (_ppbSettings)
    {
        ICommDlgBrowser2 *pcdb;
        PCWSTR pszFirstRun = L"NavPane_FirstRun";
        BOOL fFirstRun;

        if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_SCommDlgBrowser, IID_PPV_ARGS(&pcdb))))
        {
            pszFirstRun = L"NavPane_CFD_FirstRun";
            pcdb->Release();
        }

        fFirstRun = PropertyBag_ReadBOOLDefault(_ppbSettings, pszFirstRun, TRUE);
        SHPropertyBag_WriteBOOL(_ppbSettings, pszFirstRun, FALSE);

        /* The very first window shows the links and the library pane */
        if (fFirstRun)
        {
            _fExpandPending = TRUE;
            SHPropertyBag_WriteBOOL(_ppbSettings, L"NavPane_ShowLibraryPane", TRUE);
        }
    }

    AddListener(static_cast<DirectUI::IElementListener *>(this));
    return S_OK;
}

STDMETHODIMP
CProperTreeModuleInner::Notify(
    PCWSTR pszPropertyName)
{
    if (!_ppbBrowser)
        return S_OK;

    if (!StrCmpICW(pszPropertyName, c_szExpandoSize))
    {
        int cySize = 0;

        if (FAILED(SHPropertyBag_ReadInt(_ppbBrowser, c_szExpandoSize, &cySize)))
            return S_OK;

        /* Dragging the expando all the way open counts as maximizing it */
        if (_pseExpando && cySize == _pseExpando->_ComputeBoundedSize(INT_MAX))
            SHPropertyBag_WriteBOOL(_ppbBrowser, c_szExpandoMaxxedOut, TRUE);
        _UpdateButton();
    }
    else if (!StrCmpICW(pszPropertyName, c_szExpandoMaxxedOut))
    {
        _UpdateButton();
    }
    return S_OK;
}

STDMETHODIMP
CProperTreeModuleInner::OnNavigateAway()
{
    return _SaveSettings();
}

/* Writes the module bag back to its stream; skipped until the pane has a real size */
HRESULT
CProperTreeModuleInner::_SaveSettings()
{
    DirectUI::Value *pv;
    HRESULT hr = S_OK;

    if (!_ppbSettings)
        return S_OK;

    pv = GetValue(DirectUI::Element::ExtentProp, PROPERTY_STORE_SPECIFIED, NULL);
    if (pv->GetSize()->cx && pv->GetSize()->cy)
    {
        IStream *pstm;
        int cySize = 0;

        if (_ppbBrowser && SUCCEEDED(SHPropertyBag_ReadInt(_ppbBrowser, c_szExpandoSize, &cySize)))
            SHPropertyBag_WriteInt(_ppbSettings, c_szExpandoPrevSize, cySize);

        hr = GetPersistStream(FRAMEPERSIST_GLOBAL, STGM_WRITE, &pstm);
        if (SUCCEEDED(hr))
        {
            if (_pHost)
                _pHost->SetSettingsBag(_ppbSettings);
            hr = NavPane_SaveSettings(pstm, _ppbSettings);
            pstm->Release();
        }
    }
    pv->Release();
    return hr;
}

STDMETHODIMP
CProperTreeModuleInner::SetSite(
    IUnknown *punkSite)
{
    DirectUI::Element *pe;
    ICommDlgBrowser2 *pcdb;

    IUnknown_Set(&_punkSite, punkSite);
    _SaveSettings();
    IUnknown_SafeReleaseAndNullPtr(&_ppbBrowser);
    IUnknown_SafeReleaseAndNullPtr(&_ppbSettings);

    if (punkSite)
    {
        IUnknown_QueryService(punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&_ppbBrowser));
        if (SUCCEEDED(SHCreatePropertyBagOnMemory(STGM_READWRITE, IID_PPV_ARGS(&_ppbSettings))))
        {
            IStream *pstm;

            if (SUCCEEDED(GetPersistStream(FRAMEPERSIST_GLOBAL, STGM_READ, &pstm)))
            {
                NavPane_LoadSettings(pstm, _ppbSettings);
                pstm->Release();
            }
        }
    }

    _peHeaderExpand = FindDescendent(StrToID(L"ProperTreeHeaderExpand"));
    _peHeaderCollapse = FindDescendent(StrToID(L"ProperTreeHeaderCollapse"));

    pe = FindDescendent(StrToID(L"ProperTreeHost"));
    _pHost = (pe && pe->GetClassInfoW()->IsSubclassOf(CProperTreeHost::Class)) ? static_cast<CProperTreeHost *>(pe)
                                                                                : NULL;
    if (_pHost)
    {
        _pHost->SetSettingsBag(_ppbSettings);
        static_cast<IObjectWithSite *>(_pHost)->SetSite(punkSite);
    }

    pe = FindDescendent(StrToID(L"ProperTreeExpandoSizer"));
    _pseExpando = (pe && pe->GetClassInfoW()->IsSubclassOf(CDUISizerElement::Class)) ?
                  static_cast<CDUISizerElement *>(pe) : NULL;

    /* The save dialog keeps its nav pane narrow */
    if (_pseExpando && SUCCEEDED(IUnknown_QueryService(punkSite, SID_SCommDlgBrowser, IID_PPV_ARGS(&pcdb))))
    {
        DWORD dwFlags = 0;

        if (SUCCEEDED(pcdb->GetViewFlags(&dwFlags)) && (dwFlags & CDB2GVF_ISFILESAVE))
        {
            DirectUI::Value *pv;
            int cxSize = NAVPANE_SAVEDIALOG_SIZE;

            SHLogicalToPhysicalDPI(&cxSize, NULL);
            pv = DirectUI::Value::CreateInt(cxSize);
            if (pv)
            {
                _pseExpando->SetValue(CDUISizerElement::InitialSizeProp, 1, pv);
                pv->Release();
            }
        }
        pcdb->Release();
    }
    return S_OK;
}

BOOL
CProperTreeModuleInner::_FolderTreeHasFocus()
{
    return _pHost && SHIsChildOrSelf(_pHost->GetHWND(), GetFocus()) == S_OK;
}

STDMETHODIMP
CProperTreeModuleInner::HasFocusIO()
{
    return _FolderTreeHasFocus() ? S_OK : S_FALSE;
}

STDMETHODIMP
CProperTreeModuleInner::TranslateAcceleratorIO(
    MSG *pmsg)
{
    if (pmsg->message != WM_KEYDOWN)
        return S_FALSE;

    /* Enter goes straight to the tree, which opens the selected folder */
    if (pmsg->wParam == VK_RETURN && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0)
    {
        TranslateMessage(pmsg);
        DispatchMessageW(pmsg);
        return S_OK;
    }

    if (_pHost && _FolderTreeHasFocus())
        return _pHost->TranslateAcceleratorTree(pmsg);
    return S_FALSE;
}

void
CProperTreeModuleInner::OnInput(
    DirectUI::InputEvent *pie)
{
    BOOL fHandled = FALSE;

    if (_peHeaderExpand && _peHeaderCollapse &&
        (pie->peTarget == _peHeaderExpand || pie->peTarget == _peHeaderCollapse))
    {
        fHandled = _OnInputExpando(pie);
    }

    if (!fHandled)
        DirectUI::Element::OnInput(pie);
}

static
void
ResetButtonPressedState(
    _In_ DirectUI::Element *pe)
{
    if (pe->GetClassInfoW()->IsSubclassOf(DirectUI::Button::GetClassInfoPtr()))
        static_cast<DirectUI::Button *>(pe)->SetPressed(false);
}

/* Header clicks and Enter/Space toggle the expando; arrow keys stay on the header */
BOOL
CProperTreeModuleInner::_OnInputExpando(
    _Inout_ DirectUI::InputEvent *pie)
{
    BOOL fToggle;

    if (pie->nStage != GMF_ROUTED)
        return FALSE;

    if (pie->nDevice == GINPUT_KEYBOARD)
    {
        WCHAR ch = static_cast<DirectUI::KeyboardEvent *>(pie)->ch;

        if (pie->nCode != GKEY_DOWN)
            return FALSE;

        if (ch >= VK_LEFT && ch <= VK_DOWN)
        {
            pie->fHandled = true;
            return TRUE;
        }
        if (ch != VK_RETURN && ch != VK_SPACE)
            return FALSE;
        fToggle = TRUE;
    }
    else if (pie->nDevice == GINPUT_MOUSE)
    {
        if (static_cast<DirectUI::MouseEvent *>(pie)->bButton != GBUTTON_LEFT)
            return FALSE;

        if (pie->nCode == GMOUSE_DOWN)
        {
            _fExpandoPressed = TRUE;
            return FALSE;
        }
        if (pie->nCode != GMOUSE_UP)
            return FALSE;

        fToggle = _fExpandoPressed;
        _fExpandoPressed = FALSE;
    }
    else
    {
        return FALSE;
    }

    if (fToggle)
    {
        _ExpandFolders(pie->peTarget == _peHeaderExpand);

        /* The header that replaces the one with focus takes it over */
        if (_peHeaderExpand->GetKeyFocused())
        {
            _peHeaderCollapse->SetKeyFocus();
            ResetButtonPressedState(_peHeaderCollapse);
        }
        else if (_peHeaderCollapse->GetKeyFocused())
        {
            _peHeaderExpand->SetKeyFocus();
            ResetButtonPressedState(_peHeaderExpand);
        }
    }
    return FALSE;
}

/* CProperTreeHost */

HRESULT
WINAPI
CProperTreeHost::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CProperTreeHost *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CProperTreeHost>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->DirectUI::HWNDHost::Initialize(ROSUI_HHC_SYNCPAINT, 3, peParent, pdwDeferCookie);
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
CProperTreeHost::Register()
{
    return DirectUI::ClassInfo<CProperTreeHost, DirectUI::HWNDHost,
                               DirectUI::StandardCreator<CProperTreeHost>>::Register(
        DUIFRAME_HINST, L"ProperTreeHost", c_rgpiProperTreeHost, _countof(c_rgpiProperTreeHost), false);
}

STDMETHODIMP
CProperTreeHost::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CProperTreeHost, IObjectWithSite),
        QITABENT(CProperTreeHost, INameSpaceTreeControlEvents),
        QITABENT(CProperTreeHost, INameSpaceTreeAccessible),
        QITABENTMULTI(CProperTreeHost, INameSpaceTreeControlCustomDraw, INameSpaceTreeControlCustomDrawWin7),
        QITABENT(CProperTreeHost, INameSpaceTreeOrderCallback),
        QITABENT(CProperTreeHost, INameSpaceTreeCallbackPriv),
        QITABENT(CProperTreeHost, IUpdateNavPaneSettings),
        QITABENT(CProperTreeHost, IServiceProvider),
        QITABENT(CProperTreeHost, IDispatch),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

void
CProperTreeHost::SetSettingsBag(
    _In_opt_ IPropertyBag *ppb)
{
    /* The old bag learns the app locations state before it is let go */
    if (_ppbSettings)
        _IsAppLocationsExpanded();
    IUnknown_Set((IUnknown **)&_ppbSettings, ppb);
}

HWND
CProperTreeHost::CreateHWND(
    HWND hwndParent)
{
    NSTCSTYLE2 nstcs2;

    _hwndTree = NULL;
    _hwndParent = hwndParent;

    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_pnstc))))
        return NULL;

    _RefreshCompColorSetting();
    if (FAILED(_pnstc->Initialize(hwndParent, NULL,
                                  NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT | NSTCS_ROOTHASEXPANDO |
                                  NSTCS_SHOWSELECTIONALWAYS | NSTCS_NOINFOTIP | NSTCS_FADEINOUTEXPANDOS |
                                  NSTCS_ALLOWJUNCTIONS)))
    {
        return NULL;
    }

    /* Remote sessions skip the notification throttling */
    nstcs2 = NSTCS2_SHOWNULLSPACEMENU | NSTCS2_DISPLAYPADDING | NTSCS2_NOSINGLETONAUTOEXPAND |
             NTSCS2_NEVERINSERTNONENUMERATED;
    if (!GetSystemMetrics(SM_REMOTESESSION))
        nstcs2 |= NSTCS2_INTERRUPTNOTIFICATIONS;

    if (SUCCEEDED(_pnstc->SetControlStyle2(nstcs2, nstcs2)) &&
        SUCCEEDED(_pnstc->TreeAdvise(static_cast<INameSpaceTreeControlEvents *>(this), &_dwAdviseCookie)))
    {
        _SetColors();
        IUnknown_GetWindow(_pnstc, &_hwndTree);
    }
    return _hwndTree;
}

void
CProperTreeHost::OnHosted(
    DirectUI::Element *peNewHost)
{
    DirectUI::HWNDHost::OnHosted(peNewHost);
    _EnsureRootsInitialized();
}

void
CProperTreeHost::OnPropertyChanged(
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Element::ExtentProp() && (int)(ppi->Flags & 3) == iIndex)
        _EnsureRootsInitialized();
    DirectUI::HWNDHost::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

void
CProperTreeHost::OnDestroy()
{
    static_cast<IObjectWithSite *>(this)->SetSite(NULL);
    IUnknown_SetSite(_pnstc, NULL);
    if (_pnstc)
    {
        _pnstc->TreeUnadvise(_dwAdviseCookie);
        IUnknown_SafeReleaseAndNullPtr(&_pnstc);
    }
    IUnknown_SafeReleaseAndNullPtr(&_psiFavorites);
    IUnknown_SafeReleaseAndNullPtr(&_psiAppLocations);
    IUnknown_SafeReleaseAndNullPtr(&_psiNavRoot);
    IUnknown_SafeReleaseAndNullPtr(&_ppbSettings);
    delete _pExpanded;
    _pExpanded = NULL;
    if (_haccel)
    {
        DestroyAcceleratorTable(_haccel);
        _haccel = NULL;
    }
    _fDestroyed = TRUE;
    DirectUI::HWNDHost::OnDestroy();
}

bool
CProperTreeHost::OnMessage(
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT *plRet)
{
    if (_fDestroyed)
        return DirectUI::HWNDHost::OnMessage(uMsg, wParam, lParam, plRet);

    switch (uMsg)
    {
        case WM_DESTROY:
            IUnknown_SetSite(_pnstc, NULL);
            break;

        case WM_NOTIFY:
            if (((NMHDR *)lParam)->code == NM_SETFOCUS)
                SetKeyFocus();
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == FCIDM_NAVPANE_SHOWCURRENT)
            {
                IShellItem *psi;

                if (SUCCEEDED(_GetViewItem(IID_PPV_ARGS(&psi))))
                {
                    _ExpandItemInTree(psi, NSTCIS_SELECTED | NSTCIS_EXPANDED);
                    psi->Release();
                }
                return true;
            }
            break;

        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
        case WM_PALETTECHANGED:
            if (_pnstc)
            {
                IWinEventHandler *pweh;
                bool fHandled = false;

                /* The tree repaints for the new colors and settings itself */
                _RefreshCompColorSetting();
                if (SUCCEEDED(_pnstc->QueryInterface(IID_PPV_ARGS(&pweh))))
                {
                    pweh->OnWinEvent(_hwndTree, uMsg, wParam, lParam, plRet);
                    fHandled = true;
                    pweh->Release();
                }
                if (uMsg == WM_SETTINGCHANGE)
                    _SetColors();
                if (fHandled)
                    return true;
            }
            break;
    }
    return DirectUI::HWNDHost::OnMessage(uMsg, wParam, lParam, plRet);
}

HRESULT
CProperTreeHost::TranslateAcceleratorTree(
    _In_ MSG *pmsg)
{
    WCHAR szClass[32];
    HWND hwndFocus;

    if (!_haccel)
        _haccel = LoadAcceleratorsW(DUIFRAME_HINST, MAKEINTRESOURCEW(IDA_NAVPANE_TREE));
    if (!_haccel)
        return S_FALSE;

    /* A label being edited gets the editing keys before the tree's shortcuts */
    hwndFocus = GetFocus();
    if (GetClassNameW(hwndFocus, szClass, _countof(szClass)) && !StrCmpCW(szClass, WC_EDITW))
        return EditBox_TranslateAccelerator(pmsg);

    return ::TranslateAcceleratorW(_hwndTree, _haccel, pmsg) ? S_OK : S_FALSE;
}

void
CProperTreeHost::_RefreshCompColorSetting()
{
    SHELLSTATE ss = { 0 };

    SHGetSetSettings(&ss, SSF_SHOWCOMPCOLOR, FALSE);
    _fShowCompColor = ss.fShowCompColor;
}

/* The tree takes the element's colors: system color indices or solid fills */
void
CProperTreeHost::_GetSetColorProp(
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ IVisualProperties *pvp,
    _In_ VPCOLORFLAGS vpcf)
{
    DirectUI::Value *pv = GetValue(ppi, PROPERTY_STORE_COMPUTED, NULL);

    if (pv->GetType() == (int)DirectUI::ValueType::Int)
        pvp->SetColor(vpcf, ColorFromEnumI(pv->GetInt()) | 0x02000000);
    else if (pv->GetType() == (int)DirectUI::ValueType::Color)
        pvp->SetColor(vpcf, pv->GetFill()->ref.cr & 0x00FFFFFF);
    pv->Release();
}

void
CProperTreeHost::_SetColors()
{
    IVisualProperties *pvp;

    if (SUCCEEDED(_pnstc->QueryInterface(IID_PPV_ARGS(&pvp))))
    {
        _GetSetColorProp(DirectUI::Element::ForegroundProp(), pvp, VPCF_TEXT);
        _GetSetColorProp(DirectUI::Element::BackgroundProp(), pvp, VPCF_BACKGROUND);
        pvp->Release();
    }
}

STDMETHODIMP
CProperTreeHost::SetSite(
    IUnknown *punkSite)
{
    IUnknown_Set(&_punkSite, punkSite);

    if (punkSite)
    {
        /* Navigations of the browser keep the tree's selection in step */
        if (_pnstc &&
            (SUCCEEDED(IUnknown_QueryService(punkSite, IID_IWebBrowserApp, IID_PPV_ARGS(&_pcpcBrowser))) ||
             SUCCEEDED(IUnknown_QueryService(punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&_pcpcBrowser)))))
        {
            ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DWebBrowserEvents2, TRUE,
                                     _pcpcBrowser, &_dwEventsCookie, NULL);
        }
    }
    else
    {
        ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DWebBrowserEvents2, FALSE,
                                 _pcpcBrowser, &_dwEventsCookie, NULL);
        IUnknown_SafeReleaseAndNullPtr(&_pcpcBrowser);
    }

    if (_punkSite)
    {
        /* Option changes find the tree through the browser */
        if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&_pps))))
        {
            _pps->ProfferService(IID_INameSpaceTreeControl, static_cast<IServiceProvider *>(this),
                                 &_dwProfferCookie);
        }
    }
    else
    {
        if (_pps)
        {
            _pps->RevokeService(_dwProfferCookie);
            IUnknown_SafeReleaseAndNullPtr(&_pps);
        }
        if (_pNullSpaceMenu)
        {
            _pNullSpaceMenu->_pHost = NULL;
            _pNullSpaceMenu->Release();
            _pNullSpaceMenu = NULL;
        }
        _SaveExpandedItemArray();
    }

    IUnknown_SetSite(_pnstc, punkSite);
    return S_OK;
}

STDMETHODIMP
CProperTreeHost::GetSite(
    REFIID riid,
    void **ppvSite)
{
    *ppvSite = NULL;
    return _punkSite ? _punkSite->QueryInterface(riid, ppvSite) : E_FAIL;
}

STDMETHODIMP
CProperTreeHost::QueryService(
    REFGUID guidService,
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (!IsEqualGUID(guidService, IID_INameSpaceTreeControl))
        return E_NOTIMPL;

    if (IsEqualIID(riid, IID_IUpdateNavPaneSettings))
        return QueryInterface(riid, ppv);
    return _pnstc ? _pnstc->QueryInterface(riid, ppv) : E_NOTIMPL;
}

STDMETHODIMP
CProperTreeHost::UpdateSetting(
    int iSetting)
{
    if (!_pnstc)
        return E_FAIL;

    if (iSetting == NAVPANE_SETTING_SHOWALLFOLDERS)
    {
        HRESULT hr;

        /* The roots change shape, so the tree starts over */
        _SaveExpandedItemArray();
        delete _pExpanded;
        _pExpanded = NULL;

        hr = _pnstc->RemoveAllRoots();
        if (SUCCEEDED(hr))
        {
            _fRootsInitialized = FALSE;
            _EnsureRootsInitialized();
        }
        return hr;
    }

    if (iSetting == NAVPANE_SETTING_EXPANDTOCURRENT)
    {
        _OnNavigate();
        return S_OK;
    }
    return E_NOTIMPL;
}

STDMETHODIMP
CProperTreeHost::Invoke(
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pDispParams,
    VARIANT *pVarResult,
    EXCEPINFO *pExcepInfo,
    UINT *puArgErr)
{
    if (pVarResult)
        VariantInit(pVarResult);

    if (dispIdMember != DISPID_NAVIGATECOMPLETE2)
        return E_INVALIDARG;

    _OnNavigate();
    return S_OK;
}

HRESULT
CProperTreeHost::_GetViewItem(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IShellItemBrowser *psib;
    HRESULT hr;

    *ppv = NULL;
    hr = IUnknown_QueryService(_punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&psib));
    if (SUCCEEDED(hr))
    {
        hr = psib->GetCurrentItem(riid, ppv);
        psib->Release();
    }
    return hr;
}

HRESULT
CProperTreeHost::_GetSelectedItem(
    _COM_Outptr_ IShellItem **ppsi)
{
    IShellItemArray *psia;
    HRESULT hr;

    *ppsi = NULL;
    hr = _pnstc->GetSelectedItems(&psia);
    if (SUCCEEDED(hr))
    {
        hr = psia->GetItemAt(0, ppsi);
        psia->Release();
    }
    return hr;
}

/* Same item, or with fCheckTarget a shortcut in the tree that points at the view's item */
BOOL
CProperTreeHost::_AreItemsEquivalent(
    _In_ IShellItem *psi1,
    _In_ IShellItem *psi2,
    _In_ BOOL fCheckTarget)
{
    SFGAOF sfgao;
    int iOrder;
    BOOL fSame;

    fSame = SUCCEEDED(psi1->Compare(psi2, SICHINT_CANONICAL, &iOrder)) && !iOrder;
    if (!fSame && fCheckTarget && psi2->GetAttributes(SFGAO_LINK, &sfgao) == S_OK)
    {
        IShellItem *psiTarget;

        if (SUCCEEDED(GetLinkTargetItem(psi2, IID_PPV_ARGS(&psiTarget))))
        {
            fSame = SUCCEEDED(psi1->Compare(psiTarget, SICHINT_CANONICAL, &iOrder)) && !iOrder;
            psiTarget->Release();
        }
    }
    return fSame;
}

/* Moves the tree's selection to the folder the browser now shows */
void
CProperTreeHost::_OnNavigate()
{
    IShellItem *psiView;
    IShellItem *psiSelected;
    BOOL fExpandTo;

    if (!_pnstc || GetLayoutPos() == ROSUI_LP_NONE)
        return;

    if (FAILED(_GetViewItem(IID_PPV_ARGS(&psiView))))
        return;

    if (SUCCEEDED(_GetSelectedItem(&psiSelected)))
    {
        fExpandTo = ShouldNavPaneExpandToCurrentFolder();
        if (_AreItemsEquivalent(psiView, psiSelected, !fExpandTo))
        {
            _pnstc->EnsureItemVisible(psiSelected);
        }
        else
        {
            NSTCITEMSTATE nstcis = fExpandTo ? NSTCIS_SELECTED : NSTCIS_SELECTEDNOEXPAND;

            if (SUCCEEDED(_pnstc->SetItemState(psiView, nstcis, nstcis)))
                _pnstc->EnsureItemVisible(psiView);
        }
        psiSelected->Release();
    }
    psiView->Release();
}

HRESULT
CProperTreeHost::_NavigateTo(
    _In_ IShellItem *psi)
{
    IShellItemBrowser *psib;
    IShellItem *psiTarget = psi;
    SFGAOF sfgao = 0;
    HRESULT hr;

    psi->AddRef();

    /* Files open with their default verb; folders and shortcuts to them browse */
    if (FAILED(psi->GetAttributes(SFGAO_FOLDER | SFGAO_LINK, &sfgao)) || !(sfgao & (SFGAO_FOLDER | SFGAO_LINK)))
    {
        hr = InvokeDefaultCommandOnItem(_hwndTree, _punkSite, psi);
        psi->Release();
        return hr;
    }

    if (sfgao & SFGAO_LINK)
    {
        IShellItem *psiResolved;

        if (SUCCEEDED(GetLinkTargetItem(psi, IID_PPV_ARGS(&psiResolved))))
        {
            psiTarget->Release();
            psiTarget = psiResolved;
        }
    }

    hr = IUnknown_QueryService(_punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&psib));
    if (SUCCEEDED(hr))
    {
        hr = psib->BrowseItem(psiTarget, SBSP_SAMEBROWSER | SBSP_ACTIVATE_NOFOCUS);
        psib->Release();
    }
    psiTarget->Release();
    return hr;
}

STDMETHODIMP
CProperTreeHost::OnItemClick(
    IShellItem *psi,
    NSTCEHITTEST nstceHitTest,
    NSTCECLICKTYPE nstceClickType)
{
    if ((nstceClickType & NSTCECT_BUTTON) == NSTCECT_LBUTTON &&
        (nstceHitTest & (NSTCEHT_ONITEMICON | NSTCEHT_ONITEMLABEL | NSTCEHT_ONITEMRIGHT | NSTCEHT_ONITEMSTATEICON)))
    {
        _NavigateTo(psi);
        return S_OK;
    }
    return S_FALSE;
}

STDMETHODIMP
CProperTreeHost::OnKeyboardInput(
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    IShellItem *psi;

    if (uMsg != WM_KEYDOWN && uMsg != WM_SYSKEYDOWN)
        return S_OK;

    if (wParam == VK_RETURN)
    {
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
            return S_OK;
    }
    else if (wParam != VK_SPACE)
    {
        return S_OK;
    }

    if (_pnstc && SUCCEEDED(_GetSelectedItem(&psi)))
    {
        _NavigateTo(psi);
        psi->Release();
    }
    return S_OK;
}

/* Without "expand to current folder" an expanded parent still marks the view's folder */
STDMETHODIMP
CProperTreeHost::OnAfterExpand(
    IShellItem *psi)
{
    IShellItem *psiView;

    if (ShouldNavPaneExpandToCurrentFolder() || FAILED(_GetViewItem(IID_PPV_ARGS(&psiView))))
        return S_OK;

    if (IsItemDescendantOf(psi, psiView))
        _pnstc->SetItemState(psiView, NSTCIS_SELECTEDNOEXPAND, NSTCIS_SELECTEDNOEXPAND);
    psiView->Release();
    return S_OK;
}

static
BOOL
IsItemInFolderOfClass(
    _In_ IShellItem *psi,
    _In_ REFCLSID rclsid)
{
    IParentAndItem *ppai;
    IShellFolder *psf;
    PITEMID_CHILD pidl;
    BOOL fMatch = FALSE;

    if (FAILED(psi->QueryInterface(IID_PPV_ARGS(&ppai))))
        return FALSE;

    if (SUCCEEDED(ppai->GetParentAndItem(NULL, &psf, &pidl)))
    {
        CLSID clsid;

        fMatch = SUCCEEDED(IUnknown_GetClassID(psf, &clsid)) && IsEqualCLSID(clsid, rclsid);
        psf->Release();
        CoTaskMemFree(pidl);
    }
    ppai->Release();
    return fMatch;
}

STDMETHODIMP
CProperTreeHost::OnItemAdded(
    IShellItem *psi,
    BOOL fIsRoot)
{
    HRESULT hr = S_OK;

    /* Top level items come back the way the user left them */
    if (IsItemInFolderOfClass(psi, CLSID_ShellDesktop) && _IsItemExpanded(psi))
        hr = _pnstc->SetItemState(psi, NSTCIS_EXPANDED, NSTCIS_EXPANDED);

    if (!fIsRoot)
        _OptionallyExpandParent(psi);
    return hr;
}

/* A homegroup with a single member opens up once, so the member is visible right away */
void
CProperTreeHost::_OptionallyExpandParent(
    _In_ IShellItem *psi)
{
    IShellItem *psiParent;

    if (_fExpandedOtherUsers || !IsItemInFolderOfClass(psi, c_clsidHomeGroupUsers))
        return;

    if (FAILED(_pnstc->GetNextItem(psi, NSTCGNI_PARENT, &psiParent)))
        return;

    if (_IsItemExpanded(psiParent))
    {
        IShellItem *psiChild;

        _fExpandedOtherUsers = TRUE;
        if (SUCCEEDED(_pnstc->GetNextItem(psiParent, NSTCGNI_CHILD, &psiChild)))
        {
            IShellItem *psiSibling;

            if (FAILED(_pnstc->GetNextItem(psiChild, NSTCGNI_NEXT, &psiSibling)))
                _pnstc->SetItemState(psiParent, NSTCIS_EXPANDED, NSTCIS_EXPANDED);
            else
                psiSibling->Release();
            psiChild->Release();
        }
    }
    psiParent->Release();
}

HRESULT
CProperTreeHost::_ExpandItemInTree(
    _In_ IShellItem *psi,
    _In_ NSTCITEMSTATE nstcis)
{
    HRESULT hr = _pnstc->SetItemState(psi, nstcis, nstcis);

    if (SUCCEEDED(hr) && (nstcis & NSTCIS_SELECTED))
        _pnstc->EnsureItemVisible(psi);
    return hr;
}

/* Unknown items are remembered as collapsed */
BOOL
CProperTreeHost::_IsItemExpanded(
    _In_ IShellItem *psi)
{
    BOOL fExpanded;

    if (!_pExpanded)
        return FALSE;

    if (_pExpanded->Find(psi, &fExpanded) < 0)
        _pExpanded->Add(psi, FALSE);
    return fExpanded;
}

HRESULT
CProperTreeHost::_InitExpandedItemArray()
{
    IStream *pstm;
    HRESULT hr = E_FAIL;

    _pExpanded = new CNavPaneExpandedState();
    if (!_pExpanded || !_pExpanded->Init())
    {
        delete _pExpanded;
        _pExpanded = NULL;
        return E_OUTOFMEMORY;
    }

    pstm = SHOpenRegStream2W(HKEY_CURRENT_USER, c_szNavPaneKey, L"ExpandedState", STGM_READ);
    if (pstm)
    {
        hr = _pExpanded->Load(pstm);
        pstm->Release();
    }

    /* First run: Favorites, Libraries and Homegroup start open */
    if (FAILED(hr))
    {
        static const KNOWNFOLDERID *const c_rgpkfidOpen[] = { &FOLDERID_UsersLibraries, &FOLDERID_HomeGroup };
        IShellItem *psi;

        if (SUCCEEDED(_EnsureFavorites()))
            _pExpanded->Add(_psiFavorites, TRUE);

        for (SIZE_T i = 0; i < _countof(c_rgpkfidOpen); i++)
        {
            if (SUCCEEDED(SHGetKnownFolderItem(*c_rgpkfidOpen[i], KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psi))))
            {
                _pExpanded->Add(psi, TRUE);
                psi->Release();
            }
        }
    }
    return S_OK;
}

/* Records what is open now for items the tree has filled, then writes the list out */
HRESULT
CProperTreeHost::_SaveExpandedItemArray()
{
    IStream *pstm;
    HRESULT hr;
    int i;

    if (!_fRootsInitialized || !_pExpanded || !_pnstc)
        return E_FAIL;

    for (i = _pExpanded->GetCount() - 1; i >= 0; i -= 1)
    {
        IShellItem *psi = _pExpanded->GetItem(i);
        IShellItem *psiChild;

        if (SUCCEEDED(_pnstc->GetNextItem(psi, NSTCGNI_CHILD, &psiChild)))
        {
            NSTCITEMSTATE nstcis;

            if (SUCCEEDED(_pnstc->GetItemState(psi, NSTCIS_EXPANDED, &nstcis)))
                _pExpanded->SetExpanded(i, nstcis == NSTCIS_EXPANDED);
            psiChild->Release();
        }
    }

    pstm = SHOpenRegStream2W(HKEY_CURRENT_USER, c_szNavPaneKey, L"ExpandedState", STGM_WRITE | STGM_CREATE);
    if (!pstm)
        return E_FAIL;

    hr = _pExpanded->Save(pstm);
    pstm->Release();
    return hr;
}

HRESULT
CProperTreeHost::_EnsureFavorites()
{
    if (!_ShowPlaces())
        return E_FAIL;
    if (_psiFavorites)
        return S_OK;
    return SHCreateItemFromParsingName(c_szFavoritesItem, NULL, IID_PPV_ARGS(&_psiFavorites));
}

/* The file dialogs choose their own places; ReactOS's dialogs do not host this pane */
BOOL
CProperTreeHost::_ShowPlaces()
{
    if (!_iShowPlaces)
        _iShowPlaces = 1;
    return _iShowPlaces == 1;
}

BOOL
CProperTreeHost::_IsInCFD()
{
    ICommDlgBrowser2 *pcdb;

    if (!_punkSite || FAILED(IUnknown_QueryService(_punkSite, SID_SCommDlgBrowser, IID_PPV_ARGS(&pcdb))))
        return FALSE;

    pcdb->Release();
    return TRUE;
}

SHCONTF
CProperTreeHost::_GetEnumFlags()
{
    SHELLSTATE ss = { 0 };

    SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS, FALSE);
    return ss.fShowAllObjects ? (SHCONTF_FOLDERS | SHCONTF_INCLUDEHIDDEN) : SHCONTF_FOLDERS;
}

/* App locations and a forced navigation root exist only inside the file dialogs */
HRESULT
CProperTreeHost::_MaybeGetAppLocations()
{
    return _psiAppLocations ? S_OK : E_FAIL;
}

HRESULT
CProperTreeHost::_MaybeGetBrowserNavigationRoot()
{
    _fNavRootChecked = TRUE;
    return _psiNavRoot ? S_OK : E_FAIL;
}

BOOL
CProperTreeHost::_IsAppLocationsExpanded()
{
    BOOL fExpanded = TRUE;

    if (!_pnstc || FAILED(_MaybeGetAppLocations()))
        return fExpanded;

    NSTCITEMSTATE nstcis;

    if (_fRootsInitialized && SUCCEEDED(_pnstc->GetItemState(_psiAppLocations, NSTCIS_EXPANDED, &nstcis)))
    {
        fExpanded = (nstcis == NSTCIS_EXPANDED);
        if (_ppbSettings)
            SHPropertyBag_WriteBOOL(_ppbSettings, c_szAppLocationsExpanded, fExpanded);
    }
    else if (_ppbSettings)
    {
        fExpanded = PropertyBag_ReadBOOLDefault(_ppbSettings, c_szAppLocationsExpanded, TRUE);
    }
    return fExpanded;
}

/* Fills the tree once the host has a size and a site: favorites, desktop, then the view's folder */
void
CProperTreeHost::_EnsureRootsInitialized()
{
    DirectUI::Value *pv;
    BOOL fHasSize;

    pv = GetValue(DirectUI::Element::ExtentProp, PROPERTY_STORE_SPECIFIED, NULL);
    fHasSize = pv->GetSize()->cx && pv->GetSize()->cy;
    pv->Release();

    if (!fHasSize || _fRootsInitialized || !_punkSite || !_pnstc)
        return;

    _pnstc->SetControlStyle2(NSTCS2_DISPLAYPINNEDONLY,
                             ShouldNavPaneShowAllFolders() ? NSTCS2_DEFAULT : NSTCS2_DISPLAYPINNEDONLY);

    pv = GetValue(TopMarginProp, PROPERTY_STORE_COMPUTED, NULL);
    int iTopMargin = pv->GetInt();
    pv->Release();
    if (iTopMargin)
    {
        IObjectWithTopMargin *potm;

        if (SUCCEEDED(_pnstc->QueryInterface(IID_PPV_ARGS(&potm))))
        {
            potm->SetTopMargin(iTopMargin);
            potm->Release();
        }
    }

    if (FAILED(_InitExpandedItemArray()))
        return;

    if (SUCCEEDED(_MaybeGetAppLocations()))
    {
        _pnstc->AppendRoot(_psiAppLocations, SHCONTF_FOLDERS,
                           _IsAppLocationsExpanded() ? NSTCRS_EXPANDED : NSTCRS_VISIBLE, NULL);
    }

    if (SUCCEEDED(_EnsureFavorites()))
    {
        CNavPaneFavoritesFilter *pFilter = new CNavPaneFavoritesFilter();

        if (pFilter)
        {
            _pnstc->AppendRoot(_psiFavorites, SHCONTF_NONFOLDERS,
                               _IsItemExpanded(_psiFavorites) ? NSTCRS_EXPANDED : NSTCRS_VISIBLE, pFilter);
            pFilter->Release();
        }
    }

    if (SUCCEEDED(_MaybeGetBrowserNavigationRoot()))
    {
        _pnstc->AppendRoot(_psiNavRoot, _GetEnumFlags(),
                           _IsItemExpanded(_psiNavRoot) ? NSTCRS_EXPANDED : NSTCRS_VISIBLE, NULL);
    }

    if (_ShowPlaces())
    {
        IShellItem *psi;

        /* With "show all folders" the desktop itself is the root, otherwise its children are */
        if (SUCCEEDED(SHCreateItemFromIDList(&c_idlDesktopRoot, IID_PPV_ARGS(&psi))))
        {
            _pnstc->AppendRoot(psi, _GetEnumFlags(),
                               ShouldNavPaneShowAllFolders() ? NSTCRS_EXPANDED : (NSTCRS_HIDDEN | NSTCRS_EXPANDED),
                               NULL);
            psi->Release();
        }

        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_UsersLibraries, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psi))))
        {
            if (_IsItemExpanded(psi))
                _ExpandItemInTree(psi, NSTCIS_EXPANDED);
            psi->Release();
        }

        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_HomeGroup, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psi))))
        {
            SFGAOF sfgao;

            if (psi->GetAttributes(SFGAO_NONENUMERATED, &sfgao) != S_OK && _IsItemExpanded(psi))
                _ExpandItemInTree(psi, NSTCIS_EXPANDED);
            psi->Release();
        }

        if (SUCCEEDED(_GetViewItem(IID_PPV_ARGS(&psi))))
        {
            NSTCITEMSTATE nstcis = NSTCIS_SELECTEDNOEXPAND;
            SFGAOF sfgao;
            BOOL fInitialExpand;

            pv = GetValue(InitialExpandProp, PROPERTY_STORE_COMPUTED, NULL);
            fInitialExpand = pv->GetBool();
            pv->Release();

            /* Removable media open up so their content shows next to the view */
            if (ShouldNavPaneExpandToCurrentFolder() || fInitialExpand ||
                (!_IsInCFD() && psi->GetAttributes(SFGAO_REMOVABLE, &sfgao) == S_OK))
            {
                nstcis = NSTCIS_SELECTED | NSTCIS_EXPANDED;
            }
            _ExpandItemInTree(psi, nstcis);
            psi->Release();
        }
    }

    _fRootsInitialized = TRUE;
}

STDMETHODIMP
CProperTreeHost::PrePaint(
    HDC hdc,
    RECT *prc,
    LRESULT *plres)
{
    *plres = _fShowCompColor ? CDRF_NOTIFYITEMDRAW : CDRF_DODEFAULT;
    return S_OK;
}

static
COLORREF
GetExplorerColorSetting(
    _In_ PCWSTR pszValue,
    _In_ COLORREF crDefault)
{
    COLORREF cr;
    DWORD cb = sizeof(cr);

    if (SHGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", pszValue, NULL,
                    &cr, &cb) != ERROR_SUCCESS || cb != sizeof(cr))
    {
        cr = crDefault;
    }
    return cr;
}

/* Compressed and encrypted items get their alternate colors unless selected or hot */
STDMETHODIMP
CProperTreeHost::ItemPrePaint(
    HDC hdc,
    RECT *prc,
    NSTCCUSTOMDRAW *pnstccdItem,
    COLORREF *pclrText,
    COLORREF *pclrTextBk,
    LRESULT *plres)
{
    SFGAOF sfgao;
    HRESULT hr;

    *plres = CDRF_DODEFAULT;
    if (pnstccdItem->uItemState & (CDIS_SELECTED | CDIS_HOT))
        return S_OK;

    hr = pnstccdItem->psi->GetAttributes(SFGAO_COMPRESSED | SFGAO_ENCRYPTED, &sfgao);
    if (FAILED(hr))
        return hr;

    if (sfgao & SFGAO_COMPRESSED)
        *pclrText = GetExplorerColorSetting(L"AltColor", NAVPANE_COMPRESSED_COLOR);
    else if (sfgao & SFGAO_ENCRYPTED)
        *pclrText = GetExplorerColorSetting(L"AltEncryptionColor", NAVPANE_ENCRYPTED_COLOR);
    return hr;
}

STDMETHODIMP
CProperTreeHost::ShouldReorderChildItems(
    IShellItem *psi)
{
    int iOrder;
    HRESULT hr;

    if (FAILED(_EnsureFavorites()))
        return S_FALSE;

    hr = _psiFavorites->Compare(psi, SICHINT_CANONICAL, &iOrder);
    return (SUCCEEDED(hr) && !iOrder) ? S_OK : S_FALSE;
}

/* Favorites keep the order the user dragged them into */
STDMETHODIMP
CProperTreeHost::GetOrderStream(
    IShellItem *psi,
    DWORD grfMode,
    IStream **ppstm)
{
    int iOrder;

    *ppstm = NULL;
    if (FAILED(_EnsureFavorites()) || FAILED(_psiFavorites->Compare(psi, SICHINT_CANONICAL, &iOrder)) || iOrder)
        return E_NOTIMPL;

    *ppstm = SHOpenRegStream2W(HKEY_CURRENT_USER, c_szCommonPlacesKey, L"Order", grfMode);
    return *ppstm ? S_OK : E_FAIL;
}

STDMETHODIMP
CProperTreeHost::OnBeforeContextMenu(
    IShellItem *psi,
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (psi || !_ShowPlaces())
        return E_FAIL;

    if (!_pNullSpaceMenu)
    {
        _pNullSpaceMenu = new CNavPaneNullSpaceMenu();
        if (!_pNullSpaceMenu)
            return E_OUTOFMEMORY;

        /* A file dialog has no browser to pass the change on, so the tree refreshes itself */
        if (_IsInCFD())
            _pNullSpaceMenu->_pHost = this;
    }
    return _pNullSpaceMenu->QueryInterface(riid, ppv);
}

STDMETHODIMP
CProperTreeHost::OnGetDefaultAccessibilityAction(
    IShellItem *psi,
    BSTR *pbstrDefaultAction)
{
    WCHAR szAction[MAX_PATH];

    LoadStringW(DUIFRAME_HINST, IDS_NAVPANE_NAVIGATE, szAction, _countof(szAction));
    *pbstrDefaultAction = SysAllocString(szAction);
    return *pbstrDefaultAction ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CProperTreeHost::OnDoDefaultAccessibilityAction(
    IShellItem *psi)
{
    return _NavigateTo(psi);
}
