/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Reading, info and library pane elements
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <hlink.h>

/* Preview handler interfaces (public Windows SDK shobjidl) that the ReactOS SDK lacks */

typedef struct PREVIEWHANDLERFRAMEINFO
{
    HACCEL haccel;
    UINT cAccelEntries;
} PREVIEWHANDLERFRAMEINFO;

MIDL_INTERFACE("8895B1C6-B41F-4C1C-A562-0D564250836F")
IPreviewHandler : public IUnknown
{
    STDMETHOD(SetWindow)(
        _In_ HWND hwnd,
        _In_ const RECT *prc) PURE;
    STDMETHOD(SetRect)(
        _In_ const RECT *prc) PURE;
    STDMETHOD(DoPreview)() PURE;
    STDMETHOD(Unload)() PURE;
    STDMETHOD(SetFocus)() PURE;
    STDMETHOD(QueryFocus)(
        _Out_ HWND *phwnd) PURE;
    STDMETHOD(TranslateAccelerator)(
        _In_ MSG *pmsg) PURE;
};

MIDL_INTERFACE("FEC87AAF-35F9-447A-ADB7-20234491401A")
IPreviewHandlerFrame : public IUnknown
{
    STDMETHOD(GetWindowContext)(
        _Out_ PREVIEWHANDLERFRAMEINFO *pinfo) PURE;
    STDMETHOD(TranslateAccelerator)(
        _In_ MSG *pmsg) PURE;
};

MIDL_INTERFACE("196BF9A5-B346-4EF0-AA1E-5DCDB76768B1")
IPreviewHandlerVisuals : public IUnknown
{
    STDMETHOD(SetBackgroundColor)(
        _In_ COLORREF color) PURE;
    STDMETHOD(SetFont)(
        _In_ const LOGFONTW *plf) PURE;
    STDMETHOD(SetTextColor)(
        _In_ COLORREF color) PURE;
};

MIDL_INTERFACE("7F73BE3F-FB79-493C-A6C7-7EE14E245841")
IInitializeWithItem : public IUnknown
{
    STDMETHOD(Initialize)(
        _In_ IShellItem *psi,
        _In_ DWORD grfMode) PURE;
};

/* Shell private interfaces, named after the public symbols */

/* Payload of IPreviewHostCallback::OnError and of the reading pane's PreviewError event */
typedef struct NotifyError
{
    HRESULT hrReason;
    CLSID clsidPreviewer;
} NotifyError;

MIDL_INTERFACE("151E7011-888E-47FA-A9E6-B5CDD5534B53")
IPreviewHost : public IUnknown
{
    STDMETHOD(Load)(
        _In_ IShellItem *psi) PURE;
    STDMETHOD(Unload)() PURE;
    STDMETHOD(DoPreview)() PURE;
    STDMETHOD(WindowSized)() PURE;
    STDMETHOD(SetFocus)() PURE;
    STDMETHOD(QueryWindowFocus)(
        _Out_ HWND *phwnd) PURE;
};

MIDL_INTERFACE("7A77FFE8-53BA-4A10-BE4F-255489A49451")
IPreviewHostCallback : public IUnknown
{
    STDMETHOD(OnLoad)(
        _In_ HRESULT hr) PURE;
    STDMETHOD(OnShow)(
        _In_ HRESULT hr) PURE;
    STDMETHOD(OnUnload)(
        _In_ HRESULT hr) PURE;
    STDMETHOD(OnTranslateAccelerator)(
        _In_ const MSG *pmsg,
        _Out_ HRESULT *phr) PURE;
    STDMETHOD(OnError)(
        _In_ const NotifyError *pError) PURE;
};

/* Implemented by folders that put a title and a link line above the view */
MIDL_INTERFACE("38698B65-1CA7-458C-B4D6-E0A51379C1D2")
IInfoPaneProvider : public IUnknown
{
    STDMETHOD(GetTitle)(
        _Outptr_ PWSTR *ppszTitle) PURE;
    STDMETHOD(GetDescription)(
        _Outptr_ PWSTR *ppszDescription) PURE;
    STDMETHOD(LinkClicked)() PURE;
};

EXTERN_C const IID IID_IPreviewHandler;
EXTERN_C const IID IID_IPreviewHandlerFrame;
EXTERN_C const IID IID_IPreviewHandlerVisuals;
EXTERN_C const IID IID_IInitializeWithItem;
EXTERN_C const IID IID_IPreviewHost;
EXTERN_C const IID IID_IPreviewHostCallback;
EXTERN_C const IID IID_IInfoPaneProvider;
EXTERN_C const CLSID CLSID_PreviewWindowCreator;
EXTERN_C const GUID TOID_ReadingPaneUpdateTask;
EXTERN_C const GUID TOID_ReadingPaneHostUpdateTask;

/* Reading pane status HRESULTs carried by NotifyError and the cover sheets */
#define PREVIEW_E_FILEINUSE         ((HRESULT)0x80041200L)
#define PREVIEW_E_CANNOTPREVIEW     ((HRESULT)0x80041201L)
#define PREVIEW_E_PREVIEWERFAILED   ((HRESULT)0x80041202L)
#define PREVIEW_E_LOCATION          ((HRESULT)0x80041203L)
#define PREVIEW_E_NOPREVIEW         ((HRESULT)0x80041204L)
#define PREVIEW_E_UNSAFE            ((HRESULT)0x80041205L)

/* Info pane: title and link text a folder supplies through IInfoPaneProvider */
class CInfoPaneInner :
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

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CElementWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override { return S_OK; }
    STDMETHODIMP OnNavigateAway() override { return S_OK; }
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    STDMETHODIMP OnSelectedItemChanged() override { return S_OK; }
    STDMETHODIMP OnSelectionChanged() override { return S_OK; }
    STDMETHODIMP OnContentsChanged() override;
    STDMETHODIMP OnFolderChanged() override { return S_OK; }

    static DirectUI::IClassInfo *Class;

private:
    static BOOL WINAPI s_LinkNotifyHandler(
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam,
        _Out_ LRESULT *plResult,
        _In_opt_ PVOID pvData);

    HRESULT _SetText();
};

/* Lays out the library title, the location line and the arrange-by control in one band */
class LibraryPaneLayout : public DirectUI::Layout
{
public:
    void DoLayout(DirectUI::Element *peContainer, int cx, int cy) override;
    SIZE UpdateDesiredSize(DirectUI::Element *peContainer, int cxConstraint, int cyConstraint,
                           DirectUI::Surface *psrf) override;
    DirectUI::Element *GetAdjacent(DirectUI::Element *peContainer, DirectUI::Element *peFrom, int iNavDir,
                                   DirectUI::NavReference const *pnr, unsigned long dwFlags) override;

private:
    void _DoLayoutWorker(
        _In_ DirectUI::Element *peContainer,
        _In_ const RECT *prc);
};

/* Places the only child at the origin with its own width and height, clipping the rest */
class ClipLayout : public DirectUI::Layout
{
public:
    void DoLayout(DirectUI::Element *peContainer, int cx, int cy) override;
    SIZE UpdateDesiredSize(DirectUI::Element *peContainer, int cxConstraint, int cyConstraint,
                           DirectUI::Surface *psrf) override;
};

/* What the library pane shows instead of a library: the Libraries folder or the homegroup */
typedef enum _LIBRARYPANESPECIAL
{
    LPS_NONE = 0,
    LPS_LIBRARIES = 1,
    LPS_HOMEGROUP = 2,
    LPS_HOMEGROUPMEMBER = 3
} LIBRARYPANESPECIAL;

/* Library pane: library name, its locations and the arrange-by control above the view */
class CLibraryPaneModuleInner :
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
    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CElementWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;
    STDMETHODIMP OnNavigateAway() override;
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    STDMETHODIMP OnSelectedItemChanged() override { return S_OK; }
    STDMETHODIMP OnSelectionChanged() override { return S_OK; }
    STDMETHODIMP OnContentsChanged() override;
    STDMETHODIMP OnFolderChanged() override { return OnContentsChanged(); }

    static DirectUI::IClassInfo *Class;

private:
    HRESULT _SetInfo();
    HRESULT _EnsurePane();
    HRESULT _GetLibraryFromItem(
        _In_ IShellItem *psi,
        _COM_Outptr_ IShellItem **ppsiLibrary);
    LIBRARYPANESPECIAL _SpecialLibraryPane(
        _In_ IShellItem *psi);
    HRESULT _GetDisplayTitle(
        _Outptr_ PWSTR *ppszTitle);
    HRESULT _SetLocationInfo();
    HRESULT _GetSubText(
        _In_ IShellItem *psi,
        _Outptr_ PWSTR *ppszText);
    void _ShowLocationButton(
        _In_ BOOL fShow);
    HRESULT _InitializeTopViewControl();

    IShellItem *_psiLibrary;
    BOOL _fLayoutInitialized;
    LIBRARYPANESPECIAL _special;
    DirectUI::Element *_peTopView;
};

class CPreviewHost;
class CReadingPaneModuleHost;

/* Reports preview host progress back to the frame's property bag */
class CReadingPaneCallback : public IPreviewHostCallback, public CObjectWithSite, public CDUIFrameZeroAlloc
{
public:
    CReadingPaneCallback(
        _In_ HWND hwndHost);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP OnLoad(HRESULT hr) override;
    STDMETHODIMP OnShow(HRESULT hr) override;
    STDMETHODIMP OnUnload(HRESULT hr) override;
    STDMETHODIMP OnTranslateAccelerator(const MSG *pmsg, HRESULT *phr) override;
    STDMETHODIMP OnError(const NotifyError *pError) override;

private:
    virtual ~CReadingPaneCallback() {}

    LONG _cRef;
    HWND _hwndHost;
};

/* Window the preview handlers draw into */
class CReadingPaneModuleHost : public DirectUI::HWNDHost
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
    virtual void OnDestroy();
    virtual void OnKeyFocusMoved(DirectUI::Element *peFrom, DirectUI::Element *peTo);

    void Update(
        _In_ BOOL fSelectedItemChanged);

    static UID WINAPI PreviewError();
    static DirectUI::IClassInfo *Class;

private:
    friend class CReadingPaneModuleInner;
    friend class CReadingPaneHostUpdateWorkItem;

    static LRESULT CALLBACK s_ModuleHostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HRESULT _QueueUpdateWorkItem(
        _In_ BOOL fSelectedItemChanged,
        _In_ IPropertyBag *ppb,
        _In_ IShellItemArray *psia);
    void _UnloadCurrentPreviewer(
        _In_ IPropertyBag *ppb);

    IPreviewHost *_pph;
    IPreviewHostCallback *_pphc;
    BOOL _fUpdatePending;
    BOOL _fDispatching;
    IUnknown *_punkSite;
    IShellItem *_psiPreviewed;
    IFrameWorkItem *_pfwiPending;
};

/* Payload of CReadingPaneModuleHost::PreviewError */
typedef struct _PREVIEWERROREVENT : DirectUI::Event
{
    NotifyError error;
} PREVIEWERROREVENT;

/* Preview pane content: preview handler host plus the status cover sheets */
class CReadingPaneModuleInner :
    public CElementWithSite,
    public IFrameShellViewClient,
    public IFrameNotificationClient,
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

    virtual void OnEvent(DirectUI::Event *pev);
    virtual void OnDestroy();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP OnSelectedItemChanged() override;
    STDMETHODIMP OnSelectionChanged() override;
    STDMETHODIMP OnContentsChanged() override;
    STDMETHODIMP OnFolderChanged() override { return S_OK; }

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;
    STDMETHODIMP OnNavigateAway() override;
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    STDMETHODIMP UIActivateIO(BOOL fActivate, MSG *pmsg) override { return E_NOTIMPL; }
    STDMETHODIMP HasFocusIO() override;
    STDMETHODIMP TranslateAcceleratorIO(MSG *pmsg) override { return E_NOTIMPL; }

    static DirectUI::IClassInfo *Class;

private:
    friend class CReadingPaneUpdateWorkItem;

    DirectUI::Element *_EnsureCoverPage(
        _In_ PCWSTR pszID);
    HRESULT _Update();
    HRESULT _QueueUpdateWorkItem(
        _In_ IShellItemArray *psia);
    void _UpdateCoverSheets(
        _In_ int iState,
        _In_opt_ PCWSTR pszText,
        _In_ HRESULT hrStatus);

    CReadingPaneModuleHost *_pHost;
    DirectUI::Element *_peNoSelSheet;
    DirectUI::Element *_peReadingPane;
    IFrameWorkItem *_pfwiPending;
    DirectUI::Element *_peTransitionSheet;
    BOOL _fPaneVisible;
    BOOL _fCoverShown;
    HRESULT _hrStatus;
};

/* Picks the item to preview off the frame thread and decides which cover sheet shows */
class CReadingPaneUpdateWorkItem : public CFrameWorkItem
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ CReadingPaneModuleInner *pInner,
        _In_ IShellItemArray *psia,
        _COM_Outptr_ IFrameWorkItem **ppfwi);

    STDMETHODIMP DoWork() override;
    STDMETHODIMP Dispatch() override;

protected:
    ~CReadingPaneUpdateWorkItem();

private:
    IStream *_pstmItems;
    IStream *_pstmItem;
    CReadingPaneModuleInner *_pInner;
};

/* Picks the item to preview off the frame thread and hands it to the preview host */
class CReadingPaneHostUpdateWorkItem : public CFrameWorkItem
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ CReadingPaneModuleHost *pHost,
        _In_ BOOL fSelectedItemChanged,
        _In_ IPropertyBag *ppb,
        _In_ IShellItemArray *psia,
        _COM_Outptr_ IFrameWorkItem **ppfwi);

    STDMETHODIMP DoWork() override;
    STDMETHODIMP Dispatch() override;

protected:
    ~CReadingPaneHostUpdateWorkItem();

private:
    IStream *_pstmItems;
    IStream *_pstmItem;
    BOOL _fItemReady;
    BOOL _fSelectedItemChanged;
    CReadingPaneModuleHost *_pHost;
    IPropertyBag *_ppb;
};

class CRichPreviewThreadData;

/* Runs each preview handler class on its own thread and reports back through a message window */
class CPreviewHost : public IPreviewHost, public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ HINSTANCE hinst,
        _In_ HWND hwndHost,
        _In_ IPreviewHostCallback *pphc,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Load(IShellItem *psi) override;
    STDMETHODIMP Unload() override;
    STDMETHODIMP DoPreview() override;
    STDMETHODIMP WindowSized() override;
    STDMETHODIMP SetFocus() override;
    STDMETHODIMP QueryWindowFocus(HWND *phwnd) override;

private:
    CPreviewHost();
    ~CPreviewHost();

    static LRESULT CALLBACK s_MsgWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HRESULT _CreateMsgWindow();
    CRichPreviewThreadData *_FindInCache(
        _In_ REFCLSID rclsid);
    HRESULT _CreateAndAddToCache(
        _In_ REFCLSID rclsid,
        _Outptr_ CRichPreviewThreadData **ppData);

    LONG _cRef;
    HINSTANCE _hinst;
    IPreviewHostCallback *_pphc;
    UINT _uCookie;
    UINT _uUnloadCookie;
    CRichPreviewThreadData *_pCurrent;
    HDPA _hdpaCache;
    HWND _hwndMsg;
    HWND _hwndHost;
};

/* Frame thread view of one preview handler thread */
class CRichPreviewThreadData : public CDUIFrameZeroAlloc
{
public:
    CRichPreviewThreadData() : _cRef(1) {}

    ULONG AddRef();
    ULONG Release();
    HRESULT _PostMessageToBGThread(
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);

    LONG _cRef;
    CLSID _clsid;
    HWND _hwndPreviewer;
    HWND _hwndBG;
    HANDLE _hevIdle;
};

class CRichPreviewBackgroundThreadData;

/* Handed to a new preview handler thread; filled in by its start-up code */
class CRichPreviewThreadParams : public CDUIFrameZeroAlloc
{
public:
    CRichPreviewThreadParams() : _cRef(1) {}

    ULONG AddRef();
    ULONG Release();

    HINSTANCE _hinst;
    CLSID _clsid;
    HWND _hwndFGMsg;
    HWND _hwndHost;
    CRichPreviewBackgroundThreadData *_pData;
    HRESULT _hr;
    HWND _hwndBG;
    HWND _hwndPreviewer;
    HANDLE _hevIdle;
    LONG _cRef;
};

/* Lives on the preview handler thread and hosts the handler as its frame */
class CRichPreviewBackgroundThreadData :
    public IPreviewHandlerFrame,
    public IHlinkFrame,
    public IServiceProvider,
    public CDUIFrameZeroAlloc
{
public:
    CRichPreviewBackgroundThreadData() : _cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO *pinfo) override;
    STDMETHODIMP TranslateAccelerator(MSG *pmsg) override;

    STDMETHODIMP SetBrowseContext(IHlinkBrowseContext *pihlbc) override { return E_NOTIMPL; }
    STDMETHODIMP GetBrowseContext(IHlinkBrowseContext **ppihlbc) override { return E_NOTIMPL; }
    STDMETHODIMP Navigate(DWORD grfHLNF, LPBC pbc, IBindStatusCallback *pibsc, IHlink *pihlNavigate) override;
    STDMETHODIMP OnNavigate(DWORD grfHLNF, IMoniker *pimkTarget, LPCWSTR pwzLocation, LPCWSTR pwzFriendlyName,
                            DWORD dwreserved) override { return E_NOTIMPL; }
    STDMETHODIMP UpdateHlink(ULONG uHLID, IMoniker *pimkTarget, LPCWSTR pwzLocation,
                             LPCWSTR pwzFriendlyName) override { return E_NOTIMPL; }

    STDMETHODIMP QueryService(REFGUID guidService, REFIID riid, void **ppv) override;

    static DWORD WINAPI s_SyncThreadProc(_In_ void *pv);
    static DWORD WINAPI s_ThreadProc(_In_ void *pv);

private:
    ~CRichPreviewBackgroundThreadData();

    static LRESULT CALLBACK s_BGWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HRESULT _InitializePreviewerWindow();
    HRESULT _InitializeMessageWindow();
    void _NotifyError(
        _In_ HRESULT hrReason);
    BOOL _InitializeWithStream(
        _In_ IShellItem *psi,
        _In_ IInitializeWithStream *piws);
    HRESULT _InitializePreviewer(
        _In_ IShellItem *psi);
    void _DoLoadFile(
        _In_ IStream *pstm,
        _In_ UINT uCookie);
    void _DoShowFile();
    void _CleanupTempFile();
    HRESULT _UnloadNow();
    void _DoWindowResized();

    HWND _hwndFGMsg;
    HWND _hwndHost;
    IOleWindow *_powCreator;
    HWND _hwndPreviewer;
    HWND _hwndBG;
    LONG _cRef;
    BOOL _fLoaded;
    CLSID _clsid;
    HINSTANCE _hinst;
    UINT _uCookie;
    IPreviewHandler *_pph;
    PWSTR _pszTempFile;
    HANDLE _hevIdle;
    BOOL _fOwnPreviewerWindow;
};

/* Asks the handler thread which window has focus and waits for the answer */
class CQueryFocusParams : public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _Outptr_ CQueryFocusParams **ppParams);

    ULONG AddRef();
    ULONG Release();

    HWND _hwndFocus;
    HRESULT _hr;
    HANDLE _hevDone;
    LONG _cRef;
};
