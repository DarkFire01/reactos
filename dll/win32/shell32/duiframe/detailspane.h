/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Details pane elements: item thumbnail and property rows
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Shared helpers (thumbnail.cpp) */

HRESULT
WINAPI
DetailsPane_GetTaskManager(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_ IFrameTaskManager **ppftm);

BOOL
WINAPI
DetailsPane_IsPreviewPaneVisible(
    _In_opt_ IUnknown *punkSite);

BOOL
WINAPI
DetailsPane_IsReadingPaneVisible(
    _In_opt_ IUnknown *punkSite);

/* Scales prc to the largest size that fits in psizeMax with the same aspect ratio */
VOID
WINAPI
DetailsPane_FitToSize(
    _In_ const SIZE *psizeMax,
    _Inout_ RECT *prc);

/* Item thumbnail: draws the images of up to a few items, stacked */

/* IItemThumbnail::SetItems flags */
#define ITSIF_ICONSONLY         0x01
#define ITSIF_FORCEREFRESH      0x02
#define ITSIF_HIDEPENDINGICON   0x08
#define ITSIF_SINGLECOLUMN      0x10

MIDL_INTERFACE("099AB00E-1FE6-4460-9E1B-4B2567DA41CA")
IItemThumbnailCallback : public IUnknown
{
    STDMETHOD(OnNewImageAvailable)() PURE;
};

MIDL_INTERFACE("B8156BC6-2DD0-43FF-9F48-985A0DA8DC7F")
IItemThumbnail : public IUnknown
{
    STDMETHOD(SetAsync)(
        _In_ BOOL fAsync) PURE;
    STDMETHOD(SetItems)(
        _In_ IShellItemArray *psia,
        _In_ DWORD dwFlags) PURE;
    STDMETHOD(SetMultiItemLayout)(
        _In_ int dxStep,
        _In_ int dyStep,
        _In_ int cMaxItems) PURE;
    STDMETHOD(GetPrefetchTask)(
        _In_ int cx,
        _In_ int cy,
        _In_opt_ IItemThumbnailCallback *pCallback,
        _COM_Outptr_ IFrameWorkItem **ppfwi) PURE;
    STDMETHOD(GetContentBounds)(
        _In_ const RECT *prcMax,
        _Out_ RECT *prcContent) PURE;
    STDMETHOD(Draw)(
        _In_ HDC hdc,
        _In_ const RECT *prc,
        _In_ BOOL fRTL) PURE;
    STDMETHOD(GetMaxImageCount)(
        _Out_ UINT *pcMax) PURE;
    STDMETHOD(Reset)() PURE;
};

/* One item of the thumbnail; the image arrives from the fetch work item */
typedef struct _ITEMTHUMBNAILENTRY
{
    PITEMID_CHILD pidl;
    int iSysImage;
    int iCutoff;
    HBITMAP hbmImage;
    HICON hiconImage;
    SIZE sizeImage;
} ITEMTHUMBNAILENTRY;

class CItemImageFetchWorkItem;

class CItemThumbnail : public IItemThumbnail, public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ int cxMax,
        _In_ int dxStep,
        _In_ int dyStep,
        _In_ int cMaxItems,
        _In_ BOOL fAsync,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP SetAsync(BOOL fAsync) override;
    STDMETHODIMP SetItems(IShellItemArray *psia, DWORD dwFlags) override;
    STDMETHODIMP SetMultiItemLayout(int dxStep, int dyStep, int cMaxItems) override;
    STDMETHODIMP GetPrefetchTask(int cx, int cy, IItemThumbnailCallback *pCallback, IFrameWorkItem **ppfwi) override;
    STDMETHODIMP GetContentBounds(const RECT *prcMax, RECT *prcContent) override;
    STDMETHODIMP Draw(HDC hdc, const RECT *prc, BOOL fRTL) override;
    STDMETHODIMP GetMaxImageCount(UINT *pcMax) override;
    STDMETHODIMP Reset() override;

    HRESULT NotifyFetchComplete(
        _In_ UINT uGeneration,
        _In_ int cxRequested,
        _In_ HDSA hdsaImages);

private:
    friend class CItemImageFetchWorkItem;

    CItemThumbnail();
    ~CItemThumbnail();

    static int CALLBACK s_FreeEntryCB(_In_ void *p, _In_opt_ void *pData);

    HRESULT _CreateEntries(
        _In_ IShellItemArray *psia,
        _COM_Outptr_ IShellFolder **ppsf,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlParent,
        _Out_ HDSA *phdsa);
    BOOL _NewItemsEqualToCurrent(
        _In_ IShellFolder *psf,
        _In_ PCIDLIST_ABSOLUTE pidlParent,
        _In_ HDSA hdsa);
    void _ReleaseItems();
    HRESULT _GetMaxImageDimensions(_Out_ SIZE *psize);
    void _GetImageScaledRect(_In_ const ITEMTHUMBNAILENTRY *pEntry, _In_ SIZE sizeBox, _Out_ RECT *prc);

    LONG _cRef;
    BOOL _fAsync;
    BOOL _fSingleColumn;
    int _cMaxItems;
    UINT _uGeneration;
    int _dxStep;
    int _dyStep;
    int _cxMax;
    int _cxFetched;
    DWORD _dwFlags;
    PIDLIST_ABSOLUTE _pidlParent;
    IShellFolder *_psfParent;
    HDSA _hdsaItems;
};

/* Element that draws the item thumbnail and sizes itself to the images */
class CThumbnailControlHost : public DirectUI::Element
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void Paint(HDC hdc, const RECT *prcBounds, const RECT *prcInvalid, RECT *prcSkipBorder,
                       RECT *prcSkipContent);
    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);
    virtual void OnDestroy();

    HRESULT EnsureItemThumbnail();
    HRESULT SetMaxSize(
        _In_ SIZE sizeMax,
        _COM_Outptr_result_maybenull_ IFrameWorkItem **ppfwi);
    HRESULT Update();

    IItemThumbnail *GetItemThumbnail() { return _pit; }

    static DirectUI::IClassInfo *Class;

private:
    friend class CThumbnailControlCallback;

    IItemThumbnail *_pit;
    class CThumbnailControlCallback *_pCallback;
    SIZE _sizeMax;
    BOOL _fUpdating;
};

/* The thumbnail of the details pane (horizontal) or of the reading pane (vertical) */
class CPreviewThumbnailInner :
    public CElementWithSite,
    public IDispatch,
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

    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);
    virtual void OnDestroy();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) override;
    STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo) override;
    STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId) override;
    STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr) override;

    STDMETHODIMP LayoutInitialized() override;
    STDMETHODIMP Notify(PCWSTR pszPropertyName) override;
    STDMETHODIMP OnNavigateAway() override { return S_OK; }
    STDMETHODIMP LayoutUninitialized() override { return S_OK; }

    STDMETHODIMP OnSelectedItemChanged() override;
    STDMETHODIMP OnSelectionChanged() override { return S_OK; }
    STDMETHODIMP OnContentsChanged() override;
    STDMETHODIMP OnFolderChanged() override;

    HRESULT SetAndFetch(
        _In_ IShellItemArray *psia,
        _In_ BOOL fFolderItem,
        _In_ BOOL fForce,
        _In_ BOOL fRefetch);

    static DirectUI::IClassInfo *Class;

protected:
    virtual void _SelfLayoutDoLayout(int cx, int cy);
    virtual SIZE _SelfLayoutUpdateDesiredSize(int cxConstraint, int cyConstraint, DirectUI::Surface *psrf);

private:
    HRESULT _GetThumbnailControlHost(_Outptr_ CThumbnailControlHost **ppHost);
    HRESULT _UpdateItems(_In_ BOOL fForce, _In_ BOOL fRefetch);
    HRESULT _QueueUpFetchWorkItem(
        _In_ IShellItemArray *psia,
        _In_ BOOL fForce,
        _In_ BOOL fRefetch,
        _In_ UINT cMaxItems);
    HRESULT _FetchThumbnail(_In_opt_ CThumbnailControlHost *pHost, _In_ SIZE size);
    HRESULT _ConnectToView(_In_ BOOL fConnect);
    BOOL _HasSelection();
    HRESULT _LaunchContextMenu(_In_ DirectUI::MouseEvent *pme);
    void _CancelPendingWorkItem();

    IFrameWorkItem *_pfwiDevirtualize;
    IDispatch *_pdispView;
    DWORD _dwViewCookie;
    BOOL _fUpdatePending;
    BOOL _fFetchPending;
    BOOL _fVertical;
    BOOL _fAlwaysGetIconFromView;
    BOOL _fForceUseIcon;
    int _cxMaxThumb;
    SIZE _sizeFetch;
    GUID _guidFetchTask;
};

/* Details pane property rows */

/* IPropertyValue and IPropertyDescriptionPriv come from modernuipriv.h */

/* IPropertyDescriptionPriv::GetControlCLSID control types */
#define PROPCONTROL_DRAW    ((PROPDESC_CONTROL_TYPE)0)
#define PROPCONTROL_EDIT    ((PROPDESC_CONTROL_TYPE)1)

HRESULT
WINAPI
DetailsPane_CreatePropertyValue(
    _In_ REFPROPVARIANT propvar,
    _In_ REFPROPERTYKEY key,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

/* Lays the rows out in columns of equal height that fill the pane left to right */
class MetadataLayout : public DirectUI::Layout
{
public:
    static HRESULT WINAPI Create(
        _Outptr_ DirectUI::Layout **ppLayout);

    virtual void DoLayout(DirectUI::Element *peContainer, int cx, int cy);
    virtual SIZE UpdateDesiredSize(DirectUI::Element *peContainer, int cxConstraint, int cyConstraint,
                                   DirectUI::Surface *psrf);
    virtual ~MetadataLayout();

private:
    void _FreeLayoutObjects();
    void _ResetWidth();
    void _AddSpacerIfNeeded(_In_ DirectUI::Element *peContainer);
    UINT _FindChild(_In_ DirectUI::Element *pe);
    void _RemoveChildAt(_In_ UINT i);
    void _InsertChildAt(_In_ UINT i, _In_ DirectUI::Element *pe);

    DirectUI::Element *_peEditButtons;
    DirectUI::Element **_rgpeChildren;
    UINT _cChildren;
    UINT _cRowsPerColumn;
    UINT _cColumns;
    int _cyRow;
    UINT _iLastFitColumn;
    int *_rgcxColumns;
    int _cxLast;
    int _cyLast;
};

/* Label of a property row: the property's display name */
class CMetadataLabel : public DirectUI::Element
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    static const DirectUI::PropertyInfo *const ThemeSubAppProp;
    static DirectUI::IClassInfo *Class;
};

class CDisplayElement;

/* Value of a property row; hosts the resting display of the value */
class CMetadataElement : public CMetadataLabel
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnDestroy();

    HRESULT SetCurrentValue(_In_opt_ IUnknown *punkValue);
    HRESULT InitializeForDisplay(
        _In_ IPropertyDescription *ppd,
        _In_ BOOL fEditable,
        _In_ REFCLSID rclsidEditControl);

    static DirectUI::IClassInfo *Class;

private:
    HRESULT _SwitchToResting();
    HRESULT _AddRestingControl();

    IUnknown *_punkValue;
    IPropertyDescription *_ppd;
    IPropertyDescriptionPriv *_ppdp;
    BOOL _fInitialized;
    CLSID _clsidEditControl;
    CDisplayElement *_peDisplay;
    BOOL _fReadOnly;
    BOOL _fDestroyed;
};

/* Draws the formatted value of a property */
class CDisplayElement : public DirectUI::Button
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnDestroy();

    HRESULT InitializeTargetInfo(
        _In_opt_ IUnknown *punkValue,
        _In_ IPropertyDescription *ppd,
        _In_ IPropertyDescriptionPriv *ppdp,
        _In_ BOOL fReadOnly);

    void SetValueObject(_In_opt_ IUnknown *punkValue);

    static const DirectUI::PropertyInfo *const ThemeSubAppProp;
    static DirectUI::IClassInfo *Class;

private:
    IUnknown *_punkValue;
    BOOL _fReadOnly;
};

class CMetadataExtractorWorkItem;

/* Property rows of the details pane: title, item count and the folder or selection properties */
class CPreviewMetadataInner :
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
    virtual void OnInput(DirectUI::InputEvent *pie);
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

    STDMETHODIMP OnSelectedItemChanged() override;
    STDMETHODIMP OnSelectionChanged() override;
    STDMETHODIMP OnContentsChanged() override;
    STDMETHODIMP OnFolderChanged() override;

    HRESULT ClearPreviewPane();
    void SetItemsShowingMode(_In_ UINT uMode);
    void SetSelectionObjects(_In_opt_ IShellItemArray *psia);
    void CancelDelayedClear();
    UINT GetItemsShowingMode() { return _uShowingMode; }

    static DirectUI::IClassInfo *Class;

private:
    friend class CMetadataExtractorWorkItem;

    static VOID CALLBACK s_ClearPane(_In_ GMA_ACTIONINFO *pInfo);

    HRESULT _RefreshMetadata();
    HRESULT _FillPreviewPane(_In_opt_ IShellItemArray *psia, _In_ UINT cItems, _In_ UINT cSelected);
    HRESULT _QueueUpExtractionWorkItem(_In_opt_ IShellItemArray *psia, _In_ UINT cItems, _In_ UINT cSelected);
    BOOL _IsDifferentSelection(_In_opt_ IShellItemArray *psia, _In_ UINT cItems, _In_ UINT cSelected);
    HRESULT _GetSelectedCount(_Out_ UINT *pcSelected);
    HRESULT _GetNullSelectPropLists(_Outptr_ PWSTR *ppszTitleList, _Outptr_ PWSTR *ppszList);
    BOOL _EnsureNullSelectCache();
    void _ClearPaneAndSetDefaults();

    IShellItemArray *_psiaLast;
    IShellItemArray *_psiaShown;
    IPropertyStoreCache *_ppscNullSelect;
    CMetadataExtractorWorkItem *_pwiPending;
    ITrackEvents *_pte;
    HACTION _hactClear;
    UINT _uShowingMode;
    UINT _cSelectedLast;
    UINT _cItemsLast;
    UINT _uNullSelectCacheCount;
    BOOL _fReadOnly;
    BOOL _fRefreshPending;
    BOOL _fTracking;
    BOOL _fForceRefresh;
};

/* Builds the property rows on a worker thread, then fills the pane on the frame thread */
class CMetadataExtractorWorkItem : public CFrameWorkItem
{
public:
    CMetadataExtractorWorkItem();

    HRESULT SetInfo(
        _In_ CPreviewMetadataInner *pInner,
        _In_ BOOL fReadOnly,
        _In_ PCWSTR pszNullSelectList,
        _In_ PCWSTR pszNullSelectTitleList,
        _In_opt_ IPropertyStoreCache *ppscNullSelect,
        _In_opt_ IShellItemArray *psia,
        _In_ UINT cItems,
        _In_ UINT cSelected,
        _In_ IUnknown *punkSite);
    void DetachInner() { _pInner = NULL; }
    UINT GetItemCount() { return _cItems; }
    BOOL HasSelection() { return _cSelected != 0; }

    STDMETHODIMP DoWork() override;
    STDMETHODIMP Dispatch() override;

protected:
    virtual ~CMetadataExtractorWorkItem();

private:
    void _ExtractSelection(_In_ UINT cSelected, _In_ IShellItemArray *psia);
    void _ExtractNullSelection(_In_ UINT cItems);
    HRESULT _AddTitleToPropArray(
        _In_ IShellItem *psi,
        _In_ IPropertyStore *pps,
        _In_ IPropertyDescription *ppd);
    HRESULT _AddFileCount(_In_ UINT cItems);
    HRESULT _AddKeyAndValueToPropArray(
        _In_ REFPROPERTYKEY key,
        _In_ REFPROPVARIANT propvar,
        _In_ PROPDESC_VIEW_FLAGS pdvf,
        _In_ BOOL fReadOnly);
    HRESULT _AddToPropArray(
        _In_ IPropertyStore *pps,
        _In_ IPropertyDescription *ppd,
        _In_opt_ IPropertyStoreCache *ppscCache,
        _Inout_opt_ BOOL *pfAnyEditable);
    void _AddListToPropArray(
        _In_ IPropertyStore *pps,
        _In_ IPropertyDescriptionList *ppdl,
        _In_opt_ IPropertyStoreCache *ppscCache,
        _Inout_opt_ BOOL *pfAnyEditable);
    BOOL _ComputeItemReadOnly(_In_ IShellItemArray *psia);
    HRESULT _GetKeyAt(_In_ UINT i, _Out_ PROPERTYKEY *pkey);
    void _RemoveKey(_In_ REFPROPERTYKEY key);
    UINT _AddTitle(
        _In_ BOOL fHasTitle,
        _In_ UINT iKey,
        _In_ PCWSTR pszResID,
        _In_ PCWSTR pszControlID,
        _In_ DirectUI::DUIXmlParser *pParser);
    HRESULT _AddTitleRow(
        _In_ REFPROPERTYKEY key,
        _In_ REFPROPVARIANT propvar,
        _In_ PCWSTR pszResID,
        _In_ PCWSTR pszControlID,
        _In_ BOOL fReadOnly,
        _In_ DirectUI::DUIXmlParser *pParser);

    CPreviewMetadataInner *_pInner;
    IPropertyStoreCache *_ppscNullSelect;
    IPropertyStoreCache *_ppscValues;
    HDSA _hdsaKeys;
    ITrackEvents *_pte;
    BOOL _fTrackEnded;
    UINT _uReadOnlyState;
    BOOL _fAnyEditableRow;
    UINT _cTitles;
    PWSTR _pszNullSelectList;
    PWSTR _pszNullSelectTitleList;
    PIDLIST_ABSOLUTE _pidlFolder;
    IShellItemArray *_psiaSelection;
    UINT _cItems;
    UINT _cSelected;
};
