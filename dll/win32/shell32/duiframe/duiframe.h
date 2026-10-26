/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DirectUI view frame: private interfaces, helpers and element classes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define DUIFRAME_HINST ((HINSTANCE)&__ImageBase)

/* Interfaces between the frame and the elements it hosts */

MIDL_INTERFACE("60CE96BB-B6F9-42DC-B84E-5E5D9C370A6F")
IFrameModule : public IUnknown
{
    STDMETHOD(CreateInnerObject)(
        _COM_Outptr_ IUnknown **ppunkInner) PURE;
    STDMETHOD(SetInnerObject)(
        _In_opt_ IUnknown *punkInner) PURE;
    STDMETHOD(GetModuleID)(
        _Outptr_ PWSTR *ppszModuleID) PURE;
};

MIDL_INTERFACE("1D3293BC-EC01-400F-8179-8DE5D53ABEB7")
IFrameNotificationClient : public IUnknown
{
    STDMETHOD(LayoutInitialized)() PURE;
    STDMETHOD(Notify)(
        _In_ PCWSTR pszPropertyName) PURE;
    STDMETHOD(OnNavigateAway)() PURE;
    STDMETHOD(LayoutUninitialized)() PURE;
};

MIDL_INTERFACE("6B353825-C58B-4F03-AEC4-8DE179122661")
IFrameShellViewClient : public IUnknown
{
    STDMETHOD(OnSelectedItemChanged)() PURE;
    STDMETHOD(OnSelectionChanged)() PURE;
    STDMETHOD(OnContentsChanged)() PURE;
    STDMETHOD(OnFolderChanged)() PURE;
};

MIDL_INTERFACE("E571E20C-0490-4284-A6F9-64DB48921FF5")
IFrameFocusHost : public IUnknown
{
    STDMETHOD(WantsInitialFocus)(
        _Out_ BOOL *pfWantsFocus) PURE;
};

MIDL_INTERFACE("B8C7036C-8D15-456D-84F5-82FB5B1AE6A5")
IElementWrapper : public IUnknown
{
    STDMETHOD(GetElement)(
        _Outptr_ DirectUI::Element **ppe) PURE;
};

MIDL_INTERFACE("3B1D2CF7-94BF-4614-AC00-9E72401A9FE0")
ILayoutDefinitionRegistration : public IUnknown
{
    STDMETHOD(Register)() PURE;
    STDMETHOD(UnRegister)() PURE;
};

/* Nav pane setting refresh, reached through QueryService(IID_INameSpaceTreeControl) on a browser */
MIDL_INTERFACE("5925316D-20B6-4FF9-A980-96482AA885DE")
IUpdateNavPaneSettings : public IUnknown
{
    STDMETHOD(UpdateSetting)(
        _In_ int iSetting) PURE;
};

/* IUpdateNavPaneSettings::UpdateSetting values */
#define NAVPANE_SETTING_SHOWALLFOLDERS      1
#define NAVPANE_SETTING_EXPANDTOCURRENT     2

typedef enum _LAYOUTPROPERTIES
{
    LP_NONE = 0x0,
    LP_HIDECONTENTPANE = 0x1
} LAYOUTPROPERTIES;

MIDL_INTERFACE("BA1439CB-6514-4152-8A5A-9B9B5000E591")
ILayoutProperties : public IUnknown
{
    STDMETHOD(GetLayoutProperties)(
        _Out_ LAYOUTPROPERTIES *plp) PURE;
};

/* GetPersistStream types; the sizer's FramePersistType names one */
#define FRAMEPERSIST_PERFOLDER      0
#define FRAMEPERSIST_PERLAYOUT      1
#define FRAMEPERSIST_GLOBAL         2
#define FRAMEPERSIST_PERFOLDERTYPE  3

/* DShellFolderViewEvents ids beyond the ones ReactOS's shdispid.h has */
#ifndef DISPID_VIEWMODECHANGED
#define DISPID_VIEWMODECHANGED      205
#endif
#ifndef DISPID_CONTENTSCHANGED
#define DISPID_CONTENTSCHANGED      207
#endif
#ifndef DISPID_COLUMNSCHANGED
#define DISPID_COLUMNSCHANGED       212
#endif
#ifndef DISPID_ICONSIZECHANGED
#define DISPID_ICONSIZECHANGED      215
#endif
#ifndef DISPID_FOLDERCHANGED
#define DISPID_FOLDERCHANGED        217
#endif
#ifndef DISPID_FILTERINVOKED
#define DISPID_FILTERINVOKED        218
#endif
#ifndef DISPID_SELECTEDITEMCHANGED
#define DISPID_SELECTEDITEMCHANGED  220
#endif
/* Raised by the view when its folder type, and with it the frame layout, should be re-evaluated */
#define DISPID_VIEW_FOLDERTYPECHANGED 199

/* GUIDs the frame needs that ModernUI's guid library does not carry (guids.c) */
EXTERN_C const GUID SID_PerLayoutPropertyBag;
EXTERN_C const GUID OID_FrameTaskManager;
EXTERN_C const GUID SID_DefViewFrame;
EXTERN_C const IID IID_IFrameModule;
EXTERN_C const IID IID_IFrameNotificationClient;
EXTERN_C const IID IID_IFrameShellViewClient;
EXTERN_C const IID IID_IFrameFocusHost;
EXTERN_C const IID IID_IElementWrapper;
EXTERN_C const IID IID_ILayoutDefinitionRegistration;
EXTERN_C const IID IID_ILayoutProperties;
EXTERN_C const IID IID_IDelayedVisibility;
EXTERN_C const IID IID_IUpdateNavPaneSettings;
/* XP's MruPidlList from shdocvw; shell32 gets these two from the uuid library */
EXTERN_C const CLSID CLSID_MruPidlList;
EXTERN_C const IID IID_IMruPidlList;

MIDL_INTERFACE("47851649-A2EF-4E67-BAEC-C6A153AC72EC")
IMruPidlList : public IUnknown
{
    STDMETHOD(InitList)(
        _In_ UINT uMax,
        _In_ HKEY hKey,
        _In_ PCWSTR pszSubKey) PURE;
    STDMETHOD(UsePidl)(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Out_ UINT *puSlot) PURE;
    STDMETHOD(QueryPidl)(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ UINT cSlots,
        _Out_writes_(cSlots) UINT *puSlots,
        _Out_ UINT *pcFound) PURE;
    STDMETHOD(PruneKids)(
        _In_ PCIDLIST_ABSOLUTE pidl) PURE;
};

/* Win7 shell objects start zeroed and their constructors only set what must not be zero.
   shell32's operator new does not zero, so the frame's heap objects bring their own. */
class CDUIFrameZeroAlloc
{
public:
    static void *operator new(size_t cb)
    {
        return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb);
    }

    static void operator delete(void *pv)
    {
        if (pv)
            HeapFree(GetProcessHeap(), 0, pv);
    }
};

/* DirectUI helpers */
HRESULT WINAPI InitializeDirectUI(VOID);
VOID WINAPI UnInitializeDirectUI(VOID);
HRESULT WINAPI DUI_InitThread(VOID);
VOID WINAPI DUI_UninitThread(VOID);
VOID WINAPI DUI_ProcessDetachCleanup(VOID);

HINSTANCE
WINAPI
GetShellStyleHInstance(
    _Outptr_opt_ PWSTR *ppszPath);

HRESULT
WINAPI
DUI_LoadUIFileFromResources(
    _In_ HINSTANCE hinst,
    _In_ UINT uID,
    _Outptr_ PWSTR *ppszUIFile);

HRESULT
WINAPI
DUI_CreateParser(
    _In_ PCWSTR pszUIFile,
    _Outptr_ DirectUI::DUIXmlParser **ppParser,
    _In_opt_ IUnknown *punkSheetContext,
    _In_opt_ DirectUI::ParseErrorCallback pfnError);

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
    _In_opt_ IUnknown *punkSheetContext);

DirectUI::Value *
WINAPI
DUI_GetSheetCB(
    _In_ const wchar_t *pszSheetID,
    _In_opt_ void *pvContext);

HRESULT
WINAPI
DUI_CreateParserWithCallbackFromResource(
    _In_ HINSTANCE hinst,
    _In_ UINT uID,
    _Outptr_ DirectUI::DUIXmlParser **ppParser,
    _In_ DirectUI::GetSheetCallback pfnGetSheet,
    _In_opt_ void *pvContext);

HRESULT
WINAPI
DUI_ReloadTheme(
    _In_opt_ DirectUI::Element *peRoot);

HRESULT
WINAPI
DUI_GetElementRootHWND(
    _In_ DirectUI::Element *pe,
    _Out_ HWND *phwnd);

HRESULT
WINAPI
DUI_GetElementRootHWNDElement(
    _In_ DirectUI::Element *pe,
    _Outptr_ DirectUI::HWNDElement **ppheRoot);

typedef VOID (WINAPI *PFNUNKNOWNELEMENTWALK)(IUnknown *punk, LPARAM lParam);

VOID
WINAPI
DUI_WalkIUnknownElements(
    _In_opt_ DirectUI::Element *peRoot,
    _In_ PFNUNKNOWNELEMENTWALK pfnCallback,
    _In_ LPARAM lParam);

VOID WINAPI DUI_SendInitializationToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SendNotificationToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SendNavigateAwayToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SendSelectedItemChangedToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SendSelectionChangedToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SendContentsChangedToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SendFolderChangedToUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_SetSiteOnUnknown(_In_ IUnknown *punk, _In_ LPARAM lParam);
VOID WINAPI DUI_TranslateAcceleratorIO(_In_ IUnknown *punk, _In_ LPARAM lParam);

VOID
WINAPI
DUI_ApplyLayoutStateToView(
    _In_ IFolderView2 *pfv,
    _In_opt_ DirectUI::Element *peViewHost);

BOOL
WINAPI
DUI_TrackPopupMenu(
    _In_ HMENU hmenu,
    _In_ UINT uFlags,
    _In_ int x,
    _In_ int y,
    _In_ HWND hwnd,
    _In_opt_ const RECT *prc,
    _In_ BOOL fRTL);

HRESULT
WINAPI
DUI_GetMouseEventAbsolutePos(
    _In_ DirectUI::Element *pe,
    _In_ const POINT *pptElement,
    _Out_ POINT *pptScreen);

HRESULT
WINAPI
IUnknown_GetExplorerPaneState(
    _In_opt_ IUnknown *punkSite,
    _In_ REFEXPLORERPANE ep,
    _Out_ EXPLORERPANESTATE *peps);

BOOL
WINAPI
PropertyBag_ReadBOOLDefault(
    _In_ IPropertyBag *ppb,
    _In_ PCWSTR pszName,
    _In_ BOOL fDefault);

int
WINAPI
IsVK_TABCycler(
    _In_opt_ const MSG *pmsg);

HRESULT
WINAPI
CreateWrapperForElement(
    _In_ DirectUI::Element *pe,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

/* Element classes */

class CElementWithIUnknown : public DirectUI::Element, public IUnknown
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW();

    static IUnknown *WINAPI GetUnknownFromElement(
        _In_ DirectUI::Element *pe);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    static DirectUI::IClassInfo *Class;
};

class CElementWithSite : public CElementWithIUnknown, public IObjectWithSite
{
public:
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override;

    HRESULT GetPersistStream(
        _In_ UINT uPersistType,
        _In_ DWORD grfMode,
        _COM_Outptr_ IStream **ppstm);

    static DirectUI::IClassInfo *Class;

protected:
    IUnknown *_punkSite;
};

class CFrameModule : public CElementWithSite, public IFrameModule
{
public:
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CElementWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP CreateInnerObject(IUnknown **ppunkInner) override;
    STDMETHODIMP SetInnerObject(IUnknown *punkInner) override;
    STDMETHODIMP GetModuleID(PWSTR *ppszModuleID) override;

    static const DirectUI::PropertyInfo *const ModuleIDProp;
    static DirectUI::IClassInfo *Class;
};

/* Modules whose inner element is created from its own class: the outer element just wraps it */
typedef HRESULT (WINAPI *PFNCREATEINNERELEMENT)(DirectUI::Element *, ULONG *, DirectUI::Element **);

HRESULT
WINAPI
FrameModule_CreateInner(
    _In_ PFNCREATEINNERELEMENT pfnCreate,
    _In_ PCWSTR pszInnerID,
    _COM_Outptr_ IUnknown **ppunkInner);

#define DECLARE_FRAME_MODULE(ClassName)                                                  \
class ClassName : public CFrameModule                                                    \
{                                                                                        \
public:                                                                                  \
    static HRESULT WINAPI Create(                                                        \
        _In_opt_ DirectUI::Element *peParent,                                            \
        _Inout_opt_ ULONG *pdwDeferCookie,                                               \
        _Outptr_ DirectUI::Element **ppElement);                                         \
    static HRESULT WINAPI Register();                                                    \
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }              \
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }                      \
    STDMETHODIMP CreateInnerObject(IUnknown **ppunkInner) override;                      \
    static DirectUI::IClassInfo *Class;                                                  \
}

DECLARE_FRAME_MODULE(CProperTreeModule);
DECLARE_FRAME_MODULE(CCommandModule);
DECLARE_FRAME_MODULE(CInfoBarHostModule);
DECLARE_FRAME_MODULE(CLibraryPaneModule);
DECLARE_FRAME_MODULE(CInfoPane);

/* Modules that also follow the frame's property bag notifications */
class CNotifyingFrameModule : public CFrameModule, public IFrameNotificationClient
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP LayoutInitialized() override { return S_OK; }
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override { return S_OK; }
    STDMETHODIMP OnNavigateAway() override { return S_OK; }
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }
};

class CPreviewThumbnail : public CNotifyingFrameModule
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    STDMETHODIMP CreateInnerObject(IUnknown **ppunkInner) override;
    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;

    static const DirectUI::PropertyInfo *const MaxThumbSizeProp;
    static const DirectUI::PropertyInfo *const FixedSizeProp;
    static const DirectUI::PropertyInfo *const HidePropertyProp;
    static const DirectUI::PropertyInfo *const VerticalProp;
    static const DirectUI::PropertyInfo *const AlwaysGetIconFromViewProp;
    static const DirectUI::PropertyInfo *const ForceUseIconProp;
    static const DirectUI::PropertyInfo *const ThumbnailIdProp;
    static DirectUI::IClassInfo *Class;

private:
    HRESULT _UpdateLayoutPos();
};

class CPreviewMetadata : public CNotifyingFrameModule
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    STDMETHODIMP CreateInnerObject(IUnknown **ppunkInner) override;

    static const DirectUI::PropertyInfo *const NullSelectPropertyStringProp;
    static const DirectUI::PropertyInfo *const NullSelectTitlePropertyStringProp;
    static const DirectUI::PropertyInfo *const ReadOnlyProp;
    static DirectUI::IClassInfo *Class;
};

class CReadingPaneModule : public CNotifyingFrameModule
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    STDMETHODIMP CreateInnerObject(IUnknown **ppunkInner) override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;

    static DirectUI::IClassInfo *Class;
};

class CProperTreeHost;
class CDUISizerElement;

/* Nav pane content: the tree host plus the optional links expando and the pane sizer */
class CProperTreeModuleInner :
    public CElementWithSite,
    public IFrameNotificationClient,
    public DirectUI::IElementListener,
    public IInputObject
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnHosted(DirectUI::Element *peNewHost);
    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;
    STDMETHODIMP OnNavigateAway() override;
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    void OnListenerAttach(DirectUI::Element *peFrom) override {}
    void OnListenerDetach(DirectUI::Element *peFrom) override {}
    bool OnListenedPropertyChanging(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                    DirectUI::Value *pvOld, DirectUI::Value *pvNew) override { return true; }
    void OnListenedPropertyChanged(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                   DirectUI::Value *pvOld, DirectUI::Value *pvNew) override;
    void OnListenedInput(DirectUI::Element *peFrom, DirectUI::InputEvent *pie) override {}
    void OnListenedEvent(DirectUI::Element *peFrom, DirectUI::Event *pev) override {}

    STDMETHODIMP UIActivateIO(BOOL fActivate, MSG *pmsg) override { return E_NOTIMPL; }
    STDMETHODIMP HasFocusIO() override;
    STDMETHODIMP TranslateAcceleratorIO(MSG *pmsg) override;

    static DirectUI::IClassInfo *Class;

private:
    void _ExpandFolders(_In_ BOOL fExpand);
    void _UpdateButton();
    BOOL _FolderTreeHasFocus();
    BOOL _OnInputExpando(_Inout_ DirectUI::InputEvent *pie);
    HRESULT _SaveSettings();

    IPropertyBag *_ppbBrowser;
    IPropertyBag *_ppbSettings;
    BOOL _fExpandPending;
    BOOL _fLayoutInitialized;
    BOOL _fExpandoPressed;
    DirectUI::Element *_peHeaderExpand;
    DirectUI::Element *_peHeaderCollapse;
    CDUISizerElement *_pseExpando;
    CProperTreeHost *_pHost;
};

/* Push button of the command bar that sinks into the bar while pressed */
class Button3d : public DirectUI::Button
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);

    HRESULT SetEvaluatingAsyncState(_In_ bool fEvaluating);

    static const DirectUI::PropertyInfo *const PressedPaddingShiftProp;
    static const DirectUI::PropertyInfo *const EvaluatingAsyncStateProp;
    static DirectUI::IClassInfo *Class;
};

/* Command bar content: the folder's commands plus the preview pane and help buttons */
class CCommandModuleSite;
typedef struct _COMMANDBUTTONDATA COMMANDBUTTONDATA;

class CCommandModuleInner :
    public CElementWithSite,
    public IFrameNotificationClient,
    public IFrameShellViewClient
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnEvent(DirectUI::Event *pev);
    virtual void OnDestroy();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;
    STDMETHODIMP OnNavigateAway() override;
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    STDMETHODIMP OnSelectedItemChanged() override { return S_OK; }
    STDMETHODIMP OnSelectionChanged() override;
    STDMETHODIMP OnContentsChanged() override;
    STDMETHODIMP OnFolderChanged() override;

    HWND GetWorkerWindow() { return _hwndWorker; }
    DirectUI::Element *GetMenuOwner() { return _peMenuOwner; }

    static DirectUI::IClassInfo *Class;

private:
    static LRESULT CALLBACK s_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static DirectUI::Value *WINAPI s_GetSheetCallback(_In_ const wchar_t *pszSheetID, _In_opt_ void *pvContext);

    void _DeferRefreshButtons(_In_ BOOL fDelay);
    void _InitRefreshButtons(_In_ BOOL fDelay);
    HRESULT _RefreshButtons();
    void _EndRefreshTracking();
    void _UpdatePreviewButton();

    IServiceProvider *_GetCommandSite();
    HRESULT _GetButtonStyles();
    HRESULT _AllocSheetName(_In_opt_ DirectUI::Element *peModule, _In_ const DirectUI::PropertyInfo *ppi,
                            _In_ PCWSTR pszDefault, _Outptr_ PWSTR *ppszSheet);
    HRESULT _GetCommandElementParser(_In_ UINT uID, _Outptr_ DirectUI::DUIXmlParser **ppParser);
    HRESULT _CreateButtons();
    HRESULT _UpdateButton(_In_ IShellFolder *psf, _In_ PCUITEMID_CHILD pidl, _In_opt_ IShellItemArray *psia,
                          _Inout_ int *piButton, _Inout_ BOOL *pfCreated, _Inout_ BOOL *pfDestroyed);
    HRESULT _CreateButton(_In_ IShellFolder *psf, _In_ PCUITEMID_CHILD pidl, _In_ const COMMANDBUTTONDATA *pData,
                          _In_ DirectUI::Element *peParent, _Outptr_ DirectUI::Element **ppe);
    HRESULT _CreateSplitButton(_In_ IShellFolder *psf, _In_ PCUITEMID_CHILD pidl,
                               _In_ const COMMANDBUTTONDATA *pData, _In_ DirectUI::Element *peParent,
                               _Outptr_ DirectUI::Element **ppe);
    HRESULT _CreateCustomElement(_In_ UINT uID, _In_ DirectUI::Element *peParent, _Outptr_ DirectUI::Element **ppe);
    HRESULT _InsertButtonToBand(_In_ DirectUI::Element *pe);
    HRESULT _AddTrackedElement(_In_ PCUITEMID_CHILD pidl, _In_ REFGUID rguidCanonical,
                               _In_opt_ DirectUI::Element *pe, _In_ int iButton);
    int _GetButtonCount();
    void _DestroyButton(_In_ int iButton);
    void _DestroyButtons();
    void _GetDisplayedAndTotalCommands(_Out_opt_ int *pcDisplayed, _Out_opt_ int *pcTotal,
                                       _Out_opt_ int *pcLeftSide);
    void _InsertBackgroundTaskButton(_In_ const COMMANDBUTTONDATA *pData, _In_ UINT uRefreshID);
    HRESULT _ShowOverflowMenu(_In_ DirectUI::Element *peButton);

    HDPA _hdpaButtons;
    HDPA _hdpaParsers;
    HWND _hwndWorker;
    UINT _uRefreshID;
    ITrackEvents *_pte;
    CCommandModuleSite *_pSite;
    DirectUI::Element *_peMenuOwner;
    BOOL _fOverflowMenuUp;
    PWSTR _pszSplitButtonSheet;
    PWSTR _pszTaskButtonSheet;
    BOOL _fNoFolderBand;
    BOOL _fRefreshPending;
    BOOL _fRefreshedOnce;
    UINT _uViewOperation;
};

class CTemplateBackground : public CElementWithSite, public IFrameNotificationClient, public IDispatch
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP LayoutInitialized() override { return S_OK; }
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override { return S_OK; }
    STDMETHODIMP OnNavigateAway() override { return S_OK; }
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) override;
    STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo) override;
    STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId) override;
    STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr) override;

    static const DirectUI::PropertyInfo *const VerticalProp;
    static DirectUI::IClassInfo *Class;

private:
    HRESULT _SetFolderType();

    DWORD _dwCookie;
    IShellView *_psv;
    IDispatch *_pdispView;
};

class CPreviewBackground : public CElementWithSite
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnInput(DirectUI::InputEvent *pie);

    static DirectUI::IClassInfo *Class;

private:
    void _LaunchContextMenu(_In_ DirectUI::MouseEvent *pme);
    HRESULT _GetSelection(_COM_Outptr_ IShellItemArray **ppsia);
    void _RemoveProperties();
};

class CDUISizerElement : public CElementWithSite, public IFrameNotificationClient, public DirectUI::IElementListener
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void OnDestroy();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;
    STDMETHODIMP OnNavigateAway() override;
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    void OnListenerAttach(DirectUI::Element *peFrom) override {}
    void OnListenerDetach(DirectUI::Element *peFrom) override {}
    bool OnListenedPropertyChanging(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                    DirectUI::Value *pvOld, DirectUI::Value *pvNew) override { return true; }
    void OnListenedPropertyChanged(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                   DirectUI::Value *pvOld, DirectUI::Value *pvNew) override;
    void OnListenedInput(DirectUI::Element *peFrom, DirectUI::InputEvent *pie) override {}
    void OnListenedEvent(DirectUI::Element *peFrom, DirectUI::Event *pev) override {}

    HRESULT LoadFromStream();
    HRESULT SaveToStream();

    static const DirectUI::PropertyInfo *const SizingTargetProp;
    static const DirectUI::PropertyInfo *const ReservedTargetProp;
    static const DirectUI::PropertyInfo *const ReservedMasterProp;
    static const DirectUI::PropertyInfo *const SlidesUpAndDownProp;
    static const DirectUI::PropertyInfo *const InitialVisibilityProp;
    static const DirectUI::PropertyInfo *const InitialMaxxedOutProp;
    static const DirectUI::PropertyInfo *const DownOrRightGrowsTargetProp;
    static const DirectUI::PropertyInfo *const InitialSizeProp;
    static const DirectUI::PropertyInfo *const MinSizeProp;
    static const DirectUI::PropertyInfo *const ReservedSpaceProp;
    static const DirectUI::PropertyInfo *const FramePersistTypeProp;
    static const DirectUI::PropertyInfo *const DisablingPolicyProp;
    static const DirectUI::PropertyInfo *const ExplorerVisibilityProp;
    static const DirectUI::PropertyInfo *const GrowTargetFirstProp;
    static const DirectUI::PropertyInfo *const PrioritySizerProp;
    static DirectUI::IClassInfo *Class;

private:
    friend class CProperTreeModuleInner;

    int _GetIntProp(_In_ const DirectUI::PropertyInfo *ppi);
    BOOL _GetBoolProp(_In_ const DirectUI::PropertyInfo *ppi);
    ATOM _GetAtomProp(_In_ const DirectUI::PropertyInfo *ppi);
    BOOL _IsVertical() { return _GetBoolProp(SlidesUpAndDownProp); }
    int _GetExtentAlongAxis(_In_ DirectUI::Element *pe);
    void _CheckPaneVisibility();
    HRESULT _GetLogicalContainerSize(_Out_ int *pcx);
    int _ComputeBoundedSize(_In_ int cx);
    HRESULT _ReadSizeFromPropBag(_Out_ int *pcx);
    HRESULT _AdjustTargetSize();
    HRESULT _Render();
    void _StopListening();

    IPropertyBag *_ppbBrowser;
    DirectUI::Element *_peTarget;
    DirectUI::Element *_peContainer;
    DirectUI::Element *_peReserved;
    CDUISizerElement *_psePriority;
    int _iTargetLayoutPos;
    int _iSizerLayoutPos;
    int _cxContainerSeen;
    int _cxLastRendered;
    int _cxGrowBackPoint;
    int _cxContainerDelta;
    DWORD _dwPaneState;
    BOOL _fDragging;
    BOOL _fLoading;
    BOOL _fDirty;
    BOOL _fPolicyDisabled;
    BOOL _fListening;
    BOOL _fLayoutInitialized;
    BOOL _fSkipPaneStateOverride;
    WCHAR _szID[MAX_PATH];
    WCHAR _szSizeProp[MAX_PATH];
    WCHAR _szVisibleProp[MAX_PATH];
    WCHAR _szLoadedProp[MAX_PATH];
    WCHAR _szMaxxedOutProp[MAX_PATH];
};

class CHWNDView : public DirectUI::HWNDElement
{
public:
    static HRESULT WINAPI Create(
        _In_ HWND hwndParent,
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement) { return E_NOTIMPL; }
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void OnEvent(DirectUI::Event *pev);
    virtual void ActivateTooltip(DirectUI::Element *pe, unsigned long dwFlags);
    virtual LRESULT WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    BOOL Navigate(_In_ MSG *pmsg);

    static DirectUI::IClassInfo *Class;

private:
    DirectUI::Element *_FindAppropriateElement(_In_ DirectUI::Element *peParent, _In_ HWND hwndFocus);
    BOOL _ShouldDoDefaultActionOnEnter(_In_ DirectUI::Element *pe);

    BOOL _fNavigated;
};

class CDUIViewHost : public DirectUI::Element
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    static const DirectUI::PropertyInfo *const SortColumnProp;
    static const DirectUI::PropertyInfo *const SubTextProp;
    static DirectUI::IClassInfo *Class;
};

class CDUIView : public DirectUI::HWNDHost
{
public:
    static HRESULT WINAPI Create(
        _In_ HWND hwndView,
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement) { return E_NOTIMPL; }
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void SetKeyFocus();
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *plRet);
    virtual HWND CreateHWND(HWND hwndParent);
    virtual void OnDestroy();

    void DetachListview();

    static DirectUI::IClassInfo *Class;

private:
    HWND _hwndHost;
    HWND _hwndOriginalParent;
    HWND _hwndView;
    BOOL _fHadClientEdge;
};

class CNavPaneExpandedState;
class CNavPaneNullSpaceMenu;

/* Hosts the namespace tree control of the nav pane */
class CProperTreeHost :
    public DirectUI::HWNDHost,
    public IObjectWithSite,
    public INameSpaceTreeAccessible,
    public INameSpaceTreeControlEvents,
    public INameSpaceTreeControlCustomDrawWin7,
    public INameSpaceTreeOrderCallback,
    public INameSpaceTreeCallbackPriv,
    public IServiceProvider,
    public IUpdateNavPaneSettings,
    public IDispatch
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual HWND CreateHWND(HWND hwndParent);
    virtual void OnHosted(DirectUI::Element *peNewHost);
    virtual void OnDestroy();
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *plRet);
    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);

    // *** IUnknown ***
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    // *** IObjectWithSite ***
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override;

    // *** INameSpaceTreeAccessible ***
    STDMETHODIMP OnGetDefaultAccessibilityAction(IShellItem *psi, BSTR *pbstrDefaultAction) override;
    STDMETHODIMP OnDoDefaultAccessibilityAction(IShellItem *psi) override;
    STDMETHODIMP OnGetAccessibilityRole(IShellItem *psi, VARIANT *pvarRole) override { return E_NOTIMPL; }

    // *** INameSpaceTreeControlEvents ***
    STDMETHODIMP OnItemClick(IShellItem *psi, NSTCEHITTEST nstceHitTest, NSTCECLICKTYPE nstceClickType) override;
    STDMETHODIMP OnPropertyItemCommit(IShellItem *psi) override { return E_NOTIMPL; }
    STDMETHODIMP OnItemStateChanging(IShellItem *psi, NSTCITEMSTATE nstcisMask, NSTCITEMSTATE nstcisState) override
    {
        return S_OK;
    }
    STDMETHODIMP OnItemStateChanged(IShellItem *psi, NSTCITEMSTATE nstcisMask, NSTCITEMSTATE nstcisState) override
    {
        return S_OK;
    }
    STDMETHODIMP OnSelectionChanged(IShellItemArray *psiaSelection) override { return S_OK; }
    STDMETHODIMP OnKeyboardInput(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    STDMETHODIMP OnBeforeExpand(IShellItem *psi) override { return S_OK; }
    STDMETHODIMP OnAfterExpand(IShellItem *psi) override;
    STDMETHODIMP OnBeginLabelEdit(IShellItem *psi) override { return S_OK; }
    STDMETHODIMP OnEndLabelEdit(IShellItem *psi) override { return S_OK; }
    STDMETHODIMP OnGetToolTip(IShellItem *psi, LPWSTR pszTip, int cchTip) override { return E_FAIL; }
    STDMETHODIMP OnBeforeItemDelete(IShellItem *psi) override { return S_OK; }
    STDMETHODIMP OnItemAdded(IShellItem *psi, BOOL fIsRoot) override;
    STDMETHODIMP OnItemDeleted(IShellItem *psi, BOOL fIsRoot) override { return S_OK; }
    STDMETHODIMP OnBeforeContextMenu(IShellItem *psi, REFIID riid, void **ppv) override;
    STDMETHODIMP OnAfterContextMenu(IShellItem *psi, IContextMenu *pcmIn, REFIID riid, void **ppv) override
    {
        *ppv = NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP OnBeforeStateImageChange(IShellItem *psi) override { return E_NOTIMPL; }
    STDMETHODIMP OnGetDefaultIconIndex(IShellItem *psi, int *piDefaultIcon, int *piOpenIcon) override
    {
        return E_NOTIMPL;
    }

    // *** INameSpaceTreeControlCustomDraw ***
    STDMETHODIMP PrePaint(HDC hdc, RECT *prc, LRESULT *plres) override;
    STDMETHODIMP PostPaint(HDC hdc, RECT *prc) override { return S_OK; }
    STDMETHODIMP ItemPrePaint(HDC hdc, RECT *prc, NSTCCUSTOMDRAW *pnstccdItem, COLORREF *pclrText,
                              COLORREF *pclrTextBk, LRESULT *plres) override;
    STDMETHODIMP ItemPostPaint(HDC hdc, RECT *prc, NSTCCUSTOMDRAW *pnstccdItem) override { return S_OK; }

    // *** INameSpaceTreeOrderCallback ***
    STDMETHODIMP ShouldReorderChildItems(IShellItem *psi) override;
    STDMETHODIMP GetOrderStream(IShellItem *psi, DWORD grfMode, IStream **ppstm) override;

    // *** INameSpaceTreeCallbackPriv ***
    STDMETHODIMP OnEnumDone(IShellFolder *psf, int cItems) override { return S_OK; }
    STDMETHODIMP OnGetItemCustomState(IShellItem *psi) override { return E_NOTIMPL; }

    // *** IServiceProvider ***
    STDMETHODIMP QueryService(REFGUID guidService, REFIID riid, void **ppv) override;

    // *** IUpdateNavPaneSettings ***
    STDMETHODIMP UpdateSetting(int iSetting) override;

    // *** IDispatch ***
    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) override { return E_NOTIMPL; }
    STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr) override;

    HRESULT TranslateAcceleratorTree(_In_ MSG *pmsg);
    void SetSettingsBag(_In_opt_ IPropertyBag *ppb);

    static const DirectUI::PropertyInfo *const TopMarginProp;
    static const DirectUI::PropertyInfo *const InitialExpandProp;
    static DirectUI::IClassInfo *Class;

private:
    friend class CNavPaneNullSpaceMenu;

    void _EnsureRootsInitialized();
    void _OnNavigate();
    void _RefreshCompColorSetting();
    void _SetColors();
    void _GetSetColorProp(_In_ const DirectUI::PropertyInfo *ppi, _In_ IVisualProperties *pvp, _In_ VPCOLORFLAGS vpcf);
    HRESULT _GetViewItem(_In_ REFIID riid, _COM_Outptr_ void **ppv);
    HRESULT _GetSelectedItem(_COM_Outptr_ IShellItem **ppsi);
    BOOL _AreItemsEquivalent(_In_ IShellItem *psi1, _In_ IShellItem *psi2, _In_ BOOL fCheckTarget);
    HRESULT _NavigateTo(_In_ IShellItem *psi);
    HRESULT _ExpandItemInTree(_In_ IShellItem *psi, _In_ NSTCITEMSTATE nstcis);
    BOOL _IsItemExpanded(_In_ IShellItem *psi);
    void _OptionallyExpandParent(_In_ IShellItem *psi);
    HRESULT _InitExpandedItemArray();
    HRESULT _SaveExpandedItemArray();
    HRESULT _EnsureFavorites();
    BOOL _ShowPlaces();
    BOOL _IsInCFD();
    SHCONTF _GetEnumFlags();
    HRESULT _MaybeGetAppLocations();
    HRESULT _MaybeGetBrowserNavigationRoot();
    BOOL _IsAppLocationsExpanded();

    HWND _hwndTree;
    HWND _hwndParent;
    INameSpaceTreeControl2 *_pnstc;
    BOOL _fDestroyed;
    BOOL _fShowCompColor;
    BOOL _fRootsInitialized;
    BOOL _fExpandedOtherUsers;
    int _iShowPlaces;
    DWORD _dwAdviseCookie;
    DWORD _dwProfferCookie;
    DWORD _dwEventsCookie;
    IUnknown *_punkSite;
    IProfferService *_pps;
    IConnectionPointContainer *_pcpcBrowser;
    HACCEL _haccel;
    IPropertyBag *_ppbSettings;
    CNavPaneNullSpaceMenu *_pNullSpaceMenu;
    CNavPaneExpandedState *_pExpanded;
    IShellItem *_psiFavorites;
    IShellItem *_psiAppLocations;
    IShellItem *_psiNavRoot;
    BOOL _fNavRootChecked;
};

/* Registers every element class the frame layouts use */
HRESULT WINAPI InitializeDUIFrameClasses(VOID);

/* Frame task manager (taskmanager.cpp), reached through IObjectProvider(OID_FrameTaskManager) on the frame */

/* IFrameTaskManager::StartWorkItem flag: cancel pending items that share the task id */
#define FTMF_REPLACE    0x1

/* Base of the frame work items: DoWork runs on a worker thread, Dispatch back on the frame thread */
class CFrameWorkItem : public IFrameWorkItem, public CDUIFrameZeroAlloc
{
public:
    CFrameWorkItem();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP SetDispatchable(BOOL fDispatchable) override;
    STDMETHODIMP GetDispatchable(BOOL *pfDispatchable) override;

protected:
    virtual ~CFrameWorkItem();

    BOOL _ShouldContinue();

private:
    LONG _cRef;
    BOOL _fDispatchable;
    BOOL _fStateKnown;
    CRITICAL_SECTION _cs;
};

class CFrameTask : public CRunnableTask
{
public:
    CFrameTask(
        _In_ HWND hwndDispatch,
        _In_ IFrameWorkItem *pfwi);

    STDMETHODIMP InternalResumeRT() override;

protected:
    virtual ~CFrameTask();

private:
    IFrameWorkItem *_pfwi;
    HWND _hwndDispatch;
};

typedef struct _FRAMEWORKITEMENTRY
{
    IFrameWorkItem *pfwi;
    GUID guidTask;
} FRAMEWORKITEMENTRY;

class CFrameTaskManager : public IFrameTaskManager, public CObjectWithSite, public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP StartWorkItem(IFrameWorkItem *pfwi, REFCLSID rclsidTaskID, int dwFlags) override;
    STDMETHODIMP DispatchCompletedWorkItems() override;
    STDMETHODIMP CancelAllWorkItems() override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override;

private:
    CFrameTaskManager();
    ~CFrameTaskManager();

    static int CALLBACK s_ReleaseEntryCB(_In_ void *p, _In_opt_ void *pData);
    static int CALLBACK s_DispatchWorkItem(_In_ void *p, _In_opt_ void *pData);
    static int CALLBACK s_ReleaseItemCB(_In_ void *p, _In_opt_ void *pData);

    static LRESULT CALLBACK s_DispatchWndProc(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam);

    HRESULT _EnsureTaskScheduler();
    HRESULT _EnsureWorkItemList();
    HRESULT _EnsureDispatchWindow();

    LONG _cRef;
    IShellTaskScheduler2 *_pScheduler;
    HDSA _hdsaItems;
    HWND _hwndDispatch;
};

/* COM objects; shell32 creates them through the extern "C" entry points in exports.cpp */

class CStreamWithLayoutType : public IStream, public ILayoutType, public CDUIFrameZeroAlloc
{
public:
    CStreamWithLayoutType(_In_ IStream *pstm, _In_ LAYOUTTYPE layoutType);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override;
    STDMETHODIMP Write(const void *pv, ULONG cb, ULONG *pcbWritten) override;
    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override;
    STDMETHODIMP SetSize(ULARGE_INTEGER libNewSize) override;
    STDMETHODIMP CopyTo(IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten) override;
    STDMETHODIMP Commit(DWORD grfCommitFlags) override;
    STDMETHODIMP Revert() override;
    STDMETHODIMP LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override;
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override;
    STDMETHODIMP Stat(STATSTG *pstatstg, DWORD grfStatFlag) override;
    STDMETHODIMP Clone(IStream **ppstm) override;

    STDMETHODIMP GetLayoutType(LAYOUTTYPE *pType) override;

    static HRESULT WINAPI Create(
        _In_ IStream *pstm,
        _In_ LAYOUTTYPE layoutType,
        _COM_Outptr_ IUnknown **ppunk);

private:
    ~CStreamWithLayoutType();

    LONG _cRef;
    LAYOUTTYPE _layoutType;
    IStream *_pstm;
};

HRESULT
WINAPI
CommonLayoutDefinition_CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
CommonLayoutModifier_CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
DUIViewFrame_CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
GetLayoutDefinitionAllowRedirect(
    _In_ GETLAYOUTREASON reason,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ HWND hwnd,
    _In_ REFGUID rguidRedirectKind,
    _COM_Outptr_ IFrameLayoutDefinition **ppfld);
