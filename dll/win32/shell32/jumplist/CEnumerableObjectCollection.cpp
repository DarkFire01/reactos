/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Object collection handed to and returned by jump list APIs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "CEnumerableObjectCollection.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* Growth step of the object array when the caller gives none */
#define COLLECTION_DEFAULT_GROW     4

CEnumerableObjectCollection::CEnumerableObjectCollection() :
    m_cGrow(COLLECTION_DEFAULT_GROW),
    m_iNext(0),
    m_hdpa(NULL)
{
}

CEnumerableObjectCollection::~CEnumerableObjectCollection()
{
    if (m_hdpa)
        DPA_DestroyCallback(m_hdpa, DPA_ReleaseUnknownCB, NULL);
}

HRESULT
CEnumerableObjectCollection::Initialize(
    _In_ UINT cGrow)
{
    m_cGrow = cGrow;
    return S_OK;
}

UINT
CEnumerableObjectCollection::_GetCount() const
{
    return m_hdpa ? DPA_GetPtrCount(m_hdpa) : 0;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::GetCount(
    _Out_ UINT *pcObjects)
{
    *pcObjects = _GetCount();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::GetAt(
    _In_ UINT uiIndex,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    IUnknown *punk;

    *ppv = NULL;
    if (uiIndex >= _GetCount())
        return E_INVALIDARG;

    punk = (IUnknown *)DPA_GetPtr(m_hdpa, uiIndex);
    return punk->QueryInterface(riid, ppv);
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::AddObject(
    _In_ IUnknown *punk)
{
    if (!m_hdpa)
    {
        m_hdpa = DPA_Create(m_cGrow);
        if (!m_hdpa)
            return E_OUTOFMEMORY;
    }

    if (DPA_AppendPtr(m_hdpa, punk) == -1)
        return E_OUTOFMEMORY;

    punk->AddRef();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::AddFromArray(
    _In_ IObjectArray *poaSource)
{
    UINT cObjects;
    HRESULT hr;

    hr = poaSource->GetCount(&cObjects);
    for (UINT i = 0; SUCCEEDED(hr) && i < cObjects; i++)
    {
        IUnknown *punk;

        hr = poaSource->GetAt(i, IID_PPV_ARG(IUnknown, &punk));
        if (SUCCEEDED(hr))
        {
            hr = AddObject(punk);
            punk->Release();
        }
    }
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::RemoveObjectAt(
    _In_ UINT uiIndex)
{
    IUnknown *punk;

    if (uiIndex >= _GetCount())
        return E_INVALIDARG;

    punk = (IUnknown *)DPA_DeletePtr(m_hdpa, uiIndex);
    punk->Release();
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Clear()
{
    if (m_hdpa)
    {
        DPA_DestroyCallback(m_hdpa, DPA_ReleaseUnknownCB, NULL);
        m_hdpa = NULL;
    }
    m_iNext = 0;
    return S_OK;
}

/* Hands out up to celt objects from the cursor on; S_FALSE when fewer were left */
HRESULT
CEnumerableObjectCollection::_NextAs(
    _In_ ULONG celt,
    _In_ REFIID riid,
    _Out_writes_to_(celt, *pceltFetched) void **rgelt,
    _Out_opt_ ULONG *pceltFetched)
{
    UINT iEnd = m_iNext + celt;
    UINT iItem;

    if (iEnd >= _GetCount())
        iEnd = _GetCount();

    for (iItem = m_iNext; iItem < iEnd; iItem++)
    {
        IUnknown *punk = (IUnknown *)DPA_GetPtr(m_hdpa, iItem);
        HRESULT hr = punk->QueryInterface(riid, &rgelt[iItem - m_iNext]);

        if (FAILED(hr))
        {
            /* Nothing is handed out when one object lacks the interface */
            while (iItem > m_iNext)
            {
                iItem -= 1;
                ((IUnknown *)rgelt[iItem - m_iNext])->Release();
            }
            if (pceltFetched)
                *pceltFetched = 0;
            return hr;
        }
    }

    ULONG cFetched = iItem - m_iNext;
    m_iNext = iItem;
    if (pceltFetched)
        *pceltFetched = cFetched;
    return (cFetched < celt) ? S_FALSE : S_OK;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Next(
    _In_ ULONG celt,
    _In_ REFIID riid,
    _Out_writes_to_(celt, *pceltFetched) void **rgelt,
    _Out_opt_ ULONG *pceltFetched)
{
    return _NextAs(celt, riid, rgelt, pceltFetched);
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Next(
    _In_ ULONG celt,
    _Out_writes_to_(celt, *pceltFetched) IUnknown **rgelt,
    _Out_opt_ ULONG *pceltFetched)
{
    return _NextAs(celt, IID_IUnknown, (void **)rgelt, pceltFetched);
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Skip(
    _In_ ULONG celt)
{
    UINT iOld = m_iNext;
    UINT iNew = m_iNext + celt;

    if (iNew >= _GetCount())
        iNew = _GetCount();

    m_iNext = iNew;
    return (iNew - iOld < celt) ? S_FALSE : S_OK;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Reset()
{
    m_iNext = 0;
    return S_OK;
}

/* Copies the objects, the growth step and the cursor into a new collection */
HRESULT
CEnumerableObjectCollection::_CloneInto(
    _Inout_ CEnumerableObjectCollection *pClone)
{
    UINT cItems = _GetCount();
    HRESULT hr = S_OK;

    pClone->m_hdpa = DPA_Create(max(m_cGrow, cItems));
    if (!pClone->m_hdpa)
        return E_OUTOFMEMORY;

    for (UINT i = 0; SUCCEEDED(hr) && i < cItems; i++)
    {
        IUnknown *punk = (IUnknown *)DPA_GetPtr(m_hdpa, i);

        if (!punk)
        {
            hr = E_UNEXPECTED;
            break;
        }

        if (DPA_AppendPtr(pClone->m_hdpa, punk) == -1)
        {
            hr = E_OUTOFMEMORY;
            break;
        }

        punk->AddRef();
        pClone->m_cGrow = m_cGrow;
        pClone->m_iNext = m_iNext;
    }
    return hr;
}

HRESULT
CEnumerableObjectCollection::_Clone(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IObjectCollection> poc;
    CEnumerableObjectCollection *pClone;
    HRESULT hr;

    *ppv = NULL;
    hr = ShellObjectCreatorInit<CEnumerableObjectCollection>(m_cGrow, IID_PPV_ARG(IObjectCollection, &poc));
    if (FAILED(hr))
        return hr;

    pClone = static_cast<CEnumerableObjectCollection *>(poc.p);
    pClone->m_iNext = m_iNext;
    hr = _CloneInto(pClone);
    if (SUCCEEDED(hr))
        hr = poc->QueryInterface(riid, ppv);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Clone(
    _Outptr_ IEnumObjects **ppenum)
{
    return _Clone(IID_PPV_ARG(IEnumObjects, ppenum));
}

HRESULT
STDMETHODCALLTYPE
CEnumerableObjectCollection::Clone(
    _Outptr_ IEnumUnknown **ppenum)
{
    return _Clone(IID_PPV_ARG(IEnumUnknown, ppenum));
}

HRESULT
CreateEnumerableObjectCollection(
    _In_ UINT cGrow,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return ShellObjectCreatorInit<CEnumerableObjectCollection>(cGrow, riid, ppv);
}
