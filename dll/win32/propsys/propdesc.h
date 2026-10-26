/*
 * PROJECT:     ReactOS Property System
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Built-in property schema, property descriptions and display formatting
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* How a value is turned into display text beyond what its type implies */
typedef enum _PROPSCHEMA_FORMAT
{
    PSFMT_DEFAULT = 0,
    PSFMT_FILESIZE,
    PSFMT_FREESPACE,
    PSFMT_DURATION,
    PSFMT_KBPS,
    PSFMT_KHZ,
    PSFMT_FRAMERATE,
    PSFMT_DPI,
    PSFMT_PIXELS,
    PSFMT_PERCENT,
    PSFMT_ATTRIBUTES
} PROPSCHEMA_FORMAT;

typedef struct _PROPSCHEMA_ENUM
{
    PROPENUMTYPE enumType;
    VARTYPE vt;
    LONG lValue;
    PCWSTR pszValue;
    VARTYPE vtMin;
    LONG lMin;
    UINT idsText;
} PROPSCHEMA_ENUM;

typedef struct _PROPSCHEMA_ENTRY
{
    PCWSTR pszName;
    PROPERTYKEY key;
    VARTYPE vt;
    UINT idsDisplayName;
    UINT idsEditInvitation;
    PROPDESC_TYPE_FLAGS typeFlags;
    PROPDESC_VIEW_FLAGS viewFlags;
    UINT cxChars;
    PROPDESC_DISPLAYTYPE displayType;
    SHCOLSTATEF columnState;
    PROPDESC_GROUPING_RANGE groupingRange;
    PROPDESC_RELATIVEDESCRIPTION_TYPE relativeDescription;
    PROPDESC_SORTDESCRIPTION sortDescription;
    PROPDESC_AGGREGATION_TYPE aggregation;
    PROPDESC_CONDITION_TYPE conditionType;
    CONDITION_OPERATION conditionOperation;
    PROPSCHEMA_FORMAT format;
    USHORT iFirstEnum;
    USHORT cEnums;
} PROPSCHEMA_ENTRY;

extern const PROPSCHEMA_ENTRY g_PropSchema[];
extern const UINT g_cPropSchema;
extern const PROPSCHEMA_ENUM g_PropSchemaEnums[];
extern IMAGE_DOS_HEADER __ImageBase;
#define PROPSYS_HINST ((HINSTANCE)&__ImageBase)

#include "resource.h"

/* Private property description interface: the controls and extended flags of a description */
typedef enum _PROPDESC_CONTROL_TYPE
{
    PDCONTROL_DRAW = 0,
    PDCONTROL_EDIT = 1
} PROPDESC_CONTROL_TYPE;

typedef UINT PROPDESC_TYPE_FLAGS_EX;

#undef INTERFACE
#define INTERFACE IPropertyDescriptionPriv
DECLARE_INTERFACE_(IPropertyDescriptionPriv, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(GetControlCLSID)(THIS_ PROPDESC_CONTROL_TYPE type, CLSID *pclsid) PURE;
    STDMETHOD(GetEnumTypeListEx)(THIS_ UINT uFlags, IUnknown *punkContext, BOOL fFiltered, REFIID riid,
                                 void **ppv) PURE;
    STDMETHOD(GetTypeFlagsEx)(THIS_ PROPDESC_TYPE_FLAGS_EX mask, PROPDESC_TYPE_FLAGS_EX *pflags) PURE;
};
#undef INTERFACE

extern const IID IID_IPropertyDescriptionPriv;

/* Exports the SDK headers do not declare yet */
HRESULT
WINAPI
PSGetPropertyDescriptionByName(
    _In_ LPCWSTR pszCanonicalName,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
WINAPI
PSGetNameFromPropertyKey(
    _In_ REFPROPERTYKEY propkey,
    _Outptr_ PWSTR *ppszCanonicalName);

HRESULT
WINAPI
PSGetPropertyKeyFromName(
    _In_ PCWSTR pszName,
    _Out_ PROPERTYKEY *ppropkey);

HRESULT
WINAPI
PSEnumeratePropertyDescriptions(
    _In_ PROPDESC_ENUMFILTER filterOn,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
WINAPI
PSFormatForDisplay(
    _In_ REFPROPERTYKEY propkey,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdfFlags,
    _Out_writes_(cchText) LPWSTR pwszText,
    _In_ DWORD cchText);

HRESULT
WINAPI
PSFormatForDisplayAlloc(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay);

HRESULT
WINAPI
PSFormatPropertyValue(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ LPWSTR *ppszDisplay);

HRESULT
WINAPI
PSGetPropertyValue(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _Out_ PROPVARIANT *ppropvar);

HRESULT
WINAPI
PSSetPropertyValue(
    _In_ IPropertyStore *pps,
    _In_ IPropertyDescription *ppd,
    _In_ REFPROPVARIANT propvar);

HRESULT
WINAPI
PSCoerceToCanonicalValue(
    _In_ REFPROPERTYKEY key,
    _Inout_ PROPVARIANT *ppropvar);

HRESULT
WINAPI
PSCreateMultiplexPropertyStore(
    _In_reads_(cStores) IUnknown **prgpunkStores,
    _In_ DWORD cStores,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
WINAPI
PSCreateDelayedMultiplexPropertyStore(
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_ IDelayedPropertyStoreFactory *pdpsf,
    _In_reads_(cStores) const DWORD *rgStoreIds,
    _In_ DWORD cStores,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

/* propdesc.c */
const PROPSCHEMA_ENTRY *PropSchema_FindByKey(_In_ REFPROPERTYKEY key);
const PROPSCHEMA_ENTRY *PropSchema_FindByName(_In_ PCWSTR pszName);
HRESULT
PropDesc_Create(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ PROPDESC_VIEW_FLAGS vfExtra,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
PropSchema_LoadString(
    _In_ UINT ids,
    _Outptr_ PWSTR *ppsz);

/* propfmt.c */
HRESULT
PropFormat_Value(
    _In_ const PROPSCHEMA_ENTRY *pEntry,
    _In_ REFPROPVARIANT propvar,
    _In_ PROPDESC_FORMAT_FLAGS pdff,
    _Outptr_ PWSTR *ppszDisplay);

