/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Library pane module: library name, included locations and arrange-by control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <DUser.h>

/* UIFILEs of the pane and of its arrange-by control */
#define IDR_UIFILE_LIBRARYPANE      59
#define IDR_UIFILE_LIBRARYTOPVIEW   60

#define IDS_LIBRARIES               38330
#define IDS_LIBRARIES_HINT          34568
#define IDS_HOMEGROUP               34582
#define IDS_HOMEGROUP_HINT          34583
#define IDS_HOMEGROUPMEMBER_HINT    34591
#define IDS_LIBRARYPANE_NAME        38232
#define IDS_LIBRARY_LOCATION        38304
#define IDS_LIBRARY_LOCATIONS       38305
#define IDS_LIBRARY_TITLE           38306
#define IDS_ARRANGEBY               50690

/* GetSearchOnlyPropFromLibrary results */
#define LIBRARY_SEARCH_NONE         1
#define LIBRARY_SEARCH_SOME         2
#define LIBRARY_SEARCH_ALL          3

#ifndef CDB2GVF_ISFOLDERPICKER
#define CDB2GVF_ISFOLDERPICKER      0x20
#endif

#ifndef SIGDN_PARENTRELATIVEFORUI
#define SIGDN_PARENTRELATIVEFORUI   ((SIGDN)0x80094001)
#endif

/* Characters of a location name shown in the location button tooltip */
#define LOCATION_NAME_MAX           40

/* Libraries property set: the library or location is reached through search only */
static const PROPERTYKEY c_pkeyIsSearchOnlyItem =
    { { 0x5d76b67f, 0x9b3d, 0x44bb, { 0xb6, 0xae, 0x25, 0xda, 0x4f, 0x63, 0x8a, 0x67 } }, 4 };

DirectUI::IClassInfo *CLibraryPaneModuleInner::Class = NULL;

/* Helpers */

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

/* Formats a message string resource with one argument into a CoTaskMem string */
static
HRESULT
FormatResMessageAlloc(
    _In_ UINT uID,
    _Outptr_ PWSTR *ppsz,
    ...)
{
    WCHAR szFormat[256];
    PWSTR pszLocal = NULL;
    va_list args;
    HRESULT hr;

    *ppsz = NULL;
    if (!LoadStringW(DUIFRAME_HINST, uID, szFormat, _countof(szFormat)))
        return HRESULT_FROM_WIN32(GetLastError());

    va_start(args, ppsz);
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER, szFormat, 0, 0,
                        (PWSTR)&pszLocal, 0, &args))
    {
        va_end(args);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    va_end(args);

    hr = SHStrDupW(pszLocal, ppsz);
    LocalFree(pszLocal);
    return hr;
}

static
HRESULT
LoadResStringAlloc(
    _In_ UINT uID,
    _Outptr_ PWSTR *ppsz)
{
    WCHAR szText[256];

    *ppsz = NULL;
    if (!LoadStringW(DUIFRAME_HINST, uID, szText, _countof(szText)))
        return HRESULT_FROM_WIN32(GetLastError());
    return SHStrDupW(szText, ppsz);
}

/* The folder in view as a shell item */
static
HRESULT
QueryCurrentItem(
    _In_opt_ IUnknown *punkSite,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IPersistFolder2 *ppf2;
    PIDLIST_ABSOLUTE pidl;
    IFolderView *pfv;
    HRESULT hr;

    *ppv = NULL;
    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (FAILED(hr))
        return hr;

    hr = pfv->GetFolder(riid, ppv);
    if (FAILED(hr) && SUCCEEDED(pfv->GetFolder(IID_PPV_ARGS(&ppf2))))
    {
        hr = ppf2->GetCurFolder(&pidl);
        if (SUCCEEDED(hr))
        {
            hr = SHCreateItemFromIDList(pidl, riid, ppv);
            ILFree(pidl);
        }
        ppf2->Release();
    }
    pfv->Release();
    return hr;
}

/* The common file dialog picking folders has no library pane */
static
BOOL
IsFolderPicker(
    _In_opt_ IUnknown *punkSite)
{
    ICommDlgBrowser2 *pcdb;
    DWORD dwFlags;
    BOOL fPicker = FALSE;

    if (SUCCEEDED(IUnknown_QueryService(punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&pcdb))))
    {
        if (SUCCEEDED(pcdb->GetViewFlags(&dwFlags)) && (dwFlags & CDB2GVF_ISFOLDERPICKER))
            fPicker = TRUE;
        pcdb->Release();
    }
    return fPicker;
}

/* Walks up from the item to the first ancestor that is a library */
static
HRESULT
GetRootLibraryFromItem(
    _In_ IShellItem *psi,
    _COM_Outptr_ IShellItem **ppsiLibrary)
{
    IShellItem *psiWalk = psi;
    IShellItem *psiParent;
    ILibraryDescription *pld;

    *ppsiLibrary = NULL;
    psiWalk->AddRef();
    while (!*ppsiLibrary)
    {
        if (SUCCEEDED(psiWalk->BindToHandler(NULL, BHID_SFObject, IID_PPV_ARGS(&pld))))
        {
            IUnknown_Set((IUnknown **)ppsiLibrary, psiWalk);
            pld->Release();
        }

        if (FAILED(psiWalk->GetParent(&psiParent)))
            break;
        psiWalk->Release();
        psiWalk = psiParent;
    }
    psiWalk->Release();
    return *ppsiLibrary ? S_OK : E_FAIL;
}

/* A location stored on another machine (a roaming library) is not reachable from here */
static
BOOL
IsAccessibleLibraryLocation(
    _In_ ILocationDescription *pld)
{
    INamedPropertyStore *pnps;
    PROPVARIANT pv;
    BOOL fAccessible = TRUE;

    if (FAILED(pld->GetPropertyStore(IID_PPV_ARGS(&pnps))))
        return TRUE;

    PropVariantInit(&pv);
    if (SUCCEEDED(pnps->GetNamedValue(L"MachineGUID", &pv)))
    {
        GUID guidLocation;

        if (SUCCEEDED(PropVariantToGUID(&pv, &guidLocation)) && !IsEqualGUID(guidLocation, GUID_NULL))
        {
            WCHAR szMachine[64];
            DWORD cbMachine = sizeof(szMachine);
            GUID guidMachine;

            if (SHRegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                               SRRF_RT_REG_SZ, NULL, szMachine, &cbMachine) == ERROR_SUCCESS &&
                GUIDFromStringW(szMachine, &guidMachine))
            {
                fAccessible = IsEqualGUID(guidLocation, guidMachine);
            }
        }
        PropVariantClear(&pv);
    }
    pnps->Release();
    return fAccessible;
}

static
HRESULT
GetAccessibleLibraryLocationCount(
    _In_ ILibraryDescription *pld,
    _Out_ UINT *pcLocations)
{
    ILocationDescription *ploc;
    HRESULT hr;

    *pcLocations = 0;
    hr = pld->GetLocationCount(pcLocations);
    for (int i = (int)*pcLocations - 1; SUCCEEDED(hr) && i >= 0; i -= 1)
    {
        hr = pld->GetLocationAt(i, &ploc);
        if (SUCCEEDED(hr))
        {
            if (!IsAccessibleLibraryLocation(ploc))
                *pcLocations -= 1;
            ploc->Release();
        }
    }

    if (FAILED(hr))
        *pcLocations = 0;
    return hr;
}

static
BOOL
GetBoolProperty(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key)
{
    PROPVARIANT pv;
    BOOL fValue = FALSE;

    PropVariantInit(&pv);
    if (SUCCEEDED(pps->GetValue(key, &pv)))
    {
        if (FAILED(PropVariantToBoolean(pv, &fValue)))
            fValue = FALSE;
        PropVariantClear(&pv);
    }
    return fValue;
}

/* A library is search only when it says so, or when every location of it is */
static
UINT
GetSearchOnlyPropFromLibrary(
    _In_ ILibraryDescription *pld)
{
    ILocationDescription *ploc;
    IPropertyStore *pps;
    UINT uResult = LIBRARY_SEARCH_NONE;
    UINT cLocations = 0;
    UINT cSearchOnly = 0;

    if (SUCCEEDED(pld->GetPropertyStore(GPS_DELAYCREATION, IID_PPV_ARGS(&pps))))
    {
        if (GetBoolProperty(pps, c_pkeyIsSearchOnlyItem))
            uResult = LIBRARY_SEARCH_ALL;
        pps->Release();
    }

    if (uResult != LIBRARY_SEARCH_NONE)
        return uResult;

    if (FAILED(pld->GetLocationCount(&cLocations)) || !cLocations)
        return uResult;

    for (UINT i = 0; i < cLocations; i++)
    {
        if (SUCCEEDED(pld->GetLocationAt(i, &ploc)))
        {
            if (SUCCEEDED(ploc->GetPropertyStore(IID_PPV_ARGS(&pps))))
            {
                if (GetBoolProperty(pps, c_pkeyIsSearchOnlyItem))
                    cSearchOnly++;
                pps->Release();
            }
            ploc->Release();
        }
    }

    if (cSearchOnly == cLocations)
        return LIBRARY_SEARCH_ALL;
    return cSearchOnly ? LIBRARY_SEARCH_SOME : LIBRARY_SEARCH_NONE;
}

/* The nav pane opens the library so its new locations show */
static
VOID
ExpandLibraryInNavigationPane(
    _In_opt_ IUnknown *punkSite,
    _In_ IShellItem *psiLibrary)
{
    INameSpaceTreeControl *pnstc = NULL;

    if (!punkSite)
        return;

    if (FAILED(IUnknown_QueryService(punkSite, IID_INameSpaceTreeControl, IID_PPV_ARGS(&pnstc))))
        punkSite->QueryInterface(IID_PPV_ARGS(&pnstc));
    if (!pnstc)
        return;

    SHChangeNotify(0, SHCNF_FLUSH, NULL, NULL);
    if (SUCCEEDED(pnstc->SetItemState(psiLibrary, NSTCIS_EXPANDED, NSTCIS_EXPANDED)))
        pnstc->EnsureItemVisible(psiLibrary);
    pnstc->Release();
}

/* LibraryPaneLayout */

void
LibraryPaneLayout::DoLayout(
    DirectUI::Element *peContainer,
    int cx,
    int cy)
{
    RECT rc = { 0, 0, cx, cy };

    _DoLayoutWorker(peContainer, &rc);
}

/* All three parts on one line when they fit; otherwise the title shrinks and the location line wraps */
void
LibraryPaneLayout::_DoLayoutWorker(
    _In_ DirectUI::Element *peContainer,
    _In_ const RECT *prc)
{
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pChildren;
    DirectUI::Element *peTitle;
    DirectUI::Element *peLocations;
    DirectUI::Element *peTopView;
    DirectUI::Value *pv = NULL;
    SIZE sizeTitle;
    SIZE sizeLocations;
    SIZE sizeTopView;
    int cx = prc->right - prc->left;
    int cy = prc->bottom - prc->top;
    int cxWidest;

    pChildren = peContainer->GetChildren(&pv);
    if (pChildren && pChildren->GetSize() == 3)
    {
        peTitle = pChildren->GetItem(0);
        peLocations = pChildren->GetItem(1);
        peTopView = pChildren->GetItem(2);

        peTitle->SetTooltip(false);
        sizeLocations = *peLocations->GetDesiredSize();
        sizeTitle = *peTitle->GetDesiredSize();
        sizeTopView = *peTopView->GetDesiredSize();

        UpdateLayoutRect(peContainer, cx, cy, peTitle, 0, 0, 0, 0);
        UpdateLayoutRect(peContainer, cx, cy, peLocations, 0, 0, 0, 0);
        UpdateLayoutRect(peContainer, cx, cy, peTopView, 0, 0, 0, 0);

        cxWidest = max(sizeTitle.cx, sizeLocations.cx);
        if (sizeTopView.cx + cxWidest >= cx)
        {
            DirectUI::Element *peLeaf;
            int cxLeft = cx - sizeTopView.cx;
            int cxText = max(cxLeft, 0);

            if (cxLeft < sizeTitle.cx)
                peTitle->SetTooltip(true);

            UpdateLayoutRect(peContainer, cx, cy, peTitle, prc->left, prc->top, cxText, sizeTitle.cy);

            peLeaf = peLocations->FindDescendent(StrToID(L"Leaf"));
            if ((peLeaf && peLeaf->GetWidth()) || cxLeft > sizeLocations.cx)
            {
                UpdateLayoutRect(peContainer, cx, cy, peLocations, prc->left, prc->top + sizeTitle.cy, cxText,
                                 sizeLocations.cy);
            }

            UpdateLayoutRect(peContainer, cx, cy, peTopView, max((int)(prc->right - sizeTopView.cx), 0),
                             prc->top + (cy - sizeTopView.cy) / 2, sizeTopView.cx, sizeTopView.cy);
        }
        else
        {
            UpdateLayoutRect(peContainer, cx, cy, peTitle, prc->left, prc->top, sizeTitle.cx, sizeTitle.cy);
            UpdateLayoutRect(peContainer, cx, cy, peLocations, prc->left, prc->top + sizeTitle.cy,
                             sizeLocations.cx, sizeLocations.cy);
            UpdateLayoutRect(peContainer, cx, cy, peTopView, prc->right - sizeTopView.cx,
                             prc->top + (cy - sizeTopView.cy) / 2, sizeTopView.cx, sizeTopView.cy);
        }
    }

    if (pv)
        pv->Release();
}

/* The pane is as wide as its parts side by side, bounded by the constraint */
SIZE
LibraryPaneLayout::UpdateDesiredSize(
    DirectUI::Element *peContainer,
    int cxConstraint,
    int cyConstraint,
    DirectUI::Surface *psrf)
{
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pChildren;
    DirectUI::Value *pv = NULL;
    SIZE size = { 0, 0 };

    pChildren = peContainer->GetChildren(&pv);
    if (pChildren)
    {
        for (UINT i = 0; i < pChildren->GetSize(); i++)
        {
            SIZE sizeChild = pChildren->GetItem(i)->_UpdateDesiredSize(cxConstraint, cyConstraint, psrf);

            size.cx += sizeChild.cx;
        }
        size.cy = cyConstraint;
        size.cx = min(size.cx, cxConstraint);
    }

    if (pv)
        pv->Release();
    return size;
}

/* Tab and arrows alternate between the title and the location line */
DirectUI::Element *
LibraryPaneLayout::GetAdjacent(
    DirectUI::Element *peContainer,
    DirectUI::Element *peFrom,
    int iNavDir,
    DirectUI::NavReference const *pnr,
    unsigned long dwFlags)
{
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pChildren;
    DirectUI::Element *peNext;
    DirectUI::Value *pv = NULL;
    int iNext = 1;

    if (iNavDir & 1)
        return DirectUI::Layout::GetAdjacent(peContainer, peFrom, iNavDir, pnr, dwFlags);

    pChildren = peContainer->GetChildren(&pv);
    if (peFrom)
        iNext = GetLayoutIndexFromChild(peContainer, peFrom) % 2 + 1;

    peNext = GetChildFromLayoutIndex(peContainer, iNext, pChildren);
    if (peNext)
    {
        DirectUI::Element *peInner = peNext->GetAdjacent(NULL, iNavDir, pnr, dwFlags);

        if (peInner)
            peNext = peInner;
    }

    if (pv)
        pv->Release();
    return peNext;
}

/* ClipLayout */

void
ClipLayout::DoLayout(
    DirectUI::Element *peContainer,
    int cx,
    int cy)
{
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pChildren;
    DirectUI::Value *pv = NULL;

    pChildren = peContainer->GetChildren(&pv);
    if (pChildren && pChildren->GetSize())
    {
        DirectUI::Element *peChild = pChildren->GetItem(0);

        UpdateLayoutRect(peContainer, cx, cy, peChild, 0, 0, peChild->GetWidth(), peChild->GetHeight());
    }

    if (pv)
        pv->Release();
}

SIZE
ClipLayout::UpdateDesiredSize(
    DirectUI::Element *peContainer,
    int cxConstraint,
    int cyConstraint,
    DirectUI::Surface *psrf)
{
    SIZE size = { cxConstraint, cyConstraint };

    return size;
}

/* CLibraryPaneModuleInner */

HRESULT
WINAPI
CLibraryPaneModuleInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Layout *pLayout = NULL;
    CLibraryPaneModuleInner *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CLibraryPaneModuleInner>();
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

    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CLibraryPaneModuleInner::Register()
{
    return DirectUI::ClassInfo<CLibraryPaneModuleInner, CElementWithSite,
                               DirectUI::StandardCreator<CLibraryPaneModuleInner>>::Register(
        DUIFRAME_HINST, L"LibraryPaneModuleInner", NULL, 0, false);
}

STDMETHODIMP
CLibraryPaneModuleInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CLibraryPaneModuleInner, IFrameNotificationClient),
        QITABENT(CLibraryPaneModuleInner, IFrameShellViewClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

void
CLibraryPaneModuleInner::OnDestroy()
{
    IUnknown_SafeReleaseAndNullPtr(&_psiLibrary);
    DirectUI::Element::OnDestroy();
}

/* Owner window for dialogs started from the pane */
static
HWND
OwnerWindowFromSite(
    _In_opt_ IUnknown *punkSite)
{
    HWND hwnd = NULL;

    if (FAILED(IUnknown_GetWindow(punkSite, &hwnd)))
        hwnd = NULL;
    return hwnd;
}

/* The library manage dialog is not part of this shell32 yet */
static
HRESULT
ShowManageLibraryUI(
    _In_ IShellItem *psiLibrary,
    _In_opt_ HWND hwndOwner)
{
    UNREFERENCED_PARAMETER(psiLibrary);
    UNREFERENCED_PARAMETER(hwndOwner);
    return E_NOTIMPL;
}

/* The location button opens the dialog that edits the library's locations */
void
CLibraryPaneModuleInner::OnEvent(
    DirectUI::Event *pev)
{
    if (pev->nStage == GMF_BUBBLED && pev->uidType.m_Value == DirectUI::Button::Click().m_Value &&
        pev->peTarget == FindDescendent(StrToID(L"LocationButton")))
    {
        if (ShowManageLibraryUI(_psiLibrary, OwnerWindowFromSite(_punkSite)) == S_OK &&
            SUCCEEDED(_SetInfo()))
        {
            ExpandLibraryInNavigationPane(_punkSite, _psiLibrary);
        }
        pev->fHandled = true;
    }

    if (!pev->fHandled)
        DirectUI::Element::OnEvent(pev);
}

STDMETHODIMP
CLibraryPaneModuleInner::LayoutInitialized()
{
    _special = LPS_NONE;
    _fLayoutInitialized = TRUE;
    return _SetInfo();
}

STDMETHODIMP
CLibraryPaneModuleInner::Notify(
    PCWSTR pszPropertyName)
{
    if (!StrCmpICW(pszPropertyName, L"SettingsChanged"))
        return _SetInfo();
    return S_OK;
}

/* Leaving the folder forgets the library and resets the pane's remembered visibility */
STDMETHODIMP
CLibraryPaneModuleInner::OnNavigateAway()
{
    IPropertyBag *ppb;

    IUnknown_SafeReleaseAndNullPtr(&_psiLibrary);
    if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        VARIANT var;

        VariantInit(&var);
        ppb->Write(L"LibraryPaneSizer_Visible", &var);
        ppb->Release();
    }
    return S_OK;
}

STDMETHODIMP
CLibraryPaneModuleInner::OnContentsChanged()
{
    return _SetInfo();
}

/* Title, then either the location line of a library or the hint text of a special folder */
HRESULT
CLibraryPaneModuleInner::_SetInfo()
{
    DirectUI::Element *peIdentifier;
    PWSTR pszTitle;
    HRESULT hr;

    hr = _EnsurePane();
    if (FAILED(hr))
        return hr;

    hr = _GetDisplayTitle(&pszTitle);
    if (FAILED(hr))
        return hr;

    peIdentifier = FindDescendent(StrToID(L"LibraryIdentifier"));
    if (peIdentifier)
    {
        peIdentifier->SetContentString(pszTitle);
        peIdentifier->SetAccDesc(pszTitle);
    }
    CoTaskMemFree(pszTitle);

    if (_special == LPS_NONE)
    {
        hr = _SetLocationInfo();
    }
    else
    {
        UINT uID = 0;
        PWSTR pszHint;

        switch (_special)
        {
            case LPS_LIBRARIES:
                uID = IDS_LIBRARIES_HINT;
                break;
            case LPS_HOMEGROUP:
                uID = IDS_HOMEGROUP_HINT;
                break;
            case LPS_HOMEGROUPMEMBER:
                uID = IDS_HOMEGROUPMEMBER_HINT;
                break;
            default:
                break;
        }

        hr = LoadResStringAlloc(uID, &pszHint);
        if (SUCCEEDED(hr))
        {
            DirectUI::Element *peLeaf;

            _ShowLocationButton(FALSE);
            peLeaf = FindDescendent(StrToID(L"Leaf"));
            if (peLeaf)
                peLeaf->SetContentString(pszHint);
            CoTaskMemFree(pszHint);
        }
    }

    if (SUCCEEDED(hr) && _peTopView)
    {
        if (_special != LPS_NONE)
        {
            _peTopView->SetWidth(0);
        }
        else
        {
            _peTopView->RemoveLocalValue(DirectUI::Element::WidthProp);
            _InitializeTopViewControl();
        }
    }
    return hr;
}

/* The pane exists only inside a library, the Libraries folder or the homegroup */
HRESULT
CLibraryPaneModuleInner::_EnsurePane()
{
    DirectUI::Element *pePane;
    DirectUI::Element *peLibraryPane;
    DirectUI::Element *peOverlay;
    IShellItem *psiCurrent;
    WCHAR szName[64];
    HRESULT hr = E_FAIL;

    if (_special != LPS_NONE || _psiLibrary)
    {
        hr = S_OK;
    }
    else if (_fLayoutInitialized && !IsFolderPicker(_punkSite))
    {
        hr = QueryCurrentItem(_punkSite, IID_PPV_ARGS(&psiCurrent));
        if (SUCCEEDED(hr))
        {
            hr = _GetLibraryFromItem(psiCurrent, &_psiLibrary);
            if (FAILED(hr))
            {
                _special = _SpecialLibraryPane(psiCurrent);
                hr = (_special != LPS_NONE) ? S_OK : E_FAIL;
            }
            psiCurrent->Release();
        }
    }

    if (FAILED(hr))
    {
        DUI_WalkIUnknownElements(this, DUI_SetSiteOnUnknown, 0);
        DestroyAll(false);
        _peTopView = NULL;
        return hr;
    }

    if (ElementHasChildren(this))
        return S_OK;

    hr = DUI_CreateElementFromResource(DUIFRAME_HINST, IDR_UIFILE_LIBRARYPANE, L"LibraryPane", NULL, this, NULL,
                                       &pePane, NULL);
    if (FAILED(hr))
        return hr;

    hr = Add(pePane);
    if (FAILED(hr))
        return hr;

    peLibraryPane = FindDescendent(StrToID(L"LibraryPane"));
    if (peLibraryPane)
    {
        LibraryPaneLayout *pLayout = DirectUI::HNew<LibraryPaneLayout>();

        if (!pLayout)
            return E_OUTOFMEMORY;
        pLayout->Initialize();

        SetGadgetStyle(GetDisplayNode(), GS_CLIPINSIDE, GS_CLIPINSIDE);
        LoadStringW(DUIFRAME_HINST, IDS_LIBRARYPANE_NAME, szName, _countof(szName));
        peLibraryPane->SetAccName(szName);
        peLibraryPane->SetAccDesc(szName);
        hr = peLibraryPane->SetLayout(pLayout);
        if (FAILED(hr))
            return hr;
    }

    peOverlay = FindDescendent(StrToID(L"LibraryPaneOverlayContainer"));
    if (peOverlay)
    {
        ClipLayout *pClip = DirectUI::HNew<ClipLayout>();

        if (!pClip)
            return E_OUTOFMEMORY;
        pClip->Initialize();
        hr = peOverlay->SetLayout(pClip);
        if (FAILED(hr))
            return hr;
    }

    /* The arrange-by control comes from its own markup and needs the split button class */
    {
        DirectUI::DUIXmlParser *pParser;
        DirectUI::Element *peControl = NULL;
        DirectUI::Element *pePrefix = NULL;
        DirectUI::Element *peHolder;
        PWSTR pszUIFile;

        if (SUCCEEDED(DUI_LoadUIFileFromResources(DUIFRAME_HINST, IDR_UIFILE_LIBRARYTOPVIEW, &pszUIFile)))
        {
            if (SUCCEEDED(DUI_CreateParser(pszUIFile, &pParser, NULL, NULL)))
            {
                if (SUCCEEDED(pParser->CreateElement(L"CommandElement", NULL, NULL, NULL, &peControl)) && peControl)
                {
                    DirectUI::Element *peAction;

                    peHolder = FindDescendent(StrToID(L"TopViewControl"));
                    if (SUCCEEDED(pParser->CreateElement(L"TopViewPrefixElement", NULL, peControl, NULL,
                                                         &pePrefix)) &&
                        SUCCEEDED(peControl->Insert(pePrefix, 0)) &&
                        peHolder && SUCCEEDED(peHolder->Add(peControl)))
                    {
                        LoadStringW(DUIFRAME_HINST, IDS_ARRANGEBY, szName, _countof(szName));
                        peAction = peControl->FindDescendent(StrToID(L"ActionButton"));
                        if (peAction)
                            peAction->SetAccName(szName);
                        _peTopView = peControl;
                    }
                    else
                    {
                        peControl->Destroy(true);
                    }
                }
                pParser->Destroy();
            }
            LocalFree(pszUIFile);
        }
    }

    DUI_WalkIUnknownElements(pePane, DUI_SetSiteOnUnknown, (LPARAM)_punkSite);
    DUI_WalkIUnknownElements(pePane, DUI_SendInitializationToUnknown, 0);
    return S_OK;
}

/* The library behind the item, unless the view is search only with no search text */
HRESULT
CLibraryPaneModuleInner::_GetLibraryFromItem(
    _In_ IShellItem *psi,
    _COM_Outptr_ IShellItem **ppsiLibrary)
{
    ILibraryDescription *pld;
    IShellFolder *psf;
    IShellItem *psiLibrary;
    UINT cLocations;
    BOOL fShow = FALSE;
    HRESULT hr;

    *ppsiLibrary = NULL;
    hr = psi->BindToHandler(NULL, BHID_SFObject, IID_PPV_ARGS(&psf));
    if (FAILED(hr))
        return hr;

    hr = GetRootLibraryFromItem(psi, &psiLibrary);
    if (SUCCEEDED(hr))
    {
        hr = psiLibrary->BindToHandler(NULL, BHID_SFObject, IID_PPV_ARGS(&pld));
        if (SUCCEEDED(hr))
        {
            if (SUCCEEDED(GetAccessibleLibraryLocationCount(pld, &cLocations)) && cLocations)
                fShow = (GetSearchOnlyPropFromLibrary(pld) != LIBRARY_SEARCH_ALL);

            hr = fShow ? S_OK : E_FAIL;
            if (SUCCEEDED(hr))
                IUnknown_Set((IUnknown **)ppsiLibrary, psiLibrary);
            pld->Release();
        }
        psiLibrary->Release();
    }
    psf->Release();
    return hr;
}

static
BOOL
IsSameItem(
    _In_ IShellItem *psi1,
    _In_ IShellItem *psi2)
{
    int iOrder;

    return psi1->Compare(psi2, SICHINT_CANONICAL, &iOrder) == S_OK;
}

/* The Libraries folder and the homegroup or one of its members get a hint instead of locations */
LIBRARYPANESPECIAL
CLibraryPaneModuleInner::_SpecialLibraryPane(
    _In_ IShellItem *psi)
{
    LIBRARYPANESPECIAL special = LPS_NONE;
    IShellItem *psiKnown;
    IShellItem *psiParent;

    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Libraries, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psiKnown))))
    {
        if (IsSameItem(psiKnown, psi))
            special = LPS_LIBRARIES;
        psiKnown->Release();
    }

    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_HomeGroup, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psiKnown))))
    {
        if (IsSameItem(psiKnown, psi))
        {
            special = LPS_HOMEGROUP;
        }
        else if (SUCCEEDED(psi->GetParent(&psiParent)))
        {
            if (IsSameItem(psiKnown, psiParent))
                special = LPS_HOMEGROUPMEMBER;
            psiParent->Release();
        }
        psiKnown->Release();
    }
    return special;
}

HRESULT
CLibraryPaneModuleInner::_GetDisplayTitle(
    _Outptr_ PWSTR *ppszTitle)
{
    PWSTR pszName;
    HRESULT hr;

    *ppszTitle = NULL;
    if (_special == LPS_LIBRARIES)
        return LoadResStringAlloc(IDS_LIBRARIES, ppszTitle);
    if (_special == LPS_HOMEGROUP || _special == LPS_HOMEGROUPMEMBER)
        return LoadResStringAlloc(IDS_HOMEGROUP, ppszTitle);

    hr = _psiLibrary->GetDisplayName(SIGDN_NORMALDISPLAY, &pszName);
    if (SUCCEEDED(hr))
    {
        hr = FormatResMessageAlloc(IDS_LIBRARY_TITLE, ppszTitle, pszName);
        CoTaskMemFree(pszName);
    }
    return hr;
}

/*
 * At the library root the button counts its locations; below it the line names the folder.
 * The button tooltip lists every location either way.
 */
HRESULT
CLibraryPaneModuleInner::_SetLocationInfo()
{
    ILibraryDescription *pld;
    ILocationDescription *ploc;
    DirectUI::Element *pe;
    IShellItem *psiCurrent;
    WCHAR szTip[1040];
    UINT cLocations;
    PWSTR pszText;
    PCWSTR pszTarget;
    HRESULT hr;

    hr = _psiLibrary->BindToHandler(NULL, BHID_SFObject, IID_PPV_ARGS(&pld));
    if (FAILED(hr))
        return hr;

    hr = GetAccessibleLibraryLocationCount(pld, &cLocations);
    if (SUCCEEDED(hr))
        hr = QueryCurrentItem(_punkSite, IID_PPV_ARGS(&psiCurrent));
    if (FAILED(hr))
    {
        pld->Release();
        return hr;
    }

    if (!IsSameItem(psiCurrent, _psiLibrary))
    {
        _ShowLocationButton(FALSE);
        hr = _GetSubText(psiCurrent, &pszText);
        pszTarget = L"Leaf";
    }
    else
    {
        _ShowLocationButton(TRUE);
        hr = FormatResMessageAlloc(cLocations == 1 ? IDS_LIBRARY_LOCATION : IDS_LIBRARY_LOCATIONS, &pszText,
                                   cLocations);
        pszTarget = L"LocationButton";
    }

    if (SUCCEEDED(hr))
    {
        pe = FindDescendent(StrToID(pszTarget));
        if (pe)
            pe->SetContentString(pszText);
        CoTaskMemFree(pszText);
    }
    psiCurrent->Release();

    if (SUCCEEDED(hr))
    {
        szTip[0] = UNICODE_NULL;
        for (UINT i = 0; i < cLocations; i++)
        {
            if (FAILED(pld->GetLocationAt(i, &ploc)))
                continue;

            if (SUCCEEDED(ploc->GetName(&pszText)))
            {
                WCHAR szName[LOCATION_NAME_MAX];
                HRESULT hrCopy = StringCchCopyW(szName, _countof(szName), pszText);

                if (i)
                    StringCchCatW(szTip, _countof(szTip), L"\n");
                StringCchCatW(szTip, _countof(szTip), szName);
                if (hrCopy == STRSAFE_E_INSUFFICIENT_BUFFER)
                    StringCchCatW(szTip, _countof(szTip), L"...");
                CoTaskMemFree(pszText);
            }
            ploc->Release();
        }

        pe = FindDescendent(StrToID(L"LocationButton"));
        if (pe)
            pe->SetAccDesc(szTip);
    }

    pld->Release();
    return hr;
}

/* Below the library root the line shows the folder's name; filtered and search views have none here */
HRESULT
CLibraryPaneModuleInner::_GetSubText(
    _In_ IShellItem *psi,
    _Outptr_ PWSTR *ppszText)
{
    HRESULT hr;

    *ppszText = NULL;
    hr = psi->GetDisplayName(SIGDN_PARENTRELATIVEFORUI, ppszText);
    if (FAILED(hr))
        hr = psi->GetDisplayName(SIGDN_NORMALDISPLAY, ppszText);
    return hr;
}

/* Either the "Includes: N locations" button or the plain location line */
void
CLibraryPaneModuleInner::_ShowLocationButton(
    _In_ BOOL fShow)
{
    DirectUI::Element *peLeaf = FindDescendent(StrToID(L"Leaf"));
    DirectUI::Element *peButton = FindDescendent(StrToID(L"LocationButton"));
    DirectUI::Element *peIncludes = FindDescendent(StrToID(L"LocationIncludes"));

    if (fShow)
    {
        if (peLeaf)
            peLeaf->SetWidth(0);
        if (peButton)
            peButton->RemoveLocalValue(DirectUI::Element::WidthProp);
        if (peIncludes)
            peIncludes->RemoveLocalValue(DirectUI::Element::WidthProp);
    }
    else
    {
        if (peLeaf)
            peLeaf->RemoveLocalValue(DirectUI::Element::WidthProp);
        if (peButton)
            peButton->SetWidth(0);
        if (peIncludes)
            peIncludes->SetWidth(0);
    }
}

/* The arrange-by commands live in the command folder, which this shell32 does not have yet */
HRESULT
CLibraryPaneModuleInner::_InitializeTopViewControl()
{
    return E_NOTIMPL;
}
