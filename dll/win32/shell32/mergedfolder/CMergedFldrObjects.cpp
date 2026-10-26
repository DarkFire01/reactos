/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Enumerator, drop target, menu, categorizer and view callback of the merged folder
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "mergedfolder.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

static const CLSID CLSID_CDBurn =
    { 0xfbeb8a05, 0xbeee, 0x4442, { 0x80, 0x4e, 0x40, 0x9d, 0x6c, 0x45, 0x15, 0xe9 } };

/* Folder change events the view listens to */
#define MERGEDVIEW_EVENTS       0x0002301F
#define MERGEDVIEW_EVENTS_CD    0x0002307F

HRESULT
MergedFolder_SetDataObjectBlob(
    _In_ IDataObject *pdtobj,
    _In_ LPCWSTR pszFormat,
    _In_reads_bytes_(cb) const void *pv,
    _In_ UINT cb)
{
    FORMATETC fmt = { 0 };
    STGMEDIUM medium = { 0 };
    HGLOBAL hGlobal;
    void *pvData;
    HRESULT hr;

    hGlobal = GlobalAlloc(GPTR, cb);
    if (!hGlobal)
        return E_OUTOFMEMORY;

    pvData = GlobalLock(hGlobal);
    CopyMemory(pvData, pv, cb);
    GlobalUnlock(hGlobal);

    fmt.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(pszFormat);
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = hGlobal;

    hr = pdtobj->SetData(&fmt, &medium, TRUE);
    if (FAILED(hr))
        GlobalFree(hGlobal);
    return hr;
}

/* The shell ID list a data object carries, locked until released */
static CIDA *
GetDataObjectIDA(
    _In_ IDataObject *pdtobj,
    _Out_ STGMEDIUM *pmedium)
{
    FORMATETC fmt = { 0 };

    ZeroMemory(pmedium, sizeof(*pmedium));
    fmt.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLISTW);
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;

    if (FAILED(pdtobj->GetData(&fmt, pmedium)))
        return NULL;

    return (CIDA *)GlobalLock(pmedium->hGlobal);
}

static VOID
ReleaseDataObjectIDA(
    _In_ CIDA *pida,
    _Inout_ STGMEDIUM *pmedium)
{
    GlobalUnlock(pmedium->hGlobal);
    ReleaseStgMedium(pmedium);
}

/* CMergedFldrEnum */

CMergedFldrEnum::CMergedFldrEnum(
    _In_ CMergedShellFolder *pmf,
    _In_ SHCONTF grfFlags,
    _In_ int iCur) :
    m_cRef(1),
    m_pmf(pmf),
    m_grfFlags(grfFlags),
    m_iCur(iCur)
{
    m_pmf->AddRef();
}

CMergedFldrEnum::~CMergedFldrEnum()
{
    m_pmf->Release();
}

STDMETHODIMP
CMergedFldrEnum::QueryInterface(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CMergedFldrEnum, IEnumIDList),
        { NULL, 0 }
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CMergedFldrEnum::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CMergedFldrEnum::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CMergedFldrEnum::Next(
    _In_ ULONG celt,
    _Out_writes_to_(celt, *pceltFetched) PITEMID_CHILD *rgelt,
    _Out_opt_ ULONG *pceltFetched)
{
    int iStart = m_iCur;
    ULONG cFetched = 0;
    HRESULT hr;

    if (!celt || !rgelt || (!pceltFetched && celt > 1))
        return E_INVALIDARG;

    *rgelt = NULL;
    do
    {
        PITEMID_CHILD pidl = NULL;

        if ((ULONG)(m_iCur - iStart) >= celt)
            break;

        /* The objects are read on the first call after the folder was enumerated */
        if (!m_pmf->HasObjects())
            m_pmf->AcquireObjects();

        if (!m_pmf->HasObjects())
        {
            hr = E_OUTOFMEMORY;
        }
        else
        {
            hr = S_FALSE;
            while (m_iCur < m_pmf->GetObjectCount())
            {
                MERGEDOBJECT *pObject = m_pmf->GetObject(m_iCur);
                BOOL fFolder = (pObject->dwAttributes & SFGAO_FOLDER) != 0;

                BOOL fShowHidden = !(pObject->dwAttributes & SFGAO_HIDDEN) ||
                    (m_grfFlags & SHCONTF_INCLUDEHIDDEN);
                BOOL fKind = fFolder ? (m_grfFlags & SHCONTF_FOLDERS) :
                    (m_grfFlags & SHCONTF_NONFOLDERS);

                if ((m_grfFlags & SHCONTF_STORAGE) || (fShowHidden && fKind))
                {
                    pidl = ILCloneFirst(pObject->pidlWrap);
                    hr = pidl ? S_OK : E_OUTOFMEMORY;
                    if (pidl && m_pmf->IsFolder(pidl))
                        m_pmf->AddAllOtherNamespacesIfNeeded(&pidl);
                    break;
                }
                m_iCur++;
            }

            if (hr == S_OK && cFetched < celt)
                rgelt[cFetched++] = pidl;
        }
        m_iCur++;
    } while (hr == S_OK);

    if (pceltFetched)
        *pceltFetched = cFetched;
    return (celt != cFetched) ? S_FALSE : S_OK;
}

STDMETHODIMP
CMergedFldrEnum::Skip(
    _In_ ULONG celt)
{
    m_iCur += celt;
    return S_OK;
}

STDMETHODIMP
CMergedFldrEnum::Reset()
{
    m_iCur = 0;
    return S_OK;
}

STDMETHODIMP
CMergedFldrEnum::Clone(
    _Outptr_ IEnumIDList **ppenum)
{
    *ppenum = new CMergedFldrEnum(m_pmf, m_grfFlags, m_iCur);
    return *ppenum ? S_OK : E_OUTOFMEMORY;
}

/* CMergedFldrDropTarget */

CMergedFldrDropTarget::CMergedFldrDropTarget(
    _In_ CMergedShellFolder *pmf,
    _In_opt_ HWND hwnd) :
    m_cRef(1),
    m_pmf(pmf),
    m_pdt(NULL),
    m_punkSite(NULL),
    m_pdtobj(NULL),
    m_hwnd(hwnd),
    m_fCommon(FALSE),
    m_pidlSource(NULL),
    m_grfKeyState(0),
    m_dwEffect(0)
{
    m_pmf->AddRef();
}

CMergedFldrDropTarget::~CMergedFldrDropTarget()
{
    if (m_pdt)
        m_pdt->Release();
    if (m_punkSite)
        m_punkSite->Release();
    if (m_pdtobj)
        m_pdtobj->Release();
    ILFree(m_pidlSource);
    m_pmf->Release();
}

STDMETHODIMP
CMergedFldrDropTarget::QueryInterface(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CMergedFldrDropTarget, IDropTarget),
        QITABENT(CMergedFldrDropTarget, IObjectWithSite),
        { NULL, 0 }
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CMergedFldrDropTarget::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CMergedFldrDropTarget::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

BOOL
CMergedFldrDropTarget::IsCommonIDList(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    static const int s_CommonFolders[] =
    {
        CSIDL_COMMON_STARTMENU, CSIDL_COMMON_PROGRAMS, CSIDL_COMMON_DESKTOPDIRECTORY
    };
    UINT i;

    for (i = 0; i < _countof(s_CommonFolders); i++)
    {
        PIDLIST_ABSOLUTE pidlCommon;
        BOOL fCommon;

        if (SHGetFolderLocation(NULL, s_CommonFolders[i], NULL, 0, &pidlCommon) != S_OK)
            continue;

        fCommon = ILIsParent(pidlCommon, pidl, FALSE);
        ILFree(pidlCommon);
        if (fCommon)
            return TRUE;
    }
    return FALSE;
}

/* Dragged merged children are all users items when none of their parts is the user's */
BOOL
CMergedFldrDropTarget::IsCommonWrappedIDList(
    _In_ IDataObject *pdtobj,
    _In_ IAugmentedMergedFolder *pamf)
{
    IShellFolder *apsf[4];
    PIDLIST_ABSOLUTE apidlFolder[4];
    PITEMID_CHILD apidlItem[4];
    BOOL fCommon = FALSE;
    BOOL fUser = FALSE;
    STGMEDIUM medium;
    LONG cFetched = 0;
    CIDA *pida;
    LONG i;

    pida = GetDataObjectIDA(pdtobj, &medium);
    if (!pida)
        return FALSE;

    if (pida->cidl &&
        SUCCEEDED(pamf->UnWrapIDList((PCUITEMID_CHILD)((BYTE *)pida + pida->aoffset[1]),
                                     _countof(apsf), apsf, apidlFolder, apidlItem, &cFetched)))
    {
        for (i = 0; i < cFetched; i++)
        {
            PIDLIST_ABSOLUTE pidlFull = ILCombine(apidlFolder[i], apidlItem[i]);

            if (pidlFull)
            {
                if (IsCommonIDList(pidlFull))
                    fCommon = TRUE;
                else
                    fUser = TRUE;
                ILFree(pidlFull);
            }

            apsf[i]->Release();
            ILFree(apidlFolder[i]);
            ILFree(apidlItem[i]);
        }
    }

    ReleaseDataObjectIDA(pida, &medium);
    return fCommon && !fUser;
}

/* Tells the target folders which drop effect each of the merged locations wants */
HRESULT
CMergedFldrDropTarget::SetDropEffectFolders()
{
    MERGEDDROPFOLDERLIST *pList;
    int cFolders = 0;
    int iSlot;
    UINT cb;
    HRESULT hr = S_OK;
    int i;

    for (i = 0; i < m_pmf->GetNamespaceCount(); i++)
    {
        CMergedFldrNamespace *pns;

        m_pmf->GetNamespace(i, &pns);
        pns->EnsureReadPropertyBag();
        if (pns->m_dwDropEffect)
            cFolders++;
    }

    if (cFolders <= 0 && !m_pmf->GetDefaultDropEffect())
        return S_OK;

    cb = FIELD_OFFSET(MERGEDDROPFOLDERLIST, aFolders) + cFolders * sizeof(MERGEDDROPFOLDER);
    if (cFolders <= 0)
        cb = FIELD_OFFSET(MERGEDDROPFOLDERLIST, aFolders);

    pList = (MERGEDDROPFOLDERLIST *)LocalAlloc(LPTR, cb);
    if (!pList)
        return E_OUTOFMEMORY;

    pList->dwDefaultDropEffect = m_pmf->GetDefaultDropEffect();
    pList->cFolders = cFolders;

    /* The list is filled from its end */
    iSlot = cFolders - 1;
    for (i = 0; iSlot >= 0 && SUCCEEDED(hr); i++)
    {
        CMergedFldrNamespace *pns;

        m_pmf->GetNamespace(i, &pns);
        pns->EnsureReadPropertyBag();
        if (!pns->m_dwDropEffect)
            continue;

        if (!pns->m_szRootPath[0])
        {
            SHGetPathFromIDListEx(pns->m_pidl, pns->m_szRootPath, _countof(pns->m_szRootPath),
                                  GPFIDL_UNCPRINTER);
        }

        pList->aFolders[iSlot].dwDropEffect = pns->m_dwDropEffect;
        hr = StringCchCopyW(pList->aFolders[iSlot].wszPath,
                            _countof(pList->aFolders[iSlot].wszPath), pns->m_szRootPath);
        iSlot -= 1;
    }

    if (SUCCEEDED(hr))
        hr = MergedFolder_SetDataObjectBlob(m_pdtobj, CFSTR_MERGEDDROPFOLDERLIST, pList, cb);

    LocalFree(pList);
    return hr;
}

STDMETHODIMP
CMergedFldrDropTarget::DragEnter(
    _In_ IDataObject *pdtobj,
    _In_ DWORD grfKeyState,
    _In_ POINTL pt,
    _Inout_ DWORD *pdwEffect)
{
    STGMEDIUM medium;
    CIDA *pida;

    IUnknown_Set((IUnknown **)&m_pdtobj, pdtobj);
    SetDropEffectFolders();

    /* Items that come from the all users locations go back there */
    pida = GetDataObjectIDA(pdtobj, &medium);
    if (pida)
    {
        ILFree(m_pidlSource);
        m_pidlSource = ILClone((PCIDLIST_ABSOLUTE)((BYTE *)pida + pida->aoffset[0]));
        ReleaseDataObjectIDA(pida, &medium);
    }

    if (m_pidlSource)
    {
        IAugmentedMergedFolder *pamf;

        m_fCommon = IsCommonIDList(m_pidlSource);
        if (!m_fCommon &&
            SUCCEEDED(SHBindToObject(NULL, m_pidlSource, NULL,
                                     IID_PPV_ARG(IAugmentedMergedFolder, &pamf))))
        {
            m_fCommon = IsCommonWrappedIDList(pdtobj, pamf);
            pamf->Release();
        }
    }

    if (m_pmf->IsCDBurn())
    {
        IShellExtInit *psei;

        if (SUCCEEDED(CoCreateInstance(CLSID_CDBurn, NULL, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARG(IShellExtInit, &psei))))
        {
            if (SUCCEEDED(psei->Initialize(m_pmf->GetPidl(), NULL, NULL)) &&
                SUCCEEDED(psei->QueryInterface(IID_PPV_ARG(IDropTarget, &m_pdt))))
            {
                IUnknown_SetSite(m_pdt, m_punkSite);
                m_pdt->DragEnter(pdtobj, grfKeyState, pt, pdwEffect);
            }
            psei->Release();
        }
    }
    else
    {
        DWORD dwFlags = m_fCommon ? MFNS_COMMON : MFNS_VIEW;
        CMergedFldrNamespace *pns;
        IShellFolder *psf;

        if (SUCCEEDED(m_pmf->FindNamespace(dwFlags, dwFlags, &pns)) &&
            SUCCEEDED(SHBindToObject(NULL, pns->m_pidl, NULL, IID_PPV_ARG(IShellFolder, &psf))))
        {
            if (SUCCEEDED(psf->CreateViewObject(m_hwnd, IID_PPV_ARG(IDropTarget, &m_pdt))))
            {
                IUnknown_SetSite(m_pdt, m_punkSite);
                m_pdt->DragEnter(pdtobj, grfKeyState, pt, pdwEffect);
            }
            psf->Release();
        }
    }

    m_grfKeyState = grfKeyState;
    m_dwEffect = *pdwEffect;
    return S_OK;
}

STDMETHODIMP
CMergedFldrDropTarget::DragOver(
    _In_ DWORD grfKeyState,
    _In_ POINTL pt,
    _Inout_ DWORD *pdwEffect)
{
    m_grfKeyState = grfKeyState;
    return m_pdt ? m_pdt->DragOver(grfKeyState, pt, pdwEffect) : S_OK;
}

STDMETHODIMP
CMergedFldrDropTarget::DragLeave()
{
    if (m_pdt)
    {
        m_pdt->DragLeave();
        IUnknown_SetSite(m_pdt, NULL);
        m_pdt->Release();
        m_pdt = NULL;
    }

    if (m_pdtobj)
    {
        MERGEDDROPFOLDERLIST empty = { 0 };

        MergedFolder_SetDataObjectBlob(m_pdtobj, CFSTR_MERGEDDROPFOLDERLIST, &empty,
                                       FIELD_OFFSET(MERGEDDROPFOLDERLIST, aFolders));
        m_pdtobj->Release();
        m_pdtobj = NULL;
    }

    m_fCommon = FALSE;
    ILFree(m_pidlSource);
    m_pidlSource = NULL;
    return S_OK;
}

STDMETHODIMP
CMergedFldrDropTarget::Drop(
    _In_ IDataObject *pdtobj,
    _In_ DWORD grfKeyState,
    _In_ POINTL pt,
    _Inout_ DWORD *pdwEffect)
{
    HRESULT hr = m_pdt ? m_pdt->Drop(pdtobj, grfKeyState, pt, pdwEffect) : E_FAIL;

    DragLeave();
    return hr;
}

STDMETHODIMP
CMergedFldrDropTarget::SetSite(
    _In_opt_ IUnknown *punkSite)
{
    IUnknown_Set(&m_punkSite, punkSite);
    IUnknown_SetSite(m_pdt, punkSite);
    return S_OK;
}

STDMETHODIMP
CMergedFldrDropTarget::GetSite(
    _In_ REFIID riid,
    _Outptr_ void **ppvSite)
{
    *ppvSite = NULL;
    return m_punkSite ? m_punkSite->QueryInterface(riid, ppvSite) : E_FAIL;
}

/* CMergedFldrContextMenu */

/* Commands of the merged folder menu */
#define MERGEDCMD_OPEN          1
#define MERGEDCMD_EXPLORE       2
#define MERGEDCMD_OPENCOMMON    3
#define MERGEDCMD_EXPLORECOMMON 4
#define MERGEDCMD_DELETE        5
#define MERGEDCMD_RENAME        6
#define MERGEDCMD_PROPERTIES    7

struct MERGEDVERB
{
    LPCWSTR pszVerbW;
    LPCSTR pszVerbA;
    UINT idCmd;
};

static const MERGEDVERB c_MergedVerbs[] =
{
    { L"delete", "delete", MERGEDCMD_DELETE },
    { L"rename", "rename", MERGEDCMD_RENAME },
    { L"properties", "properties", MERGEDCMD_PROPERTIES },
};

/* Menu for a folder that is in both the user's and the all users locations */
class CMergedFldrContextMenu :
    public IContextMenu3,
    public IObjectWithSite
{
public:
    CMergedFldrContextMenu(
        _In_opt_ HWND hwnd,
        _In_ IContextMenu *pcmCommon,
        _In_ IContextMenu *pcmUser) :
        m_cRef(1),
        m_pcmCommon(pcmCommon),
        m_pcmUser(pcmUser),
        m_hwnd(hwnd)
    {
        m_pcmCommon->AddRef();
        m_pcmUser->AddRef();
    }

    // IUnknown
    STDMETHODIMP
    QueryInterface(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENTMULTI(CMergedFldrContextMenu, IContextMenu, IContextMenu3),
            QITABENTMULTI(CMergedFldrContextMenu, IContextMenu2, IContextMenu3),
            QITABENT(CMergedFldrContextMenu, IContextMenu3),
            QITABENT(CMergedFldrContextMenu, IObjectWithSite),
            { NULL, 0 }
        };

        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    // IContextMenu
    STDMETHODIMP
    QueryContextMenu(
        _In_ HMENU hmenu,
        _In_ UINT indexMenu,
        _In_ UINT idCmdFirst,
        _In_ UINT idCmdLast,
        _In_ UINT uFlags) override;

    STDMETHODIMP
    InvokeCommand(
        _In_ LPCMINVOKECOMMANDINFO pici) override;

    STDMETHODIMP
    GetCommandString(
        _In_ UINT_PTR idCmd,
        _In_ UINT uType,
        _Reserved_ UINT *pReserved,
        _Out_writes_bytes_(cchMax) LPSTR pszName,
        _In_ UINT cchMax) override;

    // IContextMenu2
    STDMETHODIMP
    HandleMenuMsg(
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam) override;

    // IContextMenu3
    STDMETHODIMP
    HandleMenuMsg2(
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam,
        _Out_opt_ LRESULT *plResult) override;

    // IObjectWithSite
    STDMETHODIMP
    SetSite(
        _In_opt_ IUnknown *punkSite) override
    {
        IUnknown_SetSite(m_pcmCommon, punkSite);
        IUnknown_SetSite(m_pcmUser, punkSite);
        return S_OK;
    }

    STDMETHODIMP
    GetSite(
        _In_ REFIID riid,
        _Outptr_ void **ppvSite) override
    {
        return IUnknown_GetSite(m_pcmUser, riid, ppvSite);
    }

private:
    ~CMergedFldrContextMenu()
    {
        m_pcmCommon->Release();
        m_pcmUser->Release();
    }

    LONG m_cRef;
    IContextMenu *m_pcmCommon;
    IContextMenu *m_pcmUser;
    HWND m_hwnd;
};

STDMETHODIMP
CMergedFldrContextMenu::QueryContextMenu(
    _In_ HMENU hmenu,
    _In_ UINT indexMenu,
    _In_ UINT idCmdFirst,
    _In_ UINT idCmdLast,
    _In_ UINT uFlags)
{
    HMENU hmenuRes;
    HMENU hmenuPopup;
    UINT idMax;

    if (!hmenu)
        return E_INVALIDARG;

    hmenuRes = LoadMenuW(shell32_hInstance, MAKEINTRESOURCEW(IDM_MERGEDFOLDER));
    if (!hmenuRes)
        return E_OUTOFMEMORY;

    hmenuPopup = GetSubMenu(hmenuRes, 0);
    idMax = hmenuPopup ? Shell_MergeMenus(hmenu, hmenuPopup, indexMenu, idCmdFirst, idCmdLast,
                                          MM_ADDSEPARATOR) : idCmdFirst;

    /* Without verbs only open and explore stay */
    if (uFlags & CMF_VERBSONLY)
    {
        DeleteMenu(hmenu, idCmdFirst + MERGEDCMD_DELETE, MF_BYCOMMAND);
        DeleteMenu(hmenu, idCmdFirst + MERGEDCMD_RENAME, MF_BYCOMMAND);
        DeleteMenu(hmenu, idCmdFirst + MERGEDCMD_PROPERTIES, MF_BYCOMMAND);
    }

    DestroyMenu(hmenuRes);
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(idMax - idCmdFirst));
}

STDMETHODIMP
CMergedFldrContextMenu::InvokeCommand(
    _In_ LPCMINVOKECOMMANDINFO pici)
{
    CMINVOKECOMMANDINFOEX ici = { 0 };
    IContextMenu *pcm;
    LPCSTR pszVerb;
    UINT idCmd = 0;
    UINT i;

    CopyMemory(&ici, pici, min(pici->cbSize, sizeof(ici)));
    ici.cbSize = sizeof(ici);

    /* Verbs come as strings or as offsets from the first command */
    if (IS_INTRESOURCE(ici.lpVerb))
    {
        idCmd = LOWORD(ici.lpVerb);
    }
    else
    {
        BOOL fUnicode = (ici.fMask & CMIC_MASK_UNICODE) && ici.lpVerbW &&
            !IS_INTRESOURCE(ici.lpVerbW);

        for (i = 0; i < _countof(c_MergedVerbs); i++)
        {
            if (fUnicode ? !lstrcmpiW(ici.lpVerbW, c_MergedVerbs[i].pszVerbW) :
                !lstrcmpiA(ici.lpVerb, c_MergedVerbs[i].pszVerbA))
            {
                idCmd = c_MergedVerbs[i].idCmd;
                break;
            }
        }
        if (i == _countof(c_MergedVerbs))
            return E_INVALIDARG;
    }
    ici.fMask &= ~CMIC_MASK_UNICODE;

    switch (idCmd)
    {
        case MERGEDCMD_OPEN:
        case MERGEDCMD_EXPLORE:
            pcm = m_pcmUser;
            pszVerb = (idCmd == MERGEDCMD_EXPLORE) ? "explore" : "open";
            break;

        case MERGEDCMD_OPENCOMMON:
        case MERGEDCMD_EXPLORECOMMON:
            pcm = m_pcmCommon;
            pszVerb = (idCmd == MERGEDCMD_EXPLORECOMMON) ? "explore" : "open";
            break;

        case MERGEDCMD_DELETE:
            pcm = m_pcmUser;
            pszVerb = "delete";
            break;

        case MERGEDCMD_RENAME:
            pcm = m_pcmUser;
            pszVerb = "rename";
            break;

        case MERGEDCMD_PROPERTIES:
            pcm = m_pcmUser;
            pszVerb = "properties";
            break;

        default:
            return E_INVALIDARG;
    }

    return SHInvokeCommandOnContextMenu(ici.hwnd, NULL, pcm, ici.fMask, pszVerb);
}

STDMETHODIMP
CMergedFldrContextMenu::GetCommandString(
    _In_ UINT_PTR idCmd,
    _In_ UINT uType,
    _Reserved_ UINT *pReserved,
    _Out_writes_bytes_(cchMax) LPSTR pszName,
    _In_ UINT cchMax)
{
    UINT i;

    if (uType == GCS_VERBA || uType == GCS_VERBW)
    {
        for (i = 0; i < _countof(c_MergedVerbs); i++)
        {
            if (c_MergedVerbs[i].idCmd != idCmd)
                continue;

            if (uType == GCS_VERBW)
                return StringCchCopyW((LPWSTR)pszName, cchMax, c_MergedVerbs[i].pszVerbW);
            return StringCchCopyA(pszName, cchMax, c_MergedVerbs[i].pszVerbA);
        }
    }

    return m_pcmUser->GetCommandString(idCmd, uType, pReserved, pszName, cchMax);
}

STDMETHODIMP
CMergedFldrContextMenu::HandleMenuMsg(
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    IContextMenu2 *pcm2;
    HRESULT hr;

    hr = m_pcmUser->QueryInterface(IID_PPV_ARG(IContextMenu2, &pcm2));
    if (SUCCEEDED(hr))
    {
        hr = pcm2->HandleMenuMsg(uMsg, wParam, lParam);
        pcm2->Release();
    }
    return hr;
}

STDMETHODIMP
CMergedFldrContextMenu::HandleMenuMsg2(
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _Out_opt_ LRESULT *plResult)
{
    IContextMenu3 *pcm3;
    HRESULT hr;

    hr = m_pcmUser->QueryInterface(IID_PPV_ARG(IContextMenu3, &pcm3));
    if (SUCCEEDED(hr))
    {
        hr = pcm3->HandleMenuMsg2(uMsg, wParam, lParam, plResult);
        pcm3->Release();
    }
    return hr;
}

HRESULT
CMergedFldrContextMenu_CreateInstance(
    _In_opt_ HWND hwnd,
    _In_ IContextMenu *pcmCommon,
    _In_ IContextMenu *pcmUser,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CMergedFldrContextMenu *pcm = new CMergedFldrContextMenu(hwnd, pcmCommon, pcmUser);
    HRESULT hr;

    *ppv = NULL;
    if (!pcm)
        return E_OUTOFMEMORY;

    hr = pcm->QueryInterface(riid, ppv);
    pcm->Release();
    return hr;
}

/* CMergedCategorizer */

CMergedCategorizer::CMergedCategorizer() :
    m_cRef(1),
    m_pmf(NULL)
{
}

CMergedCategorizer::~CMergedCategorizer()
{
    if (m_pmf)
        m_pmf->Release();
}

STDMETHODIMP
CMergedCategorizer::QueryInterface(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CMergedCategorizer, ICategorizer),
        QITABENT(CMergedCategorizer, IShellExtInit),
        { NULL, 0 }
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CMergedCategorizer::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CMergedCategorizer::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CMergedCategorizer::Initialize(
    _In_opt_ PCIDLIST_ABSOLUTE pidlFolder,
    _In_opt_ IDataObject *pdtobj,
    _In_opt_ HKEY hkeyProgID)
{
    if (m_pmf)
    {
        m_pmf->Release();
        m_pmf = NULL;
    }

    /* The merged folder class ID hands out the folder object itself */
    return SHBindToObject(NULL, pidlFolder, NULL, CLSID_MergedFolder, (void **)&m_pmf);
}

STDMETHODIMP
CMergedCategorizer::GetDescription(
    _Out_writes_(cch) LPWSTR pszDesc,
    _In_ UINT cch)
{
    LoadStringW(shell32_hInstance, IDS_SHV_COLUMN_LOCATION, pszDesc, cch);
    return S_OK;
}

STDMETHODIMP
CMergedCategorizer::GetCategory(
    _In_ UINT cidl,
    _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
    _Out_writes_(cidl) DWORD *rgCategoryIds)
{
    UINT i;

    if (!m_pmf)
        return E_ACCESSDENIED;

    /* Items group under the namespace that answers for them */
    for (i = 0; i < cidl; i++)
    {
        CMergedFldrNamespace *pns;
        UINT iNamespace;
        WORD wFlags;
        UINT iEntry;

        rgCategoryIds[i] = (DWORD)-1;
        for (iEntry = 0;
             SUCCEEDED(m_pmf->GetSubPidl(apidl[i], iEntry, &iNamespace, &wFlags, NULL, &pns));
             iEntry++)
        {
            if (wFlags & MERGEDENTRY_IMPLICIT)
                continue;

            rgCategoryIds[i] = iNamespace;
            if (pns->m_dwFlags & (MFNS_ATTRIBUTES | MFNS_DISPLAY))
                break;
        }
    }
    return S_OK;
}

STDMETHODIMP
CMergedCategorizer::GetCategoryInfo(
    _In_ DWORD dwCategoryId,
    _Out_ MERGEDCATEGORY_INFO *pci)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    hr = ((int)dwCategoryId < 0) ? E_INVALIDARG : m_pmf->GetNamespace(dwCategoryId, &pns);
    if (FAILED(hr))
        return hr;

    pns->EnsureReadPropertyBag();
    StringCchCopyW(pci->wszName, _countof(pci->wszName), pns->m_szLocation);
    pci->cif = MERGEDCATINFO_NORMAL;
    return S_OK;
}

STDMETHODIMP
CMergedCategorizer::CompareCategory(
    _In_ DWORD csfFlags,
    _In_ DWORD dwCategoryId1,
    _In_ DWORD dwCategoryId2)
{
    if (dwCategoryId1 == dwCategoryId2)
        return S_OK;
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)((dwCategoryId2 < dwCategoryId1) ? -1 : 1));
}

/* View callback of a merged folder shown on its own */
class CMergedFolderViewCB :
    public IShellFolderViewCB,
    public IObjectWithSite
{
public:
    CMergedFolderViewCB(
        _In_ CMergedShellFolder *pmf) :
        m_cRef(1),
        m_pmf(pmf),
        m_punkSite(NULL)
    {
        m_pmf->AddRef();
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CMergedFolderViewCB, IShellFolderViewCB),
            QITABENT(CMergedFolderViewCB, IObjectWithSite),
            { NULL, 0 }
        };

        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    STDMETHODIMP
    MessageSFVCB(
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam) override
    {
        /* The view watches the merged folder itself */
        if (uMsg == SFVM_GETNOTIFY)
        {
            *(PCIDLIST_ABSOLUTE *)wParam = m_pmf->GetPidl();
            *(LONG *)lParam = m_pmf->IsCDBurn() ? MERGEDVIEW_EVENTS_CD : MERGEDVIEW_EVENTS;
            return S_OK;
        }
        return E_NOTIMPL;
    }

    STDMETHODIMP
    SetSite(
        _In_opt_ IUnknown *punkSite) override
    {
        IUnknown_Set(&m_punkSite, punkSite);
        return S_OK;
    }

    STDMETHODIMP
    GetSite(
        _In_ REFIID riid,
        _Outptr_ void **ppvSite) override
    {
        *ppvSite = NULL;
        return m_punkSite ? m_punkSite->QueryInterface(riid, ppvSite) : E_FAIL;
    }

private:
    ~CMergedFolderViewCB()
    {
        if (m_punkSite)
            m_punkSite->Release();
        m_pmf->Release();
    }

    LONG m_cRef;
    CMergedShellFolder *m_pmf;
    IUnknown *m_punkSite;
};

HRESULT
CMergedFolderViewCB_CreateInstance(
    _In_ CMergedShellFolder *pmf,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CMergedFolderViewCB *pcb = new CMergedFolderViewCB(pmf);
    HRESULT hr;

    *ppv = NULL;
    if (!pcb)
        return E_OUTOFMEMORY;

    hr = pcb->QueryInterface(riid, ppv);
    pcb->Release();
    return hr;
}
