/*
 * standard IPropertyStore implementation
 *
 * Copyright 2012 Vincent Povirk for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "rpcproxy.h"
#include "propsys.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "initguid.h"
#include "propsys_private.h"

DEFINE_GUID(FMTID_NamedProperties, 0xd5cdd505, 0x2e9c, 0x101b, 0x93, 0x97, 0x08, 0x00, 0x2b, 0x2c, 0xf9, 0xae);

WINE_DEFAULT_DEBUG_CHANNEL(propsys);

typedef struct {
    struct list entry;
    DWORD pid;
    PROPVARIANT propvar;
    PSC_STATE state;
} propstore_value;

typedef struct {
    struct list entry;
    GUID fmtid;
    struct list values; /* list of struct propstore_value */
    DWORD count;
} propstore_format;

typedef struct {
    IPropertyStoreCache IPropertyStoreCache_iface;
    LONG ref;
    CRITICAL_SECTION lock;
    struct list formats; /* list of struct propstore_format */
#ifdef __REACTOS__
    IPersistSerializedPropStorage IPersistSerializedPropStorage_iface;
    PERSIST_SPROPSTORE_FLAGS persist_flags;
#endif
} PropertyStore;

static inline PropertyStore *impl_from_IPropertyStoreCache(IPropertyStoreCache *iface)
{
    return CONTAINING_RECORD(iface, PropertyStore, IPropertyStoreCache_iface);
}

static HRESULT WINAPI PropertyStore_QueryInterface(IPropertyStoreCache *iface, REFIID iid,
    void **ppv)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(iid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, iid) || IsEqualIID(&IID_IPropertyStore, iid) ||
        IsEqualIID(&IID_IPropertyStoreCache, iid))
    {
        *ppv = &This->IPropertyStoreCache_iface;
    }
#ifdef __REACTOS__
    else if (IsEqualIID(&IID_IPersistSerializedPropStorage, iid))
    {
        *ppv = &This->IPersistSerializedPropStorage_iface;
    }
#endif
    else
    {
        FIXME("No interface for %s\n", debugstr_guid(iid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI PropertyStore_AddRef(IPropertyStoreCache *iface)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) refcount=%lu\n", iface, ref);

    return ref;
}

static void destroy_format(propstore_format *format)
{
    propstore_value *cursor, *cursor2;
    LIST_FOR_EACH_ENTRY_SAFE(cursor, cursor2, &format->values, propstore_value, entry)
    {
        PropVariantClear(&cursor->propvar);
        HeapFree(GetProcessHeap(), 0, cursor);
    }
    HeapFree(GetProcessHeap(), 0, format);
}

static ULONG WINAPI PropertyStore_Release(IPropertyStoreCache *iface)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) refcount=%lu\n", iface, ref);

    if (ref == 0)
    {
        propstore_format *cursor, *cursor2;
        This->lock.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&This->lock);
        LIST_FOR_EACH_ENTRY_SAFE(cursor, cursor2, &This->formats, propstore_format, entry)
            destroy_format(cursor);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

static HRESULT WINAPI PropertyStore_GetCount(IPropertyStoreCache *iface,
    DWORD *cProps)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_format *format;

    TRACE("%p,%p\n", iface, cProps);

    if (!cProps)
        return E_POINTER;

    *cProps = 0;

    EnterCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY(format, &This->formats, propstore_format, entry)
        *cProps += format->count;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI PropertyStore_GetAt(IPropertyStoreCache *iface,
    DWORD iProp, PROPERTYKEY *pkey)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_format *format=NULL, *format_candidate;
    propstore_value *value;
    HRESULT hr;

    TRACE("%p,%ld,%p\n", iface, iProp, pkey);

    if (!pkey)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY(format_candidate, &This->formats, propstore_format, entry)
    {
        if (format_candidate->count > iProp)
        {
            format = format_candidate;
            pkey->fmtid = format->fmtid;
            break;
        }

        iProp -= format_candidate->count;
    }

    if (format)
    {
        LIST_FOR_EACH_ENTRY(value, &format->values, propstore_value, entry)
        {
            if (iProp == 0)
            {
                pkey->pid = value->pid;
                break;
            }

            iProp--;
        }

        hr = S_OK;
    }
    else
        hr = E_INVALIDARG;

    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT PropertyStore_LookupValue(PropertyStore *This, REFPROPERTYKEY key,
                                         BOOL insert, propstore_value **result)
{
    propstore_format *format=NULL, *format_candidate;
    propstore_value *value=NULL, *value_candidate;

    if (IsEqualGUID(&key->fmtid, &FMTID_NamedProperties))
    {
        /* This is used in the property store format [MS-PROPSTORE]
         * for named values and probably gets special treatment. */
        ERR("don't know how to handle FMTID_NamedProperties\n");
        return E_FAIL;
    }

    LIST_FOR_EACH_ENTRY(format_candidate, &This->formats, propstore_format, entry)
    {
        if (IsEqualGUID(&format_candidate->fmtid, &key->fmtid))
        {
            format = format_candidate;
            break;
        }
    }

    if (!format)
    {
        if (!insert)
            return TYPE_E_ELEMENTNOTFOUND;

        format = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*format));
        if (!format)
            return E_OUTOFMEMORY;

        format->fmtid = key->fmtid;
        list_init(&format->values);
        list_add_tail(&This->formats, &format->entry);
    }

    LIST_FOR_EACH_ENTRY(value_candidate, &format->values, propstore_value, entry)
    {
        if (value_candidate->pid == key->pid)
        {
            value = value_candidate;
            break;
        }
    }

    if (!value)
    {
        if (!insert)
            return TYPE_E_ELEMENTNOTFOUND;

        value = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*value));
        if (!value)
            return E_OUTOFMEMORY;

        value->pid = key->pid;
        list_add_tail(&format->values, &value->entry);
        format->count++;
    }

    *result = value;

    return S_OK;
}

static HRESULT WINAPI PropertyStore_GetValue(IPropertyStoreCache *iface,
    REFPROPERTYKEY key, PROPVARIANT *pv)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_value *value;
    HRESULT hr;

    TRACE("%p,%p,%p\n", iface, key, pv);

    if (!pv)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    hr = PropertyStore_LookupValue(This, key, FALSE, &value);

    if (SUCCEEDED(hr))
        hr = PropVariantCopy(pv, &value->propvar);
    else if (hr == TYPE_E_ELEMENTNOTFOUND)
    {
        PropVariantInit(pv);
        hr = S_OK;
    }

    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT WINAPI PropertyStore_SetValue(IPropertyStoreCache *iface,
    REFPROPERTYKEY key, REFPROPVARIANT propvar)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_value *value;
    HRESULT hr;
    PROPVARIANT temp;

    TRACE("%p,%p,%p\n", iface, key, propvar);

#ifdef __REACTOS__
    if (This->persist_flags & FPSPS_READONLY)
        return STG_E_ACCESSDENIED;
#endif

    EnterCriticalSection(&This->lock);

    hr = PropertyStore_LookupValue(This, key, TRUE, &value);

    if (SUCCEEDED(hr))
        hr = PropVariantCopy(&temp, propvar);

    if (SUCCEEDED(hr))
    {
        PropVariantClear(&value->propvar);
        value->propvar = temp;
    }

    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT WINAPI PropertyStore_Commit(IPropertyStoreCache *iface)
{
    FIXME("%p: stub\n", iface);
    return S_OK;
}

static HRESULT WINAPI PropertyStore_GetState(IPropertyStoreCache *iface,
    REFPROPERTYKEY key, PSC_STATE *pstate)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_value *value;
    HRESULT hr;

    TRACE("%p,%p,%p\n", iface, key, pstate);

    EnterCriticalSection(&This->lock);

    hr = PropertyStore_LookupValue(This, key, FALSE, &value);

    if (SUCCEEDED(hr))
        *pstate = value->state;

    LeaveCriticalSection(&This->lock);

    if (FAILED(hr))
        *pstate = PSC_NORMAL;

    return hr;
}

static HRESULT WINAPI PropertyStore_GetValueAndState(IPropertyStoreCache *iface,
    REFPROPERTYKEY key, PROPVARIANT *ppropvar, PSC_STATE *pstate)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_value *value;
    HRESULT hr;

    TRACE("%p,%p,%p,%p\n", iface, key, ppropvar, pstate);

    EnterCriticalSection(&This->lock);

    hr = PropertyStore_LookupValue(This, key, FALSE, &value);

    if (SUCCEEDED(hr))
        hr = PropVariantCopy(ppropvar, &value->propvar);

    if (SUCCEEDED(hr))
        *pstate = value->state;

    LeaveCriticalSection(&This->lock);

    if (FAILED(hr))
    {
        PropVariantInit(ppropvar);
        *pstate = PSC_NORMAL;
    }

    return hr;
}

static HRESULT WINAPI PropertyStore_SetState(IPropertyStoreCache *iface,
    REFPROPERTYKEY key, PSC_STATE pstate)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_value *value;
    HRESULT hr;

    TRACE("%p,%p,%d\n", iface, key, pstate);

    EnterCriticalSection(&This->lock);

    hr = PropertyStore_LookupValue(This, key, FALSE, &value);

    if (SUCCEEDED(hr))
        value->state = pstate;

    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT WINAPI PropertyStore_SetValueAndState(IPropertyStoreCache *iface,
    REFPROPERTYKEY key, const PROPVARIANT *ppropvar, PSC_STATE state)
{
    PropertyStore *This = impl_from_IPropertyStoreCache(iface);
    propstore_value *value;
    HRESULT hr;
    PROPVARIANT temp;

    TRACE("%p,%p,%p,%d\n", iface, key, ppropvar, state);

    EnterCriticalSection(&This->lock);

    hr = PropertyStore_LookupValue(This, key, TRUE, &value);

    if (SUCCEEDED(hr))
        hr = PropVariantCopy(&temp, ppropvar);

    if (SUCCEEDED(hr))
    {
        PropVariantClear(&value->propvar);
        value->propvar = temp;
        value->state = state;
    }

    LeaveCriticalSection(&This->lock);

    return hr;
}

static const IPropertyStoreCacheVtbl PropertyStore_Vtbl = {
    PropertyStore_QueryInterface,
    PropertyStore_AddRef,
    PropertyStore_Release,
    PropertyStore_GetCount,
    PropertyStore_GetAt,
    PropertyStore_GetValue,
    PropertyStore_SetValue,
    PropertyStore_Commit,
    PropertyStore_GetState,
    PropertyStore_GetValueAndState,
    PropertyStore_SetState,
    PropertyStore_SetValueAndState
};

#ifdef __REACTOS__

/* [MS-PROPSTORE] storage version, "1SPS" */
#define PROPSTORE_STORAGE_VERSION 0x53505331

/* Storage header: size, version and format ID. Value header: size, ID and a reserved byte */
#define PROPSTORE_STORAGE_HEADER (2 * sizeof(DWORD) + sizeof(GUID))
#define PROPSTORE_VALUE_HEADER (2 * sizeof(DWORD) + sizeof(BYTE))

/* The type word and its padding in front of the value data */
#define PROPSTORE_TYPE_HEADER (2 * sizeof(WORD))

static inline PropertyStore *impl_from_IPersistSerializedPropStorage(IPersistSerializedPropStorage *iface)
{
    return CONTAINING_RECORD(iface, PropertyStore, IPersistSerializedPropStorage_iface);
}

static
ULONG
PropertyStore_Align4(
    _In_ ULONG cb)
{
    return (cb + 3) & ~3;
}

/* Bytes a fixed size type keeps inside the PROPVARIANT, 0 for types that are not fixed size */
static
ULONG
PropertyStore_GetFixedSize(
    _In_ VARTYPE vt)
{
    switch (vt)
    {
        case VT_I1:
        case VT_UI1:
            return sizeof(BYTE);
        case VT_I2:
        case VT_UI2:
        case VT_BOOL:
            return sizeof(WORD);
        case VT_I4:
        case VT_UI4:
        case VT_INT:
        case VT_UINT:
        case VT_R4:
        case VT_ERROR:
            return sizeof(DWORD);
        case VT_I8:
        case VT_UI8:
        case VT_R8:
        case VT_CY:
        case VT_DATE:
        case VT_FILETIME:
            return sizeof(ULONGLONG);
    }
    return 0;
}

static
ULONG
PropertyStore_GetStringSize(
    _In_opt_ PCWSTR psz)
{
    ULONG cch = psz ? lstrlenW(psz) + 1 : 0;

    return sizeof(DWORD) + PropertyStore_Align4(cch * sizeof(WCHAR));
}

/* Size of the serialized data that follows the type header, FALSE when the type cannot be saved */
static
BOOL
PropertyStore_GetDataSize(
    _In_ const PROPVARIANT *pv,
    _Out_ ULONG *pcb)
{
    ULONG cbFixed = PropertyStore_GetFixedSize(pv->vt);
    ULONG i;

    *pcb = 0;
    if (cbFixed)
    {
        *pcb = PropertyStore_Align4(cbFixed);
        return TRUE;
    }

    switch (pv->vt)
    {
        case VT_EMPTY:
        case VT_NULL:
            return TRUE;
        case VT_CLSID:
            *pcb = sizeof(GUID);
            return pv->puuid != NULL;
        case VT_LPWSTR:
            *pcb = PropertyStore_GetStringSize(pv->pwszVal);
            return TRUE;
        case VT_LPSTR:
            *pcb = sizeof(DWORD) + PropertyStore_Align4(pv->pszVal ? lstrlenA(pv->pszVal) + 1 : 0);
            return TRUE;
        case VT_BSTR:
            *pcb = sizeof(DWORD) + PropertyStore_Align4(pv->bstrVal ? SysStringByteLen(pv->bstrVal) + sizeof(WCHAR) : 0);
            return TRUE;
        case VT_BLOB:
            *pcb = sizeof(DWORD) + PropertyStore_Align4(pv->blob.cbSize);
            return TRUE;
        case VT_VECTOR | VT_LPWSTR:
            *pcb = sizeof(DWORD);
            for (i = 0; i < pv->calpwstr.cElems; i++)
                *pcb += PropertyStore_GetStringSize(pv->calpwstr.pElems[i]);
            return TRUE;
    }
    return FALSE;
}

/* Writes a counted byte run padded to a 4 byte boundary */
static
BYTE *
PropertyStore_WriteCounted(
    _Out_ BYTE *pb,
    _In_ DWORD dwCount,
    _In_reads_bytes_opt_(cb) const void *pvData,
    _In_ ULONG cb)
{
    memcpy(pb, &dwCount, sizeof(dwCount));
    pb += sizeof(dwCount);
    memset(pb, 0, PropertyStore_Align4(cb));
    if (cb)
        memcpy(pb, pvData, cb);
    return pb + PropertyStore_Align4(cb);
}

static
BYTE *
PropertyStore_WriteString(
    _Out_ BYTE *pb,
    _In_opt_ PCWSTR psz)
{
    ULONG cch = psz ? lstrlenW(psz) + 1 : 0;

    return PropertyStore_WriteCounted(pb, cch, psz, cch * sizeof(WCHAR));
}

/* Writes the type header and data of a value PropertyStore_GetDataSize accepted */
static
BYTE *
PropertyStore_WriteData(
    _Out_ BYTE *pb,
    _In_ const PROPVARIANT *pv)
{
    ULONG cbFixed = PropertyStore_GetFixedSize(pv->vt);
    ULONG cb;
    WORD wType = pv->vt;
    ULONG i;

    memcpy(pb, &wType, sizeof(wType));
    memset(pb + sizeof(wType), 0, sizeof(WORD));
    pb += PROPSTORE_TYPE_HEADER;

    if (cbFixed)
    {
        memset(pb, 0, PropertyStore_Align4(cbFixed));
        memcpy(pb, &pv->bVal, cbFixed);
        return pb + PropertyStore_Align4(cbFixed);
    }

    switch (pv->vt)
    {
        case VT_CLSID:
            memcpy(pb, pv->puuid, sizeof(GUID));
            pb += sizeof(GUID);
            break;
        case VT_LPWSTR:
            pb = PropertyStore_WriteString(pb, pv->pwszVal);
            break;
        case VT_LPSTR:
            cb = pv->pszVal ? lstrlenA(pv->pszVal) + 1 : 0;
            pb = PropertyStore_WriteCounted(pb, cb, pv->pszVal, cb);
            break;
        case VT_BSTR:
            cb = pv->bstrVal ? SysStringByteLen(pv->bstrVal) + sizeof(WCHAR) : 0;
            pb = PropertyStore_WriteCounted(pb, cb, pv->bstrVal, cb);
            break;
        case VT_BLOB:
            pb = PropertyStore_WriteCounted(pb, pv->blob.cbSize, pv->blob.pBlobData, pv->blob.cbSize);
            break;
        case VT_VECTOR | VT_LPWSTR:
            memcpy(pb, &pv->calpwstr.cElems, sizeof(DWORD));
            pb += sizeof(DWORD);
            for (i = 0; i < pv->calpwstr.cElems; i++)
                pb = PropertyStore_WriteString(pb, pv->calpwstr.pElems[i]);
            break;
    }
    return pb;
}

/* Reads a counted run; pcbUsed gets the padded size it takes in the buffer */
static
HRESULT
PropertyStore_ReadCounted(
    _In_reads_bytes_(cb) const BYTE *pb,
    _In_ ULONG cb,
    _In_ ULONG cbUnit,
    _Out_ DWORD *pdwCount,
    _Out_ const BYTE **ppbData,
    _Out_ ULONG *pcbUsed)
{
    DWORD dwCount;
    ULONG cbData;

    if (cb < sizeof(dwCount))
        return E_FAIL;
    memcpy(&dwCount, pb, sizeof(dwCount));
    if (dwCount > (cb - sizeof(dwCount)) / cbUnit)
        return E_FAIL;

    cbData = dwCount * cbUnit;
    *pdwCount = dwCount;
    *ppbData = pb + sizeof(dwCount);
    *pcbUsed = sizeof(dwCount) + PropertyStore_Align4(cbData);
    if (*pcbUsed > cb)
        *pcbUsed = cb;
    return S_OK;
}

static
HRESULT
PropertyStore_ReadString(
    _In_reads_bytes_(cb) const BYTE *pb,
    _In_ ULONG cb,
    _Outptr_result_maybenull_ LPWSTR *ppsz,
    _Out_ ULONG *pcbUsed)
{
    const BYTE *pbChars;
    DWORD cch;
    HRESULT hr;

    *ppsz = NULL;
    hr = PropertyStore_ReadCounted(pb, cb, sizeof(WCHAR), &cch, &pbChars, pcbUsed);
    if (FAILED(hr) || !cch)
        return hr;

    *ppsz = CoTaskMemAlloc(cch * sizeof(WCHAR));
    if (!*ppsz)
        return E_OUTOFMEMORY;
    memcpy(*ppsz, pbChars, cch * sizeof(WCHAR));
    (*ppsz)[cch - 1] = 0;
    return S_OK;
}

/* Reads one typed value; pb starts at the type header */
static
HRESULT
PropertyStore_ReadData(
    _In_reads_bytes_(cb) const BYTE *pb,
    _In_ ULONG cb,
    _Out_ PROPVARIANT *pv)
{
    const BYTE *pbData;
    ULONG cbFixed;
    ULONG cbUsed;
    DWORD dwCount;
    WORD wType;
    HRESULT hr = S_OK;
    ULONG i;

    PropVariantInit(pv);
    if (cb < PROPSTORE_TYPE_HEADER)
        return E_FAIL;
    memcpy(&wType, pb, sizeof(wType));
    pb += PROPSTORE_TYPE_HEADER;
    cb -= PROPSTORE_TYPE_HEADER;

    cbFixed = PropertyStore_GetFixedSize(wType);
    if (cbFixed)
    {
        if (cb < cbFixed)
            return E_FAIL;
        memcpy(&pv->bVal, pb, cbFixed);
        pv->vt = wType;
        return S_OK;
    }

    switch (wType)
    {
        case VT_EMPTY:
        case VT_NULL:
            break;
        case VT_CLSID:
            if (cb < sizeof(GUID))
                return E_FAIL;
            pv->puuid = CoTaskMemAlloc(sizeof(GUID));
            if (!pv->puuid)
                return E_OUTOFMEMORY;
            memcpy(pv->puuid, pb, sizeof(GUID));
            break;
        case VT_LPWSTR:
            hr = PropertyStore_ReadString(pb, cb, &pv->pwszVal, &cbUsed);
            break;
        case VT_LPSTR:
            hr = PropertyStore_ReadCounted(pb, cb, sizeof(CHAR), &dwCount, &pbData, &cbUsed);
            if (SUCCEEDED(hr) && dwCount)
            {
                pv->pszVal = CoTaskMemAlloc(dwCount);
                if (!pv->pszVal)
                    return E_OUTOFMEMORY;
                memcpy(pv->pszVal, pbData, dwCount);
                pv->pszVal[dwCount - 1] = 0;
            }
            break;
        case VT_BSTR:
            hr = PropertyStore_ReadCounted(pb, cb, sizeof(CHAR), &dwCount, &pbData, &cbUsed);
            if (SUCCEEDED(hr) && dwCount >= sizeof(WCHAR))
            {
                pv->bstrVal = SysAllocStringByteLen((LPCSTR)pbData, dwCount - sizeof(WCHAR));
                if (!pv->bstrVal)
                    return E_OUTOFMEMORY;
            }
            break;
        case VT_BLOB:
            hr = PropertyStore_ReadCounted(pb, cb, sizeof(BYTE), &dwCount, &pbData, &cbUsed);
            if (SUCCEEDED(hr) && dwCount)
            {
                pv->blob.pBlobData = CoTaskMemAlloc(dwCount);
                if (!pv->blob.pBlobData)
                    return E_OUTOFMEMORY;
                memcpy(pv->blob.pBlobData, pbData, dwCount);
                pv->blob.cbSize = dwCount;
            }
            break;
        case VT_VECTOR | VT_LPWSTR:
            if (cb < sizeof(DWORD))
                return E_FAIL;
            memcpy(&dwCount, pb, sizeof(dwCount));
            pb += sizeof(DWORD);
            cb -= sizeof(DWORD);
            if (dwCount > cb / sizeof(DWORD))
                return E_FAIL;
            pv->calpwstr.pElems = CoTaskMemAlloc(dwCount * sizeof(LPWSTR));
            if (!pv->calpwstr.pElems && dwCount)
                return E_OUTOFMEMORY;
            pv->vt = wType;
            for (i = 0; i < dwCount && SUCCEEDED(hr); i++)
            {
                hr = PropertyStore_ReadString(pb, cb, &pv->calpwstr.pElems[i], &cbUsed);
                if (SUCCEEDED(hr))
                {
                    pv->calpwstr.cElems++;
                    pb += cbUsed;
                    cb -= cbUsed;
                }
            }
            if (FAILED(hr))
                PropVariantClear(pv);
            return hr;
        default:
            return E_FAIL;
    }

    if (SUCCEEDED(hr))
        pv->vt = wType;
    return hr;
}

/* Drops every value and format the store holds */
static
VOID
PropertyStore_ClearAll(
    _Inout_ PropertyStore *This)
{
    propstore_format *cursor, *cursor2;

    LIST_FOR_EACH_ENTRY_SAFE(cursor, cursor2, &This->formats, propstore_format, entry)
    {
        list_remove(&cursor->entry);
        destroy_format(cursor);
    }
}

static HRESULT WINAPI PropertyStore_Persist_QueryInterface(IPersistSerializedPropStorage *iface,
    REFIID iid, void **ppv)
{
    PropertyStore *This = impl_from_IPersistSerializedPropStorage(iface);
    return IPropertyStoreCache_QueryInterface(&This->IPropertyStoreCache_iface, iid, ppv);
}

static ULONG WINAPI PropertyStore_Persist_AddRef(IPersistSerializedPropStorage *iface)
{
    PropertyStore *This = impl_from_IPersistSerializedPropStorage(iface);
    return IPropertyStoreCache_AddRef(&This->IPropertyStoreCache_iface);
}

static ULONG WINAPI PropertyStore_Persist_Release(IPersistSerializedPropStorage *iface)
{
    PropertyStore *This = impl_from_IPersistSerializedPropStorage(iface);
    return IPropertyStoreCache_Release(&This->IPropertyStoreCache_iface);
}

static
HRESULT
WINAPI
PropertyStore_Persist_SetFlags(
    _In_ IPersistSerializedPropStorage *iface,
    _In_ PERSIST_SPROPSTORE_FLAGS flags)
{
    PropertyStore *This = impl_from_IPersistSerializedPropStorage(iface);

    This->persist_flags = flags;
    return S_OK;
}

static
HRESULT
WINAPI
PropertyStore_Persist_SetPropertyStorage(
    _In_ IPersistSerializedPropStorage *iface,
    _In_reads_bytes_(cb) PCUSERIALIZEDPROPSTORAGE psps,
    _In_ DWORD cb)
{
    PropertyStore *This = impl_from_IPersistSerializedPropStorage(iface);
    const BYTE *pb = (const BYTE *)psps;
    HRESULT hr = S_OK;

    if (!psps && cb)
        return E_POINTER;

    EnterCriticalSection(&This->lock);
    PropertyStore_ClearAll(This);

    while (SUCCEEDED(hr) && cb >= sizeof(DWORD))
    {
        const BYTE *pbValue;
        DWORD cbStorage;
        DWORD dwVersion;
        ULONG cbLeft;
        PROPERTYKEY key;

        memcpy(&cbStorage, pb, sizeof(cbStorage));
        if (!cbStorage)
            break;
        if (cbStorage > cb || cbStorage < PROPSTORE_STORAGE_HEADER)
        {
            hr = E_FAIL;
            break;
        }

        memcpy(&dwVersion, pb + sizeof(DWORD), sizeof(dwVersion));
        memcpy(&key.fmtid, pb + 2 * sizeof(DWORD), sizeof(GUID));
        if (dwVersion != PROPSTORE_STORAGE_VERSION)
        {
            hr = E_FAIL;
            break;
        }

        pbValue = pb + PROPSTORE_STORAGE_HEADER;
        cbLeft = cbStorage - PROPSTORE_STORAGE_HEADER;
        while (cbLeft >= sizeof(DWORD))
        {
            propstore_value *value;
            PROPVARIANT pv;
            DWORD cbValue;

            memcpy(&cbValue, pbValue, sizeof(cbValue));
            if (!cbValue)
                break;
            if (cbValue > cbLeft || cbValue < PROPSTORE_VALUE_HEADER)
            {
                hr = E_FAIL;
                break;
            }

            /* Named values are skipped, the store only keys by property ID */
            if (!IsEqualGUID(&key.fmtid, &FMTID_NamedProperties))
            {
                memcpy(&key.pid, pbValue + sizeof(DWORD), sizeof(DWORD));
                if (SUCCEEDED(PropertyStore_ReadData(pbValue + PROPSTORE_VALUE_HEADER,
                                                     cbValue - PROPSTORE_VALUE_HEADER,
                                                     &pv)))
                {
                    hr = PropertyStore_LookupValue(This, &key, TRUE, &value);
                    if (SUCCEEDED(hr))
                    {
                        PropVariantClear(&value->propvar);
                        value->propvar = pv;
                        value->state = PSC_NORMAL;
                    }
                    else
                    {
                        PropVariantClear(&pv);
                    }
                }
            }

            pbValue += cbValue;
            cbLeft -= cbValue;
        }

        pb += cbStorage;
        cb -= cbStorage;
    }

    LeaveCriticalSection(&This->lock);
    return hr;
}

static
HRESULT
WINAPI
PropertyStore_Persist_GetPropertyStorage(
    _In_ IPersistSerializedPropStorage *iface,
    _Outptr_result_bytebuffer_(*pcb) SERIALIZEDPROPSTORAGE **ppsps,
    _Out_ DWORD *pcb)
{
    PropertyStore *This = impl_from_IPersistSerializedPropStorage(iface);
    propstore_format *format;
    propstore_value *value;
    ULONG cbTotal = sizeof(DWORD);
    ULONG cbData;
    BYTE *pb;

    if (!ppsps || !pcb)
        return E_POINTER;
    *ppsps = NULL;
    *pcb = 0;

    EnterCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY(format, &This->formats, propstore_format, entry)
    {
        ULONG cbStorage = PROPSTORE_STORAGE_HEADER + sizeof(DWORD);
        BOOL fAny = FALSE;

        LIST_FOR_EACH_ENTRY(value, &format->values, propstore_value, entry)
        {
            if (value->propvar.vt == VT_EMPTY || !PropertyStore_GetDataSize(&value->propvar, &cbData))
                continue;
            cbStorage += PROPSTORE_VALUE_HEADER + PROPSTORE_TYPE_HEADER + cbData;
            fAny = TRUE;
        }
        if (fAny)
            cbTotal += cbStorage;
    }

    pb = CoTaskMemAlloc(cbTotal);
    if (!pb)
    {
        LeaveCriticalSection(&This->lock);
        return E_OUTOFMEMORY;
    }
    *ppsps = (SERIALIZEDPROPSTORAGE *)pb;
    *pcb = cbTotal;

    LIST_FOR_EACH_ENTRY(format, &This->formats, propstore_format, entry)
    {
        BYTE *pbStorage = pb;
        DWORD dwVersion = PROPSTORE_STORAGE_VERSION;
        DWORD cbStorage;

        pb += PROPSTORE_STORAGE_HEADER;
        LIST_FOR_EACH_ENTRY(value, &format->values, propstore_value, entry)
        {
            DWORD cbValue;

            if (value->propvar.vt == VT_EMPTY || !PropertyStore_GetDataSize(&value->propvar, &cbData))
                continue;

            cbValue = PROPSTORE_VALUE_HEADER + PROPSTORE_TYPE_HEADER + cbData;
            memcpy(pb, &cbValue, sizeof(cbValue));
            memcpy(pb + sizeof(DWORD), &value->pid, sizeof(DWORD));
            pb[2 * sizeof(DWORD)] = 0;
            pb = PropertyStore_WriteData(pb + PROPSTORE_VALUE_HEADER, &value->propvar);
        }

        /* A format without anything to save leaves no storage behind */
        if (pb == pbStorage + PROPSTORE_STORAGE_HEADER)
        {
            pb = pbStorage;
            continue;
        }

        memset(pb, 0, sizeof(DWORD));
        pb += sizeof(DWORD);
        cbStorage = (DWORD)(pb - pbStorage);
        memcpy(pbStorage, &cbStorage, sizeof(cbStorage));
        memcpy(pbStorage + sizeof(DWORD), &dwVersion, sizeof(dwVersion));
        memcpy(pbStorage + 2 * sizeof(DWORD), &format->fmtid, sizeof(GUID));
    }
    memset(pb, 0, sizeof(DWORD));

    LeaveCriticalSection(&This->lock);
    return S_OK;
}

static const IPersistSerializedPropStorageVtbl PropertyStore_PersistVtbl = {
    PropertyStore_Persist_QueryInterface,
    PropertyStore_Persist_AddRef,
    PropertyStore_Persist_Release,
    PropertyStore_Persist_SetFlags,
    PropertyStore_Persist_SetPropertyStorage,
    PropertyStore_Persist_GetPropertyStorage
};

#endif /* __REACTOS__ */

HRESULT PropertyStore_CreateInstance(IUnknown *pUnkOuter, REFIID iid, void** ppv)
{
    PropertyStore *This;
    HRESULT ret;

    TRACE("(%p,%s,%p)\n", pUnkOuter, debugstr_guid(iid), ppv);

    *ppv = NULL;

    if (pUnkOuter) return CLASS_E_NOAGGREGATION;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(PropertyStore));
    if (!This) return E_OUTOFMEMORY;

    This->IPropertyStoreCache_iface.lpVtbl = &PropertyStore_Vtbl;
    This->ref = 1;
#ifdef __REACTOS__
    This->IPersistSerializedPropStorage_iface.lpVtbl = &PropertyStore_PersistVtbl;
    This->persist_flags = FPSPS_DEFAULT;
    InitializeCriticalSection(&This->lock);
#else
    InitializeCriticalSectionEx(&This->lock, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
#endif
    This->lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": PropertyStore.lock");
    list_init(&This->formats);

    ret = IPropertyStoreCache_QueryInterface(&This->IPropertyStoreCache_iface, iid, ppv);
    IPropertyStoreCache_Release(&This->IPropertyStoreCache_iface);

    return ret;
}

HRESULT WINAPI PSCreatePropertyStoreFromObject(IUnknown *obj, DWORD access, REFIID riid, void **ret)
{
    HRESULT hr;

    TRACE("(%p, %ld, %s, %p)\n", obj, access, debugstr_guid(riid), ret);

    if (!obj || !ret)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IPropertyStore) && SUCCEEDED(hr = IUnknown_QueryInterface(obj, riid, ret)))
        return hr;

    FIXME("Unimplemented for %s.\n", debugstr_guid(riid));
    return E_NOTIMPL;
}
