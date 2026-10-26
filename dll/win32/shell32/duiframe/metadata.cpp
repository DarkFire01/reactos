/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Details pane property rows: title, item count and the property list
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <DUser.h>

/* Task id of the property extraction work item */
static const GUID TOID_PreviewPropertyTask =
    { 0x93806097, 0xaaf4, 0x487d, { 0x89, 0xb8, 0xee, 0x5b, 0x6c, 0x45, 0x31, 0xc9 } };

static const PROPERTYKEY c_pkeyItemNameDisplay =
    { { 0xb725f130, 0x47ef, 0x101a, { 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac } }, 10 };
static const PROPERTYKEY c_pkeyFileCount =
    { { 0x28636aa6, 0x953d, 0x11d2, { 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0 } }, 12 };
static const PROPERTYKEY c_pkeyStatusIcons =
    { { 0x7a55582b, 0xbd8c, 0x4475, { 0xb9, 0x4c, 0xb8, 0x7a, 0x38, 0x8a, 0x78, 0x99 } }, 100 };
static const PROPERTYKEY c_pkeyPreviewDetails =
    { { 0xc9944a21, 0xa406, 0x48fe, { 0x82, 0x25, 0xae, 0xc7, 0xe2, 0x4c, 0x21, 0x1b } }, 8 };
static const PROPERTYKEY c_pkeyPreviewTitle =
    { { 0xc9944a21, 0xa406, 0x48fe, { 0x82, 0x25, 0xae, 0xc7, 0xe2, 0x4c, 0x21, 0x1b } }, 6 };
static const PROPERTYKEY c_pkeyDetailsPaneNullSelect =
    { { 0xc9944a21, 0xa406, 0x48fe, { 0x82, 0x25, 0xae, 0xc7, 0xe2, 0x4c, 0x21, 0x1b } }, 510 };
static const PROPERTYKEY c_pkeyDetailsPaneNullSelectTitle =
    { { 0xc9944a21, 0xa406, 0x48fe, { 0x82, 0x25, 0xae, 0xc7, 0xe2, 0x4c, 0x21, 0x1b } }, 511 };

/* Selections larger than this wait for "Show more details" before their properties are read */
#define METADATA_DEFAULTMAXVIEWABLE 15

/* Item showing modes: properties shown, too many items selected, all requested anyway */
#define METADATA_SHOW_NORMAL        0
#define METADATA_SHOW_TOOMANY       1
#define METADATA_SHOW_FORCED        2

/* Extraction read-only state: unknown, read-only, writable */
#define METADATA_RO_UNKNOWN         0
#define METADATA_RO_READONLY        1
#define METADATA_RO_WRITABLE        2

/* Layout metrics at 96 DPI: label limit, value column and title limit */
#define METADATA_CXLABELMAX         115
#define METADATA_CXVALUE            135
#define METADATA_CXTITLEMAX         250

/* The pane keeps this much space below the rows */
#define METADATA_BOTTOMGAP          6

/* Seconds before a slow extraction clears the pane back to the item count */
#define METADATA_CLEARDELAY         0.25f

static DWORD s_dwMaxViewableItems;

static
int
Metadata_ScaleForDpi(
    _In_ int cx)
{
    HDC hdc = GetDC(NULL);
    int iDpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;

    if (hdc)
        ReleaseDC(NULL, hdc);
    return MulDiv(cx, iDpi, 96);
}

static
HRESULT
Metadata_CreateParser(
    _In_opt_ IUnknown *punkSite,
    _Outptr_ DirectUI::DUIXmlParser **ppParser)
{
    PWSTR pszUIFile;
    HRESULT hr;

    *ppParser = NULL;
    hr = DUI_LoadUIFileFromResources(DUIFRAME_HINST, 8, &pszUIFile);
    if (SUCCEEDED(hr))
    {
        hr = DUI_CreateParser(pszUIFile, ppParser, punkSite, NULL);
        LocalFree(pszUIFile);
    }
    return hr;
}

/* The selection, or for an empty selection the item count of the whole view */
static
HRESULT
Metadata_GetShellItemArray(
    _In_ IFolderView *pfv,
    _Outptr_result_maybenull_ IShellItemArray **ppsia,
    _Out_ UINT *pcItems,
    _Out_ UINT *pcSelected)
{
    IShellItemArray *psia = NULL;
    DWORD cSelected = 0;
    int cItems;
    HRESULT hr;

    *ppsia = NULL;
    *pcItems = 0;
    *pcSelected = 0;

    if (SUCCEEDED(pfv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&psia))) &&
        SUCCEEDED(psia->GetCount(&cSelected)) && cSelected)
    {
        *pcItems = cSelected;
        *pcSelected = cSelected;
        *ppsia = psia;
        return S_OK;
    }

    if (psia)
        psia->Release();

    hr = pfv->ItemCount(SVGIO_ALLVIEW, &cItems);
    if (FAILED(hr))
        return hr;
    if (cItems < 0)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

    *pcItems = cItems;
    return S_OK;
}

/* "32 items" with an empty selection, "3 items selected" otherwise */
static
HRESULT
Metadata_FillPreviewPaneCount(
    _In_ DirectUI::Element *peInner,
    _In_ UINT cItems,
    _In_ UINT cSelected,
    _In_ DirectUI::DUIXmlParser *pParser)
{
    DirectUI::Element *peTitle = NULL;
    DirectUI::Element *peLabel;
    WCHAR szFormat[64];
    WCHAR szNumber[64];
    WCHAR szCount[128] = L"";
    WCHAR szName[64] = L"";
    WCHAR szRaw[32];
    NUMBERFMTW nf = { 0 };
    WCHAR szDecimal[8];
    WCHAR szThousand[8];
    UINT uID;
    HRESULT hr;

    hr = pParser->CreateElement(L"PreviewItemCountTitle", NULL, peInner, NULL, &peTitle);
    if (FAILED(hr))
        return hr;

    if (cSelected)
        uID = (cItems == 1) ? IDS_DUI_SELECTED_SINGULAR : IDS_DUI_SELECTED_PLURAL;
    else
        uID = (cItems == 1) ? IDS_DUI_ITEMCOUNT_SINGULAR : IDS_DUI_ITEMCOUNT_PLURAL;

    peLabel = peTitle->FindDescendent(StrToID(L"ItemCountLabel"));
    if (!peLabel)
    {
        peTitle->Destroy(true);
        return E_FAIL;
    }

    if (LoadStringW(DUIFRAME_HINST, uID, szFormat, _countof(szFormat)))
    {
        /* The count gets the user's digit grouping, without decimals */
        GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, szDecimal, _countof(szDecimal));
        GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, szThousand, _countof(szThousand));
        nf.Grouping = 3;
        nf.lpDecimalSep = szDecimal;
        nf.lpThousandSep = szThousand;
        StringCchPrintfW(szRaw, _countof(szRaw), L"%u", cSelected ? cSelected : cItems);
        if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, szRaw, &nf, szNumber, _countof(szNumber)))
            StringCchCopyW(szNumber, _countof(szNumber), szRaw);
        StringCchPrintfW(szCount, _countof(szCount), szFormat, szNumber);
    }

    hr = peLabel->SetContentString(szCount);
    if (FAILED(hr))
    {
        peTitle->Destroy(true);
        return hr;
    }

    peLabel->SetAccValue(szCount);
    LoadStringW(DUIFRAME_HINST, IDS_DUI_ITEMCOUNT_NAME, szName, _countof(szName));
    peLabel->SetAccName(szName);
    peLabel->SetAccessible(true);
    peLabel->SetAccRole(ROLE_SYSTEM_TEXT);
    peLabel->SetAccState(STATE_SYSTEM_READONLY);
    peInner->Add(peTitle);
    return S_OK;
}

/* One row: the "Name:" label and the formatted value */
static
HRESULT
Metadata_AddRow(
    _In_ DirectUI::Element *peInner,
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar,
    _In_ BOOL fReadOnly,
    _In_ DirectUI::DUIXmlParser *pParser)
{
    IPropertyDescription *ppd;
    IUnknown *punkValue;
    DirectUI::Element *peRow = NULL;
    PWSTR pszName = NULL;
    WCHAR szFormat[16];
    PWSTR pszLabel = NULL;
    HRESULT hr;

    hr = PSGetPropertyDescription(key, IID_PPV_ARGS(&ppd));
    if (FAILED(hr))
        return hr;

    hr = ppd->GetDisplayName(&pszName);
    if (SUCCEEDED(hr))
        hr = DetailsPane_CreatePropertyValue(propvar, key, IID_PPV_ARGS(&punkValue));

    if (SUCCEEDED(hr))
    {
        hr = pParser->CreateElement(L"PreviewMetadataRow", NULL, peInner, NULL, &peRow);
        if (SUCCEEDED(hr))
        {
            DirectUI::Element *peLabel;
            CMetadataElement *peValue;

            peInner->Add(peRow);
            if (!LoadStringW(DUIFRAME_HINST, IDS_DUI_METADATALABEL, szFormat, _countof(szFormat)) ||
                !FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                FORMAT_MESSAGE_ARGUMENT_ARRAY, szFormat, 0, 0, (LPWSTR)&pszLabel, 0,
                                (va_list *)&pszName))
            {
                hr = E_OUTOFMEMORY;
            }

            if (SUCCEEDED(hr))
            {
                peLabel = peRow->FindDescendent(StrToID(L"PreviewMetadataLabel"));
                if (peLabel)
                {
                    peLabel->SetContentString(pszLabel);
                    peLabel->SetAccessible(true);
                    peLabel->SetAccName(pszName);
                    peLabel->SetAccRole(ROLE_SYSTEM_STATICTEXT);
                    peLabel->SetAccState(STATE_SYSTEM_READONLY);
                }

                peValue = (CMetadataElement *)peRow->FindDescendent(StrToID(L"PreviewMetadataControl"));
                if (peValue)
                {
                    peValue->SetCurrentValue(punkValue);
                    hr = peValue->InitializeForDisplay(ppd, !fReadOnly, CLSID_NULL);
                    if (FAILED(hr))
                    {
                        peInner->Remove(&peRow, 1);
                        peRow->Destroy(true);
                    }
                }
                LocalFree(pszLabel);
            }
        }
        punkValue->Release();
    }

    CoTaskMemFree(pszName);
    ppd->Release();
    return hr;
}

/* CPropertyValue: a property key and its value as one object */

class CPropertyValue : public IPropertyValue, public CDUIFrameZeroAlloc
{
public:
    CPropertyValue() : _cRef(1) { PropVariantInit(&_propvar); }

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            { &__uuidof(IPropertyValue), OFFSETOFCLASS(IPropertyValue, CPropertyValue) },
            { &IID_IObjectWithPropertyKey, OFFSETOFCLASS(IObjectWithPropertyKey, CPropertyValue) },
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
    SetPropertyKey(
        REFPROPERTYKEY key) override
    {
        _key = key;
        return S_OK;
    }

    STDMETHODIMP
    GetPropertyKey(
        PROPERTYKEY *pkey) override
    {
        *pkey = _key;
        return S_OK;
    }

    STDMETHODIMP
    GetValue(
        PROPVARIANT *ppropvar) override
    {
        return PropVariantCopy(ppropvar, &_propvar);
    }

    STDMETHODIMP
    InitValue(
        REFPROPVARIANT propvar) override
    {
        PropVariantClear(&_propvar);
        return PropVariantCopy(&_propvar, &propvar);
    }

private:
    ~CPropertyValue() { PropVariantClear(&_propvar); }

    LONG _cRef;
    PROPERTYKEY _key;
    PROPVARIANT _propvar;
};

HRESULT
WINAPI
DetailsPane_CreatePropertyValue(
    _In_ REFPROPVARIANT propvar,
    _In_ REFPROPERTYKEY key,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CPropertyValue *ppv2;
    HRESULT hr;

    *ppv = NULL;
    ppv2 = new CPropertyValue();
    if (!ppv2)
        return E_OUTOFMEMORY;

    ppv2->SetPropertyKey(key);
    hr = ppv2->InitValue(propvar);
    if (SUCCEEDED(hr))
        hr = ppv2->QueryInterface(riid, ppv);
    ppv2->Release();
    return hr;
}

/* The value of a property value object, VT_EMPTY when there is none */
static
HRESULT
Metadata_GetPropVariant(
    _In_opt_ IUnknown *punk,
    _Out_ PROPVARIANT *ppropvar)
{
    IPropertyValue *ppv;
    HRESULT hr;

    PropVariantInit(ppropvar);
    if (!punk)
        return S_FALSE;

    hr = punk->QueryInterface(IID_PPV_ARGS(&ppv));
    if (SUCCEEDED(hr))
    {
        hr = ppv->GetValue(ppropvar);
        ppv->Release();
    }
    return hr;
}

/* MetadataLayout */

HRESULT
WINAPI
MetadataLayout::Create(
    _Outptr_ DirectUI::Layout **ppLayout)
{
    MetadataLayout *pLayout = DirectUI::HNewAndZero<MetadataLayout>();

    *ppLayout = pLayout;
    if (!pLayout)
        return E_OUTOFMEMORY;

    pLayout->Initialize();
    return S_OK;
}

MetadataLayout::~MetadataLayout()
{
    _FreeLayoutObjects();
}

void
MetadataLayout::_FreeLayoutObjects()
{
    HeapFree(GetProcessHeap(), 0, _rgcxColumns);
    _rgcxColumns = NULL;
    HeapFree(GetProcessHeap(), 0, _rgpeChildren);
    _rgpeChildren = NULL;
    _cChildren = 0;
}

UINT
MetadataLayout::_FindChild(
    _In_ DirectUI::Element *pe)
{
    for (UINT i = 0; i < _cChildren; i++)
    {
        if (_rgpeChildren[i] == pe)
            return i;
    }
    return (UINT)-1;
}

void
MetadataLayout::_RemoveChildAt(
    _In_ UINT i)
{
    MoveMemory(&_rgpeChildren[i], &_rgpeChildren[i + 1], (_cChildren - i - 1) * sizeof(*_rgpeChildren));
    _cChildren -= 1;
}

/* The child array always has room for one more entry than it was built with */
void
MetadataLayout::_InsertChildAt(
    _In_ UINT i,
    _In_ DirectUI::Element *pe)
{
    i = min(i, _cChildren);
    MoveMemory(&_rgpeChildren[i + 1], &_rgpeChildren[i], (_cChildren - i) * sizeof(*_rgpeChildren));
    _rgpeChildren[i] = pe;
    _cChildren += 1;
}

/* A new pane size lets the value controls measure their natural width again */
void
MetadataLayout::_ResetWidth()
{
    static const PCWSTR c_rgpszControls[] =
    {
        L"PreviewMetadataControl",
        L"PreviewMetadataTitleControl",
        L"PreviewMetadataSubTitleControl",
    };

    for (UINT i = 0; i < _cChildren; i++)
    {
        if (!_rgpeChildren[i])
            continue;

        for (UINT j = 0; j < _countof(c_rgpszControls); j++)
        {
            DirectUI::Element *peControl = _rgpeChildren[i]->FindDescendent(StrToID(c_rgpszControls[j]));

            if (peControl)
            {
                peControl->RemoveLocalValue(DirectUI::Element::WidthProp);
                break;
            }
        }
    }
}

/* The spacer only takes a slot when three rows fit a column: it keeps the first column to the titles */
void
MetadataLayout::_AddSpacerIfNeeded(
    _In_ DirectUI::Element *peContainer)
{
    DirectUI::Element *peSpacer = peContainer->FindDescendent(StrToID(L"PreviewMetadataSpacer"));
    UINT i;

    if (!peSpacer)
        return;

    i = _FindChild(peSpacer);
    if (i != (UINT)-1)
        _RemoveChildAt(i);

    if (_cRowsPerColumn == 3 && _cChildren > 3)
        _InsertChildAt(2, peSpacer);
}

SIZE
MetadataLayout::UpdateDesiredSize(
    DirectUI::Element *peContainer,
    int cxConstraint,
    int cyConstraint,
    DirectUI::Surface *psrf)
{
    SIZE size = { cxConstraint, cyConstraint };
    int cyAvail = max(cyConstraint - METADATA_BOTTOMGAP, 0);
    UINT cLayoutChildren;

    _cRowsPerColumn = 0;
    _iLastFitColumn = 0;
    _FreeLayoutObjects();

    cLayoutChildren = GetLayoutChildCount(peContainer);
    _rgpeChildren = (DirectUI::Element **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                   (cLayoutChildren + 1) * sizeof(*_rgpeChildren));
    if (!_rgpeChildren)
        return size;

    for (UINT i = 0; i < cLayoutChildren; i++)
    {
        DirectUI::Element *pe = GetChildFromLayoutIndex(peContainer, i, NULL);

        if (pe)
            _rgpeChildren[_cChildren++] = pe;
    }

    if (_cxLast != cxConstraint || _cyLast != cyAvail)
    {
        _ResetWidth();
        _cxLast = cxConstraint;
        _cyLast = cyAvail;
    }

    /* Every row is as tall as the first one */
    if (_cChildren && _rgpeChildren[0])
    {
        _rgpeChildren[0]->_UpdateDesiredSize(cxConstraint, cyAvail, psrf);
        _cyRow = _rgpeChildren[0]->GetDesiredSize()->cy;
        if (_cyRow > 0)
            _cRowsPerColumn = cyAvail / _cyRow;

        _AddSpacerIfNeeded(peContainer);
        if (_cRowsPerColumn)
        {
            _cColumns = (_cChildren + _cRowsPerColumn - 1) / _cRowsPerColumn;
            if (_cColumns)
            {
                _rgcxColumns = (int *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                _cColumns * sizeof(*_rgcxColumns));
            }
        }
    }

    if (_rgcxColumns)
    {
        int cxLabelMax = Metadata_ScaleForDpi(METADATA_CXLABELMAX);
        int cxValue = Metadata_ScaleForDpi(METADATA_CXVALUE);
        int cxTitleMax = Metadata_ScaleForDpi(METADATA_CXTITLEMAX);
        int cxTotal = 0;
        UINT cFit;

        for (UINT iColumn = 0; iColumn < _cColumns; iColumn++)
        {
            for (UINT iRow = 0; iRow < _cRowsPerColumn; iRow++)
            {
                UINT i = iColumn * _cRowsPerColumn + iRow;
                DirectUI::Element *pe;
                DirectUI::Element *peLabel;
                int cx;

                if (i >= _cChildren)
                    break;
                pe = _rgpeChildren[i];
                if (!pe)
                    continue;

                pe->_UpdateDesiredSize(cxConstraint, cyAvail, psrf);
                peLabel = pe->FindDescendent(StrToID(L"PreviewMetadataLabel"));
                if (peLabel)
                {
                    int cxLabel = peLabel->GetDesiredSize()->cx;

                    if (cxLabel > cxLabelMax)
                    {
                        cxLabel = cxLabelMax;
                        peLabel->SetWidth(cxLabelMax);
                    }
                    cx = cxLabel + cxValue;
                }
                else
                {
                    cx = min(pe->GetDesiredSize()->cx, cxTitleMax);
                }
                _rgcxColumns[iColumn] = max(_rgcxColumns[iColumn], cx);
            }

            /* Columns past the pane's width are not shown; the first one is clipped to it */
            cxTotal += _rgcxColumns[iColumn];
            if (cxTotal > cxConstraint)
            {
                if (!_iLastFitColumn)
                    _rgcxColumns[iColumn] = min(_rgcxColumns[iColumn], cxConstraint);
            }
            else
            {
                _iLastFitColumn = iColumn;
            }
        }

        /* Save and Cancel take the last visible slot */
        cFit = _cRowsPerColumn * (_iLastFitColumn + 1);
        if (cFit)
        {
            _peEditButtons = peContainer->FindDescendent(StrToID(L"PreviewEditMetadata"));
            if (_peEditButtons)
            {
                UINT i = _FindChild(_peEditButtons);

                if (i != (UINT)-1)
                    _RemoveChildAt(i);
                _InsertChildAt(min(_cChildren, cFit - 1), _peEditButtons);
            }
        }
    }
    return size;
}

/* Labels line up against the value column; the last visible column takes the leftover width */
void
MetadataLayout::DoLayout(
    DirectUI::Element *peContainer,
    int cx,
    int cy)
{
    int cyAvail = max(cy - METADATA_BOTTOMGAP, 0);
    int cxValue;
    int xColumn = 0;

    if (!_rgcxColumns || !_rgpeChildren || _cxLast != cx || _cyLast != cyAvail)
        return;

    for (UINT i = 0; i < _cChildren; i++)
    {
        if (_rgpeChildren[i])
            UpdateLayoutRect(peContainer, cx, cyAvail, _rgpeChildren[i], 0, 0, 0, 0);
    }

    cxValue = Metadata_ScaleForDpi(METADATA_CXVALUE);
    for (UINT iColumn = 0; iColumn < _cColumns && iColumn <= _iLastFitColumn; iColumn++)
    {
        int cxColumn = _rgcxColumns[iColumn];

        for (UINT iRow = 0; iRow < _cRowsPerColumn; iRow++)
        {
            UINT i = iColumn * _cRowsPerColumn + iRow;
            DirectUI::Element *pe;
            DirectUI::Element *peLabel;
            DirectUI::Element *peControl;
            int x = xColumn;
            int y = _cyRow * iRow;
            int cxRow;
            int cyRow;
            int cxExtra = 0;
            BOOL fValueControl = TRUE;

            if (i >= _cChildren)
                break;
            pe = _rgpeChildren[i];
            if (!pe)
                continue;

            peLabel = pe->FindDescendent(StrToID(L"PreviewMetadataLabel"));
            if (peLabel)
            {
                const SIZE *psizeLabel = peLabel->GetDesiredSize();

                cxRow = cx;
                if (cxValue + psizeLabel->cx < cx)
                    cxRow = cxValue + psizeLabel->cx;
                cyRow = psizeLabel->cy;
                x = xColumn + cxColumn - cxRow;
            }
            else
            {
                cxRow = cxColumn;
                cyRow = pe->GetDesiredSize()->cy;
            }

            if (iColumn == _iLastFitColumn)
            {
                cxExtra = cx - cxColumn - xColumn;
                cxRow += cxExtra;
            }

            if (pe == _peEditButtons)
            {
                y += 3;
                if (cyRow != _cyRow + 3)
                {
                    cyRow = _cyRow + 3;
                    _peEditButtons->SetHeight(cyRow);
                }
            }

            peControl = pe->FindDescendent(StrToID(L"PreviewMetadataControl"));
            if (!peControl)
            {
                fValueControl = FALSE;
                peControl = pe->FindDescendent(StrToID(L"PreviewMetadataTitleControl"));
                if (!peControl)
                    peControl = pe->FindDescendent(StrToID(L"PreviewMetadataSubTitleControl"));
            }
            if (peControl)
                peControl->SetWidth(fValueControl ? cxExtra + cxValue : cxRow);

            UpdateLayoutRect(peContainer, cx, cyAvail, pe, x, y, cxRow, cyRow);
        }
        xColumn += cxColumn;
    }

    _FreeLayoutObjects();
}

/* CMetadataLabel */

static DirectUI::PropCapability s_pcString = { DirectUI::ValueType::String, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidLabelThemeSubApp;
static DirectUI::PropertyInfoData s_pidDisplayThemeSubApp;

/* Normal and cascading, so the style sheets can name the control theme */
static const DirectUI::PropertyInfo s_piLabelThemeSubApp =
    { L"ThemeSubApp", 6, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidLabelThemeSubApp };
static const DirectUI::PropertyInfo s_piDisplayThemeSubApp =
    { L"ThemeSubApp", 6, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidDisplayThemeSubApp };

static const DirectUI::PropertyInfo *const c_rgpiMetadataLabel[] =
{
    &s_piLabelThemeSubApp,
};

static const DirectUI::PropertyInfo *const c_rgpiDisplayElement[] =
{
    &s_piDisplayThemeSubApp,
};

const DirectUI::PropertyInfo *const CMetadataLabel::ThemeSubAppProp = &s_piLabelThemeSubApp;
const DirectUI::PropertyInfo *const CDisplayElement::ThemeSubAppProp = &s_piDisplayThemeSubApp;

DirectUI::IClassInfo *CMetadataLabel::Class = NULL;
DirectUI::IClassInfo *CMetadataElement::Class = NULL;
DirectUI::IClassInfo *CDisplayElement::Class = NULL;
DirectUI::IClassInfo *CPreviewMetadataInner::Class = NULL;

template <class T>
static
HRESULT
WINAPI
Metadata_CreateElement(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    T *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<T>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (FAILED(hr))
        pe->Destroy(false);
    else
        *ppElement = pe;
    return hr;
}

HRESULT
WINAPI
CMetadataLabel::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return Metadata_CreateElement<CMetadataLabel>(peParent, pdwDeferCookie, ppElement);
}

HRESULT
WINAPI
CMetadataLabel::Register()
{
    return DirectUI::ClassInfo<CMetadataLabel, DirectUI::Element, DirectUI::StandardCreator<CMetadataLabel>>::Register(
        DUIFRAME_HINST, L"MetadataLabel", c_rgpiMetadataLabel, _countof(c_rgpiMetadataLabel), false);
}

/* CMetadataElement */

HRESULT
WINAPI
CMetadataElement::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return Metadata_CreateElement<CMetadataElement>(peParent, pdwDeferCookie, ppElement);
}

HRESULT
WINAPI
CMetadataElement::Register()
{
    return DirectUI::ClassInfo<CMetadataElement, CMetadataLabel, DirectUI::StandardCreator<CMetadataElement>>::Register(
        DUIFRAME_HINST, L"MetadataElement", NULL, 0, false);
}

void
CMetadataElement::OnDestroy()
{
    IUnknown_SafeReleaseAndNullPtr(&_ppd);
    IUnknown_SafeReleaseAndNullPtr(&_ppdp);
    IUnknown_SafeReleaseAndNullPtr(&_punkValue);
    _fDestroyed = TRUE;
    CMetadataLabel::OnDestroy();
}

HRESULT
CMetadataElement::SetCurrentValue(
    _In_opt_ IUnknown *punkValue)
{
    IUnknown_Set(&_punkValue, punkValue);
    if (_peDisplay)
    {
        _peDisplay->SetValueObject(punkValue);
        InvalidateGadget(_peDisplay->GetDisplayNode());
    }
    return S_OK;
}

/*
 * A row is read-only when the caller says so, when the property is innate, or when the property
 * has no edit control. ReactOS registers no property controls, so every row ends up read-only.
 */
HRESULT
CMetadataElement::InitializeForDisplay(
    _In_ IPropertyDescription *ppd,
    _In_ BOOL fEditable,
    _In_ REFCLSID rclsidEditControl)
{
    IPropertyDescriptionPriv *ppdp;
    HRESULT hr;

    IUnknown_Set((IUnknown **)&_ppd, ppd);
    hr = ppd->QueryInterface(IID_PPV_ARGS(&ppdp));
    if (FAILED(hr))
        return hr;

    IUnknown_Set((IUnknown **)&_ppdp, ppdp);
    _clsidEditControl = rclsidEditControl;
    _fReadOnly = !fEditable;
    if (fEditable)
    {
        PROPDESC_TYPE_FLAGS pdtf = PDTF_DEFAULT;
        CLSID clsid;

        ppd->GetTypeFlags(PDTF_ISINNATE, &pdtf);
        _fReadOnly = (pdtf & PDTF_ISINNATE) != 0;
        if (!_fReadOnly)
            _fReadOnly = FAILED(ppdp->GetControlCLSID(PROPCONTROL_EDIT, &clsid));
    }
    _fInitialized = TRUE;

    ppdp->Release();
    return _SwitchToResting();
}

HRESULT
CMetadataElement::_SwitchToResting()
{
    if (_fDestroyed || _peDisplay)
        return S_OK;
    return _AddRestingControl();
}

HRESULT
CMetadataElement::_AddRestingControl()
{
    DirectUI::Element *pe;
    HRESULT hr;

    hr = CDisplayElement::Create(this, NULL, &pe);
    if (FAILED(hr))
        return hr;

    _peDisplay = (CDisplayElement *)pe;
    hr = _peDisplay->InitializeTargetInfo(_punkValue, _ppd, _ppdp, _fReadOnly);
    if (SUCCEEDED(hr))
    {
        _peDisplay->SetLayoutPos(ROSUI_LP_CLIENT);
        hr = Add(_peDisplay);
    }

    if (FAILED(hr))
    {
        _peDisplay->Destroy(true);
        _peDisplay = NULL;
    }
    return hr;
}

/* CDisplayElement */

HRESULT
WINAPI
CDisplayElement::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CDisplayElement *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CDisplayElement>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->DirectUI::Button::Initialize(0, peParent, (DWORD *)pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    pe->SetActive(1);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CDisplayElement::Register()
{
    return DirectUI::ClassInfo<CDisplayElement, DirectUI::Button, DirectUI::StandardCreator<CDisplayElement>>::Register(
        DUIFRAME_HINST, L"MetadataDisplayElement", c_rgpiDisplayElement, _countof(c_rgpiDisplayElement), false);
}

void
CDisplayElement::OnDestroy()
{
    IUnknown_SafeReleaseAndNullPtr(&_punkValue);
    DirectUI::Button::OnDestroy();
}

void
CDisplayElement::SetValueObject(
    _In_opt_ IUnknown *punkValue)
{
    IUnknown_Set(&_punkValue, punkValue);
}

/* Without a draw control the element shows the formatted value as its content */
HRESULT
CDisplayElement::InitializeTargetInfo(
    _In_opt_ IUnknown *punkValue,
    _In_ IPropertyDescription *ppd,
    _In_ IPropertyDescriptionPriv *ppdp,
    _In_ BOOL fReadOnly)
{
    PROPVARIANT propvar;
    PWSTR pszDisplay = NULL;
    PWSTR pszName = NULL;
    CLSID clsid;
    HRESULT hr;

    IUnknown_Set(&_punkValue, punkValue);
    _fReadOnly = fReadOnly;

    SetAccessible(true);
    SetAccRole(ROLE_SYSTEM_TEXT);
    SetAccState(_fReadOnly ? STATE_SYSTEM_READONLY : STATE_SYSTEM_FOCUSABLE);
    SetID(_fReadOnly ? L"PreviewMetadataControlReadOnly" : L"PreviewMetadataControlEditable");
    SetActive(_fReadOnly ? 1 : 3);

    /* A draw control would render the value; none can be created here, so a row asking for one stays text */
    if (SUCCEEDED(ppdp->GetControlCLSID(_fReadOnly ? PROPCONTROL_DRAW : PROPCONTROL_EDIT, &clsid)) &&
        !IsEqualCLSID(clsid, CLSID_NULL))
    {
        _fReadOnly = TRUE;
    }

    hr = Metadata_GetPropVariant(_punkValue, &propvar);
    if (SUCCEEDED(hr))
    {
        hr = ppd->FormatForDisplay(propvar, _fReadOnly ? PDFF_READONLY : PDFF_USEEDITINVITATION, &pszDisplay);
        PropVariantClear(&propvar);
    }

    if (SUCCEEDED(ppd->GetDisplayName(&pszName)))
    {
        SetAccName(pszName);
        CoTaskMemFree(pszName);
    }

    SetAccValue(pszDisplay ? pszDisplay : L"");
    SetContentString((pszDisplay && *pszDisplay) ? pszDisplay : L" ");
    CoTaskMemFree(pszDisplay);
    return hr;
}

/* CPreviewMetadataInner */

HRESULT
WINAPI
CPreviewMetadataInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CPreviewMetadataInner *pe;
    DirectUI::Element *peMarkup = NULL;
    DirectUI::Layout *pLayout = NULL;
    WCHAR szName[64];
    HRESULT hr;

    *ppElement = NULL;

    /* An administrator can raise the selection size that is read without asking */
    if (!s_dwMaxViewableItems)
    {
        DWORD cbData = sizeof(s_dwMaxViewableItems);

        if (SHRegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                           L"DetailsPaneMaxAutoExtract", SRRF_RT_REG_DWORD, NULL, &s_dwMaxViewableItems,
                           &cbData) != ERROR_SUCCESS &&
            SHRegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                           L"DetailsPaneMaxAutoExtract", SRRF_RT_REG_DWORD, NULL, &s_dwMaxViewableItems,
                           &cbData) != ERROR_SUCCESS)
        {
            s_dwMaxViewableItems = 0;
        }
        if (!s_dwMaxViewableItems)
            s_dwMaxViewableItems = METADATA_DEFAULTMAXVIEWABLE;
    }

    pe = DirectUI::HNewAndZero<CPreviewMetadataInner>();
    if (!pe)
        return E_OUTOFMEMORY;

    pe->_uNullSelectCacheCount = (UINT)-1;
    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(1);
    if (SUCCEEDED(hr))
        hr = DirectUI::ShellBorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        hr = pe->SetLayout(pLayout);
    if (SUCCEEDED(hr))
        hr = DUI_CreateElementFromResource(DUIFRAME_HINST, 8, L"PreviewMetadata", pe, peParent, NULL, &peMarkup, NULL);
    if (SUCCEEDED(hr))
        hr = MetadataLayout::Create(&pLayout);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    LoadStringW(DUIFRAME_HINST, IDS_DUI_METADATAMODULE, szName, _countof(szName));
    pe->SetAccessible(true);
    pe->SetAccName(szName);
    pe->SetAccRole(35);
    pe->SetLayout(pLayout);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CPreviewMetadataInner::Register()
{
    return DirectUI::ClassInfo<CPreviewMetadataInner, CElementWithSite,
                               DirectUI::StandardCreator<CPreviewMetadataInner>>::Register(
        DUIFRAME_HINST, L"PreviewMetadataInner", NULL, 0, false);
}

STDMETHODIMP
CPreviewMetadataInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CPreviewMetadataInner, IFrameNotificationClient),
        QITABENT(CPreviewMetadataInner, IFrameShellViewClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

void
CPreviewMetadataInner::OnDestroy()
{
    CancelDelayedClear();
    if (_pwiPending)
    {
        _pwiPending->DetachInner();
        _pwiPending->SetDispatchable(FALSE);
        _pwiPending->Release();
        _pwiPending = NULL;
    }
    IUnknown_SafeReleaseAndNullPtr(&_psiaShown);
    IUnknown_SafeReleaseAndNullPtr(&_psiaLast);
    IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);
    if (_pte && _fTracking)
        _pte->OperationEvent(L"PreviewMetadataInner", (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)1);
    IUnknown_SafeReleaseAndNullPtr(&_pte);
    CElementWithSite::OnDestroy();
}

STDMETHODIMP
CPreviewMetadataInner::LayoutInitialized()
{
    DirectUI::Element *peModule = GetParent();

    if (peModule)
    {
        DirectUI::Value *pv = peModule->GetValue(CPreviewMetadata::ReadOnlyProp, 2, NULL);

        _fReadOnly = pv->GetBool();
        pv->Release();
    }
    return S_OK;
}

STDMETHODIMP
CPreviewMetadataInner::Notify(
    PCWSTR pszPropertyName)
{
    if (!StrCmpICW(pszPropertyName, L"PreviewPaneSizer_Visible") && DetailsPane_IsPreviewPaneVisible(_punkSite))
        _RefreshMetadata();
    return S_OK;
}

STDMETHODIMP
CPreviewMetadataInner::OnNavigateAway()
{
    _uNullSelectCacheCount = (UINT)-1;
    IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);
    return S_OK;
}

STDMETHODIMP
CPreviewMetadataInner::OnSelectedItemChanged()
{
    _fForceRefresh = TRUE;
    _RefreshMetadata();
    return S_OK;
}

STDMETHODIMP
CPreviewMetadataInner::OnSelectionChanged()
{
    _uNullSelectCacheCount = (UINT)-1;
    IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);
    _fForceRefresh = TRUE;
    _RefreshMetadata();
    return S_OK;
}

/* With nothing selected the item count and the folder's properties follow the folder contents */
STDMETHODIMP
CPreviewMetadataInner::OnContentsChanged()
{
    UINT cSelected;

    if (SUCCEEDED(_GetSelectedCount(&cSelected)) && !cSelected)
        _RefreshMetadata();
    return S_OK;
}

STDMETHODIMP
CPreviewMetadataInner::OnFolderChanged()
{
    _RefreshMetadata();
    return S_OK;
}

void
CPreviewMetadataInner::OnEvent(
    DirectUI::Event *pev)
{
    if (pev->nStage == GMF_BUBBLED && pev->uidType.m_Value == DirectUI::Button::Click().m_Value &&
        pev->peTarget)
    {
        ATOM atomID = pev->peTarget->GetID();

        if (atomID == StrToID(L"PreviewSaveMetadata") || atomID == StrToID(L"PreviewCancelMetadata"))
        {
            /* Nothing is ever edited here, so saving and canceling both just show the stored values */
            _fForceRefresh = TRUE;
            _RefreshMetadata();
        }
        else if (atomID == StrToID(L"PreviewForceLoadMetadata"))
        {
            IShellItemArray *psia;
            IFolderView *pfv;
            UINT cItems;
            UINT cSelected;

            SetItemsShowingMode(METADATA_SHOW_FORCED);
            _ClearPaneAndSetDefaults();
            if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv))))
            {
                if (SUCCEEDED(Metadata_GetShellItemArray(pfv, &psia, &cItems, &cSelected)))
                {
                    _QueueUpExtractionWorkItem(psia, cItems, cSelected);
                    if (psia)
                        psia->Release();
                }
                pfv->Release();
            }
        }
    }
    CElementWithSite::OnEvent(pev);
}

/* Escape and Enter put the stored values back */
void
CPreviewMetadataInner::OnInput(
    DirectUI::InputEvent *pie)
{
    if (pie->nDevice == GINPUT_KEYBOARD)
    {
        DirectUI::KeyboardEvent *pke = (DirectUI::KeyboardEvent *)pie;

        if ((pie->nCode == GKEY_DOWN && pke->ch == VK_ESCAPE) || (pie->nCode == GKEY_UP && pke->ch == VK_RETURN))
        {
            _fForceRefresh = TRUE;
            _RefreshMetadata();
            pie->fHandled = true;
        }
    }
    CElementWithSite::OnInput(pie);
}

HRESULT
CPreviewMetadataInner::_GetSelectedCount(
    _Out_ UINT *pcSelected)
{
    IFolderView *pfv;
    int cSelected = 0;
    HRESULT hr;

    *pcSelected = 0;
    hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (FAILED(hr))
        return hr;

    hr = pfv->ItemCount(SVGIO_SELECTION | SVGIO_FLAG_VIEWORDER, &cSelected);
    if (SUCCEEDED(hr))
    {
        if (cSelected < 0)
        {
            *pcSelected = (UINT)-1;
            hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        }
        else
        {
            *pcSelected = cSelected;
        }
    }
    pfv->Release();
    return hr;
}

void
CPreviewMetadataInner::CancelDelayedClear()
{
    if (_hactClear)
    {
        DeleteHandle(_hactClear);
        _hactClear = NULL;
    }
}

void
CALLBACK
CPreviewMetadataInner::s_ClearPane(
    _In_ GMA_ACTIONINFO *pInfo)
{
    if (!pInfo->fFinished)
        ((CPreviewMetadataInner *)pInfo->pvData)->_ClearPaneAndSetDefaults();
}

void
CPreviewMetadataInner::SetItemsShowingMode(
    _In_ UINT uMode)
{
    _uShowingMode = uMode;
}

void
CPreviewMetadataInner::SetSelectionObjects(
    _In_opt_ IShellItemArray *psia)
{
    IUnknown_Set((IUnknown **)&_psiaLast, psia);
}

HRESULT
CPreviewMetadataInner::ClearPreviewPane()
{
    DirectUI::Element *peEdit;

    if (_fRefreshPending)
        return E_FAIL;

    CancelDelayedClear();
    peEdit = FindDescendent(StrToID(L"PreviewEditMetadata"));
    if (peEdit)
        peEdit->SetVisible(false);
    DestroyAll(true);
    return S_OK;
}

/* Only the item count, plus the "Show more details" link when the selection is too large */
void
CPreviewMetadataInner::_ClearPaneAndSetDefaults()
{
    DirectUI::DUIXmlParser *pParser;

    if (SUCCEEDED(Metadata_CreateParser(_punkSite, &pParser)))
    {
        if (SUCCEEDED(ClearPreviewPane()))
        {
            Metadata_FillPreviewPaneCount(this, _cItemsLast, _cSelectedLast, pParser);
            if (_uShowingMode == METADATA_SHOW_TOOMANY)
            {
                DirectUI::Element *peLink = NULL;

                if (SUCCEEDED(pParser->CreateElement(L"PreviewForceLoadMetadataContainer", NULL, this, NULL, &peLink)))
                    Add(peLink);
            }
        }
        pParser->Destroy();
    }
    CancelDelayedClear();
}

HRESULT
CPreviewMetadataInner::_RefreshMetadata()
{
    IShellItemArray *psia = NULL;
    IFolderView *pfv;
    UINT cItems;
    UINT cSelected;
    HRESULT hr;

    if (!DetailsPane_IsPreviewPaneVisible(_punkSite))
        return S_OK;

    if (!_pte)
        IUnknown_QueryService(_punkSite, IID_ITrackEvents, IID_PPV_ARGS(&_pte));
    if (_pte && !_fTracking)
    {
        _fTracking = TRUE;
        _pte->OperationEvent(L"PreviewMetadataInner", (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)0);
    }

    _fRefreshPending = FALSE;
    hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        hr = Metadata_GetShellItemArray(pfv, &psia, &cItems, &cSelected);
        if (SUCCEEDED(hr))
        {
            if (_fForceRefresh || (_fForceRefresh = _IsDifferentSelection(psia, cItems, cSelected)) != FALSE)
            {
                SetItemsShowingMode(METADATA_SHOW_NORMAL);
                IUnknown_Set((IUnknown **)&_psiaShown, psia);
                _cSelectedLast = cSelected;
            }
            _cItemsLast = cItems;
            hr = _FillPreviewPane(psia, cItems, cSelected);
        }
        if (psia)
            psia->Release();
        pfv->Release();
    }
    if (FAILED(hr))
        _FillPreviewPane(NULL, 0, 0);

    if (_pte && _fTracking)
    {
        _pte->OperationEvent(L"PreviewMetadataInner", (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)1);
        _fTracking = FALSE;
    }
    _fForceRefresh = FALSE;
    return hr;
}

BOOL
CPreviewMetadataInner::_IsDifferentSelection(
    _In_opt_ IShellItemArray *psia,
    _In_ UINT cItems,
    _In_ UINT cSelected)
{
    BOOL fDifferent = TRUE;

    if (!psia)
        return _psiaShown || cSelected != _cSelectedLast || cItems != _cItemsLast;

    if (!_psiaShown || cSelected != _cSelectedLast)
        return TRUE;

    fDifferent = FALSE;
    for (UINT i = 0; i < cItems && !fDifferent; i++)
    {
        IShellItem *psiNew;
        IShellItem *psiOld;

        fDifferent = TRUE;
        if (SUCCEEDED(psia->GetItemAt(i, &psiNew)))
        {
            if (SUCCEEDED(_psiaShown->GetItemAt(i, &psiOld)))
            {
                int iOrder;

                if (SUCCEEDED(psiNew->Compare(psiOld, SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL, &iOrder)))
                    fDifferent = iOrder != 0;
                psiOld->Release();
            }
            psiNew->Release();
        }
    }
    return fDifferent;
}

/* The pane clears to the count if extraction is slow; large selections wait for the user */
HRESULT
CPreviewMetadataInner::_FillPreviewPane(
    _In_opt_ IShellItemArray *psia,
    _In_ UINT cItems,
    _In_ UINT cSelected)
{
    GMA_ACTION action = { 0 };

    CancelDelayedClear();
    action.cbSize = sizeof(action);
    action.flDelay = METADATA_CLEARDELAY;
    action.dwPause = (DWORD)-1;
    action.pfnProc = s_ClearPane;
    action.pvData = this;
    _hactClear = CreateAction(&action);

    if (_uShowingMode == METADATA_SHOW_FORCED || _cSelectedLast <= s_dwMaxViewableItems)
        return _QueueUpExtractionWorkItem(psia, cItems, cSelected);

    SetItemsShowingMode(METADATA_SHOW_TOOMANY);
    return S_OK;
}

/* Lists from the folder win over the ones the layout markup names */
HRESULT
CPreviewMetadataInner::_GetNullSelectPropLists(
    _Outptr_ PWSTR *ppszTitleList,
    _Outptr_ PWSTR *ppszList)
{
    IFrameManager *pfm;
    IShellItem2 *psi2 = NULL;
    DirectUI::Element *peModule;
    DirectUI::Value *pv;
    HRESULT hr;

    *ppszTitleList = NULL;
    *ppszList = NULL;

    hr = IUnknown_QueryService(_punkSite, IID_IFrameManager, IID_PPV_ARGS(&pfm));
    if (SUCCEEDED(hr))
    {
        IShellView *psv;

        hr = pfm->GetShellView(&psv);
        if (SUCCEEDED(hr))
        {
            hr = SHGetItemFromObject(psv, IID_PPV_ARGS(&psi2));
            psv->Release();
        }
        pfm->Release();
    }

    if (SUCCEEDED(hr))
    {
        hr = psi2->GetString(c_pkeyDetailsPaneNullSelect, ppszList);
        if (SUCCEEDED(hr))
        {
            hr = psi2->GetString(c_pkeyDetailsPaneNullSelectTitle, ppszTitleList);
            if (FAILED(hr))
            {
                CoTaskMemFree(*ppszList);
                *ppszList = NULL;
            }
        }
        psi2->Release();
    }

    if (SUCCEEDED(hr))
        return hr;

    peModule = GetParent();
    if (!peModule)
        return E_FAIL;

    pv = peModule->GetValue(CPreviewMetadata::NullSelectPropertyStringProp, 2, NULL);
    hr = SHStrDupW(pv->GetString() ? pv->GetString() : L"", ppszList);
    pv->Release();
    if (SUCCEEDED(hr))
    {
        pv = peModule->GetValue(CPreviewMetadata::NullSelectTitlePropertyStringProp, 2, NULL);
        hr = SHStrDupW(pv->GetString() ? pv->GetString() : L"", ppszTitleList);
        pv->Release();
        if (FAILED(hr))
        {
            CoTaskMemFree(*ppszList);
            *ppszList = NULL;
        }
    }
    return hr;
}

/* FALSE when the cached folder values of a finished pass still match the view */
BOOL
CPreviewMetadataInner::_EnsureNullSelectCache()
{
    IViewStatus *pvs;
    IShellView *psv;
    BOOL fNeedWork = TRUE;
    BOOL fDropCache = TRUE;
    UINT cSelected;

    if (SUCCEEDED(_GetSelectedCount(&cSelected)) && !cSelected &&
        SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&psv))))
    {
        if (SUCCEEDED(psv->QueryInterface(IID_PPV_ARGS(&pvs))))
        {
            DEFVIEWSTATUS dvs;
            UINT uOperation;

            if (SUCCEEDED(pvs->GetStatus(&dvs)) && (dvs & 2) && SUCCEEDED(pvs->GetOperationID(&uOperation)))
            {
                fDropCache = FALSE;
                if (_ppscNullSelect && _uNullSelectCacheCount != uOperation)
                    IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);

                if (!_ppscNullSelect)
                {
                    _uNullSelectCacheCount = uOperation;
                    PSCreateMemoryPropertyStore(IID_PPV_ARGS(&_ppscNullSelect));
                }
                else if (_pwiPending && _cItemsLast == _pwiPending->GetItemCount() && !_pwiPending->HasSelection())
                {
                    fNeedWork = FALSE;
                }
            }
            pvs->Release();
        }
        psv->Release();
    }

    if (fDropCache)
        IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);
    return fNeedWork;
}

HRESULT
CPreviewMetadataInner::_QueueUpExtractionWorkItem(
    _In_opt_ IShellItemArray *psia,
    _In_ UINT cItems,
    _In_ UINT cSelected)
{
    CMetadataExtractorWorkItem *pwi;
    IFrameTaskManager *pftm;
    PWSTR pszTitleList;
    PWSTR pszList;
    HRESULT hr;

    hr = DetailsPane_GetTaskManager(_punkSite, &pftm);
    if (FAILED(hr))
        return hr;

    if (!_EnsureNullSelectCache())
    {
        pftm->Release();
        return S_OK;
    }

    pwi = new CMetadataExtractorWorkItem();
    hr = pwi ? S_OK : E_OUTOFMEMORY;
    if (pwi)
    {
        hr = _GetNullSelectPropLists(&pszTitleList, &pszList);
        if (SUCCEEDED(hr))
        {
            hr = pwi->SetInfo(this, _fReadOnly, pszList, pszTitleList, _ppscNullSelect, psia, cItems, cSelected,
                              _punkSite);
            if (SUCCEEDED(hr))
                hr = pftm->StartWorkItem(pwi, TOID_PreviewPropertyTask, FTMF_REPLACE);
            if (SUCCEEDED(hr))
            {
                if (_pwiPending)
                {
                    _pwiPending->DetachInner();
                    _pwiPending->Release();
                }
                _pwiPending = pwi;
                pwi->AddRef();
            }
            CoTaskMemFree(pszTitleList);
            CoTaskMemFree(pszList);
        }
        pwi->Release();
    }

    if (FAILED(hr))
    {
        _uNullSelectCacheCount = (UINT)-1;
        IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);
    }
    pftm->Release();
    return hr;
}

/* CMetadataExtractorWorkItem */

CMetadataExtractorWorkItem::CMetadataExtractorWorkItem()
{
}

CMetadataExtractorWorkItem::~CMetadataExtractorWorkItem()
{
    IUnknown_SafeReleaseAndNullPtr(&_ppscValues);
    IUnknown_SafeReleaseAndNullPtr(&_psiaSelection);
    IUnknown_SafeReleaseAndNullPtr(&_ppscNullSelect);
    if (_hdsaKeys)
        DSA_Destroy(_hdsaKeys);
    CoTaskMemFree(_pszNullSelectList);
    CoTaskMemFree(_pszNullSelectTitleList);
    ILFree(_pidlFolder);
    if (_pte && !_fTrackEnded)
        _pte->OperationEvent(L"MetadataExtractorWorkItem", (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)1);
    IUnknown_SafeReleaseAndNullPtr(&_pte);
}

/*
 * Win7 marshals the selection and the folder cache to the worker thread. Both are plain shell
 * objects in ReactOS with no thread affinity, so the work item holds them directly.
 */
HRESULT
CMetadataExtractorWorkItem::SetInfo(
    _In_ CPreviewMetadataInner *pInner,
    _In_ BOOL fReadOnly,
    _In_ PCWSTR pszNullSelectList,
    _In_ PCWSTR pszNullSelectTitleList,
    _In_opt_ IPropertyStoreCache *ppscNullSelect,
    _In_opt_ IShellItemArray *psia,
    _In_ UINT cItems,
    _In_ UINT cSelected,
    _In_ IUnknown *punkSite)
{
    IShellFolder *psf;
    IFolderView *pfv;
    HRESULT hr;

    hr = SHStrDupW(pszNullSelectList, &_pszNullSelectList);
    if (SUCCEEDED(hr))
        hr = SHStrDupW(pszNullSelectTitleList, &_pszNullSelectTitleList);
    if (FAILED(hr))
        return hr;

    _uReadOnlyState = fReadOnly ? METADATA_RO_READONLY : METADATA_RO_UNKNOWN;
    _hdsaKeys = DSA_Create(sizeof(PROPERTYKEY), 8);
    if (!_hdsaKeys)
        return E_OUTOFMEMORY;

    IUnknown_Set((IUnknown **)&_ppscNullSelect, ppscNullSelect);

    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (FAILED(hr))
        return hr;

    IUnknown_Set((IUnknown **)&_psiaSelection, psia);
    _cItems = cItems;
    _cSelected = cSelected;
    _pInner = pInner;

    hr = pfv->GetFolder(IID_PPV_ARGS(&psf));
    if (SUCCEEDED(hr))
    {
        hr = SHGetIDListFromObject(psf, &_pidlFolder);
        if (SUCCEEDED(hr))
        {
            IUnknown_QueryService(punkSite, IID_ITrackEvents, IID_PPV_ARGS(&_pte));
            if (_pte)
                _pte->OperationEvent(L"MetadataExtractorWorkItem", (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)0);
        }
        psf->Release();
    }
    pfv->Release();
    return hr;
}

HRESULT
CMetadataExtractorWorkItem::_GetKeyAt(
    _In_ UINT i,
    _Out_ PROPERTYKEY *pkey)
{
    if ((int)i >= DSA_GetItemCount(_hdsaKeys))
        return E_INVALIDARG;
    *pkey = *(PROPERTYKEY *)DSA_GetItemPtr(_hdsaKeys, i);
    return S_OK;
}

void
CMetadataExtractorWorkItem::_RemoveKey(
    _In_ REFPROPERTYKEY key)
{
    for (int i = DSA_GetItemCount(_hdsaKeys) - 1; i >= 0; i -= 1)
    {
        if (IsEqualPropertyKey(*(PROPERTYKEY *)DSA_GetItemPtr(_hdsaKeys, i), key))
            DSA_DeleteItem(_hdsaKeys, i);
    }
}

/* Empty values of "show only if present" properties are left out */
HRESULT
CMetadataExtractorWorkItem::_AddKeyAndValueToPropArray(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_VIEW_FLAGS pdvf,
    _In_ BOOL fReadOnly)
{
    HRESULT hr;

    if (propvar.vt == VT_EMPTY && (pdvf & PDVF_SHOWONLYIFPRESENT))
        return E_FAIL;

    if (DSA_AppendItem(_hdsaKeys, (void *)&key) == DSA_ERR)
        return E_OUTOFMEMORY;

    hr = _ppscValues->SetValueAndState(key, &propvar, fReadOnly ? PSC_READONLY : PSC_NORMAL);
    if (FAILED(hr))
        _RemoveKey(key);
    return hr;
}

HRESULT
CMetadataExtractorWorkItem::_AddFileCount(
    _In_ UINT cItems)
{
    PROPVARIANT propvar;

    propvar.vt = VT_UI8;
    propvar.uhVal.QuadPart = cItems;
    return _AddKeyAndValueToPropArray(c_pkeyFileCount, propvar, PDVF_DEFAULT, TRUE);
}

/* A cached value wins; otherwise the store's value is read and remembered in the cache */
HRESULT
CMetadataExtractorWorkItem::_AddToPropArray(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _In_opt_ IPropertyStoreCache *ppscCache,
    _Inout_opt_ BOOL *pfAnyEditable)
{
    PROPDESC_VIEW_FLAGS pdvf;
    PROPVARIANT propvar;
    PROPERTYKEY key;
    PSC_STATE state;
    PWSTR pszDisplay;
    HRESULT hr;

    hr = ppd->GetViewFlags(&pdvf);
    if (SUCCEEDED(hr))
        hr = ppd->GetPropertyKey(&key);
    if (FAILED(hr))
        return hr;

    PropVariantInit(&propvar);
    hr = E_FAIL;
    if (ppscCache)
        hr = ppscCache->GetValueAndState(key, &propvar, &state);
    if (FAILED(hr))
    {
        hr = pps->GetValue(key, &propvar);
        if (SUCCEEDED(hr) && ppscCache)
            ppscCache->SetValueAndState(key, &propvar, PSC_NORMAL);
    }
    if (FAILED(hr))
        return hr;

    hr = ppd->FormatForDisplay(propvar, PDFF_DEFAULT, &pszDisplay);
    if (SUCCEEDED(hr))
    {
        BOOL fReadOnly = _uReadOnlyState == METADATA_RO_READONLY;

        if (!fReadOnly)
        {
            PROPDESC_TYPE_FLAGS pdtf = PDTF_DEFAULT;
            IPropertyStoreCapabilities *ppsc;

            ppd->GetTypeFlags(PDTF_ISINNATE, &pdtf);
            fReadOnly = (pdtf & PDTF_ISINNATE) != 0;
            if (!fReadOnly && SUCCEEDED(pps->QueryInterface(IID_PPV_ARGS(&ppsc))))
            {
                fReadOnly = ppsc->IsPropertyWritable(key) != S_OK;
                ppsc->Release();
            }
        }

        if (!*pszDisplay && (fReadOnly || (pdvf & PDVF_SHOWONLYIFPRESENT)))
        {
            hr = E_INVALIDARG;
        }
        else
        {
            hr = _AddKeyAndValueToPropArray(key, propvar, pdvf, fReadOnly);
            if (pfAnyEditable && !fReadOnly)
                *pfAnyEditable = TRUE;
        }
        CoTaskMemFree(pszDisplay);
    }
    PropVariantClear(&propvar);
    return hr;
}

void
CMetadataExtractorWorkItem::_AddListToPropArray(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescriptionList *ppdl,
    _In_opt_ IPropertyStoreCache *ppscCache,
    _Inout_opt_ BOOL *pfAnyEditable)
{
    UINT cDescriptions;

    if (FAILED(ppdl->GetCount(&cDescriptions)))
        return;

    for (UINT i = 0; _ShouldContinue() && i < cDescriptions; i++)
    {
        IPropertyDescription *ppd;

        if (SUCCEEDED(ppdl->GetAt(i, IID_PPV_ARGS(&ppd))))
        {
            _AddToPropArray(pps, ppd, ppscCache, pfAnyEditable);
            ppd->Release();
        }
    }
}

/* The display name shows as the title; other title properties read like any other */
HRESULT
CMetadataExtractorWorkItem::_AddTitleToPropArray(
    _In_ IShellItem *psi,
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd)
{
    PROPDESC_VIEW_FLAGS pdvf = PDVF_DEFAULT;
    PROPVARIANT propvar;
    PROPERTYKEY key;
    PWSTR pszName;
    HRESULT hr;

    hr = ppd->GetPropertyKey(&key);
    if (FAILED(hr))
        return hr;

    if (!IsEqualPropertyKey(key, c_pkeyItemNameDisplay))
        return _AddToPropArray(pps, ppd, NULL, NULL);

    hr = psi->GetDisplayName(SIGDN_NORMALDISPLAY, &pszName);
    if (SUCCEEDED(hr))
    {
        ppd->GetViewFlags(&pdvf);
        PropVariantInit(&propvar);
        propvar.vt = VT_LPWSTR;
        propvar.pwszVal = pszName;
        hr = _AddKeyAndValueToPropArray(key, propvar, pdvf, TRUE);
        CoTaskMemFree(pszName);
    }
    return hr;
}

/* Writable items drop the empty rows of read-only items again */
BOOL
CMetadataExtractorWorkItem::_ComputeItemReadOnly(
    _In_ IShellItemArray *psia)
{
    DWORD cItems;

    if (_uReadOnlyState == METADATA_RO_UNKNOWN)
    {
        _uReadOnlyState = METADATA_RO_WRITABLE;
        if (SUCCEEDED(psia->GetCount(&cItems)))
        {
            for (DWORD i = 0; _uReadOnlyState == METADATA_RO_WRITABLE && i < cItems; i++)
            {
                IShellItem *psi;
                SFGAOF sfgao;

                if (SUCCEEDED(psia->GetItemAt(i, &psi)))
                {
                    if (FAILED(psi->GetAttributes(SFGAO_READONLY | SFGAO_STORAGE, &sfgao)) ||
                        (sfgao & SFGAO_READONLY))
                    {
                        _uReadOnlyState = METADATA_RO_READONLY;
                    }
                    psi->Release();
                }
            }
        }
    }
    return _uReadOnlyState == METADATA_RO_READONLY;
}

void
CMetadataExtractorWorkItem::_ExtractSelection(
    _In_ UINT cSelected,
    _In_ IShellItemArray *psia)
{
    IPropertyDescriptionList *ppdlDetails = NULL;
    IPropertyDescriptionList *ppdlTitle = NULL;
    IPropertyStore *pps;
    BOOL fAnyEditable = FALSE;

    if (SUCCEEDED(psia->GetPropertyStore((GETPROPERTYSTOREFLAGS)(GPS_DELAYCREATION | GPS_BESTEFFORT),
                                         IID_PPV_ARGS(&pps))))
    {
        if (cSelected == 1)
        {
            IPropertyDescriptionList *ppdl;
            UINT cTitles = 0;
            HRESULT hr = E_FAIL;

            /* The type's title list, or the display name when the type has none */
            if (SUCCEEDED(psia->GetPropertyDescriptionList(c_pkeyPreviewTitle, IID_PPV_ARGS(&ppdl))))
            {
                hr = ppdl->GetCount(&cTitles);
                if (SUCCEEDED(hr))
                    hr = cTitles ? ppdl->QueryInterface(IID_PPV_ARGS(&ppdlTitle)) : E_FAIL;
                ppdl->Release();
            }
            if (FAILED(hr) &&
                FAILED(PSGetPropertyDescriptionListFromString(L"prop:System.ItemNameDisplay",
                                                              IID_PPV_ARGS(&ppdlTitle))))
            {
                ppdlTitle = NULL;
            }

            if (ppdlTitle)
            {
                IShellItem *psi;

                if (SUCCEEDED(psia->GetItemAt(0, &psi)))
                {
                    for (UINT i = 0; i < 2; i++)
                    {
                        IPropertyDescription *ppd;

                        if (SUCCEEDED(ppdlTitle->GetAt(i, IID_PPV_ARGS(&ppd))))
                        {
                            if (SUCCEEDED(_AddTitleToPropArray(psi, pps, ppd)))
                                _cTitles++;
                            ppd->Release();
                        }
                    }
                    psi->Release();
                }
                ppdlTitle->Release();
            }
        }
        else if (SUCCEEDED(_AddFileCount(cSelected)))
        {
            _cTitles++;
        }

        if (SUCCEEDED(psia->GetPropertyDescriptionList(c_pkeyPreviewDetails, IID_PPV_ARGS(&ppdlDetails))))
        {
            IPropertyDescription *ppdStatus;

            if (SUCCEEDED(PSGetPropertyDescription(c_pkeyStatusIcons, IID_PPV_ARGS(&ppdStatus))))
            {
                _AddToPropArray(pps, ppdStatus, NULL, &fAnyEditable);
                ppdStatus->Release();
            }
            _AddListToPropArray(pps, ppdlDetails, NULL, &fAnyEditable);
        }
        pps->Release();
    }

    if (!ppdlDetails)
        return;

    /* Read-only items do not show the edit invitations of their empty properties */
    if (fAnyEditable && _ComputeItemReadOnly(psia))
    {
        UINT cDescriptions;

        if (SUCCEEDED(ppdlDetails->GetCount(&cDescriptions)))
        {
            for (UINT i = 0; i < cDescriptions; i++)
            {
                IPropertyDescription *ppd;
                PROPVARIANT propvar;
                PROPERTYKEY key;
                PSC_STATE state;

                if (FAILED(ppdlDetails->GetAt(i, IID_PPV_ARGS(&ppd))))
                    continue;

                if (SUCCEEDED(ppd->GetPropertyKey(&key)) &&
                    SUCCEEDED(_ppscValues->GetValueAndState(key, &propvar, &state)))
                {
                    PWSTR pszDisplay;

                    if (SUCCEEDED(ppd->FormatForDisplay(propvar, PDFF_DEFAULT, &pszDisplay)))
                    {
                        if (!*pszDisplay)
                            _RemoveKey(key);
                        CoTaskMemFree(pszDisplay);
                    }
                    PropVariantClear(&propvar);
                }
                ppd->Release();
            }
        }
    }
    ppdlDetails->Release();
}

void
CMetadataExtractorWorkItem::_ExtractNullSelection(
    _In_ UINT cItems)
{
    IPropertyStoreCache *ppscCache = _ppscNullSelect;
    IPropertyDescriptionList *ppdl;
    IPropertyDescription *ppdStatus = NULL;
    IPropertyStore *pps;
    IShellItem2 *psi2;

    if (FAILED(SHCreateItemFromIDList(_pidlFolder, IID_PPV_ARGS(&psi2))))
        return;

    if (SUCCEEDED(psi2->GetPropertyStore((GETPROPERTYSTOREFLAGS)(GPS_DELAYCREATION | GPS_BESTEFFORT),
                                         IID_PPV_ARGS(&pps))))
    {
        if (SUCCEEDED(PSGetPropertyDescriptionListFromString(_pszNullSelectTitleList, IID_PPV_ARGS(&ppdl))))
        {
            for (UINT i = 0; i < 2; i++)
            {
                IPropertyDescription *ppd;
                PROPERTYKEY key;

                if (FAILED(ppdl->GetAt(i, IID_PPV_ARGS(&ppd))))
                    continue;

                if (SUCCEEDED(ppd->GetPropertyKey(&key)))
                {
                    HRESULT hr;

                    if (IsEqualPropertyKey(key, c_pkeyFileCount))
                        hr = _AddFileCount(cItems);
                    else
                        hr = _AddToPropArray(pps, ppd, ppscCache, NULL);
                    if (SUCCEEDED(hr))
                        _cTitles++;
                }
                ppd->Release();
            }
            ppdl->Release();
        }

        if (SUCCEEDED(PSGetPropertyDescription(c_pkeyStatusIcons, IID_PPV_ARGS(&ppdStatus))))
            _AddToPropArray(pps, ppdStatus, ppscCache, NULL);

        if (SUCCEEDED(PSGetPropertyDescriptionListFromString(_pszNullSelectList, IID_PPV_ARGS(&ppdl))))
        {
            _AddListToPropArray(pps, ppdl, ppscCache, NULL);
            ppdl->Release();
        }

        pps->Release();
        if (ppdStatus)
            ppdStatus->Release();
    }
    psi2->Release();
}

STDMETHODIMP
CMetadataExtractorWorkItem::DoWork()
{
    HRESULT hr;

    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&_ppscValues));
    if (SUCCEEDED(hr))
    {
        if (_cSelected)
            _ExtractSelection(_cSelected, _psiaSelection);
        else
            _ExtractNullSelection(_cItems);
    }

    if (_uReadOnlyState == METADATA_RO_UNKNOWN)
        _uReadOnlyState = METADATA_RO_READONLY;

    if (_pte)
    {
        _pte->OperationEvent(L"MetadataExtractorWorkItem", (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)1);
        _fTrackEnded = TRUE;
    }
    return hr;
}

/* Returns the next key index; a title slot without a value still gets its empty row */
UINT
CMetadataExtractorWorkItem::_AddTitle(
    _In_ BOOL fHasTitle,
    _In_ UINT iKey,
    _In_ PCWSTR pszResID,
    _In_ PCWSTR pszControlID,
    _In_ DirectUI::DUIXmlParser *pParser)
{
    DirectUI::Element *peRow;
    PROPERTYKEY key;
    BOOL fAdded = FALSE;

    if (fHasTitle && SUCCEEDED(_GetKeyAt(iKey, &key)))
    {
        PROPVARIANT propvar;
        PSC_STATE state;

        if (SUCCEEDED(_ppscValues->GetValueAndState(key, &propvar, &state)))
        {
            BOOL fReadOnly = state == PSC_READONLY || _uReadOnlyState == METADATA_RO_READONLY;

            if (SUCCEEDED(_AddTitleRow(key, propvar, pszResID, pszControlID, fReadOnly, pParser)))
                fAdded = TRUE;
            PropVariantClear(&propvar);
        }
        iKey++;
    }

    if (!fAdded && SUCCEEDED(pParser->CreateElement(pszResID, NULL, _pInner, NULL, &peRow)))
        _pInner->Add(peRow);
    return iKey;
}

HRESULT
CMetadataExtractorWorkItem::_AddTitleRow(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar,
    _In_ PCWSTR pszResID,
    _In_ PCWSTR pszControlID,
    _In_ BOOL fReadOnly,
    _In_ DirectUI::DUIXmlParser *pParser)
{
    IPropertyDescription *ppd;
    DirectUI::Element *peRow = NULL;
    CMetadataElement *peControl;
    IUnknown *punkValue;
    HRESULT hr;

    hr = PSGetPropertyDescription(key, IID_PPV_ARGS(&ppd));
    if (FAILED(hr))
        return hr;

    if (IsEqualPropertyKey(key, c_pkeyFileCount))
    {
        hr = Metadata_FillPreviewPaneCount(_pInner, _cItems, _cSelected, pParser);
        ppd->Release();
        return hr;
    }

    hr = pParser->CreateElement(pszResID, NULL, _pInner, NULL, &peRow);
    if (SUCCEEDED(hr))
    {
        _pInner->Add(peRow);
        peControl = (CMetadataElement *)peRow->FindDescendent(StrToID(pszControlID));
        if (!peControl)
        {
            hr = E_FAIL;
        }
        else
        {
            if (!fReadOnly)
                _fAnyEditableRow = TRUE;
            if (SUCCEEDED(DetailsPane_CreatePropertyValue(propvar, key, IID_PPV_ARGS(&punkValue))))
            {
                peControl->SetCurrentValue(punkValue);
                punkValue->Release();
            }
            hr = peControl->InitializeForDisplay(ppd, !fReadOnly, CLSID_NULL);
        }
    }
    ppd->Release();
    return hr;
}

/* Builds the rows on the frame thread: title, subtitle, then one row per property */
STDMETHODIMP
CMetadataExtractorWorkItem::Dispatch()
{
    DirectUI::DUIXmlParser *pParser;
    DirectUI::Element *peSpacer;
    ULONG dwDeferCookie = 0;
    IUnknown *punkSite;
    HRESULT hr = E_FAIL;

    if (!_pInner || _pInner->GetItemsShowingMode() == METADATA_SHOW_TOOMANY)
        return E_FAIL;

    _pInner->StartDefer(&dwDeferCookie);
    if (_ppscValues && SUCCEEDED(_pInner->ClearPreviewPane()))
    {
        hr = _pInner->GetSite(IID_PPV_ARGS(&punkSite));
        if (SUCCEEDED(hr))
        {
            hr = Metadata_CreateParser(punkSite, &pParser);
            if (SUCCEEDED(hr))
            {
                int cKeys = DSA_GetItemCount(_hdsaKeys);

                _pInner->SetSelectionObjects(_psiaSelection);
                if (cKeys > 0)
                {
                    UINT i = _AddTitle(_cTitles != 0, 0, L"PreviewMetadataTitle", L"PreviewMetadataTitleControl",
                                       pParser);

                    if ((int)i < cKeys)
                    {
                        i = _AddTitle(_cTitles > 1, i, L"PreviewMetadataSubTitle", L"PreviewMetadataSubTitleControl",
                                      pParser);
                        for (; (int)i < cKeys; i++)
                        {
                            PROPVARIANT propvar;
                            PROPERTYKEY key;
                            PSC_STATE state;

                            if (FAILED(_GetKeyAt(i, &key)) ||
                                FAILED(_ppscValues->GetValueAndState(key, &propvar, &state)))
                            {
                                continue;
                            }

                            BOOL fReadOnly = state == PSC_READONLY || _uReadOnlyState == METADATA_RO_READONLY;

                            if (!fReadOnly)
                                _fAnyEditableRow = TRUE;
                            Metadata_AddRow(_pInner, key, propvar, fReadOnly, pParser);
                            PropVariantClear(&propvar);
                        }
                    }
                }

                if (SUCCEEDED(pParser->CreateElement(L"PreviewMetadataSpacer", NULL, _pInner, NULL, &peSpacer)))
                    _pInner->Add(peSpacer);
                pParser->Destroy();
            }
            punkSite->Release();
        }
    }

    if (dwDeferCookie)
        _pInner->EndDefer(dwDeferCookie);
    return hr;
}
