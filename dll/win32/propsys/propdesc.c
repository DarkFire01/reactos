/*
 * PROJECT:     ReactOS Property System
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Property descriptions, description lists and enumeration types
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <winuser.h>
#include <winreg.h>
#include <propvarutil.h>
#include "propdesc.h"

#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(propsys);

const IID IID_IPropertyDescriptionPriv =
    { 0xdd51b970, 0x44a1, 0x43b6, { 0x80, 0x51, 0x42, 0xce, 0xed, 0xa6, 0xdd, 0x91 } };

/* Schema lookup */

const PROPSCHEMA_ENTRY *
PropSchema_FindByKey(
    _In_ REFPROPERTYKEY key)
{
    UINT i;

    for (i = 0; i < g_cPropSchema; i++)
    {
        if (g_PropSchema[i].key.pid == key->pid && IsEqualGUID(&g_PropSchema[i].key.fmtid, &key->fmtid))
            return &g_PropSchema[i];
    }
    return NULL;
}

const PROPSCHEMA_ENTRY *
PropSchema_FindByName(
    _In_ PCWSTR pszName)
{
    UINT i;

    for (i = 0; i < g_cPropSchema; i++)
    {
        if (!lstrcmpiW(g_PropSchema[i].pszName, pszName))
            return &g_PropSchema[i];
    }
    return NULL;
}

/* Allocates a copy of a resource string; an id of zero gives an empty string */
HRESULT
PropSchema_LoadString(
    _In_ UINT ids,
    _Outptr_ PWSTR *ppsz)
{
    const WCHAR *pchRes = NULL;
    int cch = ids ? LoadStringW(PROPSYS_HINST, ids, (LPWSTR)&pchRes, 0) : 0;

    *ppsz = CoTaskMemAlloc((cch + 1) * sizeof(WCHAR));
    if (!*ppsz)
        return E_OUTOFMEMORY;
    if (cch)
        CopyMemory(*ppsz, pchRes, cch * sizeof(WCHAR));
    (*ppsz)[cch] = UNICODE_NULL;
    return S_OK;
}

static
HRESULT
PropSchema_DupString(
    _In_ PCWSTR psz,
    _Outptr_ PWSTR *ppsz)
{
    SIZE_T cb = (lstrlenW(psz) + 1) * sizeof(WCHAR);

    *ppsz = CoTaskMemAlloc(cb);
    if (!*ppsz)
        return E_OUTOFMEMORY;
    CopyMemory(*ppsz, psz, cb);
    return S_OK;
}

/* Enumeration types */

typedef struct _PROPENUMTYPE_OBJ
{
    IPropertyEnumType2 IPropertyEnumType2_iface;
    LONG cRef;
    const PROPSCHEMA_ENUM *pEnum;
} PROPENUMTYPE_OBJ;

static inline PROPENUMTYPE_OBJ *
impl_from_IPropertyEnumType2(
    _In_ IPropertyEnumType2 *iface)
{
    return CONTAINING_RECORD(iface, PROPENUMTYPE_OBJ, IPropertyEnumType2_iface);
}

static
HRESULT
PropEnum_InitValue(
    _In_ VARTYPE vt,
    _In_ LONG lValue,
    _In_opt_ PCWSTR pszValue,
    _Out_ PROPVARIANT *ppropvar)
{
    PropVariantInit(ppropvar);
    switch (vt)
    {
        case VT_EMPTY:
            return S_OK;

        case VT_LPWSTR:
            ppropvar->vt = VT_LPWSTR;
            return PropSchema_DupString(pszValue ? pszValue : L"", &ppropvar->pwszVal);

        case VT_BOOL:
            ppropvar->vt = VT_BOOL;
            ppropvar->boolVal = lValue ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;

        case VT_UI2:
            ppropvar->vt = VT_UI2;
            ppropvar->uiVal = (USHORT)lValue;
            return S_OK;

        case VT_UI4:
            ppropvar->vt = VT_UI4;
            ppropvar->ulVal = (ULONG)lValue;
            return S_OK;

        default:
            ppropvar->vt = VT_I4;
            ppropvar->lVal = lValue;
            return S_OK;
    }
}

static HRESULT STDMETHODCALLTYPE
PropEnum_QueryInterface(
    _In_ IPropertyEnumType2 *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyEnumType) ||
        IsEqualIID(riid, &IID_IPropertyEnumType2))
    {
        *ppv = iface;
        IPropertyEnumType2_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
PropEnum_AddRef(
    _In_ IPropertyEnumType2 *iface)
{
    return InterlockedIncrement(&impl_from_IPropertyEnumType2(iface)->cRef);
}

static ULONG STDMETHODCALLTYPE
PropEnum_Release(
    _In_ IPropertyEnumType2 *iface)
{
    PROPENUMTYPE_OBJ *This = impl_from_IPropertyEnumType2(iface);
    ULONG cRef = InterlockedDecrement(&This->cRef);

    if (!cRef)
        HeapFree(GetProcessHeap(), 0, This);
    return cRef;
}

static HRESULT STDMETHODCALLTYPE
PropEnum_GetEnumType(
    _In_ IPropertyEnumType2 *iface,
    _Out_ PROPENUMTYPE *penumtype)
{
    *penumtype = impl_from_IPropertyEnumType2(iface)->pEnum->enumType;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropEnum_GetValue(
    _In_ IPropertyEnumType2 *iface,
    _Out_ PROPVARIANT *ppropvar)
{
    const PROPSCHEMA_ENUM *pEnum = impl_from_IPropertyEnumType2(iface)->pEnum;

    return PropEnum_InitValue(pEnum->vt, pEnum->lValue, pEnum->pszValue, ppropvar);
}

static HRESULT STDMETHODCALLTYPE
PropEnum_GetRangeMinValue(
    _In_ IPropertyEnumType2 *iface,
    _Out_ PROPVARIANT *ppropvarMin)
{
    const PROPSCHEMA_ENUM *pEnum = impl_from_IPropertyEnumType2(iface)->pEnum;

    if (pEnum->enumType != PET_RANGEDVALUE)
    {
        PropVariantInit(ppropvarMin);
        return E_INVALIDARG;
    }
    return PropEnum_InitValue(pEnum->vtMin, pEnum->lMin, NULL, ppropvarMin);
}

static HRESULT STDMETHODCALLTYPE
PropEnum_GetRangeSetValue(
    _In_ IPropertyEnumType2 *iface,
    _Out_ PROPVARIANT *ppropvarSet)
{
    const PROPSCHEMA_ENUM *pEnum = impl_from_IPropertyEnumType2(iface)->pEnum;

    if (pEnum->enumType != PET_RANGEDVALUE)
    {
        PropVariantInit(ppropvarSet);
        return E_INVALIDARG;
    }
    return PropEnum_InitValue(pEnum->vt, pEnum->lValue, NULL, ppropvarSet);
}

static HRESULT STDMETHODCALLTYPE
PropEnum_GetDisplayText(
    _In_ IPropertyEnumType2 *iface,
    _Outptr_ LPWSTR *ppszDisplay)
{
    return PropSchema_LoadString(impl_from_IPropertyEnumType2(iface)->pEnum->idsText, ppszDisplay);
}

static HRESULT STDMETHODCALLTYPE
PropEnum_GetImageReference(
    _In_ IPropertyEnumType2 *iface,
    _Outptr_ LPWSTR *ppszImageRes)
{
    *ppszImageRes = NULL;
    return E_NOTIMPL;
}

static const IPropertyEnumType2Vtbl PropEnum_Vtbl =
{
    PropEnum_QueryInterface,
    PropEnum_AddRef,
    PropEnum_Release,
    PropEnum_GetEnumType,
    PropEnum_GetValue,
    PropEnum_GetRangeMinValue,
    PropEnum_GetRangeSetValue,
    PropEnum_GetDisplayText,
    PropEnum_GetImageReference
};

static
HRESULT
PropEnum_Create(
    _In_ const PROPSCHEMA_ENUM *pEnum,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPENUMTYPE_OBJ *This;
    HRESULT hr;

    *ppv = NULL;
    This = HeapAlloc(GetProcessHeap(), 0, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IPropertyEnumType2_iface.lpVtbl = &PropEnum_Vtbl;
    This->cRef = 1;
    This->pEnum = pEnum;
    hr = IPropertyEnumType2_QueryInterface(&This->IPropertyEnumType2_iface, riid, ppv);
    IPropertyEnumType2_Release(&This->IPropertyEnumType2_iface);
    return hr;
}

/* Enumeration type lists */

typedef struct _PROPENUMLIST_OBJ
{
    IPropertyEnumTypeList IPropertyEnumTypeList_iface;
    LONG cRef;
    const PROPSCHEMA_ENTRY *pEntry;
} PROPENUMLIST_OBJ;

static inline PROPENUMLIST_OBJ *
impl_from_IPropertyEnumTypeList(
    _In_ IPropertyEnumTypeList *iface)
{
    return CONTAINING_RECORD(iface, PROPENUMLIST_OBJ, IPropertyEnumTypeList_iface);
}

static HRESULT STDMETHODCALLTYPE
PropEnumList_QueryInterface(
    _In_ IPropertyEnumTypeList *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyEnumTypeList))
    {
        *ppv = iface;
        IPropertyEnumTypeList_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
PropEnumList_AddRef(
    _In_ IPropertyEnumTypeList *iface)
{
    return InterlockedIncrement(&impl_from_IPropertyEnumTypeList(iface)->cRef);
}

static ULONG STDMETHODCALLTYPE
PropEnumList_Release(
    _In_ IPropertyEnumTypeList *iface)
{
    PROPENUMLIST_OBJ *This = impl_from_IPropertyEnumTypeList(iface);
    ULONG cRef = InterlockedDecrement(&This->cRef);

    if (!cRef)
        HeapFree(GetProcessHeap(), 0, This);
    return cRef;
}

static HRESULT STDMETHODCALLTYPE
PropEnumList_GetCount(
    _In_ IPropertyEnumTypeList *iface,
    _Out_ UINT *pctypes)
{
    *pctypes = impl_from_IPropertyEnumTypeList(iface)->pEntry->cEnums;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropEnumList_GetAt(
    _In_ IPropertyEnumTypeList *iface,
    _In_ UINT itype,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    const PROPSCHEMA_ENTRY *pEntry = impl_from_IPropertyEnumTypeList(iface)->pEntry;

    *ppv = NULL;
    if (itype >= pEntry->cEnums)
        return E_INVALIDARG;
    return PropEnum_Create(&g_PropSchemaEnums[pEntry->iFirstEnum + itype], riid, ppv);
}

static HRESULT STDMETHODCALLTYPE
PropEnumList_GetConditionAt(
    _In_ IPropertyEnumTypeList *iface,
    _In_ UINT nIndex,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    *ppv = NULL;
    return E_NOTIMPL;
}

/* Index of the enumeration entry that describes a value */
static
HRESULT
PropEnum_FindMatch(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFPROPVARIANT propvar,
    _Out_ UINT *pnIndex)
{
    LONGLONG llValue = 0;
    BOOL fNumeric = FALSE;
    UINT iDefault = (UINT)-1;
    UINT i;

    *pnIndex = 0;
    if (propvar->vt == VT_BOOL)
    {
        llValue = propvar->boolVal ? -1 : 0;
        fNumeric = TRUE;
    }
    else if (propvar->vt != VT_EMPTY && propvar->vt != VT_LPWSTR && propvar->vt != VT_BSTR)
    {
        fNumeric = SUCCEEDED(PropVariantToInt64(propvar, &llValue));
    }

    for (i = 0; i < pEntry->cEnums; i++)
    {
        const PROPSCHEMA_ENUM *pEnum = &g_PropSchemaEnums[pEntry->iFirstEnum + i];

        switch (pEnum->enumType)
        {
            case PET_DEFAULTVALUE:
                iDefault = i;
                break;

            case PET_DISCRETEVALUE:
                if (pEnum->vt == VT_LPWSTR)
                {
                    PWSTR psz;

                    if (SUCCEEDED(PropVariantToStringAlloc(propvar, &psz)))
                    {
                        BOOL fMatch = !lstrcmpiW(psz, pEnum->pszValue);

                        CoTaskMemFree(psz);
                        if (fMatch)
                        {
                            *pnIndex = i;
                            return S_OK;
                        }
                    }
                }
                else if (fNumeric && llValue == pEnum->lValue)
                {
                    *pnIndex = i;
                    return S_OK;
                }
                break;

            case PET_RANGEDVALUE:
                if (fNumeric && llValue >= pEnum->lMin && llValue <= pEnum->lValue)
                {
                    *pnIndex = i;
                    return S_OK;
                }
                break;

            default:
                break;
        }
    }

    if (iDefault != (UINT)-1 && (!fNumeric || llValue == 0))
    {
        *pnIndex = iDefault;
        return S_OK;
    }
    return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE
PropEnumList_FindMatchingIndex(
    _In_ IPropertyEnumTypeList *iface,
    _In_ REFPROPVARIANT propvarCmp,
    _Out_ UINT *pnIndex)
{
    return PropEnum_FindMatch(impl_from_IPropertyEnumTypeList(iface)->pEntry, propvarCmp, pnIndex);
}

static const IPropertyEnumTypeListVtbl PropEnumList_Vtbl =
{
    PropEnumList_QueryInterface,
    PropEnumList_AddRef,
    PropEnumList_Release,
    PropEnumList_GetCount,
    PropEnumList_GetAt,
    PropEnumList_GetConditionAt,
    PropEnumList_FindMatchingIndex
};

static
HRESULT
PropEnumList_Create(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPENUMLIST_OBJ *This;
    HRESULT hr;

    *ppv = NULL;
    if (!pEntry->cEnums)
        return E_NOTIMPL;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IPropertyEnumTypeList_iface.lpVtbl = &PropEnumList_Vtbl;
    This->cRef = 1;
    This->pEntry = pEntry;
    hr = IPropertyEnumTypeList_QueryInterface(&This->IPropertyEnumTypeList_iface, riid, ppv);
    IPropertyEnumTypeList_Release(&This->IPropertyEnumTypeList_iface);
    return hr;
}

/* Property descriptions */

typedef struct _PROPDESC_OBJ
{
    IPropertyDescription2 IPropertyDescription2_iface;
    IPropertyDescriptionPriv IPropertyDescriptionPriv_iface;
    LONG cRef;
    const PROPSCHEMA_ENTRY *pEntry;
    PROPDESC_VIEW_FLAGS vfExtra;
} PROPDESC_OBJ;

static inline PROPDESC_OBJ *
impl_from_IPropertyDescription2(
    _In_ IPropertyDescription2 *iface)
{
    return CONTAINING_RECORD(iface, PROPDESC_OBJ, IPropertyDescription2_iface);
}

static inline PROPDESC_OBJ *
impl_from_IPropertyDescriptionPriv(
    _In_ IPropertyDescriptionPriv *iface)
{
    return CONTAINING_RECORD(iface, PROPDESC_OBJ, IPropertyDescriptionPriv_iface);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_QueryInterface(
    _In_ IPropertyDescription2 *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPDESC_OBJ *This = impl_from_IPropertyDescription2(iface);

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyDescription) ||
        IsEqualIID(riid, &IID_IPropertyDescription2))
    {
        *ppv = &This->IPropertyDescription2_iface;
    }
    else if (IsEqualIID(riid, &IID_IPropertyDescriptionPriv))
    {
        *ppv = &This->IPropertyDescriptionPriv_iface;
    }
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown *)*ppv);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE
PropDesc_AddRef(
    _In_ IPropertyDescription2 *iface)
{
    return InterlockedIncrement(&impl_from_IPropertyDescription2(iface)->cRef);
}

static ULONG STDMETHODCALLTYPE
PropDesc_Release(
    _In_ IPropertyDescription2 *iface)
{
    PROPDESC_OBJ *This = impl_from_IPropertyDescription2(iface);
    ULONG cRef = InterlockedDecrement(&This->cRef);

    if (!cRef)
        HeapFree(GetProcessHeap(), 0, This);
    return cRef;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetPropertyKey(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPERTYKEY *pkey)
{
    *pkey = impl_from_IPropertyDescription2(iface)->pEntry->key;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetCanonicalName(
    _In_ IPropertyDescription2 *iface,
    _Outptr_ LPWSTR *ppszName)
{
    return PropSchema_DupString(impl_from_IPropertyDescription2(iface)->pEntry->pszName, ppszName);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetPropertyType(
    _In_ IPropertyDescription2 *iface,
    _Out_ VARTYPE *pvartype)
{
    *pvartype = impl_from_IPropertyDescription2(iface)->pEntry->vt;
    return S_OK;
}

/* Properties without a label in the schema have no display name, as in Windows */
static HRESULT STDMETHODCALLTYPE
PropDesc_GetDisplayName(
    _In_ IPropertyDescription2 *iface,
    _Outptr_ LPWSTR *ppszName)
{
    UINT ids = impl_from_IPropertyDescription2(iface)->pEntry->idsDisplayName;

    *ppszName = NULL;
    if (!ids)
        return E_FAIL;
    return PropSchema_LoadString(ids, ppszName);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetEditInvitation(
    _In_ IPropertyDescription2 *iface,
    _Outptr_ LPWSTR *ppszInvite)
{
    UINT ids = impl_from_IPropertyDescription2(iface)->pEntry->idsEditInvitation;

    *ppszInvite = NULL;
    if (!ids)
        return E_FAIL;
    return PropSchema_LoadString(ids, ppszInvite);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetTypeFlags(
    _In_ IPropertyDescription2 *iface,
    _In_ PROPDESC_TYPE_FLAGS mask,
    _Out_ PROPDESC_TYPE_FLAGS *ppdtFlags)
{
    *ppdtFlags = impl_from_IPropertyDescription2(iface)->pEntry->typeFlags & mask;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetViewFlags(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_VIEW_FLAGS *ppdvFlags)
{
    PROPDESC_OBJ *This = impl_from_IPropertyDescription2(iface);

    *ppdvFlags = This->pEntry->viewFlags | This->vfExtra;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetDefaultColumnWidth(
    _In_ IPropertyDescription2 *iface,
    _Out_ UINT *pcxChars)
{
    *pcxChars = impl_from_IPropertyDescription2(iface)->pEntry->cxChars;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetDisplayType(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_DISPLAYTYPE *pdisplaytype)
{
    *pdisplaytype = impl_from_IPropertyDescription2(iface)->pEntry->displayType;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetColumnState(
    _In_ IPropertyDescription2 *iface,
    _Out_ SHCOLSTATEF *pcsFlags)
{
    *pcsFlags = impl_from_IPropertyDescription2(iface)->pEntry->columnState;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetGroupingRange(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_GROUPING_RANGE *pgr)
{
    *pgr = impl_from_IPropertyDescription2(iface)->pEntry->groupingRange;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetRelativeDescriptionType(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_RELATIVEDESCRIPTION_TYPE *prdt)
{
    *prdt = impl_from_IPropertyDescription2(iface)->pEntry->relativeDescription;
    return S_OK;
}

/* Both values are described with their display text */
static HRESULT STDMETHODCALLTYPE
PropDesc_GetRelativeDescription(
    _In_ IPropertyDescription2 *iface,
    _In_ REFPROPVARIANT propvar1,
    _In_ REFPROPVARIANT propvar2,
    _Outptr_ LPWSTR *ppszDesc1,
    _Outptr_ LPWSTR *ppszDesc2)
{
    const PROPSCHEMA_ENTRY *pEntry = impl_from_IPropertyDescription2(iface)->pEntry;
    HRESULT hr;

    *ppszDesc1 = NULL;
    *ppszDesc2 = NULL;
    hr = PropFormat_Value(pEntry, propvar1, PDFF_DEFAULT, ppszDesc1);
    if (SUCCEEDED(hr))
        hr = PropFormat_Value(pEntry, propvar2, PDFF_DEFAULT, ppszDesc2);
    if (FAILED(hr))
    {
        CoTaskMemFree(*ppszDesc1);
        *ppszDesc1 = NULL;
    }
    return SUCCEEDED(hr) ? S_OK : hr;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetSortDescription(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_SORTDESCRIPTION *psd)
{
    *psd = impl_from_IPropertyDescription2(iface)->pEntry->sortDescription;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetSortDescriptionLabel(
    _In_ IPropertyDescription2 *iface,
    _In_ BOOL fDescending,
    _Outptr_ LPWSTR *ppszDescription)
{
    UINT ids;

    switch (impl_from_IPropertyDescription2(iface)->pEntry->sortDescription)
    {
        case PDSD_A_Z:
            ids = fDescending ? IDS_SORT_Z_A : IDS_SORT_A_Z;
            break;
        case PDSD_LOWEST_HIGHEST:
            ids = fDescending ? IDS_SORT_HIGH_LOW : IDS_SORT_LOW_HIGH;
            break;
        case PDSD_SMALLEST_BIGGEST:
            ids = fDescending ? IDS_SORT_BIG_SMALL : IDS_SORT_SMALL_BIG;
            break;
        case PDSD_OLDEST_NEWEST:
            ids = fDescending ? IDS_SORT_NEW_OLD : IDS_SORT_OLD_NEW;
            break;
        default:
            ids = fDescending ? IDS_SORT_DESCENDING : IDS_SORT_ASCENDING;
            break;
    }
    return PropSchema_LoadString(ids, ppszDescription);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetAggregationType(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_AGGREGATION_TYPE *paggtype)
{
    *paggtype = impl_from_IPropertyDescription2(iface)->pEntry->aggregation;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetConditionType(
    _In_ IPropertyDescription2 *iface,
    _Out_ PROPDESC_CONDITION_TYPE *pcontype,
    _Out_ CONDITION_OPERATION *popDefault)
{
    const PROPSCHEMA_ENTRY *pEntry = impl_from_IPropertyDescription2(iface)->pEntry;

    *pcontype = pEntry->conditionType;
    *popDefault = pEntry->conditionOperation;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetEnumTypeList(
    _In_ IPropertyDescription2 *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return PropEnumList_Create(impl_from_IPropertyDescription2(iface)->pEntry, riid, ppv);
}

/* Converts to the schema type; vectors of the element type are kept for multivalue properties */
static
HRESULT
PropDesc_Coerce(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _Inout_ PROPVARIANT *ppropvar)
{
    PROPVARIANT pvNew;
    HRESULT hr;

    if (ppropvar->vt == VT_EMPTY || ppropvar->vt == pEntry->vt)
        return S_OK;

    if (pEntry->vt == (VT_VECTOR | VT_LPWSTR) && (ppropvar->vt == VT_LPWSTR || ppropvar->vt == VT_BSTR))
    {
        PWSTR psz = NULL;

        hr = PropVariantToStringAlloc(ppropvar, &psz);
        if (FAILED(hr))
            return hr;
        hr = InitPropVariantFromStringVector((PCWSTR *)&psz, 1, &pvNew);
        CoTaskMemFree(psz);
    }
    else
    {
        hr = PropVariantChangeType(&pvNew, ppropvar, 0, pEntry->vt);
    }

    if (FAILED(hr))
    {
        PropVariantClear(ppropvar);
        return hr;
    }
    PropVariantClear(ppropvar);
    *ppropvar = pvNew;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_CoerceToCanonicalValue(
    _In_ IPropertyDescription2 *iface,
    _Inout_ PROPVARIANT *ppropvar)
{
    return PropDesc_Coerce(impl_from_IPropertyDescription2(iface)->pEntry, ppropvar);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_FormatForDisplay(
    _In_ IPropertyDescription2 *iface,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS flags,
    _Outptr_ LPWSTR *ppszDisplay)
{
    return PropFormat_Value(impl_from_IPropertyDescription2(iface)->pEntry, propvar, flags, ppszDisplay);
}

static HRESULT STDMETHODCALLTYPE
PropDesc_IsValueCanonical(
    _In_ IPropertyDescription2 *iface,
    _In_ REFPROPVARIANT propvar)
{
    const PROPSCHEMA_ENTRY *pEntry = impl_from_IPropertyDescription2(iface)->pEntry;

    if (propvar->vt == VT_EMPTY || propvar->vt == pEntry->vt)
        return S_OK;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE
PropDesc_GetImageReferenceForValue(
    _In_ IPropertyDescription2 *iface,
    _In_ REFPROPVARIANT propvar,
    _Outptr_ LPWSTR *ppszImageRes)
{
    *ppszImageRes = NULL;
    return E_FAIL;
}

static const IPropertyDescription2Vtbl PropDesc_Vtbl =
{
    PropDesc_QueryInterface,
    PropDesc_AddRef,
    PropDesc_Release,
    PropDesc_GetPropertyKey,
    PropDesc_GetCanonicalName,
    PropDesc_GetPropertyType,
    PropDesc_GetDisplayName,
    PropDesc_GetEditInvitation,
    PropDesc_GetTypeFlags,
    PropDesc_GetViewFlags,
    PropDesc_GetDefaultColumnWidth,
    PropDesc_GetDisplayType,
    PropDesc_GetColumnState,
    PropDesc_GetGroupingRange,
    PropDesc_GetRelativeDescriptionType,
    PropDesc_GetRelativeDescription,
    PropDesc_GetSortDescription,
    PropDesc_GetSortDescriptionLabel,
    PropDesc_GetAggregationType,
    PropDesc_GetConditionType,
    PropDesc_GetEnumTypeList,
    PropDesc_CoerceToCanonicalValue,
    PropDesc_FormatForDisplay,
    PropDesc_IsValueCanonical,
    PropDesc_GetImageReferenceForValue
};

static HRESULT STDMETHODCALLTYPE
PropDescPriv_QueryInterface(
    _In_ IPropertyDescriptionPriv *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return PropDesc_QueryInterface(&impl_from_IPropertyDescriptionPriv(iface)->IPropertyDescription2_iface,
                                   riid, ppv);
}

static ULONG STDMETHODCALLTYPE
PropDescPriv_AddRef(
    _In_ IPropertyDescriptionPriv *iface)
{
    return PropDesc_AddRef(&impl_from_IPropertyDescriptionPriv(iface)->IPropertyDescription2_iface);
}

static ULONG STDMETHODCALLTYPE
PropDescPriv_Release(
    _In_ IPropertyDescriptionPriv *iface)
{
    return PropDesc_Release(&impl_from_IPropertyDescriptionPriv(iface)->IPropertyDescription2_iface);
}

/* ReactOS registers no property controls, so callers fall back to the formatted text */
static HRESULT STDMETHODCALLTYPE
PropDescPriv_GetControlCLSID(
    _In_ IPropertyDescriptionPriv *iface,
    _In_ PROPDESC_CONTROL_TYPE type,
    _Out_ CLSID *pclsid)
{
    *pclsid = CLSID_NULL;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT STDMETHODCALLTYPE
PropDescPriv_GetEnumTypeListEx(
    _In_ IPropertyDescriptionPriv *iface,
    _In_ UINT uFlags,
    _In_opt_ IUnknown *punkContext,
    _In_ BOOL fFiltered,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return PropEnumList_Create(impl_from_IPropertyDescriptionPriv(iface)->pEntry, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE
PropDescPriv_GetTypeFlagsEx(
    _In_ IPropertyDescriptionPriv *iface,
    _In_ PROPDESC_TYPE_FLAGS_EX mask,
    _Out_ PROPDESC_TYPE_FLAGS_EX *pflags)
{
    *pflags = 0;
    return S_OK;
}

static const IPropertyDescriptionPrivVtbl PropDescPriv_Vtbl =
{
    PropDescPriv_QueryInterface,
    PropDescPriv_AddRef,
    PropDescPriv_Release,
    PropDescPriv_GetControlCLSID,
    PropDescPriv_GetEnumTypeListEx,
    PropDescPriv_GetTypeFlagsEx
};

HRESULT
PropDesc_Create(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ PROPDESC_VIEW_FLAGS vfExtra,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPDESC_OBJ *This;
    HRESULT hr;

    *ppv = NULL;
    This = HeapAlloc(GetProcessHeap(), 0, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IPropertyDescription2_iface.lpVtbl = &PropDesc_Vtbl;
    This->IPropertyDescriptionPriv_iface.lpVtbl = &PropDescPriv_Vtbl;
    This->cRef = 1;
    This->pEntry = pEntry;
    This->vfExtra = vfExtra;
    hr = PropDesc_QueryInterface(&This->IPropertyDescription2_iface, riid, ppv);
    PropDesc_Release(&This->IPropertyDescription2_iface);
    return hr;
}

/* Property description lists */

typedef struct _PROPLIST_ITEM
{
    const PROPSCHEMA_ENTRY *pEntry;
    PROPDESC_VIEW_FLAGS vfExtra;
} PROPLIST_ITEM;

typedef struct _PROPLIST_OBJ
{
    IPropertyDescriptionList IPropertyDescriptionList_iface;
    LONG cRef;
    UINT cItems;
    PROPLIST_ITEM rgItems[ANYSIZE_ARRAY];
} PROPLIST_OBJ;

static inline PROPLIST_OBJ *
impl_from_IPropertyDescriptionList(
    _In_ IPropertyDescriptionList *iface)
{
    return CONTAINING_RECORD(iface, PROPLIST_OBJ, IPropertyDescriptionList_iface);
}

static HRESULT STDMETHODCALLTYPE
PropList_QueryInterface(
    _In_ IPropertyDescriptionList *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyDescriptionList))
    {
        *ppv = iface;
        IPropertyDescriptionList_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
PropList_AddRef(
    _In_ IPropertyDescriptionList *iface)
{
    return InterlockedIncrement(&impl_from_IPropertyDescriptionList(iface)->cRef);
}

static ULONG STDMETHODCALLTYPE
PropList_Release(
    _In_ IPropertyDescriptionList *iface)
{
    PROPLIST_OBJ *This = impl_from_IPropertyDescriptionList(iface);
    ULONG cRef = InterlockedDecrement(&This->cRef);

    if (!cRef)
        HeapFree(GetProcessHeap(), 0, This);
    return cRef;
}

static HRESULT STDMETHODCALLTYPE
PropList_GetCount(
    _In_ IPropertyDescriptionList *iface,
    _Out_ UINT *pcelem)
{
    *pcelem = impl_from_IPropertyDescriptionList(iface)->cItems;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
PropList_GetAt(
    _In_ IPropertyDescriptionList *iface,
    _In_ UINT iElem,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPLIST_OBJ *This = impl_from_IPropertyDescriptionList(iface);

    *ppv = NULL;
    if (iElem >= This->cItems)
        return E_INVALIDARG;
    return PropDesc_Create(This->rgItems[iElem].pEntry, This->rgItems[iElem].vfExtra, riid, ppv);
}

static const IPropertyDescriptionListVtbl PropList_Vtbl =
{
    PropList_QueryInterface,
    PropList_AddRef,
    PropList_Release,
    PropList_GetCount,
    PropList_GetAt
};

static
PROPLIST_OBJ *
PropList_Alloc(
    _In_ UINT cMax)
{
    PROPLIST_OBJ *This;

    This = HeapAlloc(GetProcessHeap(), 0,
                     FIELD_OFFSET(PROPLIST_OBJ, rgItems) + (max(cMax, 1) * sizeof(This->rgItems[0])));
    if (!This)
        return NULL;

    This->IPropertyDescriptionList_iface.lpVtbl = &PropList_Vtbl;
    This->cRef = 1;
    This->cItems = 0;
    return This;
}

static
HRESULT
PropList_Finish(
    _In_ PROPLIST_OBJ *This,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    HRESULT hr = PropList_QueryInterface(&This->IPropertyDescriptionList_iface, riid, ppv);

    PropList_Release(&This->IPropertyDescriptionList_iface);
    return hr;
}

/* View flags a list entry carries in front of its name */
static
PROPDESC_VIEW_FLAGS
PropList_PrefixFlag(
    _In_ WCHAR ch)
{
    switch (ch)
    {
        case L'*':
            return PDVF_SHOWONLYIFPRESENT;
        case L'~':
            return PDVF_HIDELABEL;
        case L'!':
            return PDVF_BEGINNEWGROUP;
        case L'$':
            return PDVF_FILLAREA;
        case L'#':
            return PDVF_CANWRAP;
        default:
            return 0;
    }
}

/* "prop:" lists of names separated by semicolons; names the schema does not know are left out */
static
HRESULT
PropList_FromString(
    _In_ PCWSTR pszList,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPLIST_OBJ *This;
    PCWSTR pch;
    UINT cMax = 1;

    *ppv = NULL;
    if (!pszList)
        return E_INVALIDARG;

    if (!_wcsnicmp(pszList, L"prop:", 5))
        pszList += 5;

    for (pch = pszList; *pch; pch++)
    {
        if (*pch == L';')
            cMax++;
    }

    This = PropList_Alloc(cMax);
    if (!This)
        return E_OUTOFMEMORY;

    pch = pszList;
    while (*pch)
    {
        WCHAR szName[MAX_PATH];
        PROPDESC_VIEW_FLAGS vf = 0;
        PROPDESC_VIEW_FLAGS vfPrefix;
        const PROPSCHEMA_ENTRY *pEntry;
        UINT cch = 0;

        while (*pch == L' ')
            pch++;
        while ((vfPrefix = PropList_PrefixFlag(*pch)) != 0)
        {
            vf |= vfPrefix;
            pch++;
        }
        while (*pch && *pch != L';')
        {
            if (cch < _countof(szName) - 1)
                szName[cch++] = *pch;
            pch++;
        }
        while (cch && szName[cch - 1] == L' ')
            cch -= 1;
        szName[cch] = UNICODE_NULL;
        if (*pch == L';')
            pch++;

        if (!cch)
            continue;

        pEntry = PropSchema_FindByName(szName);
        if (!pEntry)
        {
            PROPERTYKEY key;

            if (SUCCEEDED(PSPropertyKeyFromString(szName, &key)))
                pEntry = PropSchema_FindByKey(&key);
        }
        if (!pEntry)
        {
            TRACE("Unknown property %s\n", debugstr_w(szName));
            continue;
        }

        This->rgItems[This->cItems].pEntry = pEntry;
        This->rgItems[This->cItems].vfExtra = vf;
        This->cItems++;
    }

    return PropList_Finish(This, riid, ppv);
}

static
BOOL
PropSchema_MatchesFilter(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ PROPDESC_ENUMFILTER filter)
{
    switch (filter)
    {
        case PDEF_ALL:
            return TRUE;
        case PDEF_SYSTEM:
            return (pEntry->typeFlags & PDTF_ISSYSTEMPROPERTY) != 0;
        case PDEF_NONSYSTEM:
            return (pEntry->typeFlags & PDTF_ISSYSTEMPROPERTY) == 0;
        case PDEF_VIEWABLE:
            return (pEntry->typeFlags & PDTF_ISVIEWABLE) != 0;
        case PDEF_QUERYABLE:
            return (pEntry->typeFlags & PDTF_ISQUERYABLE) != 0;
        case PDEF_INFULLTEXTQUERY:
            return (pEntry->typeFlags & PDTF_INCLUDEINFULLTEXTQUERY) != 0;
        case PDEF_COLUMN:
            return (pEntry->typeFlags & PDTF_ISVIEWABLE) && !(pEntry->typeFlags & PDTF_ISGROUP);
        default:
            return FALSE;
    }
}

/* Exports */

HRESULT
WINAPI
PSGetPropertyDescription(
    _In_ REFPROPERTYKEY propkey,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    const PROPSCHEMA_ENTRY *pEntry;

    if (!ppv)
        return E_INVALIDARG;
    *ppv = NULL;
    if (!propkey)
        return E_INVALIDARG;

    pEntry = PropSchema_FindByKey(propkey);
    if (!pEntry)
        return TYPE_E_ELEMENTNOTFOUND;
    return PropDesc_Create(pEntry, 0, riid, ppv);
}

HRESULT
WINAPI
PSGetPropertyDescriptionByName(
    _In_ LPCWSTR pszCanonicalName,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    const PROPSCHEMA_ENTRY *pEntry;

    if (!ppv)
        return E_INVALIDARG;
    *ppv = NULL;
    if (!pszCanonicalName)
        return E_INVALIDARG;

    pEntry = PropSchema_FindByName(pszCanonicalName);
    if (!pEntry)
        return TYPE_E_ELEMENTNOTFOUND;
    return PropDesc_Create(pEntry, 0, riid, ppv);
}

HRESULT
WINAPI
PSGetPropertyDescriptionListFromString(
    _In_ LPCWSTR pszPropList,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    if (!ppv)
        return E_INVALIDARG;
    return PropList_FromString(pszPropList, riid, ppv);
}

HRESULT
WINAPI
PSEnumeratePropertyDescriptions(
    _In_ PROPDESC_ENUMFILTER filterOn,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PROPLIST_OBJ *This;
    UINT i;

    if (!ppv)
        return E_INVALIDARG;
    *ppv = NULL;

    This = PropList_Alloc(g_cPropSchema);
    if (!This)
        return E_OUTOFMEMORY;

    for (i = 0; i < g_cPropSchema; i++)
    {
        if (!PropSchema_MatchesFilter(&g_PropSchema[i], filterOn))
            continue;
        This->rgItems[This->cItems].pEntry = &g_PropSchema[i];
        This->rgItems[This->cItems].vfExtra = 0;
        This->cItems++;
    }
    return PropList_Finish(This, riid, ppv);
}

HRESULT
WINAPI
PSGetPropertyKeyFromName(
    _In_ PCWSTR pszName,
    _Out_ PROPERTYKEY *ppropkey)
{
    const PROPSCHEMA_ENTRY *pEntry;

    if (!pszName || !ppropkey)
        return E_POINTER;

    pEntry = PropSchema_FindByName(pszName);
    if (!pEntry)
    {
        ZeroMemory(ppropkey, sizeof(*ppropkey));
        return TYPE_E_ELEMENTNOTFOUND;
    }
    *ppropkey = pEntry->key;
    return S_OK;
}

HRESULT
WINAPI
PSGetNameFromPropertyKey(
    _In_ REFPROPERTYKEY propkey,
    _Outptr_ PWSTR *ppszCanonicalName)
{
    const PROPSCHEMA_ENTRY *pEntry;

    if (!ppszCanonicalName)
        return E_POINTER;
    *ppszCanonicalName = NULL;

    pEntry = PropSchema_FindByKey(propkey);
    if (!pEntry)
        return TYPE_E_ELEMENTNOTFOUND;
    return PropSchema_DupString(pEntry->pszName, ppszCanonicalName);
}

HRESULT
WINAPI
PSFormatForDisplayAlloc(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay)
{
    const PROPSCHEMA_ENTRY *pEntry;

    if (!ppszDisplay)
        return E_POINTER;
    *ppszDisplay = NULL;

    pEntry = PropSchema_FindByKey(key);
    if (!pEntry)
        return TYPE_E_ELEMENTNOTFOUND;
    return PropFormat_Value(pEntry, propvar, pdff, ppszDisplay);
}

HRESULT
WINAPI
PSFormatForDisplay(
    _In_ REFPROPERTYKEY propkey,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdfFlags,
    _Out_writes_(cchText) LPWSTR pwszText,
    _In_ DWORD cchText)
{
    PWSTR psz;
    HRESULT hr;

    if (!pwszText || !cchText)
        return E_INVALIDARG;
    *pwszText = UNICODE_NULL;

    hr = PSFormatForDisplayAlloc(propkey, propvar, pdfFlags, &psz);
    if (FAILED(hr))
        return hr;

    if (FAILED(StringCchCopyW(pwszText, cchText, psz)))
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    CoTaskMemFree(psz);
    return hr;
}

HRESULT
WINAPI
PSGetPropertyValue(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _Out_ PROPVARIANT *ppropvar)
{
    PROPERTYKEY key;
    HRESULT hr;

    PropVariantInit(ppropvar);
    if (!pps || !ppd)
        return E_INVALIDARG;

    hr = IPropertyDescription_GetPropertyKey(ppd, &key);
    if (SUCCEEDED(hr))
        hr = IPropertyStore_GetValue(pps, &key, ppropvar);
    return hr;
}

HRESULT
WINAPI
PSSetPropertyValue(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _In_ REFPROPVARIANT propvar)
{
    PROPVARIANT pvCopy;
    PROPERTYKEY key;
    HRESULT hr;

    if (!pps || !ppd)
        return E_INVALIDARG;

    hr = IPropertyDescription_GetPropertyKey(ppd, &key);
    if (FAILED(hr))
        return hr;

    hr = PropVariantCopy(&pvCopy, propvar);
    if (FAILED(hr))
        return hr;

    hr = IPropertyDescription_CoerceToCanonicalValue(ppd, &pvCopy);
    if (SUCCEEDED(hr))
        hr = IPropertyStore_SetValue(pps, &key, &pvCopy);
    PropVariantClear(&pvCopy);
    return hr;
}

HRESULT
WINAPI
PSFormatPropertyValue(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ LPWSTR *ppszDisplay)
{
    PROPVARIANT propvar;
    HRESULT hr;

    if (!ppszDisplay)
        return E_POINTER;
    *ppszDisplay = NULL;

    hr = PSGetPropertyValue(pps, ppd, &propvar);
    if (SUCCEEDED(hr))
    {
        hr = IPropertyDescription_FormatForDisplay(ppd, &propvar, pdff, ppszDisplay);
        PropVariantClear(&propvar);
    }
    return hr;
}

HRESULT
WINAPI
PSCoerceToCanonicalValue(
    _In_ REFPROPERTYKEY key,
    _Inout_ PROPVARIANT *ppropvar)
{
    const PROPSCHEMA_ENTRY *pEntry;

    if (!ppropvar)
        return E_INVALIDARG;

    pEntry = PropSchema_FindByKey(key);
    if (!pEntry)
        return TYPE_E_ELEMENTNOTFOUND;
    return PropDesc_Coerce(pEntry, ppropvar);
}
