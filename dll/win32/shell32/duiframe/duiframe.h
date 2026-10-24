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

/* Registers every element class the frame layouts use */
HRESULT WINAPI InitializeDUIFrameClasses(VOID);

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
