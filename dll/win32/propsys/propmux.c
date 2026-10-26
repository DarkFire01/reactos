/*
 * PROJECT:     ReactOS Property System
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Property stores that answer from several stores in order
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <winreg.h>
#include <propvarutil.h>
#include "propdesc.h"

WINE_DEFAULT_DEBUG_CHANNEL(propsys);

typedef struct _MUXSTORE
{
    IPropertyStore IPropertyStore_iface;
    IPropertyStoreCapabilities IPropertyStoreCapabilities_iface;
    LONG cRef;
    CRITICAL_SECTION cs;
    IPropertyStore **rgStores;
    DWORD cStores;

    /* Delayed stores are created from the factory on first use */
    IDelayedPropertyStoreFactory *pdpsf;
    GETPROPERTYSTOREFLAGS gpsFlags;
    DWORD *rgStoreIds;
    BOOL fStoresCreated;

    /* Union of every store's keys, built on first GetCount or GetAt */
    PROPERTYKEY *rgKeys;
    DWORD cKeys;
    DWORD cKeysAlloc;
    BOOL fKeysBuilt;
} MUXSTORE;

static inline MUXSTORE *
impl_from_IPropertyStore(
    _In_ IPropertyStore *iface)
{
    return CONTAINING_RECORD(iface, MUXSTORE, IPropertyStore_iface);
}

static inline MUXSTORE *
impl_from_IPropertyStoreCapabilities(
    _In_ IPropertyStoreCapabilities *iface)
{
    return CONTAINING_RECORD(iface, MUXSTORE, IPropertyStoreCapabilities_iface);
}

static
VOID
MuxStore_EnsureStores(
    _In_ MUXSTORE *This)
{
    DWORD i;

    if (This->fStoresCreated || !This->pdpsf)
        return;

    This->fStoresCreated = TRUE;
    for (i = 0; i < This->cStores; i++)
    {
        if (FAILED(IDelayedPropertyStoreFactory_GetDelayedPropertyStore(This->pdpsf, This->gpsFlags,
                                                                       This->rgStoreIds[i],
                                                                       &IID_IPropertyStore,
                                                                       (void **)&This->rgStores[i])))
        {
            This->rgStores[i] = NULL;
        }
    }
}

static
BOOL
MuxStore_HasKey(
    _In_ MUXSTORE *This,
    _In_ REFPROPERTYKEY key)
{
    DWORD i;

    for (i = 0; i < This->cKeys; i++)
    {
        if (This->rgKeys[i].pid == key->pid && IsEqualGUID(&This->rgKeys[i].fmtid, &key->fmtid))
            return TRUE;
    }
    return FALSE;
}

static
HRESULT
MuxStore_EnsureKeys(
    _In_ MUXSTORE *This)
{
    DWORD i;

    if (This->fKeysBuilt)
        return S_OK;

    MuxStore_EnsureStores(This);
    for (i = 0; i < This->cStores; i++)
    {
        DWORD cKeys = 0;
        DWORD j;

        if (!This->rgStores[i] || FAILED(IPropertyStore_GetCount(This->rgStores[i], &cKeys)))
            continue;

        for (j = 0; j < cKeys; j++)
        {
            PROPERTYKEY key;

            if (FAILED(IPropertyStore_GetAt(This->rgStores[i], j, &key)) || MuxStore_HasKey(This, &key))
                continue;

            if (This->cKeys == This->cKeysAlloc)
            {
                DWORD cNew = This->cKeysAlloc ? This->cKeysAlloc * 2 : 8;
                PROPERTYKEY *rgNew;

                if (This->rgKeys)
                    rgNew = HeapReAlloc(GetProcessHeap(), 0, This->rgKeys, sizeof(*rgNew) * cNew);
                else
                    rgNew = HeapAlloc(GetProcessHeap(), 0, sizeof(*rgNew) * cNew);
                if (!rgNew)
                    return E_OUTOFMEMORY;
                This->rgKeys = rgNew;
                This->cKeysAlloc = cNew;
            }
            This->rgKeys[This->cKeys++] = key;
        }
    }
    This->fKeysBuilt = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
MuxStore_QueryInterface(
    _In_ IPropertyStore *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyStore))
    {
        *ppv = &This->IPropertyStore_iface;
    }
    else if (IsEqualIID(riid, &IID_IPropertyStoreCapabilities))
    {
        *ppv = &This->IPropertyStoreCapabilities_iface;
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
MuxStore_AddRef(
    _In_ IPropertyStore *iface)
{
    return InterlockedIncrement(&impl_from_IPropertyStore(iface)->cRef);
}

static ULONG STDMETHODCALLTYPE
MuxStore_Release(
    _In_ IPropertyStore *iface)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);
    ULONG cRef = InterlockedDecrement(&This->cRef);
    DWORD i;

    if (cRef)
        return cRef;

    for (i = 0; i < This->cStores; i++)
    {
        if (This->rgStores[i])
            IPropertyStore_Release(This->rgStores[i]);
    }
    if (This->pdpsf)
        IDelayedPropertyStoreFactory_Release(This->pdpsf);
    HeapFree(GetProcessHeap(), 0, This->rgStores);
    HeapFree(GetProcessHeap(), 0, This->rgStoreIds);
    HeapFree(GetProcessHeap(), 0, This->rgKeys);
    DeleteCriticalSection(&This->cs);
    HeapFree(GetProcessHeap(), 0, This);
    return 0;
}

static HRESULT STDMETHODCALLTYPE
MuxStore_GetCount(
    _In_ IPropertyStore *iface,
    _Out_ DWORD *cProps)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);
    HRESULT hr;

    if (!cProps)
        return E_POINTER;

    EnterCriticalSection(&This->cs);
    hr = MuxStore_EnsureKeys(This);
    *cProps = SUCCEEDED(hr) ? This->cKeys : 0;
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE
MuxStore_GetAt(
    _In_ IPropertyStore *iface,
    _In_ DWORD iProp,
    _Out_ PROPERTYKEY *pkey)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);
    HRESULT hr;

    if (!pkey)
        return E_POINTER;

    EnterCriticalSection(&This->cs);
    hr = MuxStore_EnsureKeys(This);
    if (SUCCEEDED(hr))
    {
        if (iProp < This->cKeys)
            *pkey = This->rgKeys[iProp];
        else
            hr = E_INVALIDARG;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

/* The first store with a value wins */
static HRESULT STDMETHODCALLTYPE
MuxStore_GetValue(
    _In_ IPropertyStore *iface,
    _In_ REFPROPERTYKEY key,
    _Out_ PROPVARIANT *pv)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);
    HRESULT hr = S_OK;
    DWORD i;

    if (!pv)
        return E_POINTER;
    PropVariantInit(pv);

    EnterCriticalSection(&This->cs);
    MuxStore_EnsureStores(This);
    for (i = 0; i < This->cStores; i++)
    {
        if (!This->rgStores[i])
            continue;

        hr = IPropertyStore_GetValue(This->rgStores[i], key, pv);
        if (SUCCEEDED(hr) && pv->vt != VT_EMPTY)
            break;
        PropVariantClear(pv);
        hr = S_OK;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE
MuxStore_SetValue(
    _In_ IPropertyStore *iface,
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);
    HRESULT hr = STG_E_ACCESSDENIED;
    DWORD i;

    EnterCriticalSection(&This->cs);
    MuxStore_EnsureStores(This);
    for (i = 0; i < This->cStores; i++)
    {
        IPropertyStoreCapabilities *ppsc;

        if (!This->rgStores[i])
            continue;

        if (SUCCEEDED(IPropertyStore_QueryInterface(This->rgStores[i], &IID_IPropertyStoreCapabilities,
                                                    (void **)&ppsc)))
        {
            HRESULT hrWritable = IPropertyStoreCapabilities_IsPropertyWritable(ppsc, key);

            IPropertyStoreCapabilities_Release(ppsc);
            if (hrWritable != S_OK)
                continue;
        }

        hr = IPropertyStore_SetValue(This->rgStores[i], key, propvar);
        if (SUCCEEDED(hr))
            break;
    }
    This->fKeysBuilt = FALSE;
    This->cKeys = 0;
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE
MuxStore_Commit(
    _In_ IPropertyStore *iface)
{
    MUXSTORE *This = impl_from_IPropertyStore(iface);
    HRESULT hr = S_OK;
    DWORD i;

    EnterCriticalSection(&This->cs);
    for (i = 0; i < This->cStores; i++)
    {
        HRESULT hrStore;

        if (!This->rgStores[i])
            continue;
        hrStore = IPropertyStore_Commit(This->rgStores[i]);
        if (FAILED(hrStore) && hrStore != E_NOTIMPL && SUCCEEDED(hr))
            hr = hrStore;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static const IPropertyStoreVtbl MuxStore_Vtbl =
{
    MuxStore_QueryInterface,
    MuxStore_AddRef,
    MuxStore_Release,
    MuxStore_GetCount,
    MuxStore_GetAt,
    MuxStore_GetValue,
    MuxStore_SetValue,
    MuxStore_Commit
};

static HRESULT STDMETHODCALLTYPE
MuxStoreCaps_QueryInterface(
    _In_ IPropertyStoreCapabilities *iface,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return MuxStore_QueryInterface(&impl_from_IPropertyStoreCapabilities(iface)->IPropertyStore_iface, riid, ppv);
}

static ULONG STDMETHODCALLTYPE
MuxStoreCaps_AddRef(
    _In_ IPropertyStoreCapabilities *iface)
{
    return MuxStore_AddRef(&impl_from_IPropertyStoreCapabilities(iface)->IPropertyStore_iface);
}

static ULONG STDMETHODCALLTYPE
MuxStoreCaps_Release(
    _In_ IPropertyStoreCapabilities *iface)
{
    return MuxStore_Release(&impl_from_IPropertyStoreCapabilities(iface)->IPropertyStore_iface);
}

/* Writable when any store takes the key; stores without the interface count as writable */
static HRESULT STDMETHODCALLTYPE
MuxStoreCaps_IsPropertyWritable(
    _In_ IPropertyStoreCapabilities *iface,
    _In_ REFPROPERTYKEY key)
{
    MUXSTORE *This = impl_from_IPropertyStoreCapabilities(iface);
    HRESULT hr = S_FALSE;
    DWORD i;

    EnterCriticalSection(&This->cs);
    MuxStore_EnsureStores(This);
    for (i = 0; i < This->cStores && hr != S_OK; i++)
    {
        IPropertyStoreCapabilities *ppsc;

        if (!This->rgStores[i])
            continue;

        if (SUCCEEDED(IPropertyStore_QueryInterface(This->rgStores[i], &IID_IPropertyStoreCapabilities,
                                                    (void **)&ppsc)))
        {
            hr = IPropertyStoreCapabilities_IsPropertyWritable(ppsc, key);
            IPropertyStoreCapabilities_Release(ppsc);
        }
        else
        {
            hr = S_OK;
        }
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static const IPropertyStoreCapabilitiesVtbl MuxStoreCaps_Vtbl =
{
    MuxStoreCaps_QueryInterface,
    MuxStoreCaps_AddRef,
    MuxStoreCaps_Release,
    MuxStoreCaps_IsPropertyWritable
};

static
MUXSTORE *
MuxStore_Alloc(
    _In_ DWORD cStores)
{
    MUXSTORE *This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));

    if (!This)
        return NULL;

    This->rgStores = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This->rgStores) * max(cStores, 1));
    if (!This->rgStores)
    {
        HeapFree(GetProcessHeap(), 0, This);
        return NULL;
    }

    This->IPropertyStore_iface.lpVtbl = &MuxStore_Vtbl;
    This->IPropertyStoreCapabilities_iface.lpVtbl = &MuxStoreCaps_Vtbl;
    This->cRef = 1;
    This->cStores = cStores;
    InitializeCriticalSection(&This->cs);
    return This;
}

static
HRESULT
MuxStore_Finish(
    _In_ MUXSTORE *This,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    HRESULT hr = MuxStore_QueryInterface(&This->IPropertyStore_iface, riid, ppv);

    MuxStore_Release(&This->IPropertyStore_iface);
    return hr;
}

HRESULT
WINAPI
PSCreateMultiplexPropertyStore(
    _In_reads_(cStores) IUnknown **prgpunkStores,
    _In_ DWORD cStores,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    MUXSTORE *This;
    DWORD i;

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (!prgpunkStores && cStores)
        return E_INVALIDARG;

    This = MuxStore_Alloc(cStores);
    if (!This)
        return E_OUTOFMEMORY;

    for (i = 0; i < cStores; i++)
    {
        if (!prgpunkStores[i] ||
            FAILED(IUnknown_QueryInterface(prgpunkStores[i], &IID_IPropertyStore, (void **)&This->rgStores[i])))
        {
            MuxStore_Release(&This->IPropertyStore_iface);
            return E_INVALIDARG;
        }
    }
    return MuxStore_Finish(This, riid, ppv);
}

HRESULT
WINAPI
PSCreateDelayedMultiplexPropertyStore(
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_ IDelayedPropertyStoreFactory *pdpsf,
    _In_reads_(cStores) const DWORD *rgStoreIds,
    _In_ DWORD cStores,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    MUXSTORE *This;

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (!pdpsf || (!rgStoreIds && cStores))
        return E_INVALIDARG;

    This = MuxStore_Alloc(cStores);
    if (!This)
        return E_OUTOFMEMORY;

    This->rgStoreIds = HeapAlloc(GetProcessHeap(), 0, sizeof(*This->rgStoreIds) * max(cStores, 1));
    if (!This->rgStoreIds)
    {
        MuxStore_Release(&This->IPropertyStore_iface);
        return E_OUTOFMEMORY;
    }
    if (cStores)
        CopyMemory(This->rgStoreIds, rgStoreIds, sizeof(*rgStoreIds) * cStores);

    This->pdpsf = pdpsf;
    IDelayedPropertyStoreFactory_AddRef(pdpsf);
    This->gpsFlags = flags;
    return MuxStore_Finish(This, riid, ppv);
}
