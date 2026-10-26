/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Object collection handed to and returned by jump list APIs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "jumplist.h"

class CEnumerableObjectCollection :
    public CPinZeroAlloc,
    public CComCoClass<CEnumerableObjectCollection, &CLSID_EnumerableObjectCollection>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IObjectCollection,
    public IEnumObjects,
    public IEnumUnknown
{
public:
    CEnumerableObjectCollection();
    virtual ~CEnumerableObjectCollection();

    HRESULT
    Initialize(
        _In_ UINT cGrow);

    // *** IObjectArray methods ***
    STDMETHODIMP GetCount(UINT *pcObjects) override;
    STDMETHODIMP GetAt(UINT uiIndex, REFIID riid, void **ppv) override;

    // *** IObjectCollection methods ***
    STDMETHODIMP AddObject(IUnknown *punk) override;
    STDMETHODIMP AddFromArray(IObjectArray *poaSource) override;
    STDMETHODIMP RemoveObjectAt(UINT uiIndex) override;
    STDMETHODIMP Clear() override;

    // *** IEnumObjects methods ***
    STDMETHODIMP Next(ULONG celt, REFIID riid, void **rgelt, ULONG *pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumObjects **ppenum) override;

    // *** IEnumUnknown methods ***
    STDMETHODIMP Next(ULONG celt, IUnknown **rgelt, ULONG *pceltFetched) override;
    STDMETHODIMP Clone(IEnumUnknown **ppenum) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_ENUMERABLEOBJECTCOLLECTION)
    DECLARE_NOT_AGGREGATABLE(CEnumerableObjectCollection)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CEnumerableObjectCollection)
        COM_INTERFACE_ENTRY_IID(IID_IObjectCollection, IObjectCollection)
        COM_INTERFACE_ENTRY_IID(IID_IObjectArray, IObjectArray)
        COM_INTERFACE_ENTRY_IID(IID_IEnumObjects, IEnumObjects)
        COM_INTERFACE_ENTRY_IID(IID_IEnumUnknown, IEnumUnknown)
    END_COM_MAP()

private:
    UINT
    _GetCount() const;

    HRESULT
    _CloneInto(
        _Inout_ CEnumerableObjectCollection *pClone);

    HRESULT
    _Clone(
        _In_ REFIID riid,
        _Outptr_ void **ppv);

    HRESULT
    _NextAs(
        _In_ ULONG celt,
        _In_ REFIID riid,
        _Out_writes_to_(celt, *pceltFetched) void **rgelt,
        _Out_opt_ ULONG *pceltFetched);

    UINT m_cGrow;
    UINT m_iNext;
    HDPA m_hdpa;
};
