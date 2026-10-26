/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Custom destination lists (categories and tasks) applications publish
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "jumplist.h"

/* One category of a custom list: named, known (frequent or recent) or the tasks */
class CDestinationCategory :
    public CPinZeroAlloc
{
public:
    CDestinationCategory();

    ULONG
    AddReference(VOID);

    ULONG
    ReleaseReference(VOID);

    HRESULT
    Load(
        _In_ IStream *pstm,
        _Inout_ UINT *pcTotalDests);

    HRESULT
    Save(
        _In_ IStream *pstm) const;

    HRESULT
    AppendDestination(
        _In_ IShellItem *psi);

    HRESULT
    AppendLink(
        _In_ IShellLinkW *psl);

    UINT
    GetDestCount() const;

    IUnknown *
    GetDest(
        _In_ UINT iDest) const;

    VOID
    DeleteDest(
        _In_ UINT iDest);

    LONG m_cRef;
    KNOWNDESTCATEGORY m_KnownCategory;
    PWSTR m_pszName;
    HDPA m_hdpaDests;
    DESTCATTYPE m_Type;

private:
    ~CDestinationCategory();

    HRESULT
    _LoadDests(
        _In_ IStream *pstm,
        _Inout_ UINT *pcTotalDests);

    HRESULT
    _WriteDests(
        _In_ IStream *pstm) const;
};

class CDestinationList :
    public CPinZeroAlloc,
    public CComCoClass<CDestinationList, &CLSID_DestinationList>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public ICustomDestinationList,
    public IInternalCustomDestinationList
{
public:
    CDestinationList();
    virtual ~CDestinationList();

    // *** ICustomDestinationList methods ***
    STDMETHODIMP SetAppID(PCWSTR pszAppID) override;
    STDMETHODIMP BeginList(UINT *pcMinSlots, REFIID riid, void **ppv) override;
    STDMETHODIMP AppendCategory(PCWSTR pszCategory, IObjectArray *poa) override;
    STDMETHODIMP AppendKnownCategory(KNOWNDESTCATEGORY category) override;
    STDMETHODIMP AddUserTasks(IObjectArray *poa) override;
    STDMETHODIMP CommitList() override;
    STDMETHODIMP GetRemovedDestinations(REFIID riid, void **ppv) override;
    STDMETHODIMP DeleteList(PCWSTR pszAppID) override;
    STDMETHODIMP AbortList() override;

    // *** IInternalCustomDestinationList methods ***
    STDMETHODIMP SetMinItems(UINT cMinItems) override;
    STDMETHODIMP SetApplicationID(PCWSTR pszAppID) override;
    STDMETHODIMP GetSlotCount(UINT *pcSlots) override;
    STDMETHODIMP GetCategoryCount(UINT *pcCategories) override;
    STDMETHODIMP GetCategory(UINT iCategory, GETCATFLAG flags, APPDESTCATEGORY *pCategory) override;
    STDMETHODIMP DeleteCategory(UINT iCategory, BOOL fDeleteDestinations) override;
    STDMETHODIMP EnumerateCategoryDestinations(UINT iCategory, REFIID riid, void **ppv) override;
    STDMETHODIMP RemoveDestination(IUnknown *punk) override;
    STDMETHODIMP ResolveDestination(HWND hwnd, DWORD dwFlags, IShellItem *psi, REFIID riid, void **ppv) override;

    static
    HRESULT
    s_DeleteList(
        _In_opt_ PCWSTR pszAppID,
        _In_opt_ PCWSTR pszListPath,
        _In_ UINT uOptions);

    DECLARE_REGISTRY_RESOURCEID(IDR_DESTINATIONLIST)
    DECLARE_NOT_AGGREGATABLE(CDestinationList)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CDestinationList)
        COM_INTERFACE_ENTRY_IID(IID_ICustomDestinationList, ICustomDestinationList)
        COM_INTERFACE_ENTRY_IID(IID_IInternalCustomDestinationList, IInternalCustomDestinationList)
    END_COM_MAP()

private:
    HRESULT
    _EnsureAppID(VOID);

    HRESULT
    _EnsureLoaded(VOID);

    HRESULT
    _LoadList(
        _In_opt_ PCWSTR pszListPath);

    HRESULT
    _LoadCategories(
        _In_ IStream *pstm,
        _In_ UINT cCategories);

    HRESULT
    _LoadRemovedItems(
        _In_ IStream *pstm,
        _In_ UINT cRemoved);

    HRESULT
    _AppendCategory(
        _In_opt_ PCWSTR pszCategory,
        _In_ IObjectArray *poa,
        _In_ BOOL fTasks);

    HRESULT
    _EnsureAddedItemIsNotRemoved(
        _In_ IUnknown *punk);

    HRESULT
    _GetApplicationCustomPath(
        _In_ PCWSTR pszAppID,
        _Outptr_ PWSTR *ppszPath);

    HRESULT
    _WriteList(VOID);

    HRESULT
    _WriteListToFile(
        _In_ IStream *pstm);

    UINT
    _GetCategoryCount() const;

    CDestinationCategory *
    _GetCategory(
        _In_ UINT iCategory) const;

    VOID
    _DestroyCategories(VOID);

    VOID
    _DestroyRemoved(VOID);

    BOOL m_fInList;
    BOOL m_fLoaded;
    BOOL m_fAddedRemovedItem;
    PWSTR m_pszAppID;
    PWSTR m_pszListPath;
    PWSTR m_pszAppPath;
    HDPA m_hdpaCategories;
    HDPA m_hdpaRemoved;
};
