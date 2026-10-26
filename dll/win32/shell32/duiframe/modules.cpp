/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Frame module elements named by the layout markup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define MODULE_PROPFLAGS 2

/* Modules that take mouse input themselves */
#define MODULE_ACTIVE_MOUSE 1

/* Constant default values: a reference count of -1 keeps them out of reference counting */
static const DirectUI::_StaticValue c_svIntMinusOne = { (int)DirectUI::ValueType::Int, 0, -1, -1 };
static const DirectUI::_StaticValuePtr c_svEmptyString = { (int)DirectUI::ValueType::String, 0, -1, (void *)L"" };
static const DirectUI::_StaticValuePtr c_svFileCountProperty =
    { (int)DirectUI::ValueType::String, 0, -1, (void *)L"prop:System.FileCount" };

static DirectUI::Value *WINAPI svIntMinusOne() { return (DirectUI::Value *)&c_svIntMinusOne; }
static DirectUI::Value *WINAPI svEmptyString() { return (DirectUI::Value *)&c_svEmptyString; }
static DirectUI::Value *WINAPI svFileCountProperty() { return (DirectUI::Value *)&c_svFileCountProperty; }

static DirectUI::PropCapability s_pcInt = { DirectUI::ValueType::Int, 0, UINT_MAX };
static DirectUI::PropCapability s_pcBool = { DirectUI::ValueType::Bool, 0, UINT_MAX };
static DirectUI::PropCapability s_pcString = { DirectUI::ValueType::String, 0, UINT_MAX };

/* PreviewThumbnail */

static DirectUI::PropertyInfoData s_pidMaxThumbSize;
static DirectUI::PropertyInfoData s_pidFixedThumbSize;
static DirectUI::PropertyInfoData s_pidHideProperty;
static DirectUI::PropertyInfoData s_pidVertical;
static DirectUI::PropertyInfoData s_pidAlwaysGetIconFromView;
static DirectUI::PropertyInfoData s_pidForceUseIcon;
static DirectUI::PropertyInfoData s_pidThumbnailId;

static const DirectUI::PropertyInfo s_piMaxThumbSize =
    { L"MaxThumbSize", MODULE_PROPFLAGS, 0, &s_pcInt, NULL, svIntMinusOne, &s_pidMaxThumbSize };
static const DirectUI::PropertyInfo s_piFixedThumbSize =
    { L"FixedThumbSize", MODULE_PROPFLAGS, 0, &s_pcInt, NULL, svIntMinusOne, &s_pidFixedThumbSize };
static const DirectUI::PropertyInfo s_piHideProperty =
    { L"HideProperty", MODULE_PROPFLAGS, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidHideProperty };
static const DirectUI::PropertyInfo s_piVertical =
    { L"Vertical", MODULE_PROPFLAGS, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse, &s_pidVertical };
static const DirectUI::PropertyInfo s_piAlwaysGetIconFromView =
    { L"AlwaysGetIconFromView", MODULE_PROPFLAGS, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse,
      &s_pidAlwaysGetIconFromView };
static const DirectUI::PropertyInfo s_piForceUseIcon =
    { L"ForceUseIcon", MODULE_PROPFLAGS, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse, &s_pidForceUseIcon };
static const DirectUI::PropertyInfo s_piThumbnailId =
    { L"ThumbnailId", MODULE_PROPFLAGS, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidThumbnailId };

static const DirectUI::PropertyInfo *const c_rgpiPreviewThumbnail[] =
{
    &s_piMaxThumbSize,
    &s_piFixedThumbSize,
    &s_piHideProperty,
    &s_piVertical,
    &s_piAlwaysGetIconFromView,
    &s_piForceUseIcon,
    &s_piThumbnailId,
};

const DirectUI::PropertyInfo *const CPreviewThumbnail::MaxThumbSizeProp = &s_piMaxThumbSize;
const DirectUI::PropertyInfo *const CPreviewThumbnail::FixedSizeProp = &s_piFixedThumbSize;
const DirectUI::PropertyInfo *const CPreviewThumbnail::HidePropertyProp = &s_piHideProperty;
const DirectUI::PropertyInfo *const CPreviewThumbnail::VerticalProp = &s_piVertical;
const DirectUI::PropertyInfo *const CPreviewThumbnail::AlwaysGetIconFromViewProp = &s_piAlwaysGetIconFromView;
const DirectUI::PropertyInfo *const CPreviewThumbnail::ForceUseIconProp = &s_piForceUseIcon;
const DirectUI::PropertyInfo *const CPreviewThumbnail::ThumbnailIdProp = &s_piThumbnailId;

/* PreviewMetadata */

static DirectUI::PropertyInfoData s_pidNullSelectPropertyString;
static DirectUI::PropertyInfoData s_pidNullSelectTitlePropertyString;
static DirectUI::PropertyInfoData s_pidReadOnly;

static const DirectUI::PropertyInfo s_piNullSelectPropertyString =
    { L"NullSelectPropertyString", MODULE_PROPFLAGS, 0, &s_pcString, NULL, svEmptyString,
      &s_pidNullSelectPropertyString };
static const DirectUI::PropertyInfo s_piNullSelectTitlePropertyString =
    { L"NullSelectTitlePropertyString", MODULE_PROPFLAGS, 0, &s_pcString, NULL, svFileCountProperty,
      &s_pidNullSelectTitlePropertyString };
static const DirectUI::PropertyInfo s_piReadOnly =
    { L"ReadOnly", MODULE_PROPFLAGS, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse, &s_pidReadOnly };

static const DirectUI::PropertyInfo *const c_rgpiPreviewMetadata[] =
{
    &s_piNullSelectPropertyString,
    &s_piNullSelectTitlePropertyString,
    &s_piReadOnly,
};

const DirectUI::PropertyInfo *const CPreviewMetadata::NullSelectPropertyStringProp = &s_piNullSelectPropertyString;
const DirectUI::PropertyInfo *const CPreviewMetadata::NullSelectTitlePropertyStringProp =
    &s_piNullSelectTitlePropertyString;
const DirectUI::PropertyInfo *const CPreviewMetadata::ReadOnlyProp = &s_piReadOnly;

/* ProperTreeModule */

static DirectUI::PropertyInfoData s_pidRootMgrClsid;

static const DirectUI::PropertyInfo s_piRootMgrClsid =
    { L"RootMgrClsid", MODULE_PROPFLAGS, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidRootMgrClsid };

static const DirectUI::PropertyInfo *const c_rgpiProperTreeModule[] =
{
    &s_piRootMgrClsid,
};

/* Element allocation shared by every module class; the inner element sits in a shell border layout */
template <class T>
static
HRESULT
WINAPI
CreateModuleElement(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _In_ int iActive,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Layout *pLayout = NULL;
    T *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<T>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(iActive);
    if (SUCCEEDED(hr))
        hr = DirectUI::ShellBorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        hr = pe->SetLayout(pLayout);

    if (FAILED(hr))
        pe->Destroy(false);
    else
        *ppElement = pe;
    return hr;
}

template <class T>
static
HRESULT
WINAPI
RegisterModuleClass(
    _In_ PCWSTR pszName,
    _In_reads_opt_(cProps) const DirectUI::PropertyInfo *const *rgProps,
    _In_ UINT cProps)
{
    return DirectUI::ClassInfo<T, CFrameModule, DirectUI::StandardCreator<T>>::Register(
        DUIFRAME_HINST, pszName, rgProps, cProps, false);
}

/* Inner elements are Phase B work (notes\SHELL32_DUIFRAME.md); until then the modules stay empty */
static
HRESULT
WINAPI
CreateInnerNotImplemented(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    *ppElement = NULL;
    return E_NOTIMPL;
}

#define IMPLEMENT_FRAME_MODULE(ClassName, MarkupName, InnerCreate, InnerID, Props, PropCount)  \
DirectUI::IClassInfo *ClassName::Class = NULL;                                                 \
HRESULT WINAPI ClassName::Create(                                                              \
    _In_opt_ DirectUI::Element *peParent,                                                      \
    _Inout_opt_ ULONG *pdwDeferCookie,                                                         \
    _Outptr_ DirectUI::Element **ppElement)                                                    \
{                                                                                              \
    return CreateModuleElement<ClassName>(peParent, pdwDeferCookie, 0, ppElement);             \
}                                                                                              \
HRESULT WINAPI ClassName::Register()                                                           \
{                                                                                              \
    return RegisterModuleClass<ClassName>(MarkupName, Props, PropCount);                       \
}                                                                                              \
STDMETHODIMP ClassName::CreateInnerObject(IUnknown **ppunkInner)                               \
{                                                                                              \
    return FrameModule_CreateInner(InnerCreate, InnerID, ppunkInner);                          \
}

IMPLEMENT_FRAME_MODULE(CProperTreeModule, L"ProperTreeModule", CProperTreeModuleInner::Create,
                       L"ProperTreeModuleInner", c_rgpiProperTreeModule, _countof(c_rgpiProperTreeModule))
IMPLEMENT_FRAME_MODULE(CCommandModule, L"FolderBandModule", CCommandModuleInner::Create,
                       L"FolderBandModuleInner", c_rgpiCommandModule, c_cpiCommandModule)
IMPLEMENT_FRAME_MODULE(CInfoBarHostModule, L"InfoBarHostModule", CreateInnerNotImplemented,
                       L"InfoBarHostModuleInner", NULL, 0)
IMPLEMENT_FRAME_MODULE(CLibraryPaneModule, L"LibraryPaneModule", CLibraryPaneModuleInner::Create,
                       L"LibraryPaneModuleInner", NULL, 0)
IMPLEMENT_FRAME_MODULE(CInfoPane, L"InfoPane", CInfoPaneInner::Create,
                       L"InfoPaneInner", NULL, 0)

/* CNotifyingFrameModule */

STDMETHODIMP
CNotifyingFrameModule::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CNotifyingFrameModule, IFrameNotificationClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CFrameModule::QueryInterface(riid, ppv);
    return hr;
}

/* CPreviewThumbnail */

DirectUI::IClassInfo *CPreviewThumbnail::Class = NULL;

HRESULT
WINAPI
CPreviewThumbnail::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return CreateModuleElement<CPreviewThumbnail>(peParent, pdwDeferCookie, MODULE_ACTIVE_MOUSE, ppElement);
}

HRESULT
WINAPI
CPreviewThumbnail::Register()
{
    return RegisterModuleClass<CPreviewThumbnail>(L"PreviewThumbnail", c_rgpiPreviewThumbnail,
                                                  _countof(c_rgpiPreviewThumbnail));
}

STDMETHODIMP
CPreviewThumbnail::CreateInnerObject(
    IUnknown **ppunkInner)
{
    return FrameModule_CreateInner(CPreviewThumbnailInner::Create, L"PreviewThumbnailInner", ppunkInner);
}

STDMETHODIMP
CPreviewThumbnail::LayoutInitialized()
{
    _UpdateLayoutPos();
    return S_OK;
}

STDMETHODIMP
CPreviewThumbnail::Notify(
    PCWSTR pszPropertyName)
{
    DirectUI::Value *pv = GetValue(HidePropertyProp, 2, NULL);
    PCWSTR pszHide = pv->GetString();

    if (pszHide && !StrCmpICW(pszPropertyName, pszHide))
        _UpdateLayoutPos();
    pv->Release();
    return S_OK;
}

/*
 * A thumbnail hidden through its HideProperty bag value collapses; a fixed size wins over a
 * maximum size, which only bounds the side the thumbnail docks against.
 */
HRESULT
CPreviewThumbnail::_UpdateLayoutPos()
{
    IPropertyBag *ppb;
    DirectUI::Value *pv;
    PCWSTR pszHide;
    BOOL fHidden = FALSE;
    int cxMax;
    int cxFixed;
    HRESULT hr;

    hr = IUnknown_QueryService(_punkSite, SID_PerLayoutPropertyBag, IID_PPV_ARGS(&ppb));
    if (FAILED(hr))
        return hr;

    pv = GetValue(HidePropertyProp, 2, NULL);
    pszHide = pv->GetString();
    if (pszHide)
    {
        BOOL fBag;

        if (SUCCEEDED(SHPropertyBag_ReadBOOL(ppb, pszHide, &fBag)))
            fHidden = fBag;
    }
    pv->Release();

    pv = GetValue(MaxThumbSizeProp, 2, NULL);
    cxMax = pv->GetInt();
    pv->Release();
    pv = GetValue(FixedSizeProp, 2, NULL);
    cxFixed = pv->GetInt();
    pv->Release();

    if (fHidden)
    {
        SetLayoutPos(ROSUI_LP_NONE);
        RemoveLocalValue(DirectUI::Element::HeightProp);
        RemoveLocalValue(DirectUI::Element::WidthProp);
    }
    else if (cxFixed != -1)
    {
        SetLayoutPos(ROSUI_LP_TOP);
        SetWidth(cxFixed);
        SetHeight(cxFixed);
    }
    else if (cxMax != -1)
    {
        BOOL fVertical;

        pv = GetValue(VerticalProp, 2, NULL);
        fVertical = pv->GetBool();
        pv->Release();

        if (fVertical)
        {
            SetLayoutPos(ROSUI_LP_TOP);
            SetWidth(cxMax);
            RemoveLocalValue(DirectUI::Element::HeightProp);
        }
        else
        {
            SetLayoutPos(ROSUI_LP_LEFT);
            SetHeight(cxMax);
            RemoveLocalValue(DirectUI::Element::WidthProp);
        }
    }
    else
    {
        SetLayoutPos(ROSUI_LP_CLIENT);
        RemoveLocalValue(DirectUI::Element::HeightProp);
        RemoveLocalValue(DirectUI::Element::WidthProp);
    }

    ppb->Release();
    return hr;
}

/* CPreviewMetadata */

DirectUI::IClassInfo *CPreviewMetadata::Class = NULL;

HRESULT
WINAPI
CPreviewMetadata::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return CreateModuleElement<CPreviewMetadata>(peParent, pdwDeferCookie, MODULE_ACTIVE_MOUSE, ppElement);
}

HRESULT
WINAPI
CPreviewMetadata::Register()
{
    return RegisterModuleClass<CPreviewMetadata>(L"PreviewMetadata", c_rgpiPreviewMetadata,
                                                 _countof(c_rgpiPreviewMetadata));
}

STDMETHODIMP
CPreviewMetadata::CreateInnerObject(
    IUnknown **ppunkInner)
{
    return FrameModule_CreateInner(CPreviewMetadataInner::Create, L"PreviewMetadataInner", ppunkInner);
}

/* CReadingPaneModule */

DirectUI::IClassInfo *CReadingPaneModule::Class = NULL;

HRESULT
WINAPI
CReadingPaneModule::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return CreateModuleElement<CReadingPaneModule>(peParent, pdwDeferCookie, MODULE_ACTIVE_MOUSE, ppElement);
}

HRESULT
WINAPI
CReadingPaneModule::Register()
{
    return RegisterModuleClass<CReadingPaneModule>(L"ReadingPaneModule", NULL, 0);
}

STDMETHODIMP
CReadingPaneModule::CreateInnerObject(
    IUnknown **ppunkInner)
{
    return FrameModule_CreateInner(CReadingPaneModuleInner::Create, L"ReadingPaneModuleInner", ppunkInner);
}

/* The reading pane only takes space while a preview handler is rendering */
STDMETHODIMP
CReadingPaneModule::Notify(
    PCWSTR pszPropertyName)
{
    IPropertyBag *ppb;

    if (!StrCmpICW(pszPropertyName, L"PreviewHandlerRendering") &&
        SUCCEEDED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        BOOL fRendering = PropertyBag_ReadBOOLDefault(ppb, L"PreviewHandlerRendering", FALSE);

        SetLayoutPos(fRendering ? ROSUI_LP_CLIENT : ROSUI_LP_NONE);
        ppb->Release();
    }
    return S_OK;
}
