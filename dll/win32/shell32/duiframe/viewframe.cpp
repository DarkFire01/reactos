/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DirectUI view frame: hosts the shell view inside the folder layout markup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

static const WCHAR c_szDUIViewWndClass[] = L"DUIViewWndClassName";
static const WCHAR c_szModulesKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Modules";

/* GetPersistStream types; the sizer's FramePersistType names one */
#define FRAMEPERSIST_PERFOLDER      0
#define FRAMEPERSIST_PERLAYOUT      1
#define FRAMEPERSIST_GLOBAL         2
#define FRAMEPERSIST_PERFOLDERTYPE  3

/* Browser command the host forwards WM_CONTEXTMENU-less help requests to */
#define FCIDM_HELPCONTENTS          0x7091

/* A module element found in the markup, keyed by its ModuleID */
typedef struct _MODULEENTRY
{
    PWSTR pszModuleID;
    IUnknown *punkInner;
} MODULEENTRY, *PMODULEENTRY;

/* Writes go to the wrapped bag and then announce the property to the frame's elements */
class CCallbackPropertyBag : public IPropertyBag, public CDUIFrameZeroAlloc
{
public:
    typedef VOID (*PFNWRITECALLBACK)(void *pvOwner, PCWSTR pszPropName);

    CCallbackPropertyBag(
        _In_ IPropertyBag *ppbInner,
        _In_ void *pvOwner,
        _In_ PFNWRITECALLBACK pfnWrite) :
        _cRef(1), _pvOwner(pvOwner), _pfnWrite(pfnWrite), _ppbInner(ppbInner)
    {
        ppbInner->AddRef();
    }

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CCallbackPropertyBag, IPropertyBag),
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
        {
            IUnknown_SafeReleaseAndNullPtr(&_ppbInner);
            delete this;
        }
        return cRef;
    }

    STDMETHODIMP
    Read(
        LPCOLESTR pszPropName,
        VARIANT *pVar,
        IErrorLog *pErrorLog) override
    {
        return _ppbInner ? _ppbInner->Read(pszPropName, pVar, pErrorLog) : E_FAIL;
    }

    STDMETHODIMP
    Write(
        LPCOLESTR pszPropName,
        VARIANT *pVar) override
    {
        HRESULT hr = E_FAIL;

        if (_ppbInner)
        {
            hr = _ppbInner->Write(pszPropName, pVar);
            if (SUCCEEDED(hr))
                _pfnWrite(_pvOwner, pszPropName);
        }
        return hr;
    }

private:
    LONG _cRef;
    void *_pvOwner;
    PFNWRITECALLBACK _pfnWrite;
    IPropertyBag *_ppbInner;
};

static
HRESULT
CreateCallbackPropertyBag(
    _In_ IPropertyBag *ppbInner,
    _In_ void *pvOwner,
    _In_ CCallbackPropertyBag::PFNWRITECALLBACK pfnWrite,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CCallbackPropertyBag *pcpb;
    HRESULT hr;

    *ppv = NULL;
    pcpb = new CCallbackPropertyBag(ppbInner, pvOwner, pfnWrite);
    if (!pcpb)
        return E_OUTOFMEMORY;

    hr = pcpb->QueryInterface(riid, ppv);
    pcpb->Release();
    return hr;
}

class CDUIViewFrame :
    public CObjectWithSite,
    public IViewFrame,
    public IFrameManager,
    public IObjectProvider,
    public IDispatch,
    public IServiceProvider,
    public IPropertyBag,
    public IShellSearchTarget,
    public IOleWindow,
    public CDUIFrameZeroAlloc
{
public:
    CDUIViewFrame();

    // *** IUnknown ***
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // *** IViewFrame ***
    STDMETHODIMP IsLayoutSupported(IFrameLayoutDefinition *pfld) override;
    STDMETHODIMP SetLayoutDefinition(IFrameLayoutDefinition *pfld) override;
    STDMETHODIMP CreateFrameWindow(IViewFrameHost *pvfh, const RECT *prc, HWND *phwnd) override;
    STDMETHODIMP DestroyFrameWindow() override;
    STDMETHODIMP SetShellView(IShellView *psv, HWND hwndView, IUnknown *punkSite, SETVIEWFLAGS svf) override;
    STDMETHODIMP UIActivateVF(UINT uState, VIEWREASONTOACTIVATE vra) override;
    STDMETHODIMP TranslateAcceleratorVF(MSG *pmsg) override;
    STDMETHODIMP TranslateAcceleratorGlobalVF(MSG *pmsg) override;
    STDMETHODIMP SetMode(VFMODE mode) override;

    // *** IFrameManager ***
    STDMETHODIMP GetLayoutType(LAYOUTTYPE *pType) override;
    STDMETHODIMP GetShellView(IShellView **ppsv) override;
    STDMETHODIMP GetPersistStream(int iType, PCWSTR pszClass, PCWSTR pszID, UINT grfMode, IStream **ppstm) override;

    // *** IObjectProvider ***
    STDMETHODIMP QueryObject(REFGUID guidObject, REFIID riid, void **ppvOut) override;

    // *** IDispatch ***
    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) override;
    STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo) override;
    STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId) override;
    STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr) override;

    // *** IServiceProvider ***
    STDMETHODIMP QueryService(REFGUID guidService, REFIID riid, void **ppv) override;

    // *** IPropertyBag ***
    STDMETHODIMP Read(LPCOLESTR pszPropName, VARIANT *pVar, IErrorLog *pErrorLog) override;
    STDMETHODIMP Write(LPCOLESTR pszPropName, VARIANT *pVar) override;

    // *** IShellSearchTarget ***
    STDMETHODIMP Search(PCWSTR pszSearchText, DWORD dwCommand) override;
    STDMETHODIMP OnSearchTextNotify(PCWSTR pszSearchText, PCWSTR pszStrippedText, SHELLSEARCHNOTIFY sn) override;
    STDMETHODIMP GetSearchText(PWSTR pszSearchText, UINT cch) override;
    STDMETHODIMP GetPromptText(PWSTR pszPromptText, UINT cch) override;
    STDMETHODIMP GetACEnumString(IEnumString **ppEnumString) override;
    STDMETHODIMP GetMenu(HMENU *phmenu) override;
    STDMETHODIMP InitMenuPopup(HMENU hMenu) override;
    STDMETHODIMP OnMenuCommand(DWORD dwCmd) override;
    STDMETHODIMP Enter(IShellSearchScope *pSearchScope) override;
    STDMETHODIMP Exit() override;

    // *** IOleWindow ***
    STDMETHODIMP GetWindow(HWND *phwnd) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL fEnterMode) override;

private:
    ~CDUIViewFrame();

    HRESULT _CreateHostWindow(_In_ HWND hwndParent);
    HRESULT _AdjustLayout(_In_ IUnknown *punkFolderLayout, _COM_Outptr_ IUnknown **ppunkLayout, _Out_opt_ BOOL *pfNoChrome);
    HRESULT _GetUIFile(_In_ IUnknown *punkFolderLayout, _Outptr_ PWSTR *ppszUIFile, _Out_opt_ BOOL *pfNoChrome);
    DirectUI::Element *_GetListviewHostElement();
    void _UpdateInitialStates();
    HRESULT _UpdateNamespaceID();
    void _DisconnectShellView();
    void _DetachElementTree();
    void _ProfferToBrowser(_In_ BOOL fProffer);
    HRESULT _PerformPendingActivate(_In_ VIEWREASONTOACTIVATE vra);
    void _FixUpKeyboardFocus(_In_ VIEWREASONTOACTIVATE vra);
    void _SetFocusToFirstTabStopElement(_In_ DirectUI::Element *pe);
    void _FixupCrossDUIViewFocus();
    void _CheckForLayoutChangeOnContentsChange();
    void _ApplyLayoutDefinitionFor(_In_ GETLAYOUTREASON reason);
    HRESULT _GetRegistryPathForLocation(_In_ PCIDLIST_ABSOLUTE pidl, _In_ BOOL fCreate, _In_ PCWSTR pszRoot,
                                        _Out_writes_(MAX_PATH) PWSTR pszSubKey);
    HRESULT _GetPerFolderPersistStream(_In_ PCWSTR pszClass, _In_ PCWSTR pszID, _In_ PCWSTR pszRoot,
                                       _In_ UINT grfMode, _COM_Outptr_ IStream **ppstm);
    HRESULT _GetPerLayoutPersistStream(_In_ PCWSTR pszClass, _In_ PCWSTR pszID, _In_ PCWSTR pszRoot,
                                       _In_ UINT grfMode, _COM_Outptr_ IStream **ppstm);
    HRESULT _GetPerFolderTypeIdPersistStream(_In_ PCWSTR pszClass, _In_ PCWSTR pszID, _In_ PCWSTR pszRoot,
                                             _In_ UINT grfMode, _COM_Outptr_ IStream **ppstm);

    static VOID WINAPI s_CreateOrSetInnerObject(_In_ IUnknown *punk, _In_ LPARAM lParam);
    static VOID WINAPI s_RemoveInnerObject(_In_ IUnknown *punk, _In_ LPARAM lParam);
    static VOID WINAPI s_SeeIfUnknownWantsFocus(_In_ IUnknown *punk, _In_ LPARAM lParam);
    static INT CALLBACK s_ModuleEntryClearCB(_In_ void *pv, _In_opt_ void *pvData);
    static int CALLBACK s_CompareModuleEntry(_In_ void *pv1, _In_ void *pv2, _In_ LPARAM lParam);
    static int s_FindModule(_In_ HDSA hdsa, _In_ MODULEENTRY *pEntry);
    static VOID s_PropertyWriteCallback(_In_ void *pvOwner, _In_ PCWSTR pszPropName);
    static VOID WINAPI s_ParserErrorCallback(const wchar_t *pszError, const wchar_t *pszToken, int iLine, void *pv);
    static LRESULT CALLBACK s_DUIHostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    LONG _cRef;
    HWND _hwndHost;
    IViewFrameHost *_pvfh;
    IUnknown *_punkTaskManager;
    HRESULT _hrDUIInit;
    DirectUI::Element *_peRoot;
    IShellView *_psv;
    HWND _hwndView;
    CDUIView *_peListview;
    LAYOUTTYPE _layoutType;
    BOOL _fLayoutTypeKnown;
    CLSID _clsidFolder;
    CLSID _clsidFolderPrev;
    BOOL _fViewWasEmpty;
    UINT _uActivateState;
    BOOL _fActivatePending;
    IDispatch *_pdispViewEvents;
    DWORD _dwViewEventsCookie;
    HDSA _hdsaModules;
    HDSA _hdsaModulesPrev;
    IShellSearchTarget *_psstView;
    ILayoutDefinitionRegistration *_pldr;
    IPropertyBag *_ppbLayout;
    ULONG _uLayoutHash;
    DWORD _dwProfferCookieFrameManager;
    DWORD _dwProfferCookieLayoutBag;
    BOOL _fProffered;
    BOOL _fContentsChangedPending;
    BOOL _fFolderChangedPending;
    BOOL _fShellViewAttached;
    BOOL _fContentsChangedSent;
    IUnknown *_punkPendingSite;
    DWORD _dwMode;
    BOOL _fInSetLayoutDefinition;
};

/* Identifies a markup string so an unchanged layout is not rebuilt */
static
ULONG
HashMarkup(
    _In_ PCWSTR psz)
{
    ULONG uHash = 2166136261u;

    for (; *psz; psz++)
        uHash = (uHash ^ *psz) * 16777619u;
    return uHash ? uHash : 1;
}

CDUIViewFrame::CDUIViewFrame() :
    _cRef(1),
    _hrDUIInit(E_FAIL),
    _layoutType(LAYOUTTYPE_NORMAL)
{
}

CDUIViewFrame::~CDUIViewFrame()
{
    IUnknown_SafeReleaseAndNullPtr(&_ppbLayout);
    IUnknown_SafeReleaseAndNullPtr(&_punkTaskManager);
}

HRESULT
WINAPI
DUIViewFrame_CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CDUIViewFrame *pdvf;
    HRESULT hr;

    *ppv = NULL;
    pdvf = new CDUIViewFrame();
    if (!pdvf)
        return E_OUTOFMEMORY;

    hr = pdvf->QueryInterface(riid, ppv);
    pdvf->Release();
    return hr;
}

STDMETHODIMP
CDUIViewFrame::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CDUIViewFrame, IViewFrame),
        QITABENT(CDUIViewFrame, IFrameManager),
        QITABENT(CDUIViewFrame, IObjectProvider),
        QITABENT(CDUIViewFrame, IDispatch),
        QITABENT(CDUIViewFrame, IServiceProvider),
        QITABENT(CDUIViewFrame, IPropertyBag),
        QITABENT(CDUIViewFrame, IShellSearchTarget),
        QITABENT(CDUIViewFrame, IOleWindow),
        QITABENT(CDUIViewFrame, IObjectWithSite),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CDUIViewFrame::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CDUIViewFrame::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* Frame host window */

HRESULT
CDUIViewFrame::_CreateHostWindow(
    _In_ HWND hwndParent)
{
    WNDCLASSW wc = { 0 };
    RECT rc;

    wc.hInstance = DUIFRAME_HINST;
    wc.lpfnWndProc = s_DUIHostWndProc;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = c_szDUIViewWndClass;
    RegisterClassW(&wc);

    GetClientRect(hwndParent, &rc);
    _hwndHost = SHFusionCreateWindowEx(WS_EX_CONTROLPARENT, c_szDUIViewWndClass, NULL,
                                       WS_CHILD | WS_CLIPCHILDREN,
                                       rc.left, rc.top, rc.right, rc.bottom,
                                       hwndParent, NULL, DUIFRAME_HINST, this);
    return _hwndHost ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

LRESULT
CALLBACK
CDUIViewFrame::s_DUIHostWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    CDUIViewFrame *pThis = (CDUIViewFrame *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lParam)->lpCreateParams);
            break;

        case WM_SIZE:
            if (pThis && pThis->_peRoot)
            {
                ULONG dwDeferCookie = 0;

                DisableAnimations();
                pThis->_peRoot->StartDefer(&dwDeferCookie);
                pThis->_peRoot->SetWidth(LOWORD(lParam));
                pThis->_peRoot->SetHeight(HIWORD(lParam));
                if (dwDeferCookie)
                    pThis->_peRoot->EndDefer(dwDeferCookie);
                EnableAnimations();
            }
            break;

        case WM_SETFOCUS:
            if (pThis && pThis->_peRoot)
            {
                HWND hwndRoot = static_cast<CHWNDView *>(pThis->_peRoot)->GetHWND();

                if (hwndRoot)
                    SetFocus(hwndRoot);
            }
            break;

        case WM_SETREDRAW:
            if (wParam)
            {
                LRESULT lRes = DefWindowProcW(hwnd, uMsg, wParam, lParam);

                RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
                return lRes;
            }
            break;

        case WM_HELP:
            if (pThis)
            {
                IShellBrowser *psb;
                HWND hwndBrowser;

                if (SUCCEEDED(IUnknown_QueryService(pThis->_pvfh, SID_STopLevelBrowser, IID_PPV_ARGS(&psb))))
                {
                    if (SUCCEEDED(psb->GetWindow(&hwndBrowser)))
                        PostMessageW(hwndBrowser, WM_COMMAND, FCIDM_HELPCONTENTS, 0);
                    psb->Release();
                }
            }
            break;

        case WM_DISPLAYCHANGE:
        case WM_QUERYNEWPALETTE:
        case WM_PALETTECHANGED:
            /* The root element's window does the drawing */
            if (pThis && pThis->_peRoot)
                return SendMessageW(static_cast<CHWNDView *>(pThis->_peRoot)->GetHWND(), uMsg, wParam, lParam);
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

STDMETHODIMP
CDUIViewFrame::CreateFrameWindow(
    IViewFrameHost *pvfh,
    const RECT *prc,
    HWND *phwnd)
{
    HWND hwndParent;
    ULONG dwDeferCookie;
    RECT rc;
    HRESULT hr;

    hr = InitializeDirectUI();
    if (SUCCEEDED(hr))
    {
        hr = DUI_InitThread();
        if (FAILED(hr))
            UnInitializeDirectUI();
    }
    _hrDUIInit = hr;

    if (SUCCEEDED(hr))
    {
        _hdsaModules = DSA_Create(sizeof(MODULEENTRY), 1);
        hr = _hdsaModules ? S_OK : E_FAIL;
    }

    if (SUCCEEDED(hr))
    {
        IUnknown_Set((IUnknown **)&_pvfh, pvfh);
        hr = IUnknown_GetWindow(_pvfh, &hwndParent);
        if (SUCCEEDED(hr))
            hr = _CreateHostWindow(hwndParent);
        if (SUCCEEDED(hr))
            hr = CHWNDView::Create(_hwndHost, NULL, &dwDeferCookie, &_peRoot);
    }

    if (FAILED(hr))
    {
        DestroyFrameWindow();
        return hr;
    }

    GetClientRect(_hwndHost, &rc);
    _peRoot->SetWidth(rc.right - rc.left);
    _peRoot->SetHeight(rc.bottom - rc.top);
    _peRoot->EndDefer(dwDeferCookie);
    _peRoot->SetVisible(true);
    *phwnd = _hwndHost;
    return S_OK;
}

STDMETHODIMP
CDUIViewFrame::DestroyFrameWindow()
{
    _DetachElementTree();
    if (_hdsaModules)
    {
        DSA_DestroyCallback(_hdsaModules, s_ModuleEntryClearCB, NULL);
        _hdsaModules = NULL;
    }
    _DisconnectShellView();
    _ProfferToBrowser(FALSE);

    IUnknown_SafeReleaseAndNullPtr(&_psv);
    IUnknown_SafeReleaseAndNullPtr(&_pvfh);
    IUnknown_SafeReleaseAndNullPtr(&_ppbLayout);
    IUnknown_SafeReleaseAndNullPtr(&_punkPendingSite);

    if (_hwndHost)
    {
        DestroyWindow(_hwndHost);
        _hwndHost = NULL;
    }
    _peRoot = NULL;

    if (_pldr)
    {
        _pldr->UnRegister();
        IUnknown_SafeReleaseAndNullPtr(&_pldr);
    }

    if (SUCCEEDED(_hrDUIInit))
    {
        DUI_UninitThread();
        UnInitializeDirectUI();
        _hrDUIInit = E_FAIL;
    }

    IUnknown_SetSite(_punkTaskManager, NULL);
    if (_hwndView)
    {
        /* The browser places the view again once it owns it */
        SetWindowPos(_hwndView, NULL, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
        _hwndView = NULL;
    }
    return S_OK;
}

/* Layout */

/* Lets the browser rewrite the folder layout, falling back to the common chrome */
HRESULT
CDUIViewFrame::_AdjustLayout(
    _In_ IUnknown *punkFolderLayout,
    _COM_Outptr_ IUnknown **ppunkLayout,
    _Out_opt_ BOOL *pfNoChrome)
{
    ILayoutModifier *plm = NULL;
    ICommonLayoutModifier *pclm;
    LAYOUTTYPE layoutType = LAYOUTTYPE_NORMAL;
    ILayoutType *plt;
    BOOL fNoChrome;
    HRESULT hr;

    *ppunkLayout = NULL;
    if (SUCCEEDED(punkFolderLayout->QueryInterface(IID_PPV_ARGS(&plt))))
    {
        plt->GetLayoutType(&layoutType);
        plt->Release();
    }

    /* Control panel and the barrier pages have no browser chrome around them */
    fNoChrome = (layoutType == LAYOUTTYPE_CONTROLPANEL ||
                 (layoutType > LAYOUTTYPE_CONTACTS && layoutType <= LAYOUTTYPE_SYNCCENTER));
    if (pfNoChrome)
        *pfNoChrome = fNoChrome;

    hr = IUnknown_QueryService(_pvfh, IID_ILayoutModifier, IID_PPV_ARGS(&plm));
    if (FAILED(hr))
    {
        hr = CommonLayoutModifier_CreateInstance(IID_PPV_ARGS(&pclm));
        if (FAILED(hr))
            return hr;

        hr = pclm->ChooseBrowserLayout(fNoChrome ? BROWSERLAYOUTTYPE_NONE : BROWSERLAYOUTTYPE_NORMAL);
        if (SUCCEEDED(hr))
            hr = pclm->QueryInterface(IID_PPV_ARGS(&plm));
        pclm->Release();
    }
    else if (SUCCEEDED(plm->QueryInterface(IID_PPV_ARGS(&pclm))))
    {
        pclm->ChooseBrowserLayout(fNoChrome ? BROWSERLAYOUTTYPE_NONE : BROWSERLAYOUTTYPE_NORMAL);
        pclm->Release();
    }

    if (SUCCEEDED(hr))
    {
        hr = plm->AdjustLayout(punkFolderLayout, ppunkLayout);
        plm->Release();
    }
    return hr;
}

HRESULT
CDUIViewFrame::_GetUIFile(
    _In_ IUnknown *punkFolderLayout,
    _Outptr_ PWSTR *ppszUIFile,
    _Out_opt_ BOOL *pfNoChrome)
{
    IUnknown *punkLayout;
    IStream *pstm;
    STATSTG stat;
    HRESULT hr;

    *ppszUIFile = NULL;
    hr = _AdjustLayout(punkFolderLayout, &punkLayout, pfNoChrome);
    if (FAILED(hr))
        return hr;

    hr = punkLayout->QueryInterface(IID_PPV_ARGS(&pstm));
    if (SUCCEEDED(hr))
    {
        ZeroMemory(&stat, sizeof(stat));
        hr = pstm->Stat(&stat, STATFLAG_NONAME);
        if (SUCCEEDED(hr))
        {
            UINT cch = stat.cbSize.LowPart / sizeof(WCHAR);

            *ppszUIFile = (PWSTR)CoTaskMemAlloc((cch + 1) * sizeof(WCHAR));
            if (*ppszUIFile)
            {
                ZeroMemory(*ppszUIFile, (cch + 1) * sizeof(WCHAR));
                hr = IStream_Read(pstm, *ppszUIFile, cch * sizeof(WCHAR));
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }
        }
        pstm->Release();
    }
    punkLayout->Release();
    return hr;
}

STDMETHODIMP
CDUIViewFrame::IsLayoutSupported(
    IFrameLayoutDefinition *pfld)
{
    IUnknown *punkFolderLayout;
    IUnknown *punkLayout;
    HRESULT hr;

    hr = pfld->GetLayoutDefinition(&punkFolderLayout);
    if (SUCCEEDED(hr))
    {
        hr = _AdjustLayout(punkFolderLayout, &punkLayout, NULL);
        if (SUCCEEDED(hr))
            punkLayout->Release();
        punkFolderLayout->Release();
    }
    return hr;
}

/* The element that receives the view: "clientviewhost" when the markup has one */
DirectUI::Element *
CDUIViewFrame::_GetListviewHostElement()
{
    DirectUI::Element *peHost = NULL;

    if (FindAtomW(L"clientviewhost"))
        peHost = _peRoot->FindDescendent(StrToID(L"clientviewhost"));

    if (!peHost && !_peRoot->GetClassInfoW()->IsSubclassOf(DirectUI::HWNDElement::GetClassInfoPtr()))
        peHost = _peRoot;
    return peHost;
}

/* Pairs modules of the new markup with the inner objects the old markup already created */
VOID
WINAPI
CDUIViewFrame::s_CreateOrSetInnerObject(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    CDUIViewFrame *pThis = (CDUIViewFrame *)lParam;
    MODULEENTRY entry = { NULL, NULL };
    IFrameModule *pfm;
    int iFound;

    if (FAILED(punk->QueryInterface(IID_PPV_ARGS(&pfm))))
        return;

    if (SUCCEEDED(pfm->GetModuleID(&entry.pszModuleID)))
    {
        iFound = pThis->_hdsaModulesPrev ?
                 s_FindModule(pThis->_hdsaModulesPrev, &entry) : -1;
        if (iFound >= 0)
        {
            PMODULEENTRY pPrev = (PMODULEENTRY)DSA_GetItemPtr(pThis->_hdsaModulesPrev, iFound);

            pfm->SetInnerObject(pPrev->punkInner);
            DSA_AppendItem(pThis->_hdsaModules, pPrev);
            DSA_DeleteItem(pThis->_hdsaModulesPrev, iFound);
        }
        else if (s_FindModule(pThis->_hdsaModules, &entry) < 0)
        {
            IUnknown *punkInner;

            if (SUCCEEDED(pfm->CreateInnerObject(&punkInner)))
            {
                MODULEENTRY newEntry = { NULL, NULL };

                pfm->SetInnerObject(punkInner);
                IUnknown_Set(&newEntry.punkInner, punkInner);
                if (SUCCEEDED(SHStrDupW(entry.pszModuleID, &newEntry.pszModuleID)) &&
                    DSA_AppendItem(pThis->_hdsaModules, &newEntry) != -1)
                {
                    newEntry.pszModuleID = NULL;
                    newEntry.punkInner = NULL;
                }
                s_ModuleEntryClearCB(&newEntry, NULL);
                punkInner->Release();
            }
        }
        s_ModuleEntryClearCB(&entry, NULL);
    }
    pfm->Release();
}

VOID
WINAPI
CDUIViewFrame::s_RemoveInnerObject(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameModule *pfm;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pfm))))
    {
        pfm->SetInnerObject(NULL);
        pfm->Release();
    }
}

int
CALLBACK
CDUIViewFrame::s_CompareModuleEntry(
    _In_ void *pv1,
    _In_ void *pv2,
    _In_ LPARAM lParam)
{
    PCWSTR psz1 = ((PMODULEENTRY)pv1)->pszModuleID;
    PCWSTR psz2 = ((PMODULEENTRY)pv2)->pszModuleID;

    /* Modules without an ID never match each other */
    if (!psz1 || !*psz1 || !psz2 || !*psz2)
        return 1;
    return StrCmpICW(psz1, psz2);
}

int
CDUIViewFrame::s_FindModule(
    _In_ HDSA hdsa,
    _In_ MODULEENTRY *pEntry)
{
    int cItems = DSA_GetItemCount(hdsa);

    for (int i = 0; i < cItems; i++)
    {
        if (s_CompareModuleEntry(DSA_GetItemPtr(hdsa, i), pEntry, 0) == 0)
            return i;
    }
    return -1;
}

/* Tells a module that is going away, then drops its inner object */
INT
CALLBACK
CDUIViewFrame::s_ModuleEntryClearCB(
    _In_ void *pv,
    _In_opt_ void *pvData)
{
    PMODULEENTRY pEntry = (PMODULEENTRY)pv;
    IElementWrapper *pew;

    if (pEntry->punkInner && SUCCEEDED(pEntry->punkInner->QueryInterface(IID_PPV_ARGS(&pew))))
    {
        DirectUI::Element *peInner;
        IUnknown *punkElement;
        IFrameNotificationClient *pfnc;

        if (SUCCEEDED(pew->GetElement(&peInner)) &&
            (punkElement = CElementWithIUnknown::GetUnknownFromElement(peInner)) != NULL &&
            SUCCEEDED(punkElement->QueryInterface(IID_PPV_ARGS(&pfnc))))
        {
            pfnc->LayoutUninitialized();
            pfnc->Release();
        }
        pew->Release();
    }

    IUnknown_SafeReleaseAndNullPtr(&pEntry->punkInner);
    CoTaskMemFree(pEntry->pszModuleID);
    pEntry->pszModuleID = NULL;
    return TRUE;
}

VOID
WINAPI
CDUIViewFrame::s_ParserErrorCallback(
    const wchar_t *pszError,
    const wchar_t *pszToken,
    int iLine,
    void *pv)
{
    /* A layout without a ViewLayout resource is normal; nothing else is reported either */
}

VOID
CDUIViewFrame::s_PropertyWriteCallback(
    _In_ void *pvOwner,
    _In_ PCWSTR pszPropName)
{
    CDUIViewFrame *pThis = (CDUIViewFrame *)pvOwner;

    DUI_WalkIUnknownElements(pThis->_peRoot, DUI_SendNotificationToUnknown, (LPARAM)pszPropName);
}

/* Remembers whether the view starts hidden and the last search, for the layout's elements to read */
void
CDUIViewFrame::_UpdateInitialStates()
{
    IPropertyBag *ppb;
    WCHAR szSearch[MAX_PATH];
    BOOL fHidden;

    if (FAILED(QueryService(SID_PerLayoutPropertyBag, IID_PPV_ARGS(&ppb))))
        return;

    if (FAILED(SHPropertyBag_ReadBOOL(ppb, L"ListViewHiddenState", &fHidden)))
    {
        DirectUI::Element *peHost = _GetListviewHostElement();

        SHPropertyBag_WriteBOOL(ppb, L"ListViewHiddenState", peHost ? peHost->GetLayoutPos() == ROSUI_LP_NONE : TRUE);
    }

    if (SUCCEEDED(GetSearchText(szSearch, _countof(szSearch))))
        SHPropertyBag_WriteStr(ppb, L"SearchText", szSearch);
    ppb->Release();
}

STDMETHODIMP
CDUIViewFrame::SetLayoutDefinition(
    IFrameLayoutDefinition *pfld)
{
    IUnknown *punkFolderLayout;
    PWSTR pszUIFile;
    BOOL fNoChrome = FALSE;
    BOOL fAnimationsOn = FALSE;
    BOOL fInitializeElements = FALSE;
    ULONG dwDeferCookie = 0;
    HRESULT hr;

    if (_fInSetLayoutDefinition)
        return E_FAIL;
    _fInSetLayoutDefinition = TRUE;

    DisableAnimations();
    SendMessageW(_hwndHost, WM_SETREDRAW, FALSE, 0);
    _peRoot->StartDefer(&dwDeferCookie);

    hr = pfld->GetLayoutDefinition(&punkFolderLayout);
    if (SUCCEEDED(hr))
    {
        hr = _GetUIFile(punkFolderLayout, &pszUIFile, &fNoChrome);
        if (SUCCEEDED(hr))
        {
            ULONG uHash = HashMarkup(pszUIFile);

            if (uHash == _uLayoutHash || (_dwMode & VFM_SUPPRESSTEMPLATECHANGES))
            {
                /* Same markup: the tree stays, only a waiting view moves in */
                if (_psv && !_fShellViewAttached)
                {
                    fInitializeElements = TRUE;
                    SetShellView(_psv, _hwndView, _punkPendingSite ? _punkPendingSite : _punkSite, SVF_NORMAL);
                }
                _fShellViewAttached = TRUE;
                _UpdateInitialStates();
            }
            else
            {
                ILayoutDefinitionRegistration *pldrNew = NULL;
                DirectUI::DUIXmlParser *pParser;

                _uLayoutHash = uHash;
                fInitializeElements = TRUE;
                if (SUCCEEDED(pfld->QueryInterface(IID_PPV_ARGS(&pldrNew))))
                    pldrNew->Register();

                hr = DUI_CreateParser(pszUIFile, &pParser, static_cast<IViewFrame *>(this), s_ParserErrorCallback);
                if (SUCCEEDED(hr))
                {
                    HWND hwndFocus = GetFocus();
                    BOOL fHadFocus = IsChild(_hwndHost, hwndFocus);
                    DirectUI::Element *peCreated;
                    ILayoutType *plt;

                    _DetachElementTree();
                    _peRoot->DestroyAll(true);
                    _peListview = NULL;
                    IUnknown_SafeReleaseAndNullPtr(&_ppbLayout);

                    /* Modules of the old tree wait in the previous list until the new tree claims them */
                    _hdsaModulesPrev = _hdsaModules;
                    _hdsaModules = DSA_Create(sizeof(MODULEENTRY), 1);
                    hr = _hdsaModules ? S_OK : E_FAIL;
                    if (SUCCEEDED(hr))
                        hr = pParser->CreateElement(L"main", _peRoot, NULL, NULL, &peCreated);

                    if (SUCCEEDED(hr))
                    {
                        DirectUI::Element *peFolderHost = _peRoot->FindDescendent(StrToID(L"FolderLayoutContainer"));
                        HRESULT hrFolder;

                        if (peFolderHost)
                        {
                            hrFolder = pParser->CreateElement(L"FolderLayout", peFolderHost, NULL, NULL, &peCreated);
                        }
                        else
                        {
                            peFolderHost = _peRoot->FindDescendent(StrToID(L"FolderLayoutContainerForce"));
                            hrFolder = peFolderHost ? S_OK : E_FAIL;
                        }

                        if (SUCCEEDED(hrFolder))
                        {
                            DirectUI::Element *peViewHost;

                            peFolderHost->SetID(L"FolderLayoutContainer");
                            peViewHost = _GetListviewHostElement();
                            if (peViewHost &&
                                SUCCEEDED(pParser->CreateElement(L"ViewLayout", peViewHost, NULL, NULL, &peCreated)))
                            {
                                peViewHost->SetID(L"ViewLayoutContainer");
                            }

                            DUI_WalkIUnknownElements(peFolderHost, s_CreateOrSetInnerObject, (LPARAM)this);
                            DUI_WalkIUnknownElements(_peRoot, s_CreateOrSetInnerObject, (LPARAM)this);
                        }
                    }

                    if (_hdsaModulesPrev)
                    {
                        DSA_DestroyCallback(_hdsaModulesPrev, s_ModuleEntryClearCB, NULL);
                        _hdsaModulesPrev = NULL;
                    }

                    _layoutType = LAYOUTTYPE_NORMAL;
                    if (SUCCEEDED(punkFolderLayout->QueryInterface(IID_PPV_ARGS(&plt))))
                    {
                        plt->GetLayoutType(&_layoutType);
                        plt->Release();
                    }
                    _fLayoutTypeKnown = TRUE;

                    _ProfferToBrowser(TRUE);
                    _UpdateInitialStates();
                    if (_psv)
                        SetShellView(_psv, _hwndView, _punkPendingSite ? _punkPendingSite : _punkSite, SVF_NORMAL);
                    _fShellViewAttached = TRUE;

                    if (fHadFocus && IsChild(_hwndHost, hwndFocus))
                        SetFocus(hwndFocus);
                    pParser->Destroy();
                }

                /* The previous definition stays registered until the new one has replaced it */
                if (_pldr)
                    _pldr->UnRegister();
                IUnknown_Set((IUnknown **)&_pldr, pldrNew);
                IUnknown_SafeReleaseAndNullPtr(&pldrNew);
            }

            /* Pages without browser chrome animate their layout in */
            if (fNoChrome)
            {
                EnableAnimations();
                fAnimationsOn = TRUE;
            }

            if (fInitializeElements)
            {
                ITrackEvents *pte = NULL;

                IUnknown_QueryService(_punkPendingSite ? _punkPendingSite : _punkSite, IID_ITrackEvents,
                                      IID_PPV_ARGS(&pte));
                if (pte)
                    pte->OperationEvent(L"InitializeFrameElements", (TRACKOPERATIONTYPE)0, TOE_START);
                DUI_WalkIUnknownElements(_peRoot, DUI_SendInitializationToUnknown, 0);
                if (pte)
                {
                    pte->OperationEvent(L"InitializeFrameElements", (TRACKOPERATIONTYPE)0, TOE_END);
                    pte->Release();
                }
            }

            if (_fActivatePending)
                _PerformPendingActivate(VRA_NORMAL);
            CoTaskMemFree(pszUIFile);
        }
        punkFolderLayout->Release();
    }

    /* View events that came in while the layout was changing are delivered now */
    if (_fContentsChangedPending)
    {
        DUI_WalkIUnknownElements(_peRoot, DUI_SendContentsChangedToUnknown, 0);
        _fContentsChangedSent = TRUE;
    }
    else if (_fFolderChangedPending)
    {
        DUI_WalkIUnknownElements(_peRoot, DUI_SendFolderChangedToUnknown, 0);
    }
    _fContentsChangedPending = FALSE;
    _fFolderChangedPending = FALSE;

    if (dwDeferCookie)
        _peRoot->EndDefer(dwDeferCookie);
    SendMessageW(_hwndHost, WM_SETREDRAW, TRUE, 0);
    if (!fAnimationsOn)
        EnableAnimations();

    _fInSetLayoutDefinition = FALSE;
    return hr;
}

/* Shell view */

HRESULT
CDUIViewFrame::_UpdateNamespaceID()
{
    IFolderView *pfv;
    IPersist *ppersist;
    HRESULT hr;

    hr = _psv->QueryInterface(IID_PPV_ARGS(&pfv));
    if (FAILED(hr))
        return hr;

    hr = pfv->GetFolder(IID_IShellFolder, (void **)&ppersist);
    if (SUCCEEDED(hr))
    {
        CLSID clsid = GUID_NULL;

        _clsidFolderPrev = _clsidFolder;
        if (FAILED(IUnknown_GetClassID(ppersist, &clsid)) || IsEqualGUID(clsid, GUID_NULL) ||
            !IsEqualGUID(clsid, _clsidFolder))
        {
            /* A different namespace: forget whether the old view was empty */
            _clsidFolder = GUID_NULL;
            IUnknown_GetClassID(ppersist, &_clsidFolder);
            _fViewWasEmpty = FALSE;
        }
        ppersist->Release();
    }
    pfv->Release();
    return hr;
}

void
CDUIViewFrame::_DisconnectShellView()
{
    if (_pdispViewEvents)
    {
        ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, FALSE,
                                 _pdispViewEvents, &_dwViewEventsCookie, NULL);
        IUnknown_SafeReleaseAndNullPtr(&_pdispViewEvents);
    }
    IUnknown_SafeReleaseAndNullPtr(&_psstView);
}

STDMETHODIMP
CDUIViewFrame::SetShellView(
    IShellView *psv,
    HWND hwndView,
    IUnknown *punkSite,
    SETVIEWFLAGS svf)
{
    ULONG dwDeferCookie = 0;
    HRESULT hr = S_OK;

    DisableAnimations();
    _peRoot->StartDefer(&dwDeferCookie);

    /* The same view already sitting in the tree needs nothing */
    if (_psv == psv && _peListview && _fShellViewAttached)
        goto Done;

    if (_psv != psv)
    {
        IViewStatus *pvs;

        _fContentsChangedPending = FALSE;
        _fFolderChangedPending = FALSE;
        _fShellViewAttached = FALSE;

        /* A view that is done filling will not report its contents again; tell the elements ourselves */
        if (SUCCEEDED(psv->QueryInterface(IID_IViewStatus, (void **)&pvs)))
        {
            DEFVIEWSTATUS dvs;

            if (SUCCEEDED(pvs->GetStatus(&dvs)) && !(dvs & DVS_CONTENTSCHANGEPENDING))
                _fContentsChangedPending = TRUE;
            pvs->Release();
        }
        else
        {
            IUnknown *punkDefView;

            if (SUCCEEDED(IUnknown_QueryService(psv, SID_DefViewFrame, IID_PPV_ARGS(&punkDefView))))
                punkDefView->Release();
            else
                _fContentsChangedPending = TRUE;
        }
        DUI_WalkIUnknownElements(_peRoot, DUI_SendNavigateAwayToUnknown, 0);
    }

    _DisconnectShellView();
    _hwndView = hwndView;
    IUnknown_Set((IUnknown **)&_psv, psv);

    if (SUCCEEDED(_psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&_pdispViewEvents))))
    {
        ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, TRUE,
                                 _pdispViewEvents, &_dwViewEventsCookie, NULL);
    }
    IUnknown_QueryService(_psv, IID_IFolderView, IID_PPV_ARGS(&_psstView));
    _UpdateNamespaceID();

    /* Before any markup, or when the caller defers the layout update, the site waits for SetLayoutDefinition */
    if (!_uLayoutHash || (_fShellViewAttached && _peListview) || (svf & SVF_SKIPLAYOUTUPDATE))
    {
        IUnknown_Set(&_punkPendingSite, punkSite);
    }
    else
    {
        DirectUI::Element *peHost;

        DUI_WalkIUnknownElements(_peRoot, DUI_SetSiteOnUnknown, 0);
        SetSite(punkSite);
        IUnknown_SafeReleaseAndNullPtr(&_punkPendingSite);
        DUI_WalkIUnknownElements(_peRoot, DUI_SetSiteOnUnknown, (LPARAM)static_cast<IFrameManager *>(this));

        SetWindowPos(hwndView, NULL, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);

        peHost = _GetListviewHostElement();
        if (peHost)
        {
            DirectUI::Element *peView;
            IFolderView2 *pfv2;

            if (_peListview)
            {
                _peListview->Destroy(true);
                _peListview = NULL;
            }

            hr = CDUIView::Create(hwndView, peHost, NULL, &peView);
            if (SUCCEEDED(hr))
            {
                _peListview = static_cast<CDUIView *>(peView);
                _peListview->SetLayoutPos(ROSUI_LP_CLIENT);
                _peListview->SetID(L"listview");
                hr = peHost->Add(_peListview);
            }

            if (SUCCEEDED(_psv->QueryInterface(IID_PPV_ARGS(&pfv2))))
            {
                DUI_ApplyLayoutStateToView(pfv2, peHost);
                pfv2->Release();
            }
        }
        _fShellViewAttached = TRUE;
    }

    SetWindowTextW(hwndView, L"ShellView");

Done:
    if (dwDeferCookie)
        _peRoot->EndDefer(dwDeferCookie);
    EnableAnimations();
    return hr;
}

/* Activation and focus */

STDMETHODIMP
CDUIViewFrame::UIActivateVF(
    UINT uState,
    VIEWREASONTOACTIVATE vra)
{
    if (!_psv)
        return E_FAIL;

    _uActivateState = uState;
    _fActivatePending = TRUE;
    if (_fLayoutTypeKnown && _fShellViewAttached)
        return _PerformPendingActivate(vra);
    return S_OK;
}

HRESULT
CDUIViewFrame::_PerformPendingActivate(
    _In_ VIEWREASONTOACTIVATE vra)
{
    IDelayedVisibility *pdv = NULL;
    UINT uState;
    BOOL fFocus;
    HWND hwndFocus;
    HRESULT hr;

    if (!_fActivatePending)
        return S_OK;

    uState = _uActivateState;
    fFocus = (uState == SVUIA_ACTIVATE_FOCUS);
    hwndFocus = GetFocus();

    /* Focus already in another part of the frame stays there */
    if ((fFocus && IsChild(_hwndHost, hwndFocus) && !IsChild(_hwndView, hwndFocus)) || !_peListview)
        uState = SVUIA_ACTIVATE_NOFOCUS;

    _fActivatePending = FALSE;
    hr = _psv ? _psv->UIActivate(uState) : E_FAIL;

    if (_peListview)
        SetWindowPos(_hwndView, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);

    if (fFocus && SUCCEEDED(hr))
    {
        _FixUpKeyboardFocus(vra);

        if (SUCCEEDED(IUnknown_QueryService(_pvfh, SID_SDelayedVisibility, IID_PPV_ARGS(&pdv))) &&
            pdv->IsVisible() != S_OK)
        {
            if (!_peListview)
                pdv->MakeVisible();
        }
        else
        {
            UpdateWindow(_hwndHost);
        }
        IUnknown_SafeReleaseAndNullPtr(&pdv);
    }
    return hr;
}

VOID
WINAPI
CDUIViewFrame::s_SeeIfUnknownWantsFocus(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    IFrameFocusHost *pffh;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pffh))))
    {
        BOOL fWants;

        if (SUCCEEDED(pffh->WantsInitialFocus(&fWants)) && fWants)
            *(BOOL *)lParam = TRUE;
        pffh->Release();
    }
}

void
CDUIViewFrame::_FixUpKeyboardFocus(
    _In_ VIEWREASONTOACTIVATE vra)
{
    if (vra != VRA_NORMAL)
    {
        DirectUI::NavReference nr;
        DirectUI::Element *peNext;

        /* Tabbing in lands on the first stop in that direction */
        nr.Init(_peRoot, NULL);
        peNext = _peRoot->GetAdjacent(NULL, (vra == VRA_TAB) ? 3 : 1, &nr, 1);
        if (peNext)
            peNext->SetKeyFocus();
    }
    else
    {
        BOOL fElementWantsFocus = FALSE;

        DUI_WalkIUnknownElements(_peRoot, s_SeeIfUnknownWantsFocus, (LPARAM)&fElementWantsFocus);
        if (!fElementWantsFocus)
            _SetFocusToFirstTabStopElement(_peListview ? static_cast<DirectUI::Element *>(_peListview) : _peRoot);
    }
}

void
CDUIViewFrame::_SetFocusToFirstTabStopElement(
    _In_ DirectUI::Element *pe)
{
    if (pe == _peListview)
    {
        pe->SetKeyFocus();
    }
    else
    {
        DirectUI::NavReference nr;
        DirectUI::Element *peNext;

        nr.Init(pe, NULL);
        peNext = pe->GetAdjacent(NULL, 3, &nr, 3);
        if (peNext)
            peNext->SetKeyFocus();
        else
            SetFocus(static_cast<CHWNDView *>(_peRoot)->GetHWND());
    }
    _FixupCrossDUIViewFocus();
}

/* Focus that landed on the view element goes on into the list control's first item */
void
CDUIViewFrame::_FixupCrossDUIViewFocus()
{
    DirectUI::Element *peFocus = DirectUI::HWNDElement::GetKeyFocusedElement();
    IListControl *plc = NULL;

    if (!peFocus || !peFocus->GetClassInfoW()->IsSubclassOf(CDUIView::Class))
        return;

    if (SUCCEEDED(IUnknown_QueryObject(_psv, OID_ListControl, IID_PPV_ARGS(&plc))))
        plc->SetInnerFocus(TRUE);
    IUnknown_SafeReleaseAndNullPtr(&plc);
}

STDMETHODIMP
CDUIViewFrame::TranslateAcceleratorGlobalVF(
    MSG *pmsg)
{
    DirectUI::Element *peTarget = NULL;
    int fMultiple = 0;
    int fWrapped = 0;

    if (!_peRoot || pmsg->message != WM_SYSCHAR || LOWORD(pmsg->wParam) <= L' ')
        return S_FALSE;

    /* Alt+letter shortcuts of the frame's elements */
    DirectUI::HWNDElement::FindShortcut((WCHAR)(ULONG_PTR)CharUpperW((PWSTR)(ULONG_PTR)LOWORD(pmsg->wParam)),
                                        _peRoot, &peTarget, &fMultiple, &fWrapped, 1);
    if (!peTarget)
        return S_FALSE;

    peTarget->SetKeyFocus();
    if (!fMultiple)
        peTarget->DefaultAction();
    return S_OK;
}

STDMETHODIMP
CDUIViewFrame::TranslateAcceleratorVF(
    MSG *pmsg)
{
    struct { HRESULT hr; MSG *pmsg; } walk = { S_FALSE, pmsg };
    BOOL fTab;
    HRESULT hr;

    if (!_fShellViewAttached)
        return _psv ? _psv->TranslateAccelerator(pmsg) : S_FALSE;

    if (pmsg->message < WM_KEYFIRST || pmsg->message > WM_KEYLAST)
        return S_FALSE;

    fTab = IsVK_TABCycler(pmsg);
    DUI_WalkIUnknownElements(_peRoot, DUI_TranslateAcceleratorIO, (LPARAM)&walk);
    if (walk.hr != S_FALSE)
        return walk.hr;

    if (fTab && static_cast<CHWNDView *>(_peRoot)->Navigate(pmsg))
    {
        _FixupCrossDUIViewFocus();
        return S_OK;
    }

    /* Messages outside the frame's elements, or inside the view, belong to the view */
    hr = S_FALSE;
    if (!IsChild(static_cast<CHWNDView *>(_peRoot)->GetHWND(), pmsg->hwnd) || IsChild(_hwndView, pmsg->hwnd))
        hr = S_OK;

    if (!fTab && hr == S_FALSE && SHIsChildOrSelf(_hwndHost, GetFocus()) == S_OK)
        return TranslateAcceleratorGlobalVF(pmsg);
    if (!_psv)
        return TranslateAcceleratorGlobalVF(pmsg);

    hr = _psv->TranslateAccelerator(pmsg);
    if (hr == S_FALSE)
        return TranslateAcceleratorGlobalVF(pmsg);
    return hr;
}

STDMETHODIMP
CDUIViewFrame::SetMode(
    VFMODE mode)
{
    _dwMode = mode;
    return S_OK;
}

/* IFrameManager */

STDMETHODIMP
CDUIViewFrame::GetLayoutType(
    LAYOUTTYPE *pType)
{
    *pType = _layoutType;
    return _fLayoutTypeKnown ? S_OK : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::GetShellView(
    IShellView **ppsv)
{
    *ppsv = NULL;
    IUnknown_Set((IUnknown **)ppsv, _psv);
    return *ppsv ? S_OK : E_FAIL;
}

/* Per folder state lives in numbered MRU slots keyed by the folder's pidl */
HRESULT
CDUIViewFrame::_GetRegistryPathForLocation(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ BOOL fCreate,
    _In_ PCWSTR pszRoot,
    _Out_writes_(MAX_PATH) PWSTR pszSubKey)
{
    IMruPidlList *pmru;
    WCHAR szSlots[MAX_PATH];
    HKEY hkey;
    DWORD cbData;
    DWORD dwSlots;
    UINT uSlot;
    UINT uFound;
    HRESULT hr;

    *pszSubKey = UNICODE_NULL;
    hr = StringCchCopyW(szSlots, _countof(szSlots), pszRoot);
    if (SUCCEEDED(hr))
        hr = StringCchCatW(szSlots, _countof(szSlots), L"\\PerFolderSettings");
    if (FAILED(hr))
        return hr;

    hr = CoCreateInstance(CLSID_MruPidlList, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pmru));
    if (FAILED(hr))
        return hr;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, szSlots, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hkey, NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        cbData = sizeof(dwSlots);
        if (SHGetValueW(HKEY_LOCAL_MACHINE, pszRoot, L"ModuleMRUSize", NULL, &dwSlots, &cbData))
            dwSlots = 500;

        hr = pmru->InitList(dwSlots, hkey, L"Slots");
        if (SUCCEEDED(hr))
        {
            if (pmru->QueryPidl(pidl, 1, &uSlot, &uFound) == S_OK || fCreate)
            {
                hr = pmru->UsePidl(pidl, &uSlot);
                if (SUCCEEDED(hr))
                    hr = StringCchPrintfW(pszSubKey, MAX_PATH, L"%ws\\%d", szSlots, uSlot);
            }
            else
            {
                hr = E_FAIL;
            }
        }
        RegCloseKey(hkey);
    }
    pmru->Release();
    return hr;
}

HRESULT
CDUIViewFrame::_GetPerFolderPersistStream(
    _In_ PCWSTR pszClass,
    _In_ PCWSTR pszID,
    _In_ PCWSTR pszRoot,
    _In_ UINT grfMode,
    _COM_Outptr_ IStream **ppstm)
{
    PIDLIST_ABSOLUTE pidl;
    WCHAR szFolder[MAX_PATH];
    WCHAR szKey[MAX_PATH];
    HRESULT hr;

    hr = SHGetIDListFromObject(_psv, &pidl);
    if (FAILED(hr))
        return hr;

    hr = _GetRegistryPathForLocation(pidl, (grfMode & (STGM_WRITE | STGM_READWRITE)) != 0, pszRoot, szFolder);
    if (SUCCEEDED(hr))
        hr = StringCchPrintfW(szKey, _countof(szKey), L"%ws\\%ws", szFolder, pszClass);
    if (SUCCEEDED(hr))
    {
        *ppstm = SHOpenRegStream2W(HKEY_CURRENT_USER, szKey, pszID, grfMode);
        hr = *ppstm ? S_OK : E_FAIL;
    }
    CoTaskMemFree(pidl);
    return hr;
}

HRESULT
CDUIViewFrame::_GetPerLayoutPersistStream(
    _In_ PCWSTR pszClass,
    _In_ PCWSTR pszID,
    _In_ PCWSTR pszRoot,
    _In_ UINT grfMode,
    _COM_Outptr_ IStream **ppstm)
{
    WCHAR szKey[MAX_PATH];
    HRESULT hr;

    hr = StringCchPrintfW(szKey, _countof(szKey), L"%ws\\PerExplorerSettings\\%d\\%ws", pszRoot, _layoutType, pszClass);
    if (FAILED(hr))
        return hr;

    *ppstm = SHOpenRegStream2W(HKEY_CURRENT_USER, szKey, pszID, grfMode);
    return *ppstm ? S_OK : E_FAIL;
}

/* Folder types are not implemented, so every folder takes the per layout stream */
HRESULT
CDUIViewFrame::_GetPerFolderTypeIdPersistStream(
    _In_ PCWSTR pszClass,
    _In_ PCWSTR pszID,
    _In_ PCWSTR pszRoot,
    _In_ UINT grfMode,
    _COM_Outptr_ IStream **ppstm)
{
    return _GetPerLayoutPersistStream(pszClass, pszID, pszRoot, grfMode, ppstm);
}

STDMETHODIMP
CDUIViewFrame::GetPersistStream(
    int iType,
    PCWSTR pszClass,
    PCWSTR pszID,
    UINT grfMode,
    IStream **ppstm)
{
    WCHAR szKey[MAX_PATH];
    HRESULT hr;

    *ppstm = NULL;
    switch (iType)
    {
        case FRAMEPERSIST_PERFOLDER:
            return _GetPerFolderPersistStream(pszClass, pszID, c_szModulesKey, grfMode, ppstm);

        case FRAMEPERSIST_PERLAYOUT:
            return _GetPerLayoutPersistStream(pszClass, pszID, c_szModulesKey, grfMode, ppstm);

        case FRAMEPERSIST_GLOBAL:
            hr = StringCchPrintfW(szKey, _countof(szKey), L"%ws\\GlobalSettings\\%ws", c_szModulesKey, pszClass);
            if (FAILED(hr))
                return hr;
            *ppstm = SHOpenRegStream2W(HKEY_CURRENT_USER, szKey, pszID, grfMode);
            return *ppstm ? S_OK : E_FAIL;

        case FRAMEPERSIST_PERFOLDERTYPE:
            return _GetPerFolderTypeIdPersistStream(pszClass, pszID, c_szModulesKey, grfMode, ppstm);

        default:
            return E_FAIL;
    }
}

/* IObjectProvider: the frame task manager is Phase B work (notes\SHELL32_DUIFRAME.md) */
STDMETHODIMP
CDUIViewFrame::QueryObject(
    REFGUID guidObject,
    REFIID riid,
    void **ppvOut)
{
    *ppvOut = NULL;
    return E_NOTIMPL;
}

/* IDispatch: the view's DShellFolderViewEvents */

STDMETHODIMP
CDUIViewFrame::GetTypeInfoCount(
    UINT *pctinfo)
{
    *pctinfo = 0;
    return E_NOTIMPL;
}

STDMETHODIMP
CDUIViewFrame::GetTypeInfo(
    UINT iTInfo,
    LCID lcid,
    ITypeInfo **ppTInfo)
{
    *ppTInfo = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CDUIViewFrame::GetIDsOfNames(
    REFIID riid,
    LPOLESTR *rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID *rgDispId)
{
    return E_NOTIMPL;
}

/* Re-evaluates the layout for the location; reason picks the empty or normal variant */
void
CDUIViewFrame::_ApplyLayoutDefinitionFor(
    _In_ GETLAYOUTREASON reason)
{
    IFrameLayoutDefinition *pfld;
    PIDLIST_ABSOLUTE pidl;
    HWND hwnd;

    if (FAILED(IUnknown_GetWindow(_pvfh, &hwnd)) || FAILED(SHGetIDListFromObject(_psv, &pidl)))
        return;

    if (SUCCEEDED(GetLayoutDefinitionAllowRedirect(reason, pidl, hwnd, _clsidFolderPrev, &pfld)))
    {
        SetLayoutDefinition(pfld);
        pfld->Release();
    }
    CoTaskMemFree(pidl);
}

/* An empty folder can have its own layout; switch when the view becomes empty or stops being empty */
void
CDUIViewFrame::_CheckForLayoutChangeOnContentsChange()
{
    IFolderView2 *pfv2;
    int cItems;
    BOOL fEmpty;

    if (!_psv || FAILED(_psv->QueryInterface(IID_PPV_ARGS(&pfv2))))
        return;

    pfv2->ItemCount(SVGIO_ALLVIEW, &cItems);
    fEmpty = (cItems == 0);
    if (!cItems || _fViewWasEmpty != fEmpty)
        _ApplyLayoutDefinitionFor(fEmpty ? GLR_VIEWEMPTY : GLR_DEFAULT);

    _fViewWasEmpty = fEmpty;
    pfv2->Release();
}

STDMETHODIMP
CDUIViewFrame::Invoke(
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pDispParams,
    VARIANT *pVarResult,
    EXCEPINFO *pExcepInfo,
    UINT *puArgErr)
{
    IPropertyBag *ppb;
    HRESULT hr = S_OK;

    if (pVarResult)
        VariantInit(pVarResult);

    switch (dispIdMember)
    {
        case DISPID_VIEW_FOLDERTYPECHANGED:
            if (!_fInSetLayoutDefinition)
                _ApplyLayoutDefinitionFor(GLR_DEFAULT);
            break;

        case DISPID_SELECTIONCHANGED:
            DUI_WalkIUnknownElements(_peRoot, DUI_SendSelectionChangedToUnknown, 0);
            break;

        case DISPID_FILELISTENUMDONE:
            _CheckForLayoutChangeOnContentsChange();
            break;

        case DISPID_CONTENTSCHANGED:
            /* Hold the notification while the view is not in the tree yet */
            _fContentsChangedSent = FALSE;
            _CheckForLayoutChangeOnContentsChange();
            if (!_fShellViewAttached)
                _fContentsChangedPending = TRUE;
            else if (!_fContentsChangedSent)
                DUI_WalkIUnknownElements(_peRoot, DUI_SendContentsChangedToUnknown, 0);
            break;

        case DISPID_FOLDERCHANGED:
            if (!_fShellViewAttached)
                _fFolderChangedPending = TRUE;
            else
                DUI_WalkIUnknownElements(_peRoot, DUI_SendFolderChangedToUnknown, 0);
            break;

        case DISPID_SELECTEDITEMCHANGED:
            DUI_WalkIUnknownElements(_peRoot, DUI_SendSelectedItemChangedToUnknown, 0);
            break;

        case DISPID_VIEWMODECHANGED:
        case DISPID_COLUMNSCHANGED:
        case DISPID_ICONSIZECHANGED:
            hr = QueryService(SID_PerLayoutPropertyBag, IID_PPV_ARGS(&ppb));
            if (SUCCEEDED(hr))
            {
                SHPropertyBag_WriteBOOL(ppb, L"SettingsChanged", TRUE);
                ppb->Release();
            }
            break;

        case DISPID_FILTERINVOKED:
            hr = QueryService(SID_PerLayoutPropertyBag, IID_PPV_ARGS(&ppb));
            if (SUCCEEDED(hr))
            {
                PWSTR pszSearch = NULL;

                if (pDispParams && pDispParams->rgvarg)
                    hr = VariantToStringAlloc(pDispParams->rgvarg[0], &pszSearch);
                else
                    hr = E_FAIL;

                if (SUCCEEDED(hr))
                {
                    SHPropertyBag_WriteStr(ppb, L"SearchText", pszSearch);
                    CoTaskMemFree(pszSearch);
                }
                ppb->Release();
            }
            break;

        default:
            hr = DISP_E_MEMBERNOTFOUND;
            break;
    }

    if (pVarResult)
    {
        V_VT(pVarResult) = VT_BOOL;
        V_BOOL(pVarResult) = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
    }
    return hr;
}

/* IServiceProvider */

STDMETHODIMP
CDUIViewFrame::QueryService(
    REFGUID guidService,
    REFIID riid,
    void **ppv)
{
    IPropertyBag *ppbInner;
    HRESULT hr;

    *ppv = NULL;
    if (IsEqualGUID(guidService, IID_IFrameManager))
        return QueryInterface(riid, ppv);

    if (IsEqualGUID(guidService, SID_PerLayoutPropertyBag))
    {
        if (!_ppbLayout)
        {
            hr = SHCreatePropertyBagOnMemory(STGM_READWRITE, IID_PPV_ARGS(&ppbInner));
            if (FAILED(hr))
                return hr;

            hr = CreateCallbackPropertyBag(ppbInner, this, s_PropertyWriteCallback, IID_PPV_ARGS(&_ppbLayout));
            ppbInner->Release();
            if (FAILED(hr))
                return hr;
        }
        return _ppbLayout->QueryInterface(riid, ppv);
    }

    /* Browser wide state comes from the host, namespace state from the view; both announce writes */
    if (IsEqualGUID(guidService, SID_PerBrowserPropertyBag) ||
        IsEqualGUID(guidService, SID_PerNamespaceIDPropertyBag))
    {
        IUnknown *punkSource = IsEqualGUID(guidService, SID_PerNamespaceIDPropertyBag) ?
                               static_cast<IUnknown *>(_psv) : static_cast<IUnknown *>(_pvfh);

        hr = IUnknown_QueryService(punkSource, guidService, IID_PPV_ARGS(&ppbInner));
        if (SUCCEEDED(hr))
        {
            hr = CreateCallbackPropertyBag(ppbInner, this, s_PropertyWriteCallback, riid, ppv);
            ppbInner->Release();
        }
        return hr;
    }

    return IUnknown_QueryService(_punkSite, guidService, riid, ppv);
}

/* IPropertyBag: reads fall through layout, namespace and browser state in that order */

STDMETHODIMP
CDUIViewFrame::Read(
    LPCOLESTR pszPropName,
    VARIANT *pVar,
    IErrorLog *pErrorLog)
{
    static const GUID *const c_rgFallbackBags[] =
    {
        &SID_PerNamespaceIDPropertyBag,
        &SID_PerBrowserPropertyBag,
    };
    HRESULT hr = E_FAIL;
    SIZE_T i;

    if (_ppbLayout)
        hr = _ppbLayout->Read(pszPropName, pVar, pErrorLog);

    for (i = 0; FAILED(hr) && i < _countof(c_rgFallbackBags); i++)
    {
        IPropertyBag *ppb;

        if (SUCCEEDED(QueryService(*c_rgFallbackBags[i], IID_PPV_ARGS(&ppb))))
        {
            hr = ppb->Read(pszPropName, pVar, pErrorLog);
            ppb->Release();
        }
    }
    return hr;
}

STDMETHODIMP
CDUIViewFrame::Write(
    LPCOLESTR pszPropName,
    VARIANT *pVar)
{
    return E_FAIL;
}

/* IShellSearchTarget: everything goes to the view's search target */

STDMETHODIMP
CDUIViewFrame::Search(
    PCWSTR pszSearchText,
    DWORD dwCommand)
{
    return _psstView ? _psstView->Search(pszSearchText, dwCommand) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::OnSearchTextNotify(
    PCWSTR pszSearchText,
    PCWSTR pszStrippedText,
    SHELLSEARCHNOTIFY sn)
{
    return _psstView ? _psstView->OnSearchTextNotify(pszSearchText, pszStrippedText, sn) : S_OK;
}

STDMETHODIMP
CDUIViewFrame::GetSearchText(
    PWSTR pszSearchText,
    UINT cch)
{
    return _psstView ? _psstView->GetSearchText(pszSearchText, cch) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::GetPromptText(
    PWSTR pszPromptText,
    UINT cch)
{
    return _psstView ? _psstView->GetPromptText(pszPromptText, cch) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::GetACEnumString(
    IEnumString **ppEnumString)
{
    *ppEnumString = NULL;
    return _psstView ? _psstView->GetACEnumString(ppEnumString) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::GetMenu(
    HMENU *phmenu)
{
    *phmenu = NULL;
    return _psstView ? _psstView->GetMenu(phmenu) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::InitMenuPopup(
    HMENU hMenu)
{
    return _psstView ? _psstView->InitMenuPopup(hMenu) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::OnMenuCommand(
    DWORD dwCmd)
{
    return _psstView ? _psstView->OnMenuCommand(dwCmd) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::Enter(
    IShellSearchScope *pSearchScope)
{
    return _psstView ? _psstView->Enter(pSearchScope) : E_FAIL;
}

STDMETHODIMP
CDUIViewFrame::Exit()
{
    return _psstView ? _psstView->Exit() : E_FAIL;
}

/* IOleWindow */

STDMETHODIMP
CDUIViewFrame::GetWindow(
    HWND *phwnd)
{
    *phwnd = _hwndHost;
    return S_OK;
}

STDMETHODIMP
CDUIViewFrame::ContextSensitiveHelp(
    BOOL fEnterMode)
{
    return S_OK;
}

/* Proffer */

void
CDUIViewFrame::_ProfferToBrowser(
    _In_ BOOL fProffer)
{
    if (fProffer)
    {
        if (_fProffered)
            return;
        _fProffered = TRUE;
        IUnknown_ProfferService(_pvfh, IID_IFrameManager, static_cast<IServiceProvider *>(this),
                                &_dwProfferCookieFrameManager);
        IUnknown_ProfferService(_pvfh, SID_PerLayoutPropertyBag, static_cast<IServiceProvider *>(this),
                                &_dwProfferCookieLayoutBag);
    }
    else if (_fProffered)
    {
        if (_dwProfferCookieFrameManager)
            IUnknown_ProfferService(_pvfh, IID_IFrameManager, NULL, &_dwProfferCookieFrameManager);
        if (_dwProfferCookieLayoutBag)
            IUnknown_ProfferService(_pvfh, SID_PerLayoutPropertyBag, NULL, &_dwProfferCookieLayoutBag);
        _fProffered = FALSE;
    }
}

/* Takes every element off the old view and site before the tree is torn down */
void
CDUIViewFrame::_DetachElementTree()
{
    ULONG dwDeferCookie = 0;
    IFolderView2 *pfv2;

    if (!_peRoot)
        return;

    DisableAnimations();
    _peRoot->StartDefer(&dwDeferCookie);

    DUI_WalkIUnknownElements(_peRoot, DUI_SendNavigateAwayToUnknown, 0);
    DUI_WalkIUnknownElements(_peRoot, DUI_SetSiteOnUnknown, 0);
    DUI_WalkIUnknownElements(_peRoot, s_RemoveInnerObject, 0);

    if (_psv && SUCCEEDED(_psv->QueryInterface(IID_PPV_ARGS(&pfv2))))
    {
        DUI_ApplyLayoutStateToView(pfv2, NULL);
        pfv2->Release();
    }

    if (_peListview)
        _peListview->DetachListview();

    IFrameTaskManager *pftm;
    if (_punkTaskManager && SUCCEEDED(_punkTaskManager->QueryInterface(IID_PPV_ARGS(&pftm))))
    {
        pftm->CancelAllWorkItems();
        pftm->Release();
    }

    if (dwDeferCookie)
        _peRoot->EndDefer(dwDeferCookie);
    EnableAnimations();
}
