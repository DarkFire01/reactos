/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DirectUI process/thread setup, style sheets and element tree helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

static CRITICAL_SECTION g_csDirectUI;
static CRITICAL_SECTION g_csStyle;
static UINT g_cDirectUIUsers;
static BOOL g_fProcessReady;
static DWORD g_dwSlotStyleSheet = TLS_OUT_OF_INDEXES;
static DWORD g_dwSlotThreadUsers = TLS_OUT_OF_INDEXES;

/* The ShellStyle.dll the cached sheets came from, and its preprocessed sheet markup */
static PWSTR g_pszStylePath;
static PWSTR g_pszStyles;

EXTERN_C
VOID
WINAPI
DUIFrame_ProcessAttach(VOID)
{
    InitializeCriticalSection(&g_csDirectUI);
    InitializeCriticalSection(&g_csStyle);
}

EXTERN_C
VOID
WINAPI
DUIFrame_ProcessDetach(VOID)
{
    DUI_ProcessDetachCleanup();
    DeleteCriticalSection(&g_csStyle);
    DeleteCriticalSection(&g_csDirectUI);
}

static
VOID
WINAPI
DUI_IgnoreParseError(
    _In_ const wchar_t *pszError,
    _In_opt_ const wchar_t *pszToken,
    _In_ int iLine,
    _In_opt_ void *pvContext)
{
}

HINSTANCE
WINAPI
GetShellStyleHInstance(
    _Outptr_opt_ PWSTR *ppszPath)
{
    WCHAR szPath[MAX_PATH];
    WCHAR szColor[100];
    HINSTANCE hinst = NULL;

    /* A theme ships its own ShellStyle.dll under Shell\<color>; the system copy is the fallback */
    if (SUCCEEDED(GetCurrentThemeName(szPath, _countof(szPath), szColor, _countof(szColor), NULL, 0)))
    {
        PathRemoveFileSpecW(szPath);
        PathAppendW(szPath, L"Shell");
        PathAppendW(szPath, szColor);
        PathAppendW(szPath, L"ShellStyle.dll");
        hinst = LoadLibraryExW(szPath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    }

    if (!hinst &&
        ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\ShellStyle.dll", szPath, _countof(szPath)))
    {
        hinst = LoadLibraryExW(szPath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    }

    if (hinst && ppszPath)
        SHStrDupW(szPath, ppszPath);
    return hinst;
}

HRESULT
WINAPI
DUI_LoadUIFileFromResources(
    _In_ HINSTANCE hinst,
    _In_ UINT uID,
    _Outptr_ PWSTR *ppszUIFile)
{
    HRSRC hrsrc;
    HGLOBAL hglob;
    LPCSTR pszAnsi;
    UINT cch;

    *ppszUIFile = NULL;

    hrsrc = FindResourceW(hinst, MAKEINTRESOURCEW(uID), L"UIFILE");
    hglob = hrsrc ? LoadResource(hinst, hrsrc) : NULL;
    pszAnsi = hglob ? (LPCSTR)LockResource(hglob) : NULL;
    if (!pszAnsi)
        return HRESULT_FROM_WIN32(GetLastError());

    /* UIFILE resources are ANSI and not terminated */
    cch = SizeofResource(hinst, hrsrc) + 1;
    *ppszUIFile = (PWSTR)LocalAlloc(LPTR, cch * sizeof(WCHAR));
    if (!*ppszUIFile)
        return E_OUTOFMEMORY;

    SHAnsiToUnicode(pszAnsi, *ppszUIFile, cch);
    return S_OK;
}

/* Reloads the shared sheet markup when the theme's ShellStyle.dll changed; hands back the loaded module */
static
HRESULT
UpdateStyle(
    _Out_opt_ HINSTANCE *phinstStyle)
{
    PWSTR pszPath = NULL;
    HINSTANCE hinst;
    HRESULT hr = S_OK;

    hinst = GetShellStyleHInstance(&pszPath);
    if (!hinst)
        return E_OUTOFMEMORY;

    EnterCriticalSection(&g_csStyle);
    if (!g_pszStylePath || StrCmpICW(g_pszStylePath, pszPath))
    {
        PWSTR pszStyles;

        hr = DUI_LoadUIFileFromResources(hinst, 1, &pszStyles);
        if (SUCCEEDED(hr))
        {
            PWSTR pszPathCopy = StrDupW(pszPath);

            if (pszPathCopy)
            {
                LocalFree(g_pszStylePath);
                g_pszStylePath = pszPathCopy;
                LocalFree(g_pszStyles);
                g_pszStyles = pszStyles;
            }
            else
            {
                hr = E_OUTOFMEMORY;
                LocalFree(pszStyles);
            }
        }
    }
    LeaveCriticalSection(&g_csStyle);
    CoTaskMemFree(pszPath);

    if (SUCCEEDED(hr) && phinstStyle)
        *phinstStyle = hinst;
    else
        FreeLibrary(hinst);
    return hr;
}

VOID
WINAPI
DUI_ProcessDetachCleanup(VOID)
{
    LocalFree(g_pszStylePath);
    g_pszStylePath = NULL;
    LocalFree(g_pszStyles);
    g_pszStyles = NULL;
}

static
HRESULT
DUI_InitProcess(VOID)
{
    HRESULT hr = E_OUTOFMEMORY;

    g_dwSlotStyleSheet = TlsAlloc();
    g_dwSlotThreadUsers = TlsAlloc();
    if (g_dwSlotStyleSheet != TLS_OUT_OF_INDEXES && g_dwSlotThreadUsers != TLS_OUT_OF_INDEXES)
    {
        hr = InitProcessPriv(ROSUI_VERSION_WIN7, DUIFRAME_HINST, TRUE, false);
        if (SUCCEEDED(hr))
        {
            hr = UpdateStyle(NULL);
            if (FAILED(hr))
                UnInitProcessPriv((unsigned short *)DUIFRAME_HINST);
        }
    }

    if (FAILED(hr))
    {
        if (g_dwSlotStyleSheet != TLS_OUT_OF_INDEXES)
            TlsFree(g_dwSlotStyleSheet);
        if (g_dwSlotThreadUsers != TLS_OUT_OF_INDEXES)
            TlsFree(g_dwSlotThreadUsers);
        g_dwSlotStyleSheet = TLS_OUT_OF_INDEXES;
        g_dwSlotThreadUsers = TLS_OUT_OF_INDEXES;
    }

    g_fProcessReady = SUCCEEDED(hr);
    return hr;
}

static
VOID
DUI_UninitProcess(VOID)
{
    if (g_dwSlotStyleSheet != TLS_OUT_OF_INDEXES)
    {
        TlsFree(g_dwSlotStyleSheet);
        g_dwSlotStyleSheet = TLS_OUT_OF_INDEXES;
    }
    if (g_dwSlotThreadUsers != TLS_OUT_OF_INDEXES)
    {
        TlsFree(g_dwSlotThreadUsers);
        g_dwSlotThreadUsers = TLS_OUT_OF_INDEXES;
    }
    UnInitProcessPriv((unsigned short *)DUIFRAME_HINST);
    g_fProcessReady = FALSE;
}

HRESULT
WINAPI
InitializeDirectUI(VOID)
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&g_csDirectUI);
    if (!g_cDirectUIUsers && !g_fProcessReady)
    {
        hr = DUI_InitProcess();
        if (SUCCEEDED(hr))
        {
            hr = InitializeDUIFrameClasses();
            if (FAILED(hr))
                DUI_UninitProcess();
        }
    }
    if (SUCCEEDED(hr))
        g_cDirectUIUsers++;
    LeaveCriticalSection(&g_csDirectUI);
    return hr;
}

VOID
WINAPI
UnInitializeDirectUI(VOID)
{
    EnterCriticalSection(&g_csDirectUI);
    if (g_cDirectUIUsers && !--g_cDirectUIUsers)
        DUI_UninitProcess();
    LeaveCriticalSection(&g_csDirectUI);
}

HRESULT
WINAPI
DUI_ReloadTheme(
    _In_opt_ DirectUI::Element *peRoot)
{
    DirectUI::DUIXmlParser *pParser;
    DirectUI::DUIXmlParser *pOld;
    HINSTANCE hinstStyle;
    HRESULT hr;

    hr = DirectUI::DUIXmlParser::Create(&pParser, NULL, NULL, DUI_IgnoreParseError, NULL);
    if (FAILED(hr))
        return hr;

    hr = UpdateStyle(&hinstStyle);
    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&g_csStyle);
        hr = pParser->SetPreprocessedXML(g_pszStyles, hinstStyle, DUIFRAME_HINST);
        LeaveCriticalSection(&g_csStyle);
        FreeLibrary(hinstStyle);
    }

    if (SUCCEEDED(hr))
    {
        /* Each thread keeps the parser that owns its sheets */
        pOld = (DirectUI::DUIXmlParser *)TlsGetValue(g_dwSlotStyleSheet);
        if (pOld)
            pOld->Destroy();

        hr = TlsSetValue(g_dwSlotStyleSheet, pParser) ? S_OK : E_FAIL;
        if (peRoot)
            pParser->UpdateSheets(peRoot);
        if (SUCCEEDED(hr))
            return hr;
    }

    pParser->Destroy();
    return hr;
}

HRESULT
WINAPI
DUI_InitThread(VOID)
{
    ULONG_PTR cUsers = (ULONG_PTR)TlsGetValue(g_dwSlotThreadUsers);
    HRESULT hr;

    if (!cUsers)
    {
        hr = InitThread(2);
        if (FAILED(hr))
            return hr;

        hr = DUI_ReloadTheme(NULL);
        if (FAILED(hr))
        {
            UnInitThread();
            return hr;
        }
    }

    TlsSetValue(g_dwSlotThreadUsers, (PVOID)(cUsers + 1));
    return S_OK;
}

VOID
WINAPI
DUI_UninitThread(VOID)
{
    ULONG_PTR cUsers = (ULONG_PTR)TlsGetValue(g_dwSlotThreadUsers) - 1;

    TlsSetValue(g_dwSlotThreadUsers, (PVOID)cUsers);
    if (cUsers)
        return;

    DirectUI::DUIXmlParser *pParser = (DirectUI::DUIXmlParser *)TlsGetValue(g_dwSlotStyleSheet);
    if (pParser)
    {
        pParser->Destroy();
        TlsSetValue(g_dwSlotStyleSheet, NULL);
    }
    UnInitThread();
}

DirectUI::Value *
WINAPI
DUI_GetSheetCB(
    _In_ const wchar_t *pszSheetID,
    _In_opt_ void *pvContext)
{
    DirectUI::DUIXmlParser *pParser = (DirectUI::DUIXmlParser *)TlsGetValue(g_dwSlotStyleSheet);
    DirectUI::Value *pvSheet = NULL;

    if (pParser)
        pParser->GetSheet(pszSheetID, &pvSheet);
    return pvSheet;
}

HRESULT
WINAPI
DUI_CreateParser(
    _In_ PCWSTR pszUIFile,
    _Outptr_ DirectUI::DUIXmlParser **ppParser,
    _In_opt_ IUnknown *punkSheetContext,
    _In_opt_ DirectUI::ParseErrorCallback pfnError)
{
    DirectUI::DUIXmlParser *pParser;
    HINSTANCE hinstStyle;
    HRESULT hr;

    *ppParser = NULL;
    hinstStyle = GetShellStyleHInstance(NULL);

    hr = DirectUI::DUIXmlParser::Create(&pParser, DUI_GetSheetCB, punkSheetContext,
                                        pfnError ? pfnError : DUI_IgnoreParseError, NULL);
    if (SUCCEEDED(hr))
    {
        if (FAILED(pParser->SetXML(pszUIFile, hinstStyle, DUIFRAME_HINST)))
        {
            pParser->Destroy();
            hr = E_FAIL;
        }
        else
        {
            *ppParser = pParser;
        }
    }

    if (hinstStyle)
        FreeLibrary(hinstStyle);
    return hr;
}

/* Parser over a UIFILE of hinst whose style sheets come through pfnGetSheet */
HRESULT
WINAPI
DUI_CreateParserWithCallbackFromResource(
    _In_ HINSTANCE hinst,
    _In_ UINT uID,
    _Outptr_ DirectUI::DUIXmlParser **ppParser,
    _In_ DirectUI::GetSheetCallback pfnGetSheet,
    _In_opt_ void *pvContext)
{
    DirectUI::DUIXmlParser *pParser;
    HINSTANCE hinstStyle;
    PWSTR pszUIFile;
    HRESULT hr;

    *ppParser = NULL;
    hr = DUI_LoadUIFileFromResources(hinst, uID, &pszUIFile);
    if (FAILED(hr))
        return hr;

    hinstStyle = GetShellStyleHInstance(NULL);
    hr = DirectUI::DUIXmlParser::Create(&pParser, pfnGetSheet, pvContext, DUI_IgnoreParseError, NULL);
    if (SUCCEEDED(hr))
    {
        if (FAILED(pParser->SetXML(pszUIFile, hinstStyle, hinst)))
        {
            pParser->Destroy();
            hr = E_FAIL;
        }
        else
        {
            *ppParser = pParser;
        }
    }

    if (hinstStyle)
        FreeLibrary(hinstStyle);
    LocalFree(pszUIFile);
    return hr;
}

/* Builds one resid of a UIFILE in this module; peSubstitute, when given, becomes the markup's top element */
HRESULT
WINAPI
DUI_CreateElementFromResource(
    _In_ HINSTANCE hinst,
    _In_ UINT uID,
    _In_ PCWSTR pszResID,
    _In_opt_ DirectUI::Element *peSubstitute,
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppe,
    _In_opt_ IUnknown *punkSheetContext)
{
    DirectUI::DUIXmlParser *pParser;
    PWSTR pszUIFile;
    HRESULT hr;

    *ppe = NULL;
    hr = DUI_LoadUIFileFromResources(hinst, uID, &pszUIFile);
    if (FAILED(hr))
        return hr;

    hr = DUI_CreateParser(pszUIFile, &pParser, punkSheetContext, NULL);
    if (SUCCEEDED(hr))
    {
        hr = pParser->CreateElement(pszResID, peSubstitute, peParent, pdwDeferCookie, ppe);
        pParser->Destroy();
    }
    LocalFree(pszUIFile);
    return hr;
}

HRESULT
WINAPI
DUI_GetElementRootHWNDElement(
    _In_ DirectUI::Element *pe,
    _Outptr_ DirectUI::HWNDElement **ppheRoot)
{
    DirectUI::Element *peRoot = pe->GetRoot();

    if (peRoot && peRoot->GetClassInfoW()->IsSubclassOf(DirectUI::HWNDElement::GetClassInfoPtr()))
    {
        *ppheRoot = static_cast<DirectUI::HWNDElement *>(peRoot);
        return S_OK;
    }

    *ppheRoot = NULL;
    return E_FAIL;
}

HRESULT
WINAPI
DUI_GetElementRootHWND(
    _In_ DirectUI::Element *pe,
    _Out_ HWND *phwnd)
{
    DirectUI::HWNDElement *pheRoot;
    HRESULT hr = DUI_GetElementRootHWNDElement(pe, &pheRoot);

    if (SUCCEEDED(hr))
    {
        *phwnd = pheRoot->GetHWND();
        hr = *phwnd ? S_OK : S_FALSE;
    }
    return hr;
}

HRESULT
WINAPI
DUI_GetMouseEventAbsolutePos(
    _In_ DirectUI::Element *pe,
    _In_ const POINT *pptElement,
    _Out_ POINT *pptScreen)
{
    RECT rcWindow;
    RECT rcElement;
    HWND hwnd;
    HRESULT hr;

    hr = DUI_GetElementRootHWND(pe, &hwnd);
    if (FAILED(hr))
        return hr;

    if (!GetWindowRect(hwnd, &rcWindow))
        return HRESULT_FROM_WIN32(GetLastError());

    hr = pe->GetRootRelativeBounds(&rcElement);
    if (SUCCEEDED(hr))
    {
        pptScreen->x = rcWindow.left + rcElement.left + pptElement->x;
        pptScreen->y = rcWindow.top + rcElement.top + pptElement->y;
    }
    return hr;
}

BOOL
WINAPI
DUI_TrackPopupMenu(
    _In_ HMENU hmenu,
    _In_ UINT uFlags,
    _In_ int x,
    _In_ int y,
    _In_ HWND hwnd,
    _In_opt_ const RECT *prc,
    _In_ BOOL fRTL)
{
    BOOL fMenuDropRight = GetSystemMetrics(SM_MENUDROPALIGNMENT);

    /* Mirrored elements pop their menus the other way */
    uFlags &= ~(TPM_LAYOUTRTL | TPM_RIGHTALIGN);
    if (fRTL)
        uFlags |= TPM_LAYOUTRTL | (fMenuDropRight ? 0 : TPM_RIGHTALIGN);
    else if (fMenuDropRight)
        uFlags |= TPM_RIGHTALIGN;

    return TrackPopupMenu(hmenu, uFlags, x, y, 0, hwnd, prc);
}

VOID
WINAPI
DUI_WalkIUnknownElements(
    _In_opt_ DirectUI::Element *peRoot,
    _In_ PFNUNKNOWNELEMENTWALK pfnCallback,
    _In_ LPARAM lParam)
{
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pdaChildren;
    DirectUI::Value *pvChildren;
    UINT i;

    if (!peRoot)
        return;

    pdaChildren = peRoot->GetChildren(&pvChildren);
    if (!pdaChildren)
        return;

    /* Depth first, parents before their children */
    for (i = 0; i < pdaChildren->GetSize(); i++)
    {
        DirectUI::Element *peChild = pdaChildren->GetItem(i);
        IUnknown *punk = CElementWithIUnknown::GetUnknownFromElement(peChild);

        if (punk)
            pfnCallback(punk, lParam);
        DUI_WalkIUnknownElements(peChild, pfnCallback, lParam);
    }
    pvChildren->Release();
}

VOID
WINAPI
DUI_SendInitializationToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameNotificationClient *pfnc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfnc))))
    {
        pfnc->LayoutInitialized();
        pfnc->Release();
    }
}

VOID
WINAPI
DUI_SendNotificationToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameNotificationClient *pfnc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfnc))))
    {
        pfnc->Notify((PCWSTR)lParam);
        pfnc->Release();
    }
}

VOID
WINAPI
DUI_SendNavigateAwayToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameNotificationClient *pfnc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfnc))))
    {
        pfnc->OnNavigateAway();
        pfnc->Release();
    }
}

VOID
WINAPI
DUI_SendSelectedItemChangedToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameShellViewClient *pfsvc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfsvc))))
    {
        pfsvc->OnSelectedItemChanged();
        pfsvc->Release();
    }
}

VOID
WINAPI
DUI_SendSelectionChangedToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameShellViewClient *pfsvc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfsvc))))
    {
        pfsvc->OnSelectionChanged();
        pfsvc->Release();
    }
}

VOID
WINAPI
DUI_SendContentsChangedToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameShellViewClient *pfsvc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfsvc))))
    {
        pfsvc->OnContentsChanged();
        pfsvc->Release();
    }
}

VOID
WINAPI
DUI_SendFolderChangedToUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameShellViewClient *pfsvc;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfsvc))))
    {
        pfsvc->OnFolderChanged();
        pfsvc->Release();
    }
}

VOID
WINAPI
DUI_SetSiteOnUnknown(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IUnknown_SetSite(punk, (IUnknown *)lParam);
}

/* lParam points at { HRESULT hr; MSG *pmsg }; the walk stops forwarding once someone handled it */
VOID
WINAPI
DUI_TranslateAcceleratorIO(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    struct ACCELWALK { HRESULT hr; MSG *pmsg; } *pWalk = (ACCELWALK *)lParam;
    IInputObject *pio;

    if (pWalk->hr == S_OK || FAILED(punk->QueryInterface(IID_PPV_ARGS(&pio))))
        return;

    if (pio->HasFocusIO() == S_OK)
        pWalk->hr = pio->TranslateAcceleratorIO(pWalk->pmsg);
    pio->Release();
}

/* IVisualProperties::SetColor value that hands the color back to the view's own default */
#define VIEW_COLOR_UNSET 0x8FFFFFFF

/* Solid fills and system color indices resolve to a COLORREF; anything else is CLR_INVALID */
static
COLORREF
DUI_GetColor(
    _In_ DirectUI::Element *pe,
    _In_ const DirectUI::PropertyInfo *ppi)
{
    DirectUI::Value *pv = pe->GetValue(ppi, 2, NULL);
    COLORREF cr = CLR_INVALID;

    if (pv->GetType() == (int)DirectUI::ValueType::Color)
        cr = pv->GetFill()->ref.cr & 0x00FFFFFF;
    else if (pv->GetType() == (int)DirectUI::ValueType::Int)
        cr = ColorFromEnumI(pv->GetInt());
    pv->Release();
    return cr;
}

/* Pushes the view host's colors into the folder view, or resets them when there is no host */
VOID
WINAPI
DUI_ApplyLayoutStateToView(
    _In_ IFolderView2 *pfv,
    _In_opt_ DirectUI::Element *peViewHost)
{
    IVisualProperties *pvp;

    if (FAILED(pfv->QueryInterface(IID_PPV_ARGS(&pvp))))
        return;

    pvp->SetColor(VPCF_TEXT, VIEW_COLOR_UNSET);
    pvp->SetColor(VPCF_BACKGROUND, VIEW_COLOR_UNSET);
    pvp->SetColor(VPCF_SORTCOLUMN, VIEW_COLOR_UNSET);
    pvp->SetColor(VPCF_SUBTEXT, VIEW_COLOR_UNSET);
    pvp->SetWatermark(NULL, VPWF_DEFAULT);

    if (peViewHost)
    {
        DirectUI::IClassInfo *pci = peViewHost->GetClassInfoW();
        COLORREF crText = DUI_GetColor(peViewHost, DirectUI::Element::ForegroundProp());
        COLORREF crBack = DUI_GetColor(peViewHost, DirectUI::Element::BackgroundProp());
        COLORREF crSort = CLR_INVALID;
        COLORREF crSub = CLR_INVALID;
        BOOL fHasSort = pci->IsValidProperty(CDUIViewHost::SortColumnProp);
        BOOL fHasSub = pci->IsValidProperty(CDUIViewHost::SubTextProp);

        if (fHasSort)
            crSort = DUI_GetColor(peViewHost, CDUIViewHost::SortColumnProp);
        if (fHasSub)
            crSub = DUI_GetColor(peViewHost, CDUIViewHost::SubTextProp);

        /* All or nothing: a partial palette would mix theme and default colors */
        if (crText != CLR_INVALID && crBack != CLR_INVALID &&
            (!fHasSort || crSort != CLR_INVALID) && (!fHasSub || crSub != CLR_INVALID))
        {
            pvp->SetColor(VPCF_TEXT, crText);
            pvp->SetColor(VPCF_BACKGROUND, crBack);
            if (crSort != CLR_INVALID)
                pvp->SetColor(VPCF_SORTCOLUMN, crSort);
            if (crSub != CLR_INVALID)
                pvp->SetColor(VPCF_SUBTEXT, crSub);
        }
    }
    pvp->Release();
}

HRESULT
WINAPI
IUnknown_GetExplorerPaneState(
    _In_opt_ IUnknown *punkSite,
    _In_ REFEXPLORERPANE ep,
    _Out_ EXPLORERPANESTATE *peps)
{
    IExplorerPaneVisibility *pepv;
    HRESULT hr;

    *peps = EPS_DONTCARE;
    hr = IUnknown_QueryService(punkSite, SID_ExplorerPaneVisibility, IID_PPV_ARGS(&pepv));
    if (SUCCEEDED(hr))
    {
        hr = pepv->GetPaneState(ep, peps);
        pepv->Release();
    }
    return hr;
}

BOOL
WINAPI
PropertyBag_ReadBOOLDefault(
    _In_ IPropertyBag *ppb,
    _In_ PCWSTR pszName,
    _In_ BOOL fDefault)
{
    BOOL fValue;

    if (FAILED(SHPropertyBag_ReadBOOL(ppb, pszName, &fValue)))
        fValue = fDefault;
    return fValue;
}

/* Returns 1 for Tab or F6, -1 with Shift held, 0 for anything else */
int
WINAPI
IsVK_TABCycler(
    _In_opt_ const MSG *pmsg)
{
    if (!pmsg || pmsg->message != WM_KEYDOWN)
        return 0;
    if (pmsg->wParam != VK_TAB && pmsg->wParam != VK_F6)
        return 0;
    if (GetKeyState(VK_CONTROL) < 0)
        return 0;

    /* Keyboard navigation starts, so focus rectangles must show from here on */
    SendMessageW(::GetParent(pmsg->hwnd), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    return (GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
}
