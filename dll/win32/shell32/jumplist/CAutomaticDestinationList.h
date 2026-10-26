/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Recent, frequent and pinned destinations the shell keeps per application
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "jumplist.h"

/* Header of the DestList stream */
typedef struct _DESTLIST_HEADER
{
    DWORD dwVersion;
    DWORD cEntries;
    DWORD cPinned;
    FLOAT flTotalPoints;
    ULONGLONG ullLastEntryId;
    DWORD cChanges;
    DWORD dwReserved;
} DESTLIST_HEADER;

C_ASSERT(sizeof(DESTLIST_HEADER) == 32);

/* One entry as the DestList stream stores it; the entry name follows on disk */
typedef struct _DESTLIST_ENTRY_DATA
{
    ULONGLONG ullChecksum;
    BYTE rgbTracking[80];
    ULONGLONG ullEntryId;
    FLOAT flPoints;
    FILETIME ftLastAccess;
    int iPinPos;
} DESTLIST_ENTRY_DATA;

C_ASSERT(sizeof(DESTLIST_ENTRY_DATA) == 112);

typedef struct _DESTLIST_ENTRY
{
    DESTLIST_ENTRY_DATA Data;
    PWSTR pszName;
} DESTLIST_ENTRY, *PDESTLIST_ENTRY;

/* iPinPos of an entry that is not pinned */
#define DESTLIST_NOT_PINNED     (-1)

/* PinItem positions that append to or drop from the pinned items */
#define DESTLIST_PIN_APPEND     (-1)
#define DESTLIST_PIN_REMOVE     (-2)

/* The compound file behind one application's automatic list */
class CAutoDestList
{
public:
    CAutoDestList();
    ~CAutoDestList();

    VOID
    SetOwner(
        _In_ PCWSTR pszPath,
        _In_opt_ PCWSTR pszAppID,
        _In_opt_ PCWSTR pszAppPath);

    HRESULT
    GetMatchingEntryIndex(
        _In_ IUnknown *punk,
        _In_ PCWSTR pszName,
        _Out_ int *piEntry);

    HRESULT
    AddItem(
        _In_ IUnknown *punk,
        _In_ PCWSTR pszName,
        _In_ BOOL fAddPoint,
        _Out_ int *piEntry);

    HRESULT
    AddPoints(
        _In_ int iEntry);

    HRESULT
    RemoveMatchingEntry(
        _In_ int iEntry);

    HRESULT
    UnPin(
        _In_ int iEntry,
        _In_ BOOL fKeep);

    HRESULT
    PinAt(
        _In_ int iEntry,
        _In_ int iPinPos);

    HRESULT
    SetData(
        _In_ int iEntry,
        _In_opt_ const FLOAT *pflPoints,
        _In_opt_ const FILETIME *pftLastAccess);

    HRESULT
    GetItemsMatchingLinks(
        _In_ DESTLISTTYPE type,
        _In_ UINT cMax,
        _In_ IObjectCollection *poc);

    HRESULT
    ResolveDestination(
        _In_opt_ HWND hwnd,
        _In_ DWORD dwFlags,
        _In_ int iEntry,
        _In_ REFIID riid,
        _Outptr_ void **ppv);

    HRESULT
    Persist(VOID);

    HRESULT
    EnsureLoaded(VOID);

    PDESTLIST_ENTRY
    GetEntry(
        _In_ int iEntry) const;

    VOID
    ClearAll(VOID);

    DESTLIST_HEADER m_Header;
    BOOL m_fDirty;

private:
    static int CALLBACK s_SortEntryCB(void *p1, void *p2, LPARAM lParam);

    static int CALLBACK s_FreeEntryCB(void *p, void *pData);

    static
    VOID
    FreeEntry(
        _In_opt_ PDESTLIST_ENTRY pEntry);

    HRESULT
    _EnsureStorage(VOID);

    HRESULT
    _GetDestListStream(
        _Outptr_ IStream **ppstm);

    HRESULT
    _GetEntryStream(
        _In_ ULONGLONG ullEntryId,
        _Outptr_ IStream **ppstm);

    HRESULT
    _LoadEntries(
        _In_ IStream *pstm);

    HRESULT
    _GetMatchingEntryIndex(
        _In_ PCWSTR pszName,
        _Out_ int *piEntry);

    HRESULT
    _SaveLink(
        _In_ ULONGLONG ullEntryId,
        _In_ IShellLinkW *psl);

    HRESULT
    _LoadItemLink(
        _In_ const DESTLIST_ENTRY *pEntry,
        _In_ REFIID riid,
        _Outptr_ void **ppv);

    HRESULT
    _ResolveAndBindLinkToAssoc(
        _In_ IShellLinkW *psl,
        _In_opt_ HWND hwnd,
        _In_ DWORD dwFlags);

    HRESULT
    _DecayPoints(VOID);

    VOID
    _DestroyEntries(VOID);

    PCWSTR m_pszPath;
    IStorage *m_pstg;
    HDPA m_hdpaEntries;
    PCWSTR m_pszAppID;
    PCWSTR m_pszAppPath;

    static ULONG s_ulDebounceTime;
    static BOOL s_fCheckedForCustomDebounceTime;
};

class CAutomaticDestinationList :
    public CPinZeroAlloc,
    public CComCoClass<CAutomaticDestinationList, &CLSID_AutomaticDestinationList>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IAutomaticDestinationList
{
public:
    CAutomaticDestinationList();
    virtual ~CAutomaticDestinationList();

    // *** IAutomaticDestinationList methods ***
    STDMETHODIMP Initialize(PCWSTR pszAppID, PCWSTR pszAppPath, PCWSTR pszListPath) override;
    STDMETHODIMP HasList(BOOL *pfHasList) override;
    STDMETHODIMP GetList(DESTLISTTYPE type, int cMaxCount, REFIID riid, void **ppv) override;
    STDMETHODIMP AddUsagePoint(IUnknown *punk) override;
    STDMETHODIMP PinItem(IUnknown *punk, int iPinPos) override;
    STDMETHODIMP IsPinned(IUnknown *punk, int *piPinPos) override;
    STDMETHODIMP RemoveDestination(IUnknown *punk) override;
    STDMETHODIMP SetUsageData(IUnknown *punk, FLOAT *pflPoints, FILETIME *pftLastAccess) override;
    STDMETHODIMP GetUsageData(IUnknown *punk, FLOAT *pflPoints, FILETIME *pftLastAccess) override;
    STDMETHODIMP ResolveDestination(HWND hwnd, DWORD dwFlags, IShellItem *psi, REFIID riid, void **ppv) override;
    STDMETHODIMP ClearList(BOOL fClearPinned) override;

    static
    HRESULT
    s_DeleteList(
        _In_ PCWSTR pszListPath,
        _In_opt_ PCWSTR pszAppID,
        _In_opt_ PCWSTR pszAppPath,
        _In_ UINT uFlags);

    DECLARE_REGISTRY_RESOURCEID(IDR_AUTOMATICDESTINATIONLIST)
    DECLARE_NOT_AGGREGATABLE(CAutomaticDestinationList)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CAutomaticDestinationList)
        COM_INTERFACE_ENTRY_IID(IID_IAutomaticDestinationList, IAutomaticDestinationList)
    END_COM_MAP()

private:
    HRESULT
    _EnsureDirectory(VOID);

    HRESULT
    _GetItemDisplayName(
        _In_ IUnknown *punk,
        _Outptr_ PWSTR *ppszName);

    HRESULT
    _EnsureItemGetIndex(
        _In_ IUnknown *punk,
        _Inout_ CAutoDestList *pList,
        _In_ BOOL fAddPoint,
        _Out_ int *piEntry,
        _Out_opt_ BOOL *pfAdded);

    HRESULT
    _GetIndex(
        _In_ IUnknown *punk,
        _Inout_ CAutoDestList *pList,
        _Out_ int *piEntry);

    VOID
    _ReleaseList(VOID);

    CAutoDestList *m_pList;
    WCHAR m_szAppHash[MAX_PATH];
    WCHAR m_szListPath[MAX_PATH];
    PWSTR m_pszAppID;
    PWSTR m_pszAppPath;
    HRESULT m_hrInit;
};
