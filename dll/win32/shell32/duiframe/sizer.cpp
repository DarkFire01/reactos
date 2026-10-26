/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Sizer element that resizes, shows and hides a frame pane
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define SIZER_PROPFLAGS         2
#define SIZER_PROPFLAGS_INIT    6

#define PROPERTY_STORE_SPECIFIED 1
#define PROPERTY_STORE_COMPUTED  2

DirectUI::IClassInfo *CDUISizerElement::Class = NULL;

static const DirectUI::_StaticValue c_svIntMinusOne = { (int)DirectUI::ValueType::Int, 0, -1, -1 };
static const DirectUI::_StaticValue c_svIntOne = { (int)DirectUI::ValueType::Int, 0, -1, 1 };

static DirectUI::Value *WINAPI svIntMinusOne() { return (DirectUI::Value *)&c_svIntMinusOne; }
static DirectUI::Value *WINAPI svIntOne() { return (DirectUI::Value *)&c_svIntOne; }

static DirectUI::PropCapability s_pcAtom = { DirectUI::ValueType::Atom, 0, UINT_MAX };
static DirectUI::PropCapability s_pcBool = { DirectUI::ValueType::Bool, 0, UINT_MAX };
static DirectUI::PropCapability s_pcInt = { DirectUI::ValueType::Int, 0, UINT_MAX };
static DirectUI::PropCapability s_pcString = { DirectUI::ValueType::String, 0, UINT_MAX };

static DirectUI::PropertyInfoData s_pidSizingTarget;
static DirectUI::PropertyInfoData s_pidReservedTarget;
static DirectUI::PropertyInfoData s_pidReservedMaster;
static DirectUI::PropertyInfoData s_pidSlidesUpAndDown;
static DirectUI::PropertyInfoData s_pidInitialVisibility;
static DirectUI::PropertyInfoData s_pidInitialMaxxedOut;
static DirectUI::PropertyInfoData s_pidDownOrRightGrowsTarget;
static DirectUI::PropertyInfoData s_pidInitialSize;
static DirectUI::PropertyInfoData s_pidMinSize;
static DirectUI::PropertyInfoData s_pidReservedSpace;
static DirectUI::PropertyInfoData s_pidFramePersistType;
static DirectUI::PropertyInfoData s_pidDisablingPolicy;
static DirectUI::PropertyInfoData s_pidExplorerVisibility;
static DirectUI::PropertyInfoData s_pidGrowTargetFirst;
static DirectUI::PropertyInfoData s_pidPrioritySizer;

static const DirectUI::PropertyInfo s_piSizingTarget =
    { L"SizingTarget", SIZER_PROPFLAGS, 0, &s_pcAtom, NULL, DirectUI::Value::GetAtomZero, &s_pidSizingTarget };
static const DirectUI::PropertyInfo s_piReservedTarget =
    { L"ReservedTarget", SIZER_PROPFLAGS, 0, &s_pcAtom, NULL, DirectUI::Value::GetAtomZero, &s_pidReservedTarget };
static const DirectUI::PropertyInfo s_piReservedMaster =
    { L"ReservedMaster", SIZER_PROPFLAGS_INIT, 0, &s_pcAtom, NULL, DirectUI::Value::GetAtomZero,
      &s_pidReservedMaster };
static const DirectUI::PropertyInfo s_piSlidesUpAndDown =
    { L"SlidesUpAndDown", SIZER_PROPFLAGS, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolTrue, &s_pidSlidesUpAndDown };
static const DirectUI::PropertyInfo s_piInitialVisibility =
    { L"InitialVisibility", SIZER_PROPFLAGS_INIT, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolTrue,
      &s_pidInitialVisibility };
static const DirectUI::PropertyInfo s_piInitialMaxxedOut =
    { L"InitialMaxxedOut", SIZER_PROPFLAGS_INIT, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse,
      &s_pidInitialMaxxedOut };
static const DirectUI::PropertyInfo s_piDownOrRightGrowsTarget =
    { L"DownOrRightGrowsTarget", SIZER_PROPFLAGS, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolTrue,
      &s_pidDownOrRightGrowsTarget };
static const DirectUI::PropertyInfo s_piInitialSize =
    { L"InitialSize", SIZER_PROPFLAGS_INIT, 0, &s_pcInt, NULL, svIntMinusOne, &s_pidInitialSize };
static const DirectUI::PropertyInfo s_piMinSize =
    { L"MinSize", SIZER_PROPFLAGS_INIT, 0, &s_pcInt, NULL, svIntMinusOne, &s_pidMinSize };
static const DirectUI::PropertyInfo s_piReservedSpace =
    { L"ReservedSpace", SIZER_PROPFLAGS_INIT, 0, &s_pcInt, NULL, svIntMinusOne, &s_pidReservedSpace };
static const DirectUI::PropertyInfo s_piFramePersistType =
    { L"FramePersistType", SIZER_PROPFLAGS_INIT, 0, &s_pcInt, NULL, svIntOne, &s_pidFramePersistType };
static const DirectUI::PropertyInfo s_piDisablingPolicy =
    { L"DisablingPolicy", SIZER_PROPFLAGS_INIT, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull,
      &s_pidDisablingPolicy };
static const DirectUI::PropertyInfo s_piExplorerVisibility =
    { L"ExplorerVisibility", SIZER_PROPFLAGS_INIT, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull,
      &s_pidExplorerVisibility };
static const DirectUI::PropertyInfo s_piGrowTargetFirst =
    { L"GrowTargetFirst", SIZER_PROPFLAGS_INIT, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse,
      &s_pidGrowTargetFirst };
static const DirectUI::PropertyInfo s_piPrioritySizer =
    { L"PrioritySizer", SIZER_PROPFLAGS, 0, &s_pcAtom, NULL, DirectUI::Value::GetAtomZero, &s_pidPrioritySizer };

static const DirectUI::PropertyInfo *const c_rgpiSizer[] =
{
    &s_piSizingTarget,
    &s_piReservedTarget,
    &s_piReservedMaster,
    &s_piSlidesUpAndDown,
    &s_piInitialVisibility,
    &s_piInitialMaxxedOut,
    &s_piDownOrRightGrowsTarget,
    &s_piInitialSize,
    &s_piMinSize,
    &s_piReservedSpace,
    &s_piFramePersistType,
    &s_piDisablingPolicy,
    &s_piExplorerVisibility,
    &s_piGrowTargetFirst,
    &s_piPrioritySizer,
};

const DirectUI::PropertyInfo *const CDUISizerElement::SizingTargetProp = &s_piSizingTarget;
const DirectUI::PropertyInfo *const CDUISizerElement::ReservedTargetProp = &s_piReservedTarget;
const DirectUI::PropertyInfo *const CDUISizerElement::ReservedMasterProp = &s_piReservedMaster;
const DirectUI::PropertyInfo *const CDUISizerElement::SlidesUpAndDownProp = &s_piSlidesUpAndDown;
const DirectUI::PropertyInfo *const CDUISizerElement::InitialVisibilityProp = &s_piInitialVisibility;
const DirectUI::PropertyInfo *const CDUISizerElement::InitialMaxxedOutProp = &s_piInitialMaxxedOut;
const DirectUI::PropertyInfo *const CDUISizerElement::DownOrRightGrowsTargetProp = &s_piDownOrRightGrowsTarget;
const DirectUI::PropertyInfo *const CDUISizerElement::InitialSizeProp = &s_piInitialSize;
const DirectUI::PropertyInfo *const CDUISizerElement::MinSizeProp = &s_piMinSize;
const DirectUI::PropertyInfo *const CDUISizerElement::ReservedSpaceProp = &s_piReservedSpace;
const DirectUI::PropertyInfo *const CDUISizerElement::FramePersistTypeProp = &s_piFramePersistType;
const DirectUI::PropertyInfo *const CDUISizerElement::DisablingPolicyProp = &s_piDisablingPolicy;
const DirectUI::PropertyInfo *const CDUISizerElement::ExplorerVisibilityProp = &s_piExplorerVisibility;
const DirectUI::PropertyInfo *const CDUISizerElement::GrowTargetFirstProp = &s_piGrowTargetFirst;
const DirectUI::PropertyInfo *const CDUISizerElement::PrioritySizerProp = &s_piPrioritySizer;

HRESULT
WINAPI
CDUISizerElement::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CDUISizerElement *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CDUISizerElement>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    *ppElement = pe;
    return pe->SetActive(1);
}

HRESULT
WINAPI
CDUISizerElement::Register()
{
    return DirectUI::ClassInfo<CDUISizerElement, CElementWithSite,
                               DirectUI::StandardCreator<CDUISizerElement>>::Register(
        DUIFRAME_HINST, L"Sizer", c_rgpiSizer, _countof(c_rgpiSizer), false);
}

STDMETHODIMP
CDUISizerElement::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CDUISizerElement, IFrameNotificationClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

int
CDUISizerElement::_GetIntProp(
    _In_ const DirectUI::PropertyInfo *ppi)
{
    DirectUI::Value *pv = GetValue(ppi, PROPERTY_STORE_COMPUTED, NULL);
    int i = pv->GetInt();

    pv->Release();
    return i;
}

BOOL
CDUISizerElement::_GetBoolProp(
    _In_ const DirectUI::PropertyInfo *ppi)
{
    DirectUI::Value *pv = GetValue(ppi, PROPERTY_STORE_COMPUTED, NULL);
    BOOL f = pv->GetBool();

    pv->Release();
    return f;
}

ATOM
CDUISizerElement::_GetAtomProp(
    _In_ const DirectUI::PropertyInfo *ppi)
{
    DirectUI::Value *pv = GetValue(ppi, PROPERTY_STORE_COMPUTED, NULL);
    ATOM atom = pv->GetAtom();

    pv->Release();
    return atom;
}

/* The element's extent along the direction this sizer moves */
int
CDUISizerElement::_GetExtentAlongAxis(
    _In_ DirectUI::Element *pe)
{
    DirectUI::Value *pv = pe->GetValue(DirectUI::Element::ExtentProp, PROPERTY_STORE_SPECIFIED, NULL);
    int cx = _IsVertical() ? pv->GetSize()->cy : pv->GetSize()->cx;

    pv->Release();
    return cx;
}

STDMETHODIMP
CDUISizerElement::SetSite(
    IUnknown *punkSite)
{
    IUnknown_SafeReleaseAndNullPtr(&_ppbBrowser);
    IUnknown_Set(&_punkSite, punkSite);
    return IUnknown_QueryService(punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&_ppbBrowser));
}

/* The browser can pin the pane open or closed; its initial state counts only when forced */
void
CDUISizerElement::_CheckPaneVisibility()
{
    DirectUI::Value *pv;
    PCWSTR pszPane;
    GUID guidPane;

    _dwPaneState = 0;
    pv = GetValue(ExplorerVisibilityProp, PROPERTY_STORE_COMPUTED, NULL);
    pszPane = pv->GetString();
    if (pv != DirectUI::Value::GetUnset() && pszPane && GUIDFromStringW(pszPane, &guidPane))
    {
        IUnknown_GetExplorerPaneState(_punkSite, &guidPane, (EXPLORERPANESTATE *)&_dwPaneState);
        if (!_fSkipPaneStateOverride && (_dwPaneState & EPS_INITIALSTATE))
            _dwPaneState = 0;
        _fSkipPaneStateOverride = FALSE;
    }
    pv->Release();
}

/* Room for the target: the container minus the reserved master and what that master's own sizer holds */
HRESULT
CDUISizerElement::_GetLogicalContainerSize(
    _Out_ int *pcx)
{
    int cx = _GetExtentAlongAxis(_peContainer);

    if (_peReserved)
    {
        cx -= _GetExtentAlongAxis(_peReserved);

        if (_peReserved->GetClassInfoW()->IsSubclassOf(Class))
        {
            CDUISizerElement *pseReserved = static_cast<CDUISizerElement *>(_peReserved);

            if (pseReserved->_peTarget)
            {
                int cxHeld = pseReserved->_GetExtentAlongAxis(pseReserved->_peTarget);

                if (!cxHeld)
                    cxHeld = pseReserved->_cxLastRendered;
                cx -= cxHeld;
            }
        }
    }

    SHPhysicalToLogicalDPI(&cx, NULL);
    *pcx = cx;
    return S_OK;
}

HRESULT
CDUISizerElement::_ReadSizeFromPropBag(
    _Out_ int *pcx)
{
    if (FAILED(SHPropertyBag_ReadInt(_ppbBrowser, _szSizeProp, pcx)))
        *pcx = _GetIntProp(InitialSizeProp);
    return S_OK;
}

/* Applies maximize, visibility, reserved space and the minimum to a requested size */
int
CDUISizerElement::_ComputeBoundedSize(
    _In_ int cx)
{
    int cxReserved;
    int cxMin;
    BOOL fFlag;

    if (_ppbBrowser && SUCCEEDED(SHPropertyBag_ReadBOOL(_ppbBrowser, _szMaxxedOutProp, &fFlag)) && fFlag)
        cx = INT_MAX;

    cxReserved = _GetIntProp(ReservedSpaceProp);
    cxMin = _GetIntProp(MinSizeProp);

    fFlag = TRUE;
    if (_ppbBrowser && SUCCEEDED(SHPropertyBag_ReadBOOL(_ppbBrowser, _szVisibleProp, &fFlag)) && !fFlag)
        return 0;

    BOOL fCollapsed = FALSE;
    int cxContainer = -1;

    if (cxReserved != -1 && SUCCEEDED(_GetLogicalContainerSize(&cxContainer)) && cxContainer > 0)
    {
        int cxAvailable = cxContainer - cxReserved;

        /* Without room for the minimum the pane collapses instead of squeezing the content */
        if (cxAvailable < 0 || cxAvailable < cxMin)
        {
            cx = 0;
            fCollapsed = TRUE;
        }
        else if (cx > cxAvailable)
        {
            cx = cxAvailable;
        }
    }

    if (cxMin != -1 && !fCollapsed && cxMin > cx)
        cx = cxMin;
    return cx;
}

HRESULT
CDUISizerElement::_Render()
{
    HRESULT hr = E_FAIL;
    BOOL fVisible = TRUE;
    int cx = 0;

    if (_ppbBrowser)
        hr = SHPropertyBag_ReadBOOL(_ppbBrowser, _szVisibleProp, &fVisible);
    if (FAILED(hr) || !fVisible)
        return hr;

    hr = _ppbBrowser ? _ReadSizeFromPropBag(&cx) : E_FAIL;
    if (FAILED(hr))
        return hr;

    cx = _ComputeBoundedSize(cx);
    SHLogicalToPhysicalDPI(&cx, NULL);

    if (_cxLastRendered == -1 || _cxLastRendered != cx)
    {
        HWND hwnd;

        _cxLastRendered = cx;
        DisableAnimations();
        if (_IsVertical())
        {
            _peTarget->SetHeight(cx);
            _peTarget->RemoveLocalValue(DirectUI::Element::WidthProp);
        }
        else
        {
            _peTarget->SetWidth(cx);
            _peTarget->RemoveLocalValue(DirectUI::Element::HeightProp);
        }
        EnableAnimations();

        if (SUCCEEDED(DUI_GetElementRootHWND(this, &hwnd)))
            RedrawWindow(hwnd, NULL, NULL, RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    return hr;
}

/*
 * When the container grows or shrinks the target follows by the same amount, giving up space to a
 * priority sizer first and remembering where it stopped at its minimum so it can grow back later.
 */
HRESULT
CDUISizerElement::_AdjustTargetSize()
{
    int cxContainer;
    int cxDelta = 0;
    HRESULT hr;

    if (FAILED(_GetLogicalContainerSize(&cxContainer)) || cxContainer <= 0)
    {
        hr = E_FAIL;
    }
    else
    {
        hr = S_OK;
        if (_cxContainerSeen)
        {
            int cxChange = cxContainer - _cxContainerSeen;

            if (_psePriority && cxChange > 0)
            {
                int cxPriority = 0;

                hr = _psePriority->_ppbBrowser ? _psePriority->_ReadSizeFromPropBag(&cxPriority) : E_FAIL;
                if (SUCCEEDED(hr))
                {
                    int cxHeld = _psePriority->_GetExtentAlongAxis(_psePriority->_peTarget);

                    if (!cxHeld)
                        cxHeld = _psePriority->_cxLastRendered;
                    if (cxHeld < cxPriority)
                    {
                        cxChange += cxHeld - cxPriority;
                        if (cxChange < 0)
                            cxChange = 0;
                    }
                }
            }
            cxDelta = cxChange - _cxContainerDelta;
        }
        _cxContainerDelta = 0;
        _cxContainerSeen = cxContainer;
    }

    if (FAILED(hr) || !cxDelta)
        return hr;

    int cxSize = 0;
    int cxNew;
    int cxMin;

    hr = _ppbBrowser ? _ReadSizeFromPropBag(&cxSize) : E_FAIL;
    if (FAILED(hr))
        return hr;

    cxMin = _GetIntProp(MinSizeProp);
    hr = _GetLogicalContainerSize(&cxContainer);
    if (FAILED(hr))
        return hr;

    if (cxDelta < 0)
    {
        if (cxSize + cxDelta <= cxMin)
        {
            if (!_cxGrowBackPoint)
                _cxGrowBackPoint = cxContainer + cxMin - cxSize - cxDelta;
            cxNew = cxMin;
        }
        else
        {
            cxNew = cxSize + cxDelta;
        }
    }
    else if (_cxGrowBackPoint)
    {
        cxNew = cxSize;
        if (cxContainer >= _cxGrowBackPoint)
        {
            cxNew += cxContainer - _cxGrowBackPoint;
            _cxGrowBackPoint = 0;
        }
    }
    else
    {
        cxNew = cxSize + cxDelta;
    }

    if (_ppbBrowser)
        SHPropertyBag_WriteInt(_ppbBrowser, _szSizeProp, cxNew);
    _fDirty = TRUE;
    return hr;
}

/* Restores size, visibility and maximize state, preferring the persisted values over the markup */
HRESULT
CDUISizerElement::LoadFromStream()
{
    IStream *pstm;
    int cxSize;
    BOOL fVisible;
    BOOL fMaxxedOut;

    _fLoading = TRUE;

    if (_fPolicyDisabled)
    {
        if (_ppbBrowser)
            SHPropertyBag_WriteBOOL(_ppbBrowser, _szVisibleProp, FALSE);
        goto Done;
    }

    cxSize = _GetIntProp(InitialSizeProp);
    fMaxxedOut = _GetBoolProp(InitialMaxxedOutProp);
    if (_dwPaneState & EPS_DEFAULT_ON)
        fVisible = TRUE;
    else if (_dwPaneState & EPS_DEFAULT_OFF)
        fVisible = FALSE;
    else
        fVisible = _GetBoolProp(InitialVisibilityProp);

    if (SUCCEEDED(GetPersistStream(_GetIntProp(FramePersistTypeProp), STGM_READ, &pstm)))
    {
        int iValue;

        if (SUCCEEDED(IStream_Read(pstm, &iValue, sizeof(iValue))))
            cxSize = iValue;
        if (SUCCEEDED(IStream_Read(pstm, &iValue, sizeof(iValue))))
            fVisible = iValue;
        if (SUCCEEDED(IStream_Read(pstm, &iValue, sizeof(iValue))))
            fMaxxedOut = iValue;

        /* The container may have changed size since the state was saved */
        _cxContainerDelta = 0;
        if (SUCCEEDED(IStream_Read(pstm, &iValue, sizeof(iValue))))
        {
            int cxContainer;

            _GetLogicalContainerSize(&cxContainer);
            _cxContainerDelta = iValue - cxContainer;
        }
        pstm->Release();
    }

    if ((_dwPaneState & EPS_STATEMASK) && (_dwPaneState & (EPS_INITIALSTATE | EPS_FORCE)))
        fVisible = _dwPaneState & EPS_DEFAULT_ON;

    if (_ppbBrowser)
    {
        SHPropertyBag_WriteInt(_ppbBrowser, _szSizeProp, cxSize);
        SHPropertyBag_WriteBOOL(_ppbBrowser, _szVisibleProp, fVisible);
        SHPropertyBag_WriteBOOL(_ppbBrowser, _szMaxxedOutProp, fMaxxedOut);
        SHPropertyBag_WriteBOOL(_ppbBrowser, _szLoadedProp, TRUE);
    }

Done:
    _fDirty = FALSE;
    _fLoading = FALSE;
    return S_OK;
}

HRESULT
CDUISizerElement::SaveToStream()
{
    IStream *pstm;
    BOOL fLoaded = FALSE;
    HRESULT hr = S_OK;
    int iValue;

    if (_fPolicyDisabled || !_fDirty || !_ppbBrowser)
        return S_OK;

    if (FAILED(SHPropertyBag_ReadBOOL(_ppbBrowser, _szLoadedProp, &fLoaded)) || !fLoaded)
        return S_OK;

    hr = GetPersistStream(_GetIntProp(FramePersistTypeProp), STGM_WRITE, &pstm);
    if (FAILED(hr))
        return hr;

    iValue = 0;
    hr = _ReadSizeFromPropBag(&iValue);
    if (SUCCEEDED(hr))
        hr = IStream_Write(pstm, &iValue, sizeof(iValue));

    if (SUCCEEDED(hr))
    {
        iValue = TRUE;
        hr = SHPropertyBag_ReadBOOL(_ppbBrowser, _szVisibleProp, &iValue);
        if (SUCCEEDED(hr))
            hr = IStream_Write(pstm, &iValue, sizeof(iValue));
    }

    if (SUCCEEDED(hr))
    {
        iValue = FALSE;
        hr = SHPropertyBag_ReadBOOL(_ppbBrowser, _szMaxxedOutProp, &iValue);
        if (SUCCEEDED(hr))
            hr = IStream_Write(pstm, &iValue, sizeof(iValue));
    }

    if (SUCCEEDED(hr))
    {
        _GetLogicalContainerSize(&iValue);
        hr = IStream_Write(pstm, &iValue, sizeof(iValue));
        if (SUCCEEDED(hr))
            _fDirty = FALSE;
    }

    pstm->Release();
    return hr;
}

void
CDUISizerElement::_StopListening()
{
    if (!_fListening)
        return;

    _peContainer->RemoveListener(this);
    if (_peReserved)
        _peReserved->RemoveListener(this);
    _fListening = FALSE;
}

/* Resolves the elements the markup names by ID once the whole layout exists */
STDMETHODIMP
CDUISizerElement::LayoutInitialized()
{
    DirectUI::Element *peRoot;
    DirectUI::Value *pv;
    PCWSTR pszPolicy;
    GUID guidPolicy;
    ATOM atomContainer;
    HRESULT hr = E_FAIL;

    _cxLastRendered = -1;
    peRoot = GetRoot();
    if (!peRoot || _fLayoutInitialized)
        return hr;

    _fLayoutInitialized = TRUE;
    _peReserved = peRoot->FindDescendent(_GetAtomProp(ReservedMasterProp));

    _fPolicyDisabled = FALSE;
    pv = GetValue(DisablingPolicyProp, PROPERTY_STORE_COMPUTED, NULL);
    pszPolicy = pv->GetString();
    if (pv != DirectUI::Value::GetUnset() && pszPolicy && GUIDFromStringW(pszPolicy, &guidPolicy))
        _fPolicyDisabled = SHWindowsPolicy(guidPolicy);
    pv->Release();

    _CheckPaneVisibility();

    atomContainer = _GetAtomProp(ReservedTargetProp);
    _peContainer = atomContainer ? peRoot->FindDescendent(atomContainer) : peRoot;

    hr = S_OK;
    if (!_peTarget)
    {
        _peTarget = peRoot->FindDescendent(_GetAtomProp(SizingTargetProp));
        if (_peTarget)
        {
            _iSizerLayoutPos = GetLayoutPos();
            _iTargetLayoutPos = _peTarget->GetLayoutPos();

            /* The browser property bag names this sizer's state after its element ID */
            if (GetAtomNameW(GetID(), _szID, _countof(_szID)))
            {
                hr = StringCchPrintfW(_szSizeProp, _countof(_szSizeProp), L"%s_Size", _szID);
                if (SUCCEEDED(hr))
                    hr = StringCchPrintfW(_szVisibleProp, _countof(_szVisibleProp), L"%s_Visible", _szID);
                if (SUCCEEDED(hr))
                    hr = StringCchPrintfW(_szLoadedProp, _countof(_szLoadedProp), L"%s_Loaded", _szID);
                if (SUCCEEDED(hr))
                    hr = StringCchPrintfW(_szMaxxedOutProp, _countof(_szMaxxedOutProp), L"%s_MaxxedOut", _szID);
            }
        }

        _GetLogicalContainerSize(&_cxContainerSeen);
        _cxGrowBackPoint = 0;

        /* A priority sizer only pairs with one that slides along the same axis */
        if (!_psePriority)
        {
            DirectUI::Element *pePriority = peRoot->FindDescendent(_GetAtomProp(PrioritySizerProp));

            if (pePriority && pePriority->GetClassInfoW()->IsSubclassOf(Class))
            {
                _psePriority = static_cast<CDUISizerElement *>(pePriority);
                if (_psePriority->_IsVertical() != _IsVertical())
                {
                    _psePriority = NULL;
                    hr = E_FAIL;
                }
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = LoadFromStream();
        if (SUCCEEDED(hr) && !_fListening)
        {
            _peContainer->AddListener(this);
            if (_peReserved)
                _peReserved->AddListener(this);
            _fListening = TRUE;
        }
    }
    return hr;
}

STDMETHODIMP
CDUISizerElement::Notify(
    PCWSTR pszPropertyName)
{
    BOOL fVisible = TRUE;

    if (!_peContainer || !GetRoot())
        return S_OK;

    if (!StrCmpICW(pszPropertyName, _szSizeProp) || !StrCmpICW(pszPropertyName, _szMaxxedOutProp))
    {
        if (!_fLoading)
            _fDirty = TRUE;
        _Render();
    }
    else if (!StrCmpICW(pszPropertyName, _szVisibleProp) && _ppbBrowser &&
             SUCCEEDED(SHPropertyBag_ReadBOOL(_ppbBrowser, _szVisibleProp, &fVisible)))
    {
        if (!_fLoading)
            _fDirty = TRUE;

        DisableAnimations();
        _peTarget->SetVisible(fVisible != FALSE);
        _peTarget->SetLayoutPos(fVisible ? _iTargetLayoutPos : ROSUI_LP_NONE);
        SetLayoutPos(fVisible ? _iSizerLayoutPos : ROSUI_LP_NONE);
        EnableAnimations();

        _Render();
        SaveToStream();
    }
    return S_OK;
}

STDMETHODIMP
CDUISizerElement::OnNavigateAway()
{
    HRESULT hr = S_OK;

    _fLayoutInitialized = FALSE;
    _StopListening();
    if (_peContainer)
    {
        hr = SaveToStream();
        if (SUCCEEDED(hr) && _ppbBrowser)
            SHPropertyBag_WriteBOOL(_ppbBrowser, _szLoadedProp, FALSE);
    }
    return hr;
}

void
CDUISizerElement::OnDestroy()
{
    _StopListening();
    DirectUI::Element::OnDestroy();
}

void
CDUISizerElement::OnListenedPropertyChanged(
    DirectUI::Element *peFrom,
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    BOOL fVisible = TRUE;

    if (!_peContainer || !GetRoot() || pvOld->IsEqual(pvNew) ||
        ppi != DirectUI::Element::ExtentProp() || (int)(ppi->Flags & 3) != iIndex)
    {
        return;
    }

    if (peFrom != _peContainer && (!_peReserved || peFrom != _peReserved))
        return;

    /* Only a growing container that should feed this pane first changes the pane's size */
    if (peFrom == _peContainer && _GetBoolProp(GrowTargetFirstProp) && _ppbBrowser &&
        SUCCEEDED(SHPropertyBag_ReadBOOL(_ppbBrowser, _szVisibleProp, &fVisible)) && fVisible)
    {
        IDelayedVisibility *pdv;

        if (_punkSite && SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFileDialogPrivate, IID_PPV_ARGS(&pdv))))
        {
            BOOL fHidden = (pdv->IsVisible() != S_OK);

            pdv->Release();
            if (fHidden)
            {
                _Render();
                return;
            }
        }

        if (FAILED(_AdjustTargetSize()))
            return;
    }

    _Render();
}

void
CDUISizerElement::OnInput(
    DirectUI::InputEvent *pie)
{
    if (pie->nDevice == GINPUT_MOUSE && static_cast<DirectUI::MouseEvent *>(pie)->bButton == GBUTTON_LEFT)
    {
        if (pie->nCode == GMOUSE_DOWN)
        {
            _fDragging = TRUE;
        }
        else if (pie->nCode == GMOUSE_UP)
        {
            _fDragging = FALSE;
            _cxGrowBackPoint = 0;
            SaveToStream();
        }
        else if (pie->nCode == GMOUSE_DRAG && _fDragging)
        {
            DirectUI::MouseDragEvent *pmde = static_cast<DirectUI::MouseDragEvent *>(pie);
            BOOL fVertical = _IsVertical();
            int cxCurrent = fVertical ? _peTarget->GetHeight() : _peTarget->GetWidth();
            int cxDrag = fVertical ? pmde->sizeDelta.cy : pmde->sizeDelta.cx;
            int cxNew;

            if (IsRTL() && !fVertical)
                cxDrag = -cxDrag;

            cxNew = _GetBoolProp(DownOrRightGrowsTargetProp) ? cxCurrent + cxDrag : cxCurrent - cxDrag;
            SHPhysicalToLogicalDPI(&cxNew, NULL);

            if (_ppbBrowser)
                SHPropertyBag_WriteInt(_ppbBrowser, _szSizeProp, _ComputeBoundedSize(max(cxNew, 0)));

            /* Dragging the pane smaller takes it out of the maximized state */
            if (cxNew < cxCurrent && _ppbBrowser)
                SHPropertyBag_WriteBOOL(_ppbBrowser, _szMaxxedOutProp, FALSE);
        }
    }
    DirectUI::Element::OnInput(pie);
}
