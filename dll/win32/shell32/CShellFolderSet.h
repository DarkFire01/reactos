/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Updatable set of the items in one folder, used by the breadcrumb bar
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

EXTERN_C const CLSID CLSID_ShellFolderSet;

EXTERN_C
HRESULT
WINAPI
SHCreateItemWithParent(
    _In_opt_ PCIDLIST_ABSOLUTE pidlParent,
    _In_opt_ IShellFolder *psfParent,
    _In_ PCUITEMID_CHILD pidl,
    _In_ REFIID riid,
    _Outptr_ void **ppvItem);

/* Only the enumerator slot is used */
struct DECLSPEC_NOVTABLE IViewFilters : public IUnknown
{
    STDMETHOD(CreateEnumerator)(
        _In_opt_ IBindCtx *pbc,
        _In_ DWORD grfFlags,
        _Outptr_ IUnknown **ppenum) PURE;
};

struct IUpdatableItemSet;

/* What OnSetChanged reports */
#define UISCBF_HASITEMSREADY    0x1
#define UISCBF_ITEMSREADY       0x2

struct DECLSPEC_UUID("C8730A0E-4D28-4813-B152-7F2D156EA908") DECLSPEC_NOVTABLE
IUpdatableItemSetCallback : public IUnknown
{
    STDMETHOD(OnSetChanged)(
        _In_ IUpdatableItemSet *puis,
        _In_ DWORD dwFlags,
        _In_ LPARAM lParam) PURE;
};

struct DECLSPEC_UUID("5C718A08-6CA1-4AE9-A9CC-7CAF009F41B1") DECLSPEC_NOVTABLE
IUpdatableItemSet : public IUnknown
{
    STDMETHOD(Initialize)(
        _In_ DWORD grfFlags,
        _In_ DWORD dwOptions,
        _In_opt_ IUpdatableItemSetCallback *pCallback,
        _In_ PCIDLIST_ABSOLUTE pidlFolder,
        _In_opt_ PCUITEMID_CHILD pidlContext,
        _In_opt_ IViewFilters *pvf,
        _In_ LPARAM lParam) PURE;
    STDMETHOD(SetItemSet)(
        _In_ UINT cItems,
        _In_reads_(cItems) IShellItem **rgpsi) PURE;
    STDMETHOD(GetItemSet)(
        _Out_ UINT *pcItems,
        _Outptr_opt_result_buffer_(*pcItems) IShellItem ***prgpsi) PURE;
    STDMETHOD(HasItems)(
        _Out_ BOOL *pfHasItems) PURE;
    STDMETHOD(RefreshSet)() PURE;
    STDMETHOD(KillEnumeration)() PURE;
};

static const IID IID_IUpdatableItemSet = __uuidof(IUpdatableItemSet);

/* Initialize options */
#define FOLDERSET_SORT          0x1     /* Order items the way the folder compares them */
#define FOLDERSET_ENUMNOW       0x2     /* Start reading right away */

struct FOLDERSET_TASK;

class CShellFolderSet :
    public CComCoClass<CShellFolderSet, &CLSID_ShellFolderSet>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public CObjectWithSiteBase,
    public IUpdatableItemSet,
    public IPersistIDList
{
public:
    CShellFolderSet();
    virtual ~CShellFolderSet();

    // *** IUpdatableItemSet methods ***
    STDMETHODIMP Initialize(DWORD grfFlags, DWORD dwOptions, IUpdatableItemSetCallback *pCallback,
                            PCIDLIST_ABSOLUTE pidlFolder, PCUITEMID_CHILD pidlContext, IViewFilters *pvf,
                            LPARAM lParam) override;
    STDMETHODIMP SetItemSet(UINT cItems, IShellItem **rgpsi) override;
    STDMETHODIMP GetItemSet(UINT *pcItems, IShellItem ***prgpsi) override;
    STDMETHODIMP HasItems(BOOL *pfHasItems) override;
    STDMETHODIMP RefreshSet() override;
    STDMETHODIMP KillEnumeration() override;

    // *** IPersist methods ***
    STDMETHODIMP GetClassID(CLSID *pclsid) override;

    // *** IPersistIDList methods ***
    STDMETHODIMP SetIDList(PCIDLIST_ABSOLUTE pidl) override;
    STDMETHODIMP GetIDList(PIDLIST_ABSOLUTE *ppidl) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_SHELLFOLDERSET)
    DECLARE_NOT_AGGREGATABLE(CShellFolderSet)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CShellFolderSet)
        COM_INTERFACE_ENTRY_IID(IID_IObjectWithSite, IObjectWithSite)
        COM_INTERFACE_ENTRY_IID(IID_IUpdatableItemSet, IUpdatableItemSet)
        COM_INTERFACE_ENTRY_IID(IID_IPersist, IPersistIDList)
        COM_INTERFACE_ENTRY_IID(IID_IPersistIDList, IPersistIDList)
    END_COM_MAP()

private:
    HRESULT StartEnumeration(
        _In_ BOOL fProbe);
    VOID ReleaseItems();
    VOID OnItemsReady(
        _In_ HDPA hdpaChildren);
    VOID OnChange(
        _In_ HANDLE hChange,
        _In_ DWORD dwProcId);

    static LRESULT CALLBACK WndProc(
        _In_ HWND hwnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);

    PIDLIST_ABSOLUTE m_pidl;
    PITEMID_CHILD m_pidlContext;
    IUpdatableItemSetCallback *m_pCallback;
    DWORD m_grfFlags;
    DWORD m_dwOptions;
    IViewFilters *m_pvf;
    LPARAM m_lParam;
    int m_iHasItems;            /* 0 not known yet, 1 has items, 2 is empty */
    HDPA m_hdpaItems;
    BOOL m_fStarted;
    BOOL m_fDone;
    HWND m_hwnd;
    ULONG m_uNotify;
    FOLDERSET_TASK *m_pTask;    /* The enumeration still running, if any */
};
