/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Updatable set of the items in one folder, used by the breadcrumb bar
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

EXTERN_C const CLSID CLSID_ShellFolderSet =
    { 0xad974ae2, 0xe292, 0x4083, { 0xa2, 0x80, 0x03, 0x42, 0xd6, 0x8d, 0xaf, 0x55 } };

#define WM_FOLDERSET_CHANGE     (WM_USER + 1)
#define WM_FOLDERSET_ITEMS      (WM_USER + 2)
#define WM_FOLDERSET_PROBED     (WM_USER + 3)

/* Folder changes that alter what a folders only set shows */
#define FOLDERSET_FOLDER_EVENTS (SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_ATTRIBUTES | SHCNE_UPDATEDIR | SHCNE_RENAMEFOLDER)
#define FOLDERSET_ALL_EVENTS    (FOLDERSET_FOLDER_EVENTS | SHCNE_RENAMEITEM | SHCNE_CREATE | SHCNE_DELETE)

/* Enumeration that runs on its own thread and reports back to the set's window */
struct FOLDERSET_TASK
{
    LONG cRef;
    LONG fCancelled;
    HWND hwndNotify;
    PIDLIST_ABSOLUTE pidlFolder;
    IViewFilters *pvf;
    DWORD grfFlags;
    DWORD dwOptions;
    BOOL fProbe;            /* Only find out whether there is anything */
};

static
VOID
ReleaseTask(
    _In_ FOLDERSET_TASK *pTask)
{
    if (InterlockedDecrement(&pTask->cRef))
        return;
    ILFree(pTask->pidlFolder);
    if (pTask->pvf)
        pTask->pvf->Release();
    LocalFree(pTask);
}

static
int
CALLBACK
CompareChildren(
    _In_ PVOID p1,
    _In_ PVOID p2,
    _In_ LPARAM lParam)
{
    IShellFolder *psf = (IShellFolder *)lParam;
    return (short)HRESULT_CODE(psf->CompareIDs(0, (PCUIDLIST_RELATIVE)p1, (PCUIDLIST_RELATIVE)p2));
}

static
INT
CALLBACK
FreeChildCallback(
    _In_ PVOID p,
    _In_opt_ PVOID pData)
{
    UNREFERENCED_PARAMETER(pData);
    ILFree((LPITEMIDLIST)p);
    return TRUE;
}

static
DWORD
WINAPI
FolderSetEnumThread(
    _In_ PVOID pData)
{
    FOLDERSET_TASK *pTask = (FOLDERSET_TASK *)pData;
    CComPtr<IShellFolder> psf;
    CComPtr<IEnumIDList> penum;
    PITEMID_CHILD pidlChild;
    BOOL fFound = FALSE;
    HDPA hdpa = NULL;
    HRESULT hr;

    if (ILIsEmpty(pTask->pidlFolder))
        hr = SHGetDesktopFolder(&psf);
    else
        hr = SHBindToObject(NULL, pTask->pidlFolder, NULL, IID_PPV_ARG(IShellFolder, &psf));

    if (SUCCEEDED(hr) && !pTask->fProbe)
    {
        hdpa = DPA_Create(10);
        if (!hdpa)
            hr = E_OUTOFMEMORY;
    }

    if (SUCCEEDED(hr))
    {
        DWORD grfFlags = pTask->grfFlags | SHCONTF_ENABLE_ASYNC;

        /* A probe is happy with whatever comes first */
        if (pTask->fProbe)
            grfFlags |= SHCONTF_FASTITEMS;

        if (pTask->pvf)
        {
            CComPtr<IUnknown> punk;

            hr = pTask->pvf->CreateEnumerator(NULL, pTask->grfFlags, &punk);
            if (SUCCEEDED(hr))
                hr = punk->QueryInterface(IID_PPV_ARG(IEnumIDList, &penum));
        }
        else
        {
            hr = psf->EnumObjects(NULL, grfFlags, &penum);
            if (hr == S_FALSE)
                hr = E_FAIL;
        }
    }

    while (SUCCEEDED(hr) && !pTask->fCancelled && penum->Next(1, &pidlChild, NULL) == S_OK)
    {
        fFound = TRUE;
        if (pTask->fProbe)
        {
            ILFree(pidlChild);
            break;
        }
        if (DPA_AppendPtr(hdpa, pidlChild) < 0)
        {
            ILFree(pidlChild);
            hr = E_OUTOFMEMORY;
        }
    }

    if (SUCCEEDED(hr) && hdpa && (pTask->dwOptions & FOLDERSET_SORT) && !pTask->pvf)
        DPA_Sort(hdpa, CompareChildren, (LPARAM)(IShellFolder *)psf);

    /* The window copies what it needs before SendMessage returns */
    if (SUCCEEDED(hr) && !pTask->fCancelled)
    {
        if (pTask->fProbe)
            SendMessageW(pTask->hwndNotify, WM_FOLDERSET_PROBED, 0, fFound);
        else
            SendMessageW(pTask->hwndNotify, WM_FOLDERSET_ITEMS, 0, (LPARAM)hdpa);
    }

    if (hdpa)
        DPA_DestroyCallback(hdpa, FreeChildCallback, NULL);
    penum.Release();
    psf.Release();
    ReleaseTask(pTask);
    return 0;
}

CShellFolderSet::CShellFolderSet() :
    m_pidl(NULL),
    m_pidlContext(NULL),
    m_pCallback(NULL),
    m_grfFlags(0),
    m_dwOptions(0),
    m_pvf(NULL),
    m_lParam(0),
    m_iHasItems(0),
    m_hdpaItems(NULL),
    m_fStarted(FALSE),
    m_fDone(FALSE),
    m_hwnd(NULL),
    m_uNotify(0),
    m_pTask(NULL)
{
}

CShellFolderSet::~CShellFolderSet()
{
    KillEnumeration();
    if (m_uNotify)
        SHChangeNotifyDeregister(m_uNotify);
    if (m_hwnd)
        DestroyWindow(m_hwnd);
    ILFree(m_pidl);
    ILFree(m_pidlContext);
    if (m_pCallback)
        m_pCallback->Release();
    if (m_pvf)
        m_pvf->Release();
    ReleaseItems();
    if (m_hdpaItems)
        DPA_Destroy(m_hdpaItems);
}

VOID
CShellFolderSet::ReleaseItems()
{
    if (!m_hdpaItems)
        return;

    for (int i = 0; i < DPA_GetPtrCount(m_hdpaItems); i++)
        ((IShellItem *)DPA_FastGetPtr(m_hdpaItems, i))->Release();
    DPA_DeleteAllPtrs(m_hdpaItems);
}

STDMETHODIMP
CShellFolderSet::Initialize(
    _In_ DWORD grfFlags,
    _In_ DWORD dwOptions,
    _In_opt_ IUpdatableItemSetCallback *pCallback,
    _In_ PCIDLIST_ABSOLUTE pidlFolder,
    _In_opt_ PCUITEMID_CHILD pidlContext,
    _In_opt_ IViewFilters *pvf,
    _In_ LPARAM lParam)
{
    HRESULT hr;

    m_grfFlags = grfFlags;
    m_dwOptions = dwOptions;
    m_lParam = lParam;
    IUnknown_Set((IUnknown **)&m_pCallback, pCallback);
    IUnknown_Set((IUnknown **)&m_pvf, pvf);

    hr = SetIDList(pidlFolder);
    if (FAILED(hr))
        return hr;

    if (pidlContext)
    {
        m_pidlContext = (PITEMID_CHILD)ILCloneFirst(pidlContext);
        if (!m_pidlContext)
            return E_OUTOFMEMORY;
    }

    if (!m_hdpaItems)
    {
        m_hdpaItems = DPA_Create(10);
        if (!m_hdpaItems)
            return E_OUTOFMEMORY;
    }

    if (!m_hwnd)
        m_hwnd = SHCreateWorkerWindowW(WndProc, HWND_MESSAGE, 0, 0, NULL, (LONG_PTR)this);
    if (!m_hwnd)
        return E_FAIL;

    if (m_dwOptions & FOLDERSET_ENUMNOW)
        StartEnumeration(FALSE);
    return S_OK;
}

HRESULT
CShellFolderSet::StartEnumeration(
    _In_ BOOL fProbe)
{
    FOLDERSET_TASK *pTask;

    if (!m_pidl || !m_hwnd)
        return E_UNEXPECTED;

    if (!fProbe)
        m_fStarted = TRUE;

    /* A newer enumeration replaces the one still running */
    KillEnumeration();

    pTask = (FOLDERSET_TASK *)LocalAlloc(LPTR, sizeof(*pTask));
    if (!pTask)
        return E_OUTOFMEMORY;

    pTask->cRef = 2;
    pTask->hwndNotify = m_hwnd;
    pTask->grfFlags = m_grfFlags;
    pTask->dwOptions = m_dwOptions;
    pTask->fProbe = fProbe;
    pTask->pidlFolder = ILClone(m_pidl);
    pTask->pvf = m_pvf;
    if (pTask->pvf)
        pTask->pvf->AddRef();
    if (!pTask->pidlFolder)
    {
        pTask->cRef = 1;
        ReleaseTask(pTask);
        return E_OUTOFMEMORY;
    }

    if (!SHCreateThread(FolderSetEnumThread, pTask, CTF_COINIT | CTF_INSIST, NULL))
    {
        pTask->cRef = 1;
        ReleaseTask(pTask);
        return E_FAIL;
    }
    m_pTask = pTask;

    /* Changes in a set that reports back are watched so it can refresh */
    if (m_pCallback && !m_pvf && !m_uNotify)
    {
        SHChangeNotifyEntry entry;
        LONG lEvents = (m_grfFlags & SHCONTF_NONFOLDERS) ? FOLDERSET_ALL_EVENTS : FOLDERSET_FOLDER_EVENTS;

        entry.pidl = m_pidl;
        entry.fRecursive = FALSE;
        m_uNotify = SHChangeNotifyRegister(m_hwnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                           lEvents, WM_FOLDERSET_CHANGE, 1, &entry);
    }
    return S_OK;
}

STDMETHODIMP
CShellFolderSet::SetItemSet(
    _In_ UINT cItems,
    _In_reads_(cItems) IShellItem **rgpsi)
{
    ReleaseItems();
    for (UINT i = 0; i < cItems; i++)
    {
        if (DPA_AppendPtr(m_hdpaItems, rgpsi[i]) < 0)
            return E_OUTOFMEMORY;
        rgpsi[i]->AddRef();
    }
    return S_OK;
}

/* S_FALSE while the items are still being read */
STDMETHODIMP
CShellFolderSet::GetItemSet(
    _Out_ UINT *pcItems,
    _Outptr_opt_result_buffer_(*pcItems) IShellItem ***prgpsi)
{
    int cItems = m_hdpaItems ? DPA_GetPtrCount(m_hdpaItems) : 0;
    HRESULT hr = S_OK;

    *pcItems = 0;
    if (prgpsi)
        *prgpsi = NULL;

    if (cItems && prgpsi)
    {
        *prgpsi = (IShellItem **)CoTaskMemAlloc(cItems * sizeof(IShellItem *));
        if (!*prgpsi)
            return E_OUTOFMEMORY;

        for (int i = 0; i < cItems; i++)
        {
            (*prgpsi)[i] = (IShellItem *)DPA_FastGetPtr(m_hdpaItems, i);
            (*prgpsi)[i]->AddRef();
        }
    }
    *pcItems = cItems;

    if (!m_fStarted)
        StartEnumeration(FALSE);
    if (!m_fDone)
        hr = S_FALSE;
    return hr;
}

/* S_FALSE when the answer is only a guess until the set reports back */
STDMETHODIMP
CShellFolderSet::HasItems(
    _Out_ BOOL *pfHasItems)
{
    if (m_iHasItems)
    {
        *pfHasItems = (m_iHasItems == 1);
        return S_OK;
    }

    if (m_fDone)
    {
        *pfHasItems = (m_hdpaItems && DPA_GetPtrCount(m_hdpaItems) > 0);
        return S_OK;
    }

    *pfHasItems = TRUE;
    if (!m_fStarted)
        StartEnumeration(TRUE);
    return S_FALSE;
}

STDMETHODIMP
CShellFolderSet::RefreshSet()
{
    return StartEnumeration(TRUE);
}

STDMETHODIMP
CShellFolderSet::KillEnumeration()
{
    if (m_pTask)
    {
        InterlockedExchange(&m_pTask->fCancelled, TRUE);
        ReleaseTask(m_pTask);
        m_pTask = NULL;
    }
    return S_OK;
}

STDMETHODIMP
CShellFolderSet::GetClassID(
    _Out_ CLSID *pclsid)
{
    *pclsid = CLSID_ShellFolderSet;
    return S_OK;
}

/* The folder can only be set once */
STDMETHODIMP
CShellFolderSet::SetIDList(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    if (m_pidl)
        return E_UNEXPECTED;
    if (!pidl)
        return E_INVALIDARG;

    m_pidl = ILClone(pidl);
    return m_pidl ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CShellFolderSet::GetIDList(
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    *ppidl = NULL;
    if (!m_pidl)
        return E_FAIL;

    *ppidl = ILClone(m_pidl);
    return *ppidl ? S_OK : E_OUTOFMEMORY;
}

VOID
CShellFolderSet::OnItemsReady(
    _In_ HDPA hdpaChildren)
{
    CComPtr<IShellFolder> psf;
    PIDLIST_ABSOLUTE pidlParent = NULL;
    PCIDLIST_ABSOLUTE pidlBind = m_pidl;

    ReleaseItems();
    m_fDone = TRUE;

    /* Filtered sets list the folder's siblings, so their parent is one level up */
    if (m_pvf && m_pidlContext)
    {
        pidlParent = ILClone(m_pidl);
        if (pidlParent)
        {
            ILRemoveLastID(pidlParent);
            pidlBind = pidlParent;
        }
    }

    if (ILIsEmpty(pidlBind) ? SUCCEEDED(SHGetDesktopFolder(&psf))
                            : SUCCEEDED(SHBindToObject(NULL, pidlBind, NULL, IID_PPV_ARG(IShellFolder, &psf))))
    {
        for (int i = 0; hdpaChildren && i < DPA_GetPtrCount(hdpaChildren); i++)
        {
            PCUITEMID_CHILD pidlChild = (PCUITEMID_CHILD)DPA_FastGetPtr(hdpaChildren, i);
            IShellItem *psi;

            if (FAILED(SHCreateItemWithParent(pidlBind, psf, pidlChild, IID_PPV_ARG(IShellItem, &psi))))
                continue;
            if (DPA_AppendPtr(m_hdpaItems, psi) < 0)
            {
                psi->Release();
                break;
            }
        }
    }
    ILFree(pidlParent);

    if (m_pCallback)
        m_pCallback->OnSetChanged(this, UISCBF_ITEMSREADY, m_lParam);
}

VOID
CShellFolderSet::OnChange(
    _In_ HANDLE hChange,
    _In_ DWORD dwProcId)
{
    PIDLIST_ABSOLUTE *ppidl;
    LONG lEvent;
    HANDLE hLock;

    hLock = SHChangeNotification_Lock(hChange, dwProcId, &ppidl, &lEvent);
    if (!hLock)
        return;

    /* Any change counts except a refresh of some other folder */
    if (lEvent != SHCNE_UPDATEDIR || SHCompareIDsFull(SHCIDS_CANONICALONLY, ppidl[0], m_pidl, NULL) == S_OK)
    {
        m_iHasItems = 0;
        m_fStarted = FALSE;
        m_fDone = FALSE;
        StartEnumeration(TRUE);
    }
    SHChangeNotification_Unlock(hLock);
}

LRESULT
CALLBACK
CShellFolderSet::WndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    CShellFolderSet *pThis = (CShellFolderSet *)GetWindowLongPtrW(hwnd, 0);

    switch (uMsg)
    {
        case WM_FOLDERSET_CHANGE:
            if (pThis)
                pThis->OnChange((HANDLE)wParam, (DWORD)lParam);
            return 0;

        case WM_FOLDERSET_ITEMS:
            if (pThis)
                pThis->OnItemsReady((HDPA)lParam);
            return 0;

        case WM_FOLDERSET_PROBED:
            if (pThis)
            {
                pThis->m_iHasItems = lParam ? 1 : 2;
                if (pThis->m_pCallback)
                    pThis->m_pCallback->OnSetChanged(pThis, UISCBF_HASITEMSREADY, pThis->m_lParam);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
