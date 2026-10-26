/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shell folder that merges several namespaces into one view
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "../hiddenid.h"
#include "mergedfolder.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

EXTERN_C const CLSID CLSID_ProgramsFolderAndFastItems =
    { 0x865e5e76, 0xad83, 0x4dca, { 0xa1, 0x09, 0x50, 0xdc, 0x21, 0x13, 0xce, 0x9a } };
EXTERN_C const CLSID CLSID_ProgramsFolder =
    { 0x7be9d83c, 0xa729, 0x4d97, { 0xb5, 0xa7, 0x1b, 0x73, 0x13, 0xc3, 0x9e, 0x0a } };
static const CLSID CLSID_CDBurnFolder =
    { 0x00eebf57, 0x477d, 0x4084, { 0x99, 0x21, 0x7a, 0xb3, 0xc2, 0xc9, 0x45, 0x9d } };
static const GUID POLID_NoStartMenuSubFolders =
    { 0x78627e11, 0xec80, 0x49a3, { 0xb3, 0xb7, 0x6a, 0xbe, 0x73, 0x96, 0x93, 0xfc } };

/* FMTID_ShellDetails properties */
static const SHCOLUMNID PKEY_MergedFindData =
    { { 0x28636aa6, 0x953d, 0x11d2, { 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0 } }, 0 };
static const SHCOLUMNID PKEY_MergedWhichFolder =
    { { 0x28636aa6, 0x953d, 0x11d2, { 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0 } }, 3 };
static const SHCOLUMNID PKEY_MergedParsingBindContext =
    { { 0xdfb9a04d, 0x362f, 0x4ca3, { 0xb3, 0x0b, 0x02, 0x54, 0xb1, 0x7b, 0x5b, 0x84 } }, 100 };

/*
 * A merged child ID is one item that carries one sub ID per namespace holding it. Version 4
 * entries are a flags WORD, the namespace index WORD, the sub ID and a terminating WORD.
 */
#include <pshpack1.h>
typedef struct _MERGEDWRAP
{
    WORD cb;
    WORD wFlags;
    DWORD dwSignature;
    DWORD dwVersion;
    DWORD cEntries;
} MERGEDWRAP, *PMERGEDWRAP;
#include <poppack.h>

#define MERGEDWRAP_SIGNATURE    0x4D677241
#define MERGEDWRAP_VERSION      4
#define MERGEDWRAP_SIMPLE       0x0001  /* Made from bind data, the item may not exist */

static const MERGEDWRAP *
GetWrap(
    _In_opt_ PCUITEMID_CHILD pidl)
{
    const MERGEDWRAP *pWrap = (const MERGEDWRAP *)pidl;

    if (!pWrap || pWrap->cb < sizeof(*pWrap) || pWrap->dwSignature != MERGEDWRAP_SIGNATURE ||
        pWrap->dwVersion < 3)
    {
        return NULL;
    }
    return pWrap;
}

static UINT
GetWrapEntryHeader(
    _In_ const MERGEDWRAP *pWrap)
{
    return (pWrap->dwVersion >= MERGEDWRAP_VERSION) ? 3 * sizeof(WORD) : 2 * sizeof(WORD);
}

static BOOL
IsSimpleWrap(
    _In_ PCUITEMID_CHILD pidl)
{
    const MERGEDWRAP *pWrap = GetWrap(pidl);

    return pWrap && (pWrap->wFlags & MERGEDWRAP_SIMPLE);
}

static HRESULT
CloneFirstID(
    _In_opt_ PCUIDLIST_RELATIVE pidl,
    _Outptr_ PITEMID_CHILD *ppidl)
{
    *ppidl = pidl ? ILCloneFirst(pidl) : NULL;
    return *ppidl ? S_OK : E_OUTOFMEMORY;
}

static int
CompareArbitraryPidls(
    _In_ PCUITEMID_CHILD pidl1,
    _In_ PCUITEMID_CHILD pidl2)
{
    UINT cb1 = ILGetSize(pidl1);
    UINT cb2 = ILGetSize(pidl2);

    if (cb1 != cb2)
        return (cb1 < cb2) ? 1 : -1;
    return memcmp(pidl1, pidl2, cb1);
}

static HRESULT
ParseNameInFolder(
    _In_ IShellFolder *psf,
    _In_opt_ HWND hwnd,
    _In_opt_ IBindCtx *pbc,
    _In_ LPCWSTR pszName,
    _Outptr_ PITEMID_CHILD *ppidl)
{
    *ppidl = NULL;
    return psf->ParseDisplayName(hwnd, pbc, (LPWSTR)pszName, NULL, (PIDLIST_RELATIVE *)ppidl, NULL);
}

static BOOL
IsSameObject(
    _In_ IUnknown *punk1,
    _In_ IUnknown *punk2)
{
    IUnknown *punkIdentity1;
    IUnknown *punkIdentity2;
    BOOL fSame = FALSE;

    if (SUCCEEDED(punk1->QueryInterface(IID_PPV_ARG(IUnknown, &punkIdentity1))))
    {
        if (SUCCEEDED(punk2->QueryInterface(IID_PPV_ARG(IUnknown, &punkIdentity2))))
        {
            fSame = (punkIdentity1 == punkIdentity2);
            punkIdentity2->Release();
        }
        punkIdentity1->Release();
    }
    return fSame;
}

static BOOL
IsSelfID(
    _In_ UINT cidl,
    _In_opt_ PCUITEMID_CHILD_ARRAY apidl)
{
    return !cidl || (cidl == 1 && (!apidl || !apidl[0] || !apidl[0]->mkid.cb));
}

static HRESULT
CreateBindCtxWithMode(
    _In_ DWORD grfMode,
    _Outptr_ IBindCtx **ppbc)
{
    HRESULT hr = CreateBindCtx(0, ppbc);
    BIND_OPTS bo = { sizeof(bo) };

    if (FAILED(hr))
        return hr;

    bo.grfMode = grfMode;
    hr = (*ppbc)->SetBindOptions(&bo);
    if (FAILED(hr))
    {
        (*ppbc)->Release();
        *ppbc = NULL;
    }
    return hr;
}

/* A bind context that keeps binding from crossing into junction folders */
static HRESULT
CreateSkipBindCtx(
    _Outptr_ IBindCtx **ppbc)
{
    HRESULT hr = CreateBindCtx(0, ppbc);
    BIND_OPTS bo = { sizeof(bo) };

    if (FAILED(hr))
        return hr;

    bo.grfFlags = OLECONTF_LINKS;
    (*ppbc)->SetBindOptions(&bo);
    return S_OK;
}

static HRESULT
MapWin32ErrorToSTG(
    _In_ HRESULT hr)
{
    switch (hr)
    {
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
            return STG_E_FILENOTFOUND;
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
            return STG_E_PATHNOTFOUND;
        case HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS):
        case HRESULT_FROM_WIN32(ERROR_FILE_EXISTS):
            return STG_E_FILEALREADYEXISTS;
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED):
            return STG_E_ACCESSDENIED;
        case HRESULT_FROM_WIN32(ERROR_INVALID_NAME):
            return STG_E_INVALIDNAME;
        case HRESULT_FROM_WIN32(ERROR_DISK_FULL):
            return STG_E_MEDIUMFULL;
    }
    return hr;
}

static VOID
SetWrapper(
    _In_ IShellFolder *psf,
    _In_opt_ IShellFolder2 *psfWrapper)
{
    ISetWrapperFolder *pswf;

    if (SUCCEEDED(psf->QueryInterface(IID_ISetWrapperFolder, (void **)&pswf)))
    {
        pswf->SetWrapperFolderWeakReference(psfWrapper);
        pswf->Release();
    }
}

HRESULT
MergedFolder_GetFolderTargetIDList(
    _In_opt_ IUnknown *punk,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    IPersistFolder3 *ppf3;
    HRESULT hr = E_NOINTERFACE;

    *ppidl = NULL;
    if (!punk)
        return E_NOINTERFACE;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IPersistFolder3, &ppf3))))
    {
        PERSIST_FOLDER_TARGET_INFO pfti = { 0 };

        if (SUCCEEDED(ppf3->GetFolderTargetInfo(&pfti)))
        {
            if (pfti.pidlTargetFolder)
            {
                *ppidl = pfti.pidlTargetFolder;
                hr = S_OK;
            }
            else if (pfti.csidl >= 0 && (pfti.csidl & CSIDL_FLAG_DONT_VERIFY))
            {
                hr = SHGetFolderLocation(NULL, pfti.csidl & ~CSIDL_FLAG_MASK, NULL, 0, ppidl);
            }
            else
            {
                hr = SHILCreateFromPathW(pfti.szTargetParsingName, ppidl, NULL);
            }
        }
        ppf3->Release();
    }

    if (FAILED(hr))
        hr = SHGetIDListFromObject(punk, ppidl);
    return hr;
}

/* Bind context that parses a name the way the given child was made */
static HRESULT
GetParsingBindCtx(
    _In_ IShellFolder2 *psf,
    _In_ PCUITEMID_CHILD pidl,
    _Outptr_ IBindCtx **ppbc)
{
    VARIANT var;
    HRESULT hr;

    *ppbc = NULL;
    VariantInit(&var);
    hr = psf->GetDetailsEx(pidl, &PKEY_MergedParsingBindContext, &var);
    if (SUCCEEDED(hr))
    {
        hr = (V_VT(&var) == VT_UNKNOWN && V_UNKNOWN(&var)) ?
            V_UNKNOWN(&var)->QueryInterface(IID_PPV_ARG(IBindCtx, ppbc)) : E_FAIL;
        VariantClear(&var);
        return hr;
    }

    hr = psf->GetDetailsEx(pidl, &PKEY_MergedFindData, &var);
    if (SUCCEEDED(hr))
    {
        WIN32_FIND_DATAW wfd;

        if (V_VT(&var) == (VT_ARRAY | VT_UI1) &&
            V_ARRAY(&var)->rgsabound[0].cElements >= sizeof(wfd))
        {
            CopyMemory(&wfd, V_ARRAY(&var)->pvData, sizeof(wfd));
            hr = IFileSystemBindData_Constructor(&wfd, ppbc);
        }
        else
        {
            hr = E_FAIL;
        }
        VariantClear(&var);
        return hr;
    }

    return CreateBindCtx(0, ppbc);
}

/* Parsing name of a child relative to the folder plus a bind context that recreates it */
static HRESULT
GetRelativeNameAndParseBindCtx(
    _In_ IShellFolder *psfFolder,
    _In_ PCUIDLIST_RELATIVE pidl,
    _Outptr_ LPWSTR *ppszName,
    _Outptr_ IBindCtx **ppbc)
{
    IShellFolder2 *psf;
    WCHAR szPath[MAX_PATH * 2];
    HRESULT hr;

    *ppbc = NULL;
    *ppszName = NULL;
    szPath[0] = UNICODE_NULL;

    hr = psfFolder->QueryInterface(IID_PPV_ARG(IShellFolder2, &psf));
    if (FAILED(hr))
        return hr;

    while (SUCCEEDED(hr) && pidl && pidl->mkid.cb)
    {
        PITEMID_CHILD pidlFirst;
        WCHAR szName[MAX_PATH];

        hr = CloneFirstID(pidl, &pidlFirst);
        if (FAILED(hr))
            break;

        hr = DisplayNameOfW(psf, pidlFirst, SHGDN_INFOLDER | SHGDN_FORPARSING, szName,
                            _countof(szName));
        if (SUCCEEDED(hr) && szPath[0])
            hr = StringCchCatW(szPath, _countof(szPath), L"\\");
        if (SUCCEEDED(hr))
            hr = StringCchCatW(szPath, _countof(szPath), szName);

        if (SUCCEEDED(hr))
        {
            pidl = ILGetNext(pidl);
            if (pidl && pidl->mkid.cb)
            {
                IShellFolder2 *psfNext;

                hr = psf->BindToObject(pidlFirst, NULL, IID_PPV_ARG(IShellFolder2, &psfNext));
                if (SUCCEEDED(hr))
                {
                    psf->Release();
                    psf = psfNext;
                }
            }
            else
            {
                hr = GetParsingBindCtx(psf, pidlFirst, ppbc);
            }
        }
        ILFree(pidlFirst);
    }

    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(szPath, ppszName);
        if (FAILED(hr) && *ppbc)
        {
            (*ppbc)->Release();
            *ppbc = NULL;
        }
    }

    psf->Release();
    return hr;
}

/* Copies the first path segment of the name and advances to the rest */
static HRESULT
NextSegment(
    _Inout_ LPCWSTR *ppszName,
    _Out_writes_(cch) LPWSTR pszSegment,
    _In_ UINT cch,
    _In_ BOOL fValidate)
{
    LPCWSTR pszSep = StrChrW(*ppszName, L'\\');
    HRESULT hr;

    if (pszSep)
    {
        if (pszSep <= *ppszName)
        {
            hr = E_INVALIDARG;
            pszSep = NULL;
        }
        else
        {
            hr = StringCchCopyNW(pszSegment, cch, *ppszName, pszSep - *ppszName);
            pszSep = pszSep[1] ? pszSep + 1 : NULL;
        }
    }
    else
    {
        hr = StringCchCopyW(pszSegment, cch, *ppszName);
    }
    *ppszName = pszSep;

    if (hr == S_OK && fValidate)
    {
        BOOL fAllDots = TRUE;
        LPCWSTR pch;

        /* "." and ".." are not names and neither are names made only of dots */
        if (!pszSegment[0] || !wcscmp(pszSegment, L".") || !wcscmp(pszSegment, L".."))
        {
            pszSegment[0] = UNICODE_NULL;
            return E_INVALIDARG;
        }

        for (pch = pszSegment; *pch; pch++)
        {
            if (!PathIsValidCharW(*pch, PATH_CHAR_CLASS_OTHER_VALID | PATH_CHAR_CLASS_SPACE |
                                  PATH_CHAR_CLASS_COMMA | PATH_CHAR_CLASS_SEMICOLON |
                                  PATH_CHAR_CLASS_DOT))
            {
                pszSegment[0] = UNICODE_NULL;
                return E_INVALIDARG;
            }
            if (*pch != L'.')
                fAllDots = FALSE;
        }

        if (fAllDots)
        {
            pszSegment[0] = UNICODE_NULL;
            return E_INVALIDARG;
        }
    }
    return hr;
}

/* Related item that answers IIdentityName with a fixed ID list */
class CMergedRelatedItem : public IIdentityName
{
public:
    CMergedRelatedItem(
        _In_ PIDLIST_ABSOLUTE pidl) :
        m_cRef(1),
        m_pidl(pidl)
    {
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IRelatedItem || riid == IID_IIdentityName)
        {
            *ppv = static_cast<IIdentityName *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
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
    GetItemIDList(
        _Outptr_ PIDLIST_ABSOLUTE *ppidl) override
    {
        *ppidl = ILClone(m_pidl);
        return *ppidl ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP
    GetItem(
        _Outptr_ IShellItem **ppsi) override
    {
        return SHCreateItemFromIDList(m_pidl, IID_PPV_ARG(IShellItem, ppsi));
    }

private:
    ~CMergedRelatedItem()
    {
        ILFree(m_pidl);
    }

    LONG m_cRef;
    PIDLIST_ABSOLUTE m_pidl;
};

/* Enumerator that reports the children of a shell folder as storage elements */
class CShellFolderStorageEnum : public IEnumSTATSTG
{
public:
    CShellFolderStorageEnum(
        _In_ IShellFolder *psf) :
        m_cRef(1),
        m_psf(psf),
        m_penum(NULL)
    {
        m_psf->AddRef();
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IEnumSTATSTG)
        {
            *ppv = static_cast<IEnumSTATSTG *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
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
    Next(
        _In_ ULONG celt,
        _Out_writes_to_(celt, *pceltFetched) STATSTG *rgelt,
        _Out_opt_ ULONG *pceltFetched) override
    {
        PITEMID_CHILD *apidl;
        ULONG cFetched = 0;
        ULONG cDone = 0;
        HRESULT hr = S_OK;
        ULONG i;

        if (!m_penum)
        {
            hr = m_psf->EnumObjects(NULL,
                                    SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
                                    &m_penum);
            if (hr != S_OK)
                return hr;
        }

        apidl = (PITEMID_CHILD *)LocalAlloc(LPTR, celt * sizeof(*apidl));
        if (!apidl)
            return E_OUTOFMEMORY;

        hr = m_penum->Next(celt, apidl, &cFetched);
        if (SUCCEEDED(hr))
        {
            for (i = 0; i < cFetched; i++)
            {
                if (SUCCEEDED(PidlToStatStg(apidl[i], &rgelt[cDone])))
                    cDone++;
                ILFree(apidl[i]);
            }

            hr = (cDone == celt) ? S_OK : S_FALSE;
            if (pceltFetched)
                *pceltFetched = cDone;
        }

        LocalFree(apidl);
        return hr;
    }

    STDMETHODIMP
    Skip(
        _In_ ULONG celt) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP
    Reset() override
    {
        return m_penum ? m_penum->Reset() : S_OK;
    }

    STDMETHODIMP
    Clone(
        _Outptr_ IEnumSTATSTG **ppenum) override
    {
        *ppenum = NULL;
        return E_NOTIMPL;
    }

private:
    ~CShellFolderStorageEnum()
    {
        m_psf->Release();
        if (m_penum)
            m_penum->Release();
    }

    HRESULT
    PidlToStatStg(
        _In_ PCUITEMID_CHILD pidl,
        _Out_ STATSTG *pstat)
    {
        IShellFolder2 *psf2;
        WIN32_FIND_DATAW wfd;
        VARIANT var;
        HRESULT hr;

        ZeroMemory(pstat, sizeof(*pstat));
        hr = m_psf->QueryInterface(IID_PPV_ARG(IShellFolder2, &psf2));
        if (FAILED(hr))
            return hr;

        VariantInit(&var);
        hr = psf2->GetDetailsEx(pidl, &PKEY_MergedFindData, &var);
        psf2->Release();
        if (FAILED(hr))
            return hr;

        if (V_VT(&var) != (VT_ARRAY | VT_UI1) ||
            V_ARRAY(&var)->rgsabound[0].cElements < sizeof(wfd))
        {
            VariantClear(&var);
            return E_FAIL;
        }
        CopyMemory(&wfd, V_ARRAY(&var)->pvData, sizeof(wfd));
        VariantClear(&var);

        pstat->atime = wfd.ftLastAccessTime;
        pstat->ctime = wfd.ftCreationTime;
        pstat->mtime = wfd.ftLastWriteTime;
        pstat->cbSize.HighPart = wfd.nFileSizeHigh;
        pstat->cbSize.LowPart = wfd.nFileSizeLow;
        pstat->type = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? STGTY_STORAGE :
            STGTY_STREAM;
        return SHStrDupW(wfd.cFileName, &pstat->pwcsName);
    }

    LONG m_cRef;
    IShellFolder *m_psf;
    IEnumIDList *m_penum;
};

/* Moves or copies one storage element of a shell folder into another storage */
static HRESULT
StgMoveElementTo(
    _In_ IShellFolder *psf,
    _In_ IStorage *pstgSrc,
    _In_ LPCWSTR pszName,
    _In_ IStorage *pstgDest,
    _In_ LPCWSTR pszNewName,
    _In_ DWORD grfFlags)
{
    PITEMID_CHILD pidl;
    IStream *pstmSrc;
    HRESULT hr;

    if (grfFlags != STGMOVE_MOVE && grfFlags != STGMOVE_COPY)
        return E_INVALIDARG;

    hr = ParseNameInFolder(psf, NULL, NULL, pszName, &pidl);
    if (FAILED(hr))
        return hr;

    if (SUCCEEDED(psf->BindToObject(pidl, NULL, IID_PPV_ARG(IStream, &pstmSrc))))
    {
        IStream *pstmDest;

        hr = pstgDest->CreateStream(pszNewName, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                    0, 0, &pstmDest);
        if (SUCCEEDED(hr))
        {
            ULARGE_INTEGER cbAll;

            cbAll.QuadPart = ~0ULL;
            hr = pstmSrc->CopyTo(pstmDest, cbAll, NULL, NULL);
            if (SUCCEEDED(hr))
                hr = pstmDest->Commit(STGC_DEFAULT);
            if (SUCCEEDED(hr) && grfFlags == STGMOVE_MOVE)
                hr = pstgSrc->DestroyElement(pszName);
            pstmDest->Release();
        }
        pstmSrc->Release();
    }
    else
    {
        IStorage *pstgChild;

        hr = psf->BindToObject(pidl, NULL, IID_PPV_ARG(IStorage, &pstgChild));
        if (SUCCEEDED(hr))
        {
            IStorage *pstgNew;

            hr = pstgDest->CreateStorage(pszNewName,
                                         STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
                                         0, 0, &pstgNew);
            if (SUCCEEDED(hr))
            {
                IEnumSTATSTG *penum;

                if (pstgChild->EnumElements(0, NULL, 0, &penum) == S_OK)
                {
                    STATSTG stat;

                    while (SUCCEEDED(hr) && penum->Next(1, &stat, NULL) == S_OK)
                    {
                        hr = pstgChild->MoveElementTo(stat.pwcsName, pstgNew, stat.pwcsName,
                                                      grfFlags);
                        if (SUCCEEDED(hr))
                            hr = pstgNew->Commit(STGC_DEFAULT);
                        CoTaskMemFree(stat.pwcsName);
                    }
                    penum->Release();
                }

                if (SUCCEEDED(hr))
                    hr = pstgNew->Commit(STGC_DEFAULT);
                pstgNew->Release();
            }
            pstgChild->Release();
        }
    }

    ILFree(pidl);
    return hr;
}

/* CMergedFldrNamespace */

CMergedFldrNamespace::CMergedFldrNamespace(
    _In_ BOOL fReadPropertyBag) :
    m_psf(NULL),
    m_guid(GUID_NULL),
    m_pidl(NULL),
    m_dwFlags(0),
    m_uGroupID(0),
    m_fReadPropertyBag(fReadPropertyBag),
    m_dwAttributes(0),
    m_dwAttributeMask(~0UL),
    m_dwDropEffect(0),
    m_iDefaultOverlay(-1),
    m_iConflictOverlay(-1)
{
    m_szLocation[0] = UNICODE_NULL;
    m_szRootPath[0] = UNICODE_NULL;
}

CMergedFldrNamespace::~CMergedFldrNamespace()
{
    ReleaseNamespace();
}

VOID
CMergedFldrNamespace::ReleaseNamespace()
{
    if (m_psf)
    {
        SetWrapper(m_psf, NULL);
        m_psf->Release();
        m_psf = NULL;
    }

    ILFree(m_pidl);
    m_pidl = NULL;
    m_dwFlags = 0;
    m_guid = GUID_NULL;
}

HRESULT
CMergedFldrNamespace::SetNamespace(
    _In_ IShellFolder2 *psfWrapper,
    _In_opt_ const GUID *pguidObject,
    _In_opt_ IShellFolder *psf,
    _In_opt_ PCIDLIST_ABSOLUTE pidl,
    _In_ DWORD dwFlags,
    _In_ UINT uGroupID)
{
    ReleaseNamespace();

    if (psf)
    {
        m_psf = psf;
        m_psf->AddRef();
    }
    else if (pidl)
    {
        SHBindToObject(NULL, pidl, NULL, IID_PPV_ARG(IShellFolder, &m_psf));
    }

    if (pidl)
        m_pidl = ILClone(pidl);
    else if (m_psf)
        MergedFolder_GetFolderTargetIDList(m_psf, &m_pidl);

    if (!m_psf || !m_pidl)
        return E_FAIL;

    SetWrapper(m_psf, psfWrapper);

    m_guid = pguidObject ? *pguidObject : GUID_NULL;
    m_dwFlags = dwFlags;
    m_uGroupID = uGroupID;
    m_szLocation[0] = UNICODE_NULL;
    m_dwAttributes = 0;
    m_dwAttributeMask = ~0UL;
    m_dwDropEffect = 0;
    m_iDefaultOverlay = -1;
    m_iConflictOverlay = -1;
    return S_OK;
}

/* The namespace class can describe itself under CLSID\{guid}\MergedFolder */
VOID
CMergedFldrNamespace::EnsureReadPropertyBag()
{
    WCHAR szGuid[40];
    WCHAR szKey[MAX_PATH];
    IPropertyBag *ppb;

    if (m_fReadPropertyBag)
        return;

    StringFromGUID2(m_guid, szGuid, _countof(szGuid));
    StringCchPrintfW(szKey, _countof(szKey), L"CLSID\\%s\\MergedFolder", szGuid);
    if (SUCCEEDED(SHCreatePropertyBagOnRegKey(HKEY_CLASSES_ROOT, szKey, STGM_READ,
                                              IID_PPV_ARG(IPropertyBag, &ppb))))
    {
        WCHAR szValue[MAX_PATH];
        WCHAR szSource[100];

        if (SUCCEEDED(SHPropertyBag_ReadStr(ppb, L"Location", szSource, _countof(szSource))))
            SHLoadIndirectString(szSource, m_szLocation, _countof(m_szLocation), NULL);

        SHPropertyBag_ReadDWORD(ppb, L"Attributes", &m_dwAttributes);
        SHPropertyBag_ReadDWORD(ppb, L"AttributeMask", &m_dwAttributeMask);
        SHPropertyBag_ReadDWORD(ppb, L"DropEffect", &m_dwDropEffect);

        szValue[0] = UNICODE_NULL;
        if (SUCCEEDED(SHPropertyBag_ReadStr(ppb, L"DefaultOverlayIcon", szValue,
                                            _countof(szValue))) &&
            szValue[0])
        {
            int iIcon = PathParseIconLocationW(szValue);
            m_iDefaultOverlay = SHGetIconOverlayIndexW(szValue, iIcon);
        }

        szValue[0] = UNICODE_NULL;
        if (SUCCEEDED(SHPropertyBag_ReadStr(ppb, L"ConflictOverlayIcon", szValue,
                                            _countof(szValue))) &&
            szValue[0])
        {
            int iIcon = PathParseIconLocationW(szValue);
            m_iConflictOverlay = SHGetIconOverlayIndexW(szValue, iIcon);
        }

        ppb->Release();
    }

    m_fReadPropertyBag = TRUE;
}

/* CMergedShellFolder */

CMergedShellFolder::CMergedShellFolder(
    _In_opt_ CMergedShellFolder *pParent,
    _In_ REFCLSID rclsid,
    _In_ DWORD dwDropEffect) :
    m_cRef(1),
    m_pidl(NULL),
    m_hdpaNamespaces(NULL),
    m_uWhichFolderColumn((UINT)-1),
    m_pstgDest(NULL),
    m_clsid(rclsid),
    m_hdpaObjects(NULL),
    m_pParent(pParent),
    m_fHasGroupedNamespaces(FALSE),
    m_fAcquiring(FALSE),
    m_fCDBurn(FALSE),
    m_dwDropEffect(dwDropEffect)
{
    if (m_pParent)
    {
        m_pParent->AddRef();
        m_fCDBurn = m_pParent->m_fCDBurn;
        m_dwDropEffect = m_pParent->m_dwDropEffect;
    }
    else
    {
        m_fCDBurn = IsEqualCLSID(m_clsid, CLSID_CDBurnFolder);
    }
}

CMergedShellFolder::~CMergedShellFolder()
{
    if (m_hdpaNamespaces)
        DPA_EnumCallback(m_hdpaNamespaces, SetOwnerCB, NULL);

    if (!m_fAcquiring)
        DestroyObjects();

    if (m_pParent)
    {
        m_pParent->Release();
        m_pParent = NULL;
    }

    if (m_pstgDest)
    {
        m_pstgDest->Release();
        m_pstgDest = NULL;
    }

    ILFree(m_pidl);
    if (m_hdpaNamespaces)
        DPA_DestroyCallback(m_hdpaNamespaces, DestroyNamespaceCB, NULL);
    m_hdpaNamespaces = NULL;
}

STDMETHODIMP
CMergedShellFolder::QueryInterface(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CMergedShellFolder, IShellService),
        QITABENT(CMergedShellFolder, ITranslateShellChangeNotify),
        QITABENTMULTI(CMergedShellFolder, IShellFolder, IAugmentedMergedFolder),
        QITABENT(CMergedShellFolder, IAugmentedMergedFolder),
        QITABENT(CMergedShellFolder, IShellFolder2),
        QITABENTMULTI(CMergedShellFolder, IPersist, IPersistFolder2),
        QITABENTMULTI(CMergedShellFolder, IPersistFolder, IPersistFolder2),
        QITABENT(CMergedShellFolder, IPersistFolder2),
        QITABENT(CMergedShellFolder, IStorage),
        QITABENT(CMergedShellFolder, IFolderType),
        QITABENT(CMergedShellFolder, IObjectWithBackReferences),
        QITABENT(CMergedShellFolder, ILocalizableItemParent),
        QITABENT(CMergedShellFolder, IAliasedNamespace),
        { NULL, 0 }
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    /* Asking for the merged folder class ID gives the object itself */
    if (FAILED(hr) && IsEqualIID(riid, CLSID_MergedFolder))
    {
        *ppv = this;
        AddRef();
        hr = S_OK;
    }
    return hr;
}

STDMETHODIMP_(ULONG)
CMergedShellFolder::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CMergedShellFolder::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

int
CMergedShellFolder::GetNamespaceCount() const
{
    return m_hdpaNamespaces ? DPA_GetPtrCount(m_hdpaNamespaces) : 0;
}

HRESULT
CMergedShellFolder::GetNamespace(
    _In_ int iNamespace,
    _Outptr_ CMergedFldrNamespace **ppns)
{
    *ppns = NULL;
    if (iNamespace >= 0 && iNamespace < GetNamespaceCount())
        *ppns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, iNamespace);
    return *ppns ? S_OK : E_INVALIDARG;
}

HRESULT
CMergedShellFolder::FindNamespace(
    _In_ DWORD dwMask,
    _In_ DWORD dwValue,
    _Outptr_ CMergedFldrNamespace **ppns)
{
    int i;

    *ppns = NULL;
    for (i = 0; i < GetNamespaceCount(); i++)
    {
        CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);

        if ((dwValue & dwMask) != (pns->m_dwFlags & dwMask))
            continue;

        /* Asking for a grouped namespace means the ungrouped one */
        if ((dwValue & dwMask & MFNS_GROUPED) && pns->m_uGroupID)
            continue;

        *ppns = pns;
        break;
    }
    return *ppns ? S_OK : E_FAIL;
}

HRESULT
CMergedShellFolder::SimpleAddNamespace(
    _In_ CMergedFldrNamespace *pns)
{
    if (!m_hdpaNamespaces)
        m_hdpaNamespaces = DPA_Create(2);

    if (!m_hdpaNamespaces || DPA_InsertPtr(m_hdpaNamespaces, DA_LAST, pns) == -1)
        return E_OUTOFMEMORY;

    if (pns->m_dwFlags & MFNS_GROUPED)
        m_fHasGroupedNamespaces = TRUE;
    return S_OK;
}

HRESULT
CMergedShellFolder::GetSubPidl(
    _In_ PCUITEMID_CHILD pidl,
    _In_ UINT iEntry,
    _Out_opt_ UINT *piNamespace,
    _Out_opt_ WORD *pwEntryFlags,
    _Outptr_opt_ PITEMID_CHILD *ppidlSub,
    _Outptr_opt_ CMergedFldrNamespace **ppns)
{
    const MERGEDWRAP *pWrap = GetWrap(pidl);
    const BYTE *pEntry;
    const BYTE *pEnd;
    UINT cbHeader;
    UINT iNamespace;
    HRESULT hr = S_OK;
    UINT i;

    if (piNamespace)
        *piNamespace = (UINT)-1;
    if (pwEntryFlags)
        *pwEntryFlags = 0;
    if (ppidlSub)
        *ppidlSub = NULL;
    if (ppns)
        *ppns = NULL;

    if (!pWrap || iEntry >= pWrap->cEntries)
        return E_INVALIDARG;

    cbHeader = GetWrapEntryHeader(pWrap);
    pEntry = (const BYTE *)(pWrap + 1);
    pEnd = (const BYTE *)pWrap + pWrap->cb;
    for (i = 0; ; i++)
    {
        const ITEMIDLIST *pidlEntry = (const ITEMIDLIST *)(pEntry + 2 * sizeof(WORD));

        if (pEntry + cbHeader + pidlEntry->mkid.cb > pEnd)
            return E_POINTER;
        if (i == iEntry)
            break;
        pEntry += cbHeader + pidlEntry->mkid.cb;
    }

    if (pWrap->dwVersion >= MERGEDWRAP_VERSION)
        iNamespace = ((const WORD *)pEntry)[1];
    else
        iNamespace = *(const DWORD *)pEntry;

    if (piNamespace)
        *piNamespace = iNamespace;
    if (pwEntryFlags && pWrap->dwVersion >= MERGEDWRAP_VERSION)
        *pwEntryFlags = ((const WORD *)pEntry)[0];
    if (ppns)
        hr = ((int)iNamespace < 0) ? E_INVALIDARG : GetNamespace(iNamespace, ppns);

    if (SUCCEEDED(hr) && ppidlSub)
        hr = CloneFirstID((PCUIDLIST_RELATIVE)(pEntry + 2 * sizeof(WORD)), ppidlSub);
    return hr;
}

HRESULT
CMergedShellFolder::NamespaceForItem(
    _In_ PCUITEMID_CHILD pidl,
    _In_ DWORD dwMask,
    _In_ DWORD dwValue,
    _Outptr_opt_ IShellFolder **ppsf,
    _Outptr_opt_ PITEMID_CHILD *ppidlSub,
    _Outptr_opt_ CMergedFldrNamespace **ppns,
    _In_ BOOL fExactOnly)
{
    CMergedFldrNamespace *pns = NULL;
    PITEMID_CHILD pidlSub = NULL;
    HRESULT hr = E_UNEXPECTED;
    WORD wFlags;
    UINT i;

    if (ppsf)
        *ppsf = NULL;
    if (ppns)
        *ppns = NULL;

    /* The first namespace the item was enumerated from that matches wins */
    for (i = 0; SUCCEEDED(GetSubPidl(pidl, i, NULL, &wFlags, &pidlSub, &pns)); i++)
    {
        if (!(wFlags & MERGEDENTRY_IMPLICIT) && (dwValue & dwMask) == (pns->m_dwFlags & dwMask))
        {
            hr = S_OK;
            break;
        }
        ILFree(pidlSub);
        pidlSub = NULL;
    }

    if (FAILED(hr) && !fExactOnly)
        hr = GetSubPidl(pidl, 0, NULL, NULL, &pidlSub, &pns);
    if (FAILED(hr))
        return hr;

    if (ppsf)
        *ppsf = pns->m_psf;
    if (ppns)
        *ppns = pns;
    if (ppidlSub)
        *ppidlSub = pidlSub;
    else
        ILFree(pidlSub);
    return hr;
}

HRESULT
CMergedShellFolder::GetSubPidlForNamespace(
    _In_ PCUITEMID_CHILD pidl,
    _In_ UINT iNamespace,
    _Outptr_ PITEMID_CHILD *ppidlSub)
{
    PITEMID_CHILD pidlSub;
    UINT iEntryNamespace;
    UINT i;

    *ppidlSub = NULL;
    for (i = 0; SUCCEEDED(GetSubPidl(pidl, i, &iEntryNamespace, NULL, &pidlSub, NULL)); i++)
    {
        if (iEntryNamespace == iNamespace)
        {
            *ppidlSub = pidlSub;
            return S_OK;
        }
        ILFree(pidlSub);
    }
    return E_INVALIDARG;
}

BOOL
CMergedShellFolder::ContainsSrcID(
    _In_ PCUITEMID_CHILD pidl,
    _In_ UINT iNamespace)
{
    UINT iEntryNamespace;
    UINT i;

    for (i = 0; SUCCEEDED(GetSubPidl(pidl, i, &iEntryNamespace, NULL, NULL, NULL)); i++)
    {
        if (iEntryNamespace == iNamespace)
            return TRUE;
    }
    return FALSE;
}

HRESULT
CMergedShellFolder::CreateWrap(
    _In_ PCUITEMID_CHILD pidl,
    _In_ UINT iNamespace,
    _In_ WORD wEntryFlags,
    _Outptr_ PITEMID_CHILD *ppidlWrap)
{
    UINT cbSub = pidl->mkid.cb;
    PMERGEDWRAP pWrap;
    WORD *pEntry;

    /* Header, entry header, sub ID, its terminator and the list terminator */
    *ppidlWrap = (PITEMID_CHILD)SHAlloc(sizeof(*pWrap) + 2 * sizeof(WORD) + cbSub +
                                       2 * sizeof(WORD));
    if (!*ppidlWrap)
        return E_OUTOFMEMORY;

    pWrap = (PMERGEDWRAP)*ppidlWrap;
    ZeroMemory(pWrap, sizeof(*pWrap) + 2 * sizeof(WORD) + cbSub + 2 * sizeof(WORD));
    pWrap->cb = (WORD)(sizeof(*pWrap) + 2 * sizeof(WORD) + cbSub + sizeof(WORD));
    pWrap->wFlags = 0;
    pWrap->dwSignature = MERGEDWRAP_SIGNATURE;
    pWrap->dwVersion = MERGEDWRAP_VERSION;
    pWrap->cEntries = 1;

    pEntry = (WORD *)(pWrap + 1);
    pEntry[0] = wEntryFlags;
    pEntry[1] = (WORD)iNamespace;
    CopyMemory(&pEntry[2], pidl, cbSub);
    return S_OK;
}

HRESULT
CMergedShellFolder::WrapAddIDList(
    _In_ PCUITEMID_CHILD pidl,
    _In_ UINT iNamespace,
    _In_ WORD wEntryFlags,
    _Inout_ PITEMID_CHILD *ppidlWrap)
{
    PMERGEDWRAP pWrap;
    PIDHIDDENBLOCK pHidden;
    BYTE *pItem;
    BYTE *pEntry;
    UINT cbOld;
    UINT cbSub = pidl ? pidl->mkid.cb : 0;
    UINT cbEntry;
    UINT cbNew;

    if (!*ppidlWrap)
        return CreateWrap(pidl, iNamespace, wEntryFlags, ppidlWrap);

    /* A namespace appears once, a new sub ID replaces the old one */
    if (ContainsSrcID(*ppidlWrap, iNamespace))
    {
        PITEMID_CHILD pidlOld = *ppidlWrap;

        if (SUCCEEDED(WrapRemoveIDList(pidlOld, iNamespace, ppidlWrap)))
            ILFree(pidlOld);
        if (!*ppidlWrap)
            return CreateWrap(pidl, iNamespace, wEntryFlags, ppidlWrap);
    }

    pWrap = (PMERGEDWRAP)*ppidlWrap;
    cbOld = pWrap->cb;
    cbEntry = GetWrapEntryHeader(pWrap) + cbSub;
    cbNew = cbOld + cbEntry;
    if (cbNew > MAXWORD - sizeof(WORD))
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

    pItem = (BYTE *)CoTaskMemRealloc(pWrap, cbNew + sizeof(WORD));
    if (!pItem)
    {
        CoTaskMemFree(*ppidlWrap);
        *ppidlWrap = NULL;
        return E_OUTOFMEMORY;
    }
    pWrap = (PMERGEDWRAP)pItem;

    /* Entries go in front of any hidden blocks, which keep their place at the end */
    pHidden = ILFindFirstHiddenID((LPCITEMIDLIST)pItem);
    if (pHidden)
    {
        BYTE *pHiddenOld = (BYTE *)pHidden;
        UINT cbTail = cbOld - (UINT)(pHiddenOld - pItem);

        MoveMemory(pHiddenOld + cbEntry, pHiddenOld, cbTail);
        *(WORD *)(pItem + cbNew - sizeof(WORD)) = (WORD)(pHiddenOld + cbEntry - pItem);
        pEntry = pHiddenOld;
    }
    else
    {
        pEntry = pItem + cbOld;
    }

    if (pWrap->dwVersion >= MERGEDWRAP_VERSION)
    {
        ((WORD *)pEntry)[0] = wEntryFlags;
        ((WORD *)pEntry)[1] = (WORD)iNamespace;
        CopyMemory(pEntry + 2 * sizeof(WORD), pidl, cbSub);
        *(WORD *)(pEntry + 2 * sizeof(WORD) + cbSub) = 0;
    }
    else
    {
        *(DWORD *)pEntry = (WORD)iNamespace;
        CopyMemory(pEntry + sizeof(DWORD), pidl, cbSub);
    }

    *(WORD *)(pItem + cbNew) = 0;
    pWrap->cEntries++;
    pWrap->cb = (WORD)cbNew;
    *ppidlWrap = (PITEMID_CHILD)pItem;
    return S_OK;
}

HRESULT
CMergedShellFolder::WrapRemoveIDList(
    _In_ PCUITEMID_CHILD pidlWrap,
    _In_ UINT iNamespace,
    _Outptr_ PITEMID_CHILD *ppidlOut)
{
    HRESULT hr = GetWrap(pidlWrap) ? S_OK : E_INVALIDARG;
    PITEMID_CHILD pidlSub;
    UINT iEntryNamespace;
    WORD wFlags;
    UINT i;

    *ppidlOut = NULL;
    for (i = 0; SUCCEEDED(hr); i++)
    {
        if (FAILED(GetSubPidl(pidlWrap, i, &iEntryNamespace, &wFlags, &pidlSub, NULL)))
            break;
        if (iEntryNamespace != iNamespace)
            hr = WrapAddIDList(pidlSub, iEntryNamespace, wFlags, ppidlOut);
        ILFree(pidlSub);
    }
    return hr;
}

HRESULT
CMergedShellFolder::WrapRemoveIDListAbs(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ UINT iNamespace,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlOut)
{
    PITEMID_CHILD pidlLast;
    HRESULT hr;

    *ppidlOut = ILClone(pidl);
    if (!*ppidlOut)
        return E_OUTOFMEMORY;
    ILRemoveLastID((LPITEMIDLIST)*ppidlOut);

    hr = WrapRemoveIDList((PCUITEMID_CHILD)ILFindLastID(pidl), iNamespace, &pidlLast);
    if (SUCCEEDED(hr))
        hr = SHILAppend((LPITEMIDLIST)pidlLast, (LPITEMIDLIST *)ppidlOut);
    return hr;
}

BOOL
CMergedShellFolder::IsFolder(
    _In_ PCUITEMID_CHILD pidl)
{
    SFGAOF sfgao = SFGAO_FOLDER | SFGAO_STREAM;

    return SUCCEEDED(GetAttributesOf(1, &pidl, &sfgao)) &&
        (sfgao & (SFGAO_FOLDER | SFGAO_STREAM)) == SFGAO_FOLDER;
}

BOOL
CMergedShellFolder::ShouldMergeNamespaces(
    _In_ int iNamespace1,
    _In_ int iNamespace2)
{
    CMergedFldrNamespace *pns1;
    CMergedFldrNamespace *pns2;

    if (!m_fHasGroupedNamespaces || iNamespace1 < 0 || iNamespace2 < 0)
        return TRUE;

    GetNamespace(iNamespace1, &pns1);
    GetNamespace(iNamespace2, &pns2);
    if (pns1 == pns2 || !pns1 || !pns2 || !(pns1->m_dwFlags & MFNS_GROUPED))
        return TRUE;
    return pns1->m_uGroupID == pns2->m_uGroupID;
}

VOID
CMergedShellFolder::AddAllOtherNamespacesIfNeeded(
    _Inout_ PITEMID_CHILD *ppidlWrap)
{
    CMergedFldrNamespace *pnsDefault;
    LPWSTR pszName;
    IBindCtx *pbc;
    int i;

    if (FAILED(GetRelativeNameAndParseBindCtx(static_cast<IShellFolder2 *>(this), *ppidlWrap,
                                              &pszName, &pbc)))
    {
        return;
    }

    if (SUCCEEDED(NamespaceForItem(*ppidlWrap, MFNS_DEFAULT, MFNS_DEFAULT, NULL, NULL, &pnsDefault,
                                   FALSE)))
    {
        /* Folders exist in every namespace of their group, new items there go to the default one */
        for (i = 0; i < GetNamespaceCount(); i++)
        {
            CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
            PITEMID_CHILD pidlSub;

            if (pns != pnsDefault && m_fHasGroupedNamespaces && pnsDefault &&
                (pns->m_dwFlags & MFNS_GROUPED) && pns->m_uGroupID != pnsDefault->m_uGroupID)
            {
                continue;
            }

            if (!ContainsSrcID(*ppidlWrap, i) &&
                SUCCEEDED(ParseNameInFolder(pns->m_psf, NULL, pbc, pszName, &pidlSub)))
            {
                WrapAddIDList(pidlSub, i, MERGEDENTRY_IMPLICIT, ppidlWrap);
                ILFree(pidlSub);
            }
        }
    }

    if (pbc)
        pbc->Release();
    CoTaskMemFree(pszName);
}

VOID
CMergedShellFolder::DestroyObjects()
{
    if (m_hdpaObjects)
    {
        DPA_DestroyCallback(m_hdpaObjects, DestroyObjectCB, NULL);
        m_hdpaObjects = NULL;
    }
}

int
CALLBACK
CMergedShellFolder::DestroyObjectCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    MERGEDOBJECT *pObject = (MERGEDOBJECT *)p;

    if (pObject)
    {
        Str_SetPtrW(&pObject->pszName, NULL);
        ILFree(pObject->pidlWrap);
        delete pObject;
    }
    return TRUE;
}

int
CALLBACK
CMergedShellFolder::DestroyNamespaceCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    delete (CMergedFldrNamespace *)p;
    return TRUE;
}

int
CALLBACK
CMergedShellFolder::SetOwnerCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    CMergedFldrNamespace *pns = (CMergedFldrNamespace *)p;

    if (!pns->m_psf)
        return FALSE;

    IUnknown_SetOwner(pns->m_psf, (IUnknown *)pData);
    return TRUE;
}

/* Items come before folders, then by name; names that may not merge keep a stable order */
int
CALLBACK
CMergedShellFolder::CompareObjects(
    _In_ void *p1,
    _In_ void *p2,
    _In_ LPARAM lParam)
{
    MERGEDOBJECT *pObject1 = (MERGEDOBJECT *)p1;
    MERGEDOBJECT *pObject2 = (MERGEDOBJECT *)p2;
    CMergedShellFolder *pThis = (CMergedShellFolder *)lParam;
    int iResult;

    if (!pObject1 || !pObject2)
        return -1;

    if ((pObject1->dwAttributes ^ pObject2->dwAttributes) & SFGAO_FOLDER)
        return (pObject1->dwAttributes & SFGAO_FOLDER) ? 1 : -1;

    iResult = lstrcmpiW(pObject1->pszName, pObject2->pszName);
    if (!iResult && !pThis->ShouldMergeNamespaces(pObject1->iNamespace, pObject2->iNamespace))
        iResult = CompareArbitraryPidls(pObject1->pidlWrap, pObject2->pidlWrap);
    return iResult;
}

void *
CALLBACK
CMergedShellFolder::MergeObjects(
    _In_ UINT uMsg,
    _In_ void *pvDest,
    _In_ void *pvSrc,
    _In_ LPARAM lParam)
{
    CMergedShellFolder *pThis = (CMergedShellFolder *)lParam;
    MERGEDOBJECT *pDest = (MERGEDOBJECT *)pvDest;

    if (uMsg == DPAMM_MERGE)
    {
        MERGEDOBJECT *pSrc = (MERGEDOBJECT *)pvSrc;
        PITEMID_CHILD pidlSub;
        UINT iNamespace;
        WORD wFlags;

        /* The object found in a later namespace adds its sub ID to the kept one */
        if (SUCCEEDED(pThis->GetSubPidl(pSrc->pidlWrap, 0, &iNamespace, &wFlags, &pidlSub, NULL)))
        {
            pThis->WrapAddIDList(pidlSub, iNamespace, wFlags, &pDest->pidlWrap);
            ILFree(pidlSub);
        }
    }
    else if (uMsg == DPAMM_INSERT)
    {
        MERGEDOBJECT *pCopy = new MERGEDOBJECT;

        if (pCopy)
        {
            pCopy->iNamespace = pDest->iNamespace;
            pCopy->pszName = NULL;
            pCopy->dwAttributes = pDest->dwAttributes;
            pCopy->pidlWrap = pDest->pidlWrap ? (PITEMID_CHILD)ILCloneFirst(pDest->pidlWrap) : NULL;
            if (!pCopy->pidlWrap || !Str_SetPtrW(&pCopy->pszName, pDest->pszName))
            {
                Str_SetPtrW(&pCopy->pszName, NULL);
                ILFree(pCopy->pidlWrap);
                delete pCopy;
                pCopy = NULL;
            }
        }
        pDest = pCopy;
    }
    return pDest;
}

/* Reads every namespace and merges the children that have the same name */
UINT
CMergedShellFolder::AcquireObjects()
{
    HDPA hdpaNew = NULL;
    int i;

    m_fAcquiring = TRUE;
    for (i = 0; i < GetNamespaceCount(); i++)
    {
        CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
        HDPA *phdpaTarget = (i == 0) ? &m_hdpaObjects : &hdpaNew;
        IEnumIDList *penum;

        if (pns->m_psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS |
                                    SHCONTF_INCLUDEHIDDEN | SHCONTF_INIT_ON_FIRST_NEXT |
                                    SHCONTF_INCLUDESUPERHIDDEN, &penum) == S_OK)
        {
            if (!*phdpaTarget)
                *phdpaTarget = DPA_Create(4);

            if (*phdpaTarget)
            {
                PITEMID_CHILD pidl;

                while (penum->Next(1, &pidl, NULL) == S_OK)
                {
                    MERGEDOBJECT *pObject = new MERGEDOBJECT;

                    if (pObject)
                    {
                        WCHAR szName[MAX_PATH];
                        BOOL fKeep = FALSE;

                        pObject->pszName = NULL;
                        pObject->pidlWrap = NULL;
                        if (SUCCEEDED(CreateWrap(pidl, i, 0, &pObject->pidlWrap)))
                        {
                            PCUITEMID_CHILD pidlWrap = pObject->pidlWrap;

                            pObject->dwAttributes = SFGAO_FOLDER | SFGAO_HIDDEN;
                            pObject->iNamespace = i;
                            fKeep =
                                SUCCEEDED(GetAttributesOf(1, &pidlWrap, &pObject->dwAttributes)) &&
                                SUCCEEDED(DisplayNameOfW(static_cast<IShellFolder2 *>(this),
                                                         pidlWrap,
                                                         SHGDN_INFOLDER | SHGDN_FORPARSING,
                                                         szName, _countof(szName))) &&
                                Str_SetPtrW(&pObject->pszName, szName) &&
                                DPA_InsertPtr(*phdpaTarget, DA_LAST, pObject) != -1;
                        }

                        if (!fKeep)
                            DestroyObjectCB(pObject, NULL);
                    }
                    ILFree(pidl);
                }
            }
            penum->Release();
        }

        if (hdpaNew && !m_hdpaObjects)
        {
            m_hdpaObjects = hdpaNew;
            hdpaNew = NULL;
        }

        if (hdpaNew)
        {
            DPA_Merge(m_hdpaObjects, hdpaNew, DPAM_UNION, CompareObjects, MergeObjects,
                      (LPARAM)this);
            DPA_DestroyCallback(hdpaNew, DestroyObjectCB, NULL);
            hdpaNew = NULL;
        }
        else if (m_hdpaObjects)
        {
            DPA_Sort(m_hdpaObjects, CompareObjects, (LPARAM)this);
        }
    }
    m_fAcquiring = FALSE;

    return m_hdpaObjects ? DPA_GetPtrCount(m_hdpaObjects) : 0;
}

MERGEDOBJECT *
CMergedShellFolder::GetObject(
    _In_ int iObject)
{
    if (!m_hdpaObjects || iObject >= DPA_GetPtrCount(m_hdpaObjects))
        return NULL;
    return (MERGEDOBJECT *)DPA_GetPtr(m_hdpaObjects, iObject);
}

HRESULT
CMergedShellFolder::GetFolder2(
    _In_opt_ PCUITEMID_CHILD pidl,
    _Outptr_opt_ PITEMID_CHILD *ppidlSub,
    _Outptr_ IShellFolder2 **ppsf)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    if (ppidlSub)
        *ppidlSub = NULL;

    if (pidl)
    {
        IShellFolder *psf;

        hr = NamespaceForItem(pidl, MFNS_DISPLAY, MFNS_DISPLAY, &psf, ppidlSub, NULL, FALSE);
        if (SUCCEEDED(hr))
        {
            hr = psf->QueryInterface(IID_PPV_ARG(IShellFolder2, ppsf));
            if (FAILED(hr) && ppidlSub)
            {
                ILFree(*ppidlSub);
                *ppidlSub = NULL;
            }
        }
        return hr;
    }

    if (FAILED(FindNamespace(MFNS_DISPLAY, MFNS_DISPLAY, &pns)))
    {
        hr = GetNamespace(0, &pns);
        if (FAILED(hr))
            return E_FAIL;
    }
    return pns->m_psf->QueryInterface(IID_PPV_ARG(IShellFolder2, ppsf));
}

HRESULT
CMergedShellFolder::GetWhichFolderColumn(
    _In_ PCUITEMID_CHILD pidl,
    _Out_writes_(MAX_PATH) LPWSTR pszLocation)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    hr = NamespaceForItem(pidl, MFNS_ATTRIBUTES | MFNS_DISPLAY, MFNS_ATTRIBUTES | MFNS_DISPLAY,
                          NULL, NULL, &pns, FALSE);
    if (FAILED(hr))
        return hr;

    pns->EnsureReadPropertyBag();
    StringCchCopyW(pszLocation, MAX_PATH, pns->m_szLocation);
    return S_OK;
}

/* IShellFolder */

STDMETHODIMP
CMergedShellFolder::ParseDisplayName(
    _In_opt_ HWND hwnd,
    _In_opt_ IBindCtx *pbc,
    _In_ LPWSTR pszName,
    _Out_opt_ ULONG *pchEaten,
    _Outptr_ PIDLIST_RELATIVE *ppidl,
    _Inout_opt_ ULONG *pdwAttributes)
{
    WCHAR szSegment[MAX_PATH];
    LPCWSTR pszRest = pszName;
    IBindCtx *pbcFolder = NULL;
    PITEMID_CHILD pidlWrap = NULL;
    IUnknown *punkParam;
    BOOL fTryAll = TRUE;
    HRESULT hrParse = S_OK;
    HRESULT hr = S_OK;
    int i;

    *ppidl = NULL;
    szSegment[0] = UNICODE_NULL;
    if (pszName)
    {
        hr = NextSegment(&pszRest, szSegment, _countof(szSegment), TRUE);
        if (FAILED(hr))
            return hr;
    }

    /* Folders on the way to a deeper name are parsed as folders */
    if (pszRest && pbc)
    {
        IUnknown *punkBindData;

        if (SUCCEEDED(pbc->GetObjectParam((LPOLESTR)STR_FILE_SYS_BIND_DATA, &punkBindData)))
        {
            IFileSystemBindData *pfsbd;

            if (SUCCEEDED(punkBindData->QueryInterface(IID_PPV_ARG(IFileSystemBindData, &pfsbd))))
            {
                WIN32_FIND_DATAW wfd;

                if (SUCCEEDED(pfsbd->GetFindData(&wfd)))
                {
                    wfd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                    IFileSystemBindData_Constructor(&wfd, &pbcFolder);
                }
                pfsbd->Release();
            }
            punkBindData->Release();
        }
    }

    IShellFolder *psfParent = NULL;

    if (pbc && SUCCEEDED(pbc->GetObjectParam((LPOLESTR)L"ParentFolder", &punkParam)))
    {
        punkParam->QueryInterface(IID_PPV_ARG(IShellFolder, &psfParent));
        punkParam->Release();
    }

    if (psfParent)
    {
        /* The caller knows which namespace holds the name */
        for (i = 0; i < GetNamespaceCount(); i++)
        {
            CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
            PITEMID_CHILD pidlSub;

            if (!IsSameObject(psfParent, pns->m_psf))
                continue;

            hrParse = TryParseNameWithNamespace(pns, hwnd, pbcFolder ? pbcFolder : pbc, szSegment,
                                                &pidlSub);
            if (SUCCEEDED(hrParse))
            {
                fTryAll = FALSE;
                hr = WrapAddIDList(pidlSub, i, 0, &pidlWrap);
                ILFree(pidlSub);
                break;
            }
        }
        psfParent->Release();
    }
    else if (pbc &&
             SUCCEEDED(pbc->GetObjectParam((LPOLESTR)STR_MERGEDFOLDER_CHILDITEM, &punkParam)))
    {
        IShellItem *psi;

        /* Parse in the namespaces the merged child came from */
        if (SUCCEEDED(punkParam->QueryInterface(IID_PPV_ARG(IShellItem, &psi))))
        {
            IParentAndItem *ppai;

            if (SUCCEEDED(psi->QueryInterface(IID_PPV_ARG(IParentAndItem, &ppai))))
            {
                IShellFolder *psfChildParent;
                PITEMID_CHILD pidlChild;

                if (SUCCEEDED(ppai->GetParentAndItem(NULL, &psfChildParent, &pidlChild)))
                {
                    CMergedFldrNamespace *pns;
                    PITEMID_CHILD pidlEntry;
                    UINT iNamespace;
                    UINT iEntry;

                    fTryAll = FALSE;
                    for (iEntry = 0; SUCCEEDED(hr); iEntry++)
                    {
                        PITEMID_CHILD pidlSub;

                        if (FAILED(GetSubPidl(pidlChild, iEntry, &iNamespace, NULL, &pidlEntry,
                                              &pns)))
                        {
                            break;
                        }

                        hrParse = TryParseNameWithNamespace(pns, hwnd, pbc, szSegment, &pidlSub);
                        if (SUCCEEDED(hrParse))
                        {
                            hr = WrapAddIDList(pidlSub, iNamespace, 0, &pidlWrap);
                            ILFree(pidlSub);
                        }
                        ILFree(pidlEntry);
                    }
                    ILFree(pidlChild);
                    psfChildParent->Release();
                }
                ppai->Release();
            }
            psi->Release();
        }
        punkParam->Release();
    }

    if (fTryAll)
    {
        for (i = 0; SUCCEEDED(hr) && i < GetNamespaceCount(); i++)
        {
            CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
            PITEMID_CHILD pidlSub;

            hrParse = TryParseNameWithNamespace(pns, hwnd, pbcFolder ? pbcFolder : pbc, szSegment,
                                                &pidlSub);
            if (SUCCEEDED(hrParse))
            {
                hr = WrapAddIDList(pidlSub, i, 0, &pidlWrap);
                ILFree(pidlSub);
            }
        }
    }

    if (pidlWrap && IsFolder(pidlWrap))
        AddAllOtherNamespacesIfNeeded(&pidlWrap);

    if (pbcFolder)
        pbcFolder->Release();

    *ppidl = (PIDLIST_RELATIVE)pidlWrap;
    if (pidlWrap)
    {
        if (pbc && BindCtx_ContainsObject(pbc, STR_FILE_SYS_BIND_DATA))
            ((PMERGEDWRAP)pidlWrap)->wFlags |= MERGEDWRAP_SIMPLE;
    }
    else
    {
        if (FAILED(hr))
            return hr;
        hr = hrParse;
    }

    if (FAILED(hr))
        return hr;

    if (pszRest)
    {
        IShellFolder *psfChild;

        hr = BindToObject(*ppidl, pbc, IID_PPV_ARG(IShellFolder, &psfChild));
        if (SUCCEEDED(hr))
        {
            PIDLIST_RELATIVE pidlRest;

            hr = psfChild->ParseDisplayName(hwnd, pbc, (LPWSTR)pszRest, NULL, &pidlRest,
                                            pdwAttributes);
            if (SUCCEEDED(hr))
                hr = SHILAppend((LPITEMIDLIST)pidlRest, (LPITEMIDLIST *)ppidl);
            psfChild->Release();
        }

        if (FAILED(hr))
        {
            ILFree(*ppidl);
            *ppidl = NULL;
        }
    }
    else if (pdwAttributes && *pdwAttributes)
    {
        PCUITEMID_CHILD pidlChild = (PCUITEMID_CHILD)*ppidl;

        GetAttributesOf(1, &pidlChild, pdwAttributes);
    }
    return hr;
}

STDMETHODIMP
CMergedShellFolder::EnumObjects(
    _In_opt_ HWND hwnd,
    _In_ SHCONTF grfFlags,
    _Outptr_ IEnumIDList **ppenum)
{
    CMergedFldrEnum *pEnum;
    HRESULT hr;

    *ppenum = NULL;
    if (!m_hdpaNamespaces)
        return E_FAIL;

    /* Every enumeration reads the namespaces again */
    if (!m_fAcquiring)
        DestroyObjects();

    pEnum = new CMergedFldrEnum(this, grfFlags, 0);
    if (!pEnum)
        return E_OUTOFMEMORY;

    hr = pEnum->QueryInterface(IID_PPV_ARG(IEnumIDList, ppenum));
    pEnum->Release();
    return hr;
}

HRESULT
CMergedShellFolder::CreateChild(
    _Outptr_ CMergedShellFolder **ppChild)
{
    *ppChild = new CMergedShellFolder(this, m_clsid, m_dwDropEffect);
    return *ppChild ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CMergedShellFolder::BindToObject(
    _In_ PCUIDLIST_RELATIVE pidl,
    _In_opt_ IBindCtx *pbc,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PCUIDLIST_RELATIVE pidlRest;
    PITEMID_CHILD pidlFirst;
    CMergedShellFolder *pChild = NULL;
    CMergedFldrNamespace *pns;
    PITEMID_CHILD pidlSub;
    HRESULT hr;
    int i;

    *ppv = NULL;
    hr = CloneFirstID(pidl, &pidlFirst);
    if (FAILED(hr))
        return hr;

    if (IsFolder(pidlFirst))
        AddAllOtherNamespacesIfNeeded(&pidlFirst);

    pidlRest = ILGetNext(pidl);
    if (IsEqualIID(riid, IID_IIdentityName) && (!pidlRest || !pidlRest->mkid.cb))
    {
        /* The identity of a merged child is its default namespace item */
        hr = NamespaceForItem(pidlFirst, MFNS_DEFAULT, MFNS_DEFAULT, NULL, &pidlSub, &pns, FALSE);
        if (SUCCEEDED(hr))
        {
            PIDLIST_ABSOLUTE pidlTarget;

            hr = MergedFolder_GetFolderTargetIDList(pns->m_psf, &pidlTarget);
            if (SUCCEEDED(hr))
            {
                PIDLIST_ABSOLUTE pidlFull = ILCombine(pidlTarget, pidlSub);

                hr = pidlFull ? S_OK : E_OUTOFMEMORY;
                if (pidlFull)
                {
                    CMergedRelatedItem *pItem = new CMergedRelatedItem(pidlFull);

                    if (pItem)
                    {
                        hr = pItem->QueryInterface(riid, ppv);
                        pItem->Release();
                    }
                    else
                    {
                        ILFree(pidlFull);
                        hr = E_OUTOFMEMORY;
                    }
                }
                ILFree(pidlTarget);
            }
            ILFree(pidlSub);
        }
        ILFree(pidlFirst);
        return hr;
    }

    /* A folder becomes a merged folder over the same folder in every namespace */
    hr = E_NOINTERFACE;
    if (IsFolder(pidlFirst))
    {
        hr = CreateChild(&pChild);
        if (SUCCEEDED(hr) && m_pidl)
        {
            pChild->m_pidl = ILCombine(m_pidl, pidlFirst);
            if (!pChild->m_pidl)
            {
                pChild->Release();
                pChild = NULL;
                hr = E_OUTOFMEMORY;
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        BOOL fAdded = FALSE;

        for (i = 0; SUCCEEDED(hr) && i < GetNamespaceCount(); i++)
        {
            IShellFolder *psfSub;

            pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
            if (FAILED(GetSubPidlForNamespace(pidlFirst, i, &pidlSub)))
                continue;

            hr = E_OUTOFMEMORY;
            if (SUCCEEDED(pns->m_psf->BindToObject(pidlSub, pbc,
                                                   IID_PPV_ARG(IShellFolder, &psfSub))))
            {
                CMergedFldrNamespace *pnsChild = new CMergedFldrNamespace(!m_fCDBurn);

                if (pnsChild)
                {
                    hr = pnsChild->SetNamespace(static_cast<IShellFolder2 *>(pChild), &pns->m_guid,
                                                psfSub, NULL, pns->m_dwFlags, pns->m_uGroupID);
                    if (SUCCEEDED(hr))
                    {
                        if (!pns->m_szRootPath[0])
                        {
                            SHGetPathFromIDListEx(pns->m_pidl, pns->m_szRootPath,
                                                  _countof(pns->m_szRootPath), GPFIDL_UNCPRINTER);
                        }
                        hr = StringCchCopyW(pnsChild->m_szRootPath,
                                            _countof(pnsChild->m_szRootPath), pns->m_szRootPath);
                    }
                    if (SUCCEEDED(hr))
                        hr = pChild->SimpleAddNamespace(pnsChild);

                    if (SUCCEEDED(hr))
                        fAdded = TRUE;
                    else
                        delete pnsChild;
                }
                psfSub->Release();
            }
            ILFree(pidlSub);
        }

        if (!fAdded)
            hr = E_FAIL;
        else if (pidlRest && pidlRest->mkid.cb)
            hr = pChild->BindToObject(pidlRest, pbc, riid, ppv);
        else
            hr = pChild->QueryInterface(riid, ppv);

        pChild->Release();
        if (SUCCEEDED(hr))
        {
            ILFree(pidlFirst);
            return hr;
        }
    }

    /* Anything else is bound in the default namespace */
    if (!pidlRest || !pidlRest->mkid.cb)
    {
        hr = NamespaceForItem(pidlFirst, MFNS_DEFAULT, MFNS_DEFAULT, NULL, &pidlSub, &pns, FALSE);
        if (SUCCEEDED(hr))
        {
            hr = pns->m_psf->BindToObject(pidlSub, pbc, riid, ppv);
            ILFree(pidlSub);
        }
    }

    ILFree(pidlFirst);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::BindToStorage(
    _In_ PCUIDLIST_RELATIVE pidl,
    _In_opt_ IBindCtx *pbc,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return BindToObject(pidl, pbc, riid, ppv);
}

/* Compares two children of the deepest level both ID lists share */
static HRESULT
CompareRelativeIDs(
    _In_ IShellFolder *psf,
    _In_ PCUIDLIST_RELATIVE pidl1,
    _In_ PCUIDLIST_RELATIVE pidl2,
    _In_ LPARAM lParam)
{
    PCUIDLIST_RELATIVE pidlNext1 = ILGetNext(pidl1);
    PCUIDLIST_RELATIVE pidlNext2 = ILGetNext(pidl2);
    BOOL fMore1 = pidlNext1 && pidlNext1->mkid.cb;
    BOOL fMore2 = pidlNext2 && pidlNext2->mkid.cb;
    PITEMID_CHILD pidlFirst;
    IShellFolder *psfChild;
    HRESULT hr;

    if (!fMore1 && !fMore2)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    if (!fMore1)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)-1);
    if (!fMore2)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);

    hr = CloneFirstID(pidl1, &pidlFirst);
    if (FAILED(hr))
        return hr;

    hr = psf->BindToObject(pidlFirst, NULL, IID_PPV_ARG(IShellFolder, &psfChild));
    ILFree(pidlFirst);
    if (FAILED(hr))
        return hr;

    hr = psfChild->CompareIDs(lParam, pidlNext1, pidlNext2);
    psfChild->Release();
    return hr;
}

HRESULT
CMergedShellFolder::CompareSingleLevelIDs(
    _In_ LPARAM lParam,
    _In_ PCUITEMID_CHILD pidl1,
    _In_ PCUITEMID_CHILD pidl2)
{
    CMergedFldrNamespace *pns1;
    CMergedFldrNamespace *pns2;
    PITEMID_CHILD pidlSub1;
    PITEMID_CHILD pidlSub2;
    IShellFolder *psf1;
    IShellFolder *psf2;
    BOOL fSimple = IsSimpleWrap(pidl1) || IsSimpleWrap(pidl2);
    int iResult = 0;
    HRESULT hr;

    /* The location column sorts on the namespace names */
    if (m_uWhichFolderColumn != (UINT)-1 && LOWORD(lParam) == m_uWhichFolderColumn)
    {
        WCHAR szLocation1[MAX_PATH];
        WCHAR szLocation2[MAX_PATH];

        if (SUCCEEDED(GetWhichFolderColumn(pidl1, szLocation1)) &&
            SUCCEEDED(GetWhichFolderColumn(pidl2, szLocation2)))
        {
            iResult = StrCmpIW(szLocation1, szLocation2);
            if (iResult)
                return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(iResult < 0 ? -1 : 1));
        }
    }

    hr = NamespaceForItem(pidl1, MFNS_DISPLAY, MFNS_DISPLAY, &psf1, &pidlSub1, &pns1, FALSE);
    if (FAILED(hr))
        return hr;

    hr = NamespaceForItem(pidl2, MFNS_DISPLAY, MFNS_DISPLAY, &psf2, &pidlSub2, &pns2, FALSE);
    if (FAILED(hr))
    {
        ILFree(pidlSub1);
        return hr;
    }

    if (!fSimple && (psf1 == psf2 || !((pns1->m_dwFlags ^ pns2->m_dwFlags) & MFNS_SORTAFTER)))
    {
        int iFolder1 = SHGetAttributes(psf1, pidlSub1, SFGAO_FOLDER) ? 1 : 0;
        int iFolder2 = SHGetAttributes(psf2, pidlSub2, SFGAO_FOLDER) ? 1 : 0;

        if (pns1->m_dwFlags & MFNS_FOLDERSLAST)
            iFolder1 = -iFolder1;
        if (pns2->m_dwFlags & MFNS_FOLDERSLAST)
            iFolder2 = -iFolder2;
        iResult = iFolder2 - iFolder1;
    }

    if (psf1 == psf2)
    {
        hr = iResult ? MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(iResult < 0 ? -1 : 1)) :
            psf1->CompareIDs(lParam, pidlSub1, pidlSub2);
    }
    else if ((pns1->m_dwFlags ^ pns2->m_dwFlags) & MFNS_SORTAFTER)
    {
        hr = MAKE_HRESULT(SEVERITY_SUCCESS, 0,
                          (USHORT)((pns1->m_dwFlags & MFNS_SORTAFTER) ? 1 : -1));
    }
    else
    {
        WCHAR szName1[MAX_PATH];
        WCHAR szName2[MAX_PATH];

        if (!iResult &&
            SUCCEEDED(DisplayNameOfW(psf1, pidlSub1, SHGDN_INFOLDER, szName1, _countof(szName1))) &&
            SUCCEEDED(DisplayNameOfW(psf2, pidlSub2, SHGDN_INFOLDER, szName2, _countof(szName2))))
        {
            iResult = StrCmpW(szName1, szName2);
            if (!iResult &&
                SUCCEEDED(DisplayNameOfW(psf1, pidlSub1, SHGDN_INFOLDER | SHGDN_FORPARSING, szName1,
                                         _countof(szName1))) &&
                SUCCEEDED(DisplayNameOfW(psf2, pidlSub2, SHGDN_INFOLDER | SHGDN_FORPARSING, szName2,
                                         _countof(szName2))))
            {
                iResult = lstrcmpW(szName1, szName2);

                /* Same names from groups that never merge still differ */
                if (!iResult && pns1 != pns2 && m_fHasGroupedNamespaces &&
                    (pns1->m_dwFlags & MFNS_GROUPED) && pns1->m_uGroupID != pns2->m_uGroupID)
                {
                    iResult = CompareArbitraryPidls(pidlSub1, pidlSub2);
                }
            }
        }
        hr = MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(iResult < 0 ? -1 : (iResult > 0 ? 1 : 0)));
    }

    ILFree(pidlSub2);
    ILFree(pidlSub1);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::CompareIDs(
    _In_ LPARAM lParam,
    _In_ PCUIDLIST_RELATIVE pidl1,
    _In_ PCUIDLIST_RELATIVE pidl2)
{
    PITEMID_CHILD pidlFirst1 = NULL;
    PITEMID_CHILD pidlFirst2 = NULL;
    HRESULT hr = E_FAIL;

    if (pidl1)
        pidlFirst1 = ILCloneFirst(pidl1);
    if (pidl2)
        pidlFirst2 = ILCloneFirst(pidl2);

    if (pidlFirst1 && pidlFirst2)
    {
        hr = CompareSingleLevelIDs(lParam, pidlFirst1, pidlFirst2);
        if (hr == S_OK)
            hr = CompareRelativeIDs(static_cast<IShellFolder2 *>(this), pidl1, pidl2, lParam);

        /* Equal children from one namespace each still order by namespace and group */
        if (hr == S_OK && !IsSimpleWrap((PCUITEMID_CHILD)pidl1) &&
            !IsSimpleWrap((PCUITEMID_CHILD)pidl2))
        {
            const MERGEDWRAP *pWrap1 = GetWrap(pidlFirst1);
            const MERGEDWRAP *pWrap2 = GetWrap(pidlFirst2);

            if (pWrap1 && pWrap2 && pWrap1->cEntries == 1 && pWrap2->cEntries == 1)
            {
                GUID guid1 = GUID_NULL;
                GUID guid2 = GUID_NULL;
                int iResult;

                GetNameSpaceID(pidlFirst1, &guid1);
                GetNameSpaceID(pidlFirst2, &guid2);
                iResult = memcmp(&guid1, &guid2, sizeof(guid1));
                if (!iResult)
                {
                    UINT uGroup1 = 0;
                    UINT uGroup2 = 0;

                    GetNameSpaceGroupID(pidlFirst1, &uGroup1);
                    GetNameSpaceGroupID(pidlFirst2, &uGroup2);
                    iResult = (int)uGroup2 - (int)uGroup1;
                }
                hr = MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)iResult);
            }
        }
    }

    ILFree(pidlFirst1);
    ILFree(pidlFirst2);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::CreateViewObject(
    _In_opt_ HWND hwndOwner,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    *ppv = NULL;
    if (IsEqualIID(riid, IID_IDropTarget))
    {
        CMergedFldrDropTarget *pdt = new CMergedFldrDropTarget(this, hwndOwner);

        if (!pdt)
            return E_OUTOFMEMORY;

        hr = pdt->QueryInterface(IID_IDropTarget, ppv);
        pdt->Release();
        return hr;
    }

    if (IsEqualIID(riid, IID_IShellView))
    {
        IShellFolderViewCB *psfvcb;
        SFV_CREATE sfvc;

        hr = CMergedFolderViewCB_CreateInstance(this, IID_PPV_ARG(IShellFolderViewCB, &psfvcb));
        if (FAILED(hr))
            return hr;

        sfvc.cbSize = sizeof(sfvc);
        sfvc.pshf = static_cast<IShellFolder2 *>(this);
        sfvc.psvOuter = NULL;
        sfvc.psfvcb = psfvcb;
        hr = SHCreateShellFolderView(&sfvc, (IShellView **)ppv);
        psfvcb->Release();
        return hr;
    }

    /* There is no category provider to group the view by location */
    if (IsEqualIID(riid, IID_ICategoryProvider))
        return E_NOINTERFACE;

    hr = FindNamespace(MFNS_VIEW, MFNS_VIEW, &pns);
    if (SUCCEEDED(hr))
        hr = pns->m_psf->CreateViewObject(hwndOwner, riid, ppv);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetAttributesOf(
    _In_ UINT cidl,
    _In_reads_opt_(cidl) PCUITEMID_CHILD_ARRAY apidl,
    _Inout_ SFGAOF *rgfInOut)
{
    HRESULT hr = S_OK;
    UINT i;

    if (!cidl || !apidl)
    {
        *rgfInOut &= SFGAO_HASSUBFOLDER | SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_LINK |
                     SFGAO_DROPTARGET | SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_CANLINK |
                     SFGAO_CANMOVE | SFGAO_CANCOPY;
        return S_OK;
    }

    for (i = 0; i < cidl && SUCCEEDED(hr); i++)
    {
        CMergedFldrNamespace *pns;
        PITEMID_CHILD pidlSub;
        IShellFolder *psf;

        hr = NamespaceForItem(apidl[i], MFNS_ATTRIBUTES | MFNS_DISPLAY,
                              MFNS_ATTRIBUTES | MFNS_DISPLAY, &psf, &pidlSub, &pns, FALSE);
        if (FAILED(hr))
            break;

        SFGAOF sfgao = *rgfInOut;
        PCUITEMID_CHILD pidlConst = pidlSub;

        hr = psf->GetAttributesOf(1, &pidlConst, &sfgao);
        if (SUCCEEDED(hr))
        {
            /* Links into a merged folder would point at one namespace only */
            pns->EnsureReadPropertyBag();
            *rgfInOut &= (pns->m_dwAttributes | (sfgao & pns->m_dwAttributeMask)) & ~SFGAO_CANLINK;
        }
        ILFree(pidlSub);
    }
    return hr;
}

HRESULT
CMergedShellFolder::GetTargetUIObjectOf(
    _In_opt_ IShellFolder *psf,
    _In_opt_ HWND hwnd,
    _In_ UINT cidl,
    _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PIDLIST_ABSOLUTE pidlTarget;
    IShellFolder *psfTarget;
    HRESULT hr;

    *ppv = NULL;
    hr = MergedFolder_GetFolderTargetIDList(psf, &pidlTarget);
    if (FAILED(hr))
        return hr;

    /* Menus act on the real folder so their verbs see the real location */
    hr = SHBindToObject(NULL, pidlTarget, NULL, IID_PPV_ARG(IShellFolder, &psfTarget));
    if (SUCCEEDED(hr))
    {
        hr = psfTarget->GetUIObjectOf(hwnd, cidl, apidl, IID_IContextMenu, NULL, ppv);
        psfTarget->Release();
    }
    ILFree(pidlTarget);
    return hr;
}

HRESULT
CMergedShellFolder::GetContextMenu(
    _In_opt_ HWND hwnd,
    _In_ UINT cidl,
    _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PITEMID_CHILD *apidlCommon;
    PITEMID_CHILD *apidlUser;
    IShellFolder *psfCommon = NULL;
    IShellFolder *psfUser = NULL;
    IContextMenu *pcmCommon = NULL;
    IContextMenu *pcmUser = NULL;
    UINT cCommon = 0;
    UINT cUser = 0;
    HRESULT hr;
    UINT i;

    *ppv = NULL;
    if (cidl == 1 && IsFolder(apidl[0]) && m_fCDBurn)
    {
        DEFCONTEXTMENU dcm = { 0 };

        dcm.hwnd = hwnd;
        dcm.pidlFolder = m_pidl;
        dcm.psf = static_cast<IShellFolder2 *>(this);
        dcm.cidl = 1;
        dcm.apidl = apidl;
        return SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }

    apidlCommon = new PITEMID_CHILD[cidl];
    if (!apidlCommon)
        return E_OUTOFMEMORY;

    apidlUser = new PITEMID_CHILD[cidl];
    if (!apidlUser)
    {
        delete[] apidlCommon;
        return E_OUTOFMEMORY;
    }

    /* Each item is split into the part in the all users locations and the part in the user's */
    for (i = 0; i < cidl; i++)
    {
        IShellFolder *psf;
        PITEMID_CHILD pidlSub;

        if (SUCCEEDED(NamespaceForItem(apidl[i], MFNS_COMMON, MFNS_COMMON, &psf, &pidlSub, NULL,
                                       TRUE)))
        {
            apidlCommon[cCommon++] = pidlSub;
            psfCommon = psf;
        }
        if (SUCCEEDED(NamespaceForItem(apidl[i], MFNS_COMMON, 0, &psf, &pidlSub, NULL, TRUE)))
        {
            apidlUser[cUser++] = pidlSub;
            psfUser = psf;
        }
    }

    if (cCommon)
    {
        GetTargetUIObjectOf(psfCommon, hwnd, cCommon, (PCUITEMID_CHILD_ARRAY)apidlCommon,
                            IID_IContextMenu, (void **)&pcmCommon);
    }
    if (cUser)
    {
        GetTargetUIObjectOf(psfUser, hwnd, cUser, (PCUITEMID_CHILD_ARRAY)apidlUser,
                            IID_IContextMenu, (void **)&pcmUser);
    }

    if (IsFolder(apidl[0]) && pcmCommon && pcmUser)
        hr = CMergedFldrContextMenu_CreateInstance(hwnd, pcmCommon, pcmUser, riid, ppv);
    else if (pcmUser)
        hr = pcmUser->QueryInterface(riid, ppv);
    else if (pcmCommon)
        hr = pcmCommon->QueryInterface(riid, ppv);
    else
        hr = E_FAIL;

    for (i = 0; i < cCommon; i++)
        ILFree(apidlCommon[i]);
    for (i = 0; i < cUser; i++)
        ILFree(apidlUser[i]);
    if (pcmCommon)
        pcmCommon->Release();
    if (pcmUser)
        pcmUser->Release();

    delete[] apidlUser;
    delete[] apidlCommon;
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetUIObjectOf(
    _In_opt_ HWND hwndOwner,
    _In_ UINT cidl,
    _In_reads_opt_(cidl) PCUITEMID_CHILD_ARRAY apidl,
    _In_ REFIID riid,
    _Reserved_ UINT *rgfReserved,
    _Outptr_ void **ppv)
{
    PITEMID_CHILD *apidlSub;
    IShellFolder *psfLast = NULL;
    UINT cSub = 0;
    HRESULT hr;
    UINT i;

    *ppv = NULL;
    if (IsEqualIID(riid, IID_IContextMenu))
        return GetContextMenu(hwndOwner, cidl, apidl, riid, ppv);

    if (IsEqualIID(riid, IID_IDropTarget) && IsFolder(apidl[0]))
    {
        IShellFolder *psfChild;

        hr = BindToObject(apidl[0], NULL, IID_PPV_ARG(IShellFolder, &psfChild));
        if (SUCCEEDED(hr))
        {
            hr = psfChild->CreateViewObject(hwndOwner, riid, ppv);
            psfChild->Release();
        }
        return hr;
    }

    /* Folder thumbnails would come from the folder itself, which has no image extractor here */
    if ((IsEqualIID(riid, IID_IMergedExtractImage) || IsEqualIID(riid, IID_IMergedExtractLogo) ||
         IsEqualIID(riid, IID_IMergedThumbnailProvider)) && IsFolder(apidl[0]))
    {
        return E_NOINTERFACE;
    }

    if (IsEqualIID(riid, IID_IDataObject))
        return SHCreateFileDataObject(m_pidl, cidl, apidl, NULL, (IDataObject **)ppv);

    apidlSub = new PITEMID_CHILD[cidl];
    if (!apidlSub)
        return E_OUTOFMEMORY;

    hr = E_FAIL;
    for (i = 0; i < cidl; i++)
    {
        IShellFolder *psf;
        PITEMID_CHILD pidlSub;

        if (SUCCEEDED(NamespaceForItem(apidl[i], MFNS_UIOBJECT, MFNS_UIOBJECT, &psf, &pidlSub, NULL,
                                       FALSE)))
        {
            apidlSub[cSub++] = pidlSub;
            psfLast = psf;
        }
    }

    if (cSub)
        hr = psfLast->GetUIObjectOf(hwndOwner, cSub, (PCUITEMID_CHILD_ARRAY)apidlSub, riid, NULL,
                                    ppv);

    for (i = 0; i < cSub; i++)
        ILFree(apidlSub[i]);
    delete[] apidlSub;
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetDisplayNameOf(
    _In_opt_ PCUITEMID_CHILD pidl,
    _In_ SHGDNF uFlags,
    _Out_ STRRET *pName)
{
    PITEMID_CHILD pidlSub;
    IShellFolder *psf;
    HRESULT hr;

    hr = NamespaceForItem(pidl, MFNS_DISPLAY, MFNS_DISPLAY, &psf, &pidlSub, NULL, FALSE);
    if (FAILED(hr))
    {
        IShellFolder2 *psf2;

        /* The folder itself is parsed like its display namespace */
        if (!IsSelfID(1, &pidl) ||
            (uFlags & (SHGDN_FORPARSING | SHGDN_FORADDRESSBAR | SHGDN_INFOLDER)) !=
            SHGDN_FORPARSING)
        {
            return E_INVALIDARG;
        }

        hr = GetFolder2(NULL, NULL, &psf2);
        if (SUCCEEDED(hr))
        {
            hr = psf2->GetDisplayNameOf(NULL, uFlags, pName);
            psf2->Release();
        }
        return hr;
    }

    hr = psf->GetDisplayNameOf(pidlSub, uFlags, pName);
    if (SUCCEEDED(hr))
    {
        hr = S_OK;

        /* An offset name points into the sub ID, which is freed below */
        if (pName->uType == STRRET_OFFSET)
        {
            LPCSTR pszName = (LPCSTR)pidlSub + pName->uOffset;
            int cch = lstrlenA(pszName) + 1;
            LPWSTR pszWide = (LPWSTR)CoTaskMemAlloc(cch * sizeof(WCHAR));

            if (pszWide)
            {
                SHAnsiToUnicode(pszName, pszWide, cch);
                pName->uType = STRRET_WSTR;
                pName->pOleStr = pszWide;
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }
        }
    }

    ILFree(pidlSub);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::SetNameOf(
    _In_opt_ HWND hwnd,
    _In_ PCUITEMID_CHILD pidl,
    _In_ LPCWSTR pszName,
    _In_ SHGDNF uFlags,
    _Outptr_opt_ PITEMID_CHILD *ppidlOut)
{
    PITEMID_CHILD pidlNewSub = NULL;
    PITEMID_CHILD pidlSub;
    WCHAR szNewName[MAX_PATH];
    IShellFolder *psf;
    HRESULT hr;

    if (ppidlOut)
        *ppidlOut = NULL;
    szNewName[0] = UNICODE_NULL;

    hr = NamespaceForItem(pidl, MFNS_DISPLAY, MFNS_DISPLAY, &psf, &pidlSub, NULL, FALSE);
    if (FAILED(hr))
        return hr;

    hr = psf->SetNameOf(hwnd, pidlSub, pszName, uFlags, ppidlOut ? &pidlNewSub : NULL);
    if (pidlNewSub)
    {
        DisplayNameOfW(psf, pidlNewSub, SHGDN_INFOLDER | SHGDN_FORPARSING, szNewName,
                       _countof(szNewName));
    }
    ILFree(pidlSub);
    ILFree(pidlNewSub);

    /* The renamed item is found again through all namespaces */
    if (SUCCEEDED(hr) && ppidlOut && szNewName[0])
        hr = ParseNameInFolder(static_cast<IShellFolder2 *>(this), NULL, NULL, szNewName, ppidlOut);
    return hr;
}

/* IShellFolder2 */

STDMETHODIMP
CMergedShellFolder::GetDefaultSearchGUID(
    _Out_ GUID *pguid)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::EnumSearches(
    _Outptr_ IEnumExtraSearch **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::GetDefaultColumn(
    _In_ DWORD dwRes,
    _Out_ ULONG *pSort,
    _Out_ ULONG *pDisplay)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::GetDefaultColumnState(
    _In_ UINT iColumn,
    _Out_ SHCOLSTATEF *pcsFlags)
{
    IShellFolder2 *psf;
    HRESULT hr;

    if (m_uWhichFolderColumn != (UINT)-1 && iColumn == m_uWhichFolderColumn)
    {
        *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT | SHCOLSTATE_SLOW;
        return S_OK;
    }

    hr = GetFolder2(NULL, NULL, &psf);
    if (SUCCEEDED(hr))
    {
        hr = psf->GetDefaultColumnState(iColumn, pcsFlags);
        psf->Release();
    }
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetDetailsEx(
    _In_ PCUITEMID_CHILD pidl,
    _In_ const SHCOLUMNID *pscid,
    _Out_ VARIANT *pv)
{
    PITEMID_CHILD pidlSub;
    IShellFolder2 *psf;
    HRESULT hr;

    if (IsEqualPropertyKey(*pscid, PKEY_MergedWhichFolder))
    {
        WCHAR szLocation[MAX_PATH];

        hr = GetWhichFolderColumn(pidl, szLocation);
        if (SUCCEEDED(hr))
        {
            V_VT(pv) = VT_BSTR;
            V_BSTR(pv) = SysAllocString(szLocation);
            if (!V_BSTR(pv))
            {
                VariantInit(pv);
                hr = E_OUTOFMEMORY;
            }
        }
        return hr;
    }

    hr = GetFolder2(pidl, &pidlSub, &psf);
    if (FAILED(hr))
        return hr;

    hr = psf->GetDetailsEx(pidlSub, pscid, pv);

    /* Parsing in this bind context should come back through the merged child */
    if (SUCCEEDED(hr) && IsEqualPropertyKey(*pscid, PKEY_MergedParsingBindContext) &&
        V_VT(pv) == VT_UNKNOWN && V_UNKNOWN(pv))
    {
        IBindCtx *pbc;

        if (SUCCEEDED(V_UNKNOWN(pv)->QueryInterface(IID_PPV_ARG(IBindCtx, &pbc))))
        {
            IShellItem *psi;

            if (SUCCEEDED(SHCreateItemWithParent(m_pidl, static_cast<IShellFolder2 *>(this), pidl,
                                                 IID_PPV_ARG(IShellItem, &psi))))
            {
                pbc->RegisterObjectParam((LPOLESTR)STR_MERGEDFOLDER_CHILDITEM, psi);
                psi->Release();
            }
            pbc->Release();
        }
    }

    psf->Release();
    ILFree(pidlSub);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetDetailsOf(
    _In_opt_ PCUITEMID_CHILD pidl,
    _In_ UINT iColumn,
    _Out_ SHELLDETAILS *psd)
{
    HRESULT hr = E_FAIL;

    /* The location column follows the last column of the display namespace */
    if (m_uWhichFolderColumn == (UINT)-1 || iColumn != m_uWhichFolderColumn)
    {
        PITEMID_CHILD pidlSub;
        IShellFolder2 *psf;

        hr = GetFolder2(pidl, &pidlSub, &psf);
        if (SUCCEEDED(hr))
        {
            hr = psf->GetDetailsOf(pidlSub, iColumn, psd);
            psf->Release();
            ILFree(pidlSub);
            if (SUCCEEDED(hr))
                return hr;
        }

        if (m_uWhichFolderColumn == (UINT)-1)
            m_uWhichFolderColumn = iColumn;
    }

    if (m_uWhichFolderColumn != (UINT)-1 && iColumn == m_uWhichFolderColumn)
    {
        WCHAR szText[MAX_PATH];

        psd->str.uType = STRRET_CSTR;
        psd->str.cStr[0] = '\0';
        if (!pidl)
        {
            psd->fmt = LVCFMT_LEFT;
            psd->cxChar = 20;
            if (!LoadStringW(shell32_hInstance, IDS_SHV_COLUMN_LOCATION, szText, _countof(szText)))
                return E_FAIL;
            return SHSetStrRet(&psd->str, szText);
        }

        if (GetWrap(pidl))
        {
            hr = GetWhichFolderColumn(pidl, szText);
            if (SUCCEEDED(hr))
                return SHSetStrRet(&psd->str, szText);
        }
    }
    return hr;
}

STDMETHODIMP
CMergedShellFolder::MapColumnToSCID(
    _In_ UINT iColumn,
    _Out_ SHCOLUMNID *pscid)
{
    IShellFolder2 *psf;
    HRESULT hr;

    if (m_uWhichFolderColumn != (UINT)-1 && iColumn == m_uWhichFolderColumn)
    {
        *pscid = PKEY_MergedWhichFolder;
        return S_OK;
    }

    hr = GetFolder2(NULL, NULL, &psf);
    if (SUCCEEDED(hr))
    {
        hr = psf->MapColumnToSCID(iColumn, pscid);
        psf->Release();
    }
    return hr;
}

/* IStorage */

HRESULT
CMergedShellFolder::ParseAndVerify(
    _In_ LPCWSTR pszName,
    _In_opt_ IBindCtx *pbc,
    _Outptr_ PITEMID_CHILD *ppidl)
{
    PIDLIST_RELATIVE pidl;
    HRESULT hr;

    *ppidl = NULL;
    hr = ParseDisplayName(NULL, pbc, (LPWSTR)pszName, NULL, &pidl, NULL);
    if (FAILED(hr))
        return hr;

    /* A storage element is a direct child */
    if (pidl && pidl->mkid.cb && ILGetNext(pidl)->mkid.cb)
    {
        ILFree(pidl);
        return E_FAIL;
    }

    *ppidl = (PITEMID_CHILD)pidl;
    return hr;
}

HRESULT
CMergedShellFolder::BindByName(
    _In_ LPCWSTR pszName,
    _In_ DWORD grfMode,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PITEMID_CHILD pidl;
    IBindCtx *pbc;
    HRESULT hr;

    *ppv = NULL;
    hr = ParseAndVerify(pszName, NULL, &pidl);
    if (FAILED(hr))
        return hr;

    hr = CreateBindCtxWithMode(grfMode, &pbc);
    if (SUCCEEDED(hr))
    {
        hr = BindToObject(pidl, pbc, riid, ppv);
        pbc->Release();
    }
    ILFree(pidl);
    return hr;
}

HRESULT
CMergedShellFolder::CreateHelper(
    _In_ LPCWSTR pszName,
    _In_ DWORD grfMode,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PITEMID_CHILD pidl;
    HRESULT hr;

    *ppv = NULL;
    if (!(grfMode & STGM_CREATE) && SUCCEEDED(ParseAndVerify(pszName, NULL, &pidl)))
    {
        ILFree(pidl);
        return MapWin32ErrorToSTG(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    }

    IBindCtx *pbc;

    hr = CreateBindCtxWithMode(STGM_CREATE, &pbc);
    if (SUCCEEDED(hr))
    {
        hr = ParseAndVerify(pszName, pbc, &pidl);
        if (SUCCEEDED(hr))
        {
            hr = StgCreate(pidl, grfMode, riid, ppv);
            ILFree(pidl);
        }
        pbc->Release();
    }
    return MapWin32ErrorToSTG(hr);
}

HRESULT
CMergedShellFolder::GetDestinationStorage(
    _In_ DWORD grfMode,
    _Outptr_ IStorage **ppstg)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    *ppstg = NULL;
    hr = FindNamespace(MFNS_DEFAULT, MFNS_DEFAULT, &pns);
    if (FAILED(hr))
        hr = GetNamespace(0, &pns);

    if (SUCCEEDED(hr))
    {
        IShellFolder *psfParent;
        PCUITEMID_CHILD pidlLast;

        /* New items go to the default namespace when it exists on disk */
        hr = SHBindToParent(pns->m_pidl, IID_PPV_ARG(IShellFolder, &psfParent), &pidlLast);
        if (SUCCEEDED(hr))
        {
            SFGAOF sfgao = SFGAO_VALIDATE;

            hr = psfParent->GetAttributesOf(1, &pidlLast, &sfgao);
            psfParent->Release();
        }

        if (SUCCEEDED(hr))
        {
            IBindCtx *pbc;

            hr = CreateBindCtxWithMode(grfMode, &pbc);
            if (SUCCEEDED(hr))
            {
                hr = SHBindToObject(NULL, pns->m_pidl, pbc, IID_PPV_ARG(IStorage, ppstg));
                pbc->Release();
                if (SUCCEEDED(hr))
                    return hr;
            }
        }
    }

    /* Otherwise the folder is made in the parent's destination */
    if (m_pParent)
    {
        IStorage *pstgParent;

        hr = m_pParent->GetDestinationStorage(grfMode, &pstgParent);
        if (SUCCEEDED(hr))
        {
            WCHAR szName[MAX_PATH];

            hr = DisplayNameOfW(static_cast<IShellFolder2 *>(m_pParent),
                                (PCUITEMID_CHILD)ILFindLastID(m_pidl), SHGDN_INFOLDER, szName,
                                _countof(szName));
            if (SUCCEEDED(hr))
                hr = pstgParent->CreateStorage(szName, STGM_READWRITE, 0, 0, ppstg);
            pstgParent->Release();
        }
    }
    return hr;
}

HRESULT
CMergedShellFolder::StgCreate(
    _In_ PCUITEMID_CHILD pidl,
    _In_ DWORD grfMode,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    WCHAR szName[MAX_PATH];
    HRESULT hr;

    *ppv = NULL;
    if (!m_pstgDest)
    {
        hr = GetDestinationStorage(grfMode, &m_pstgDest);
        if (FAILED(hr))
            return hr;
    }

    hr = DisplayNameOfW(static_cast<IShellFolder2 *>(this), pidl, SHGDN_INFOLDER | SHGDN_FORPARSING,
                        szName, _countof(szName));
    if (FAILED(hr))
        return hr;

    if (IsEqualIID(riid, IID_IStorage))
        return m_pstgDest->CreateStorage(szName, grfMode, 0, 0, (IStorage **)ppv);
    if (IsEqualIID(riid, IID_IStream))
        return m_pstgDest->CreateStream(szName, grfMode, 0, 0, (IStream **)ppv);
    return E_INVALIDARG;
}

HRESULT
CMergedShellFolder::DeleteItemByIDList(
    _In_ PCUITEMID_CHILD pidl)
{
    CMergedFldrNamespace *pns;
    PITEMID_CHILD pidlSub;
    IStorage *pstg;
    HRESULT hr;

    hr = NamespaceForItem(pidl, MFNS_DEFAULT, MFNS_DEFAULT, NULL, &pidlSub, &pns, FALSE);
    if (FAILED(hr))
        return hr;

    hr = pns->m_psf->QueryInterface(IID_PPV_ARG(IStorage, &pstg));
    if (SUCCEEDED(hr))
    {
        WCHAR szName[MAX_PATH];

        hr = DisplayNameOfW(pns->m_psf, pidlSub, SHGDN_INFOLDER | SHGDN_FORPARSING, szName,
                            _countof(szName));
        if (SUCCEEDED(hr))
            hr = pstg->DestroyElement(szName);
        pstg->Release();
    }
    ILFree(pidlSub);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::CreateStream(
    _In_ LPCOLESTR pwcsName,
    _In_ DWORD grfMode,
    _In_ DWORD reserved1,
    _In_ DWORD reserved2,
    _Outptr_ IStream **ppstm)
{
    return CreateHelper(pwcsName, grfMode, IID_IStream, (void **)ppstm);
}

STDMETHODIMP
CMergedShellFolder::OpenStream(
    _In_ LPCOLESTR pwcsName,
    _Reserved_ void *reserved1,
    _In_ DWORD grfMode,
    _In_ DWORD reserved2,
    _Outptr_ IStream **ppstm)
{
    return MapWin32ErrorToSTG(BindByName(pwcsName, grfMode, IID_IStream, (void **)ppstm));
}

STDMETHODIMP
CMergedShellFolder::CreateStorage(
    _In_ LPCOLESTR pwcsName,
    _In_ DWORD grfMode,
    _In_ DWORD reserved1,
    _In_ DWORD reserved2,
    _Outptr_ IStorage **ppstg)
{
    return CreateHelper(pwcsName, grfMode, IID_IStorage, (void **)ppstg);
}

STDMETHODIMP
CMergedShellFolder::OpenStorage(
    _In_opt_ LPCOLESTR pwcsName,
    _In_opt_ IStorage *pstgPriority,
    _In_ DWORD grfMode,
    _In_opt_ SNB snbExclude,
    _In_ DWORD reserved,
    _Outptr_ IStorage **ppstg)
{
    return MapWin32ErrorToSTG(BindByName(pwcsName, grfMode, IID_IStorage, (void **)ppstg));
}

STDMETHODIMP
CMergedShellFolder::CopyTo(
    _In_ DWORD ciidExclude,
    _In_reads_opt_(ciidExclude) const IID *rgiidExclude,
    _In_opt_ SNB snbExclude,
    _In_ IStorage *pstgDest)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::MoveElementTo(
    _In_ LPCOLESTR pwcsName,
    _In_ IStorage *pstgDest,
    _In_ LPCOLESTR pwcsNewName,
    _In_ DWORD grfFlags)
{
    return StgMoveElementTo(static_cast<IShellFolder2 *>(this), static_cast<IStorage *>(this),
                            pwcsName, pstgDest, pwcsNewName, grfFlags);
}

STDMETHODIMP
CMergedShellFolder::Commit(
    _In_ DWORD grfCommitFlags)
{
    return S_OK;
}

STDMETHODIMP
CMergedShellFolder::Revert()
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::EnumElements(
    _Reserved_ DWORD reserved1,
    _Reserved_ void *reserved2,
    _Reserved_ DWORD reserved3,
    _Outptr_ IEnumSTATSTG **ppenum)
{
    *ppenum = new CShellFolderStorageEnum(static_cast<IShellFolder2 *>(this));
    return *ppenum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CMergedShellFolder::DestroyElement(
    _In_ LPCOLESTR pwcsName)
{
    PITEMID_CHILD pidl;
    HRESULT hr;

    hr = ParseAndVerify(pwcsName, NULL, &pidl);
    if (SUCCEEDED(hr))
    {
        hr = DeleteItemByIDList(pidl);
        ILFree(pidl);
    }
    return hr;
}

STDMETHODIMP
CMergedShellFolder::RenameElement(
    _In_ LPCOLESTR pwcsOldName,
    _In_ LPCOLESTR pwcsNewName)
{
    PITEMID_CHILD pidl;
    HRESULT hr;

    hr = ParseAndVerify(pwcsOldName, NULL, &pidl);
    if (SUCCEEDED(hr))
    {
        hr = SetNameOf(NULL, pidl, pwcsNewName, SHGDN_FORPARSING, NULL);
        ILFree(pidl);
    }
    return hr;
}

STDMETHODIMP
CMergedShellFolder::SetElementTimes(
    _In_opt_ LPCOLESTR pwcsName,
    _In_opt_ const FILETIME *pctime,
    _In_opt_ const FILETIME *patime,
    _In_opt_ const FILETIME *pmtime)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::SetClass(
    _In_ REFCLSID clsid)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::SetStateBits(
    _In_ DWORD grfStateBits,
    _In_ DWORD grfMask)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CMergedShellFolder::Stat(
    _Out_ STATSTG *pstatstg,
    _In_ DWORD grfStatFlag)
{
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    ZeroMemory(pstatstg, sizeof(*pstatstg));
    hr = SHGetIDListFromObject(static_cast<IShellFolder2 *>(this), &pidl);
    if (FAILED(hr))
        return hr;

    IShellFolder *psfParent;
    PCUITEMID_CHILD pidlLast;

    hr = SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psfParent), &pidlLast);
    if (SUCCEEDED(hr))
    {
        WCHAR szName[MAX_PATH];

        hr = DisplayNameOfW(psfParent, pidlLast, SHGDN_INFOLDER | SHGDN_FORPARSING, szName,
                            _countof(szName));
        if (SUCCEEDED(hr))
        {
            pstatstg->grfMode = 0;
            if (!(grfStatFlag & STATFLAG_NONAME))
                hr = SHStrDupW(szName, &pstatstg->pwcsName);
        }
        psfParent->Release();
    }
    ILFree(pidl);
    return hr;
}

/* IAugmentedMergedFolder */

STDMETHODIMP
CMergedShellFolder::AddNameSpace(
    _In_opt_ const GUID *pguidObject,
    _In_opt_ IShellFolder *psf,
    _In_opt_ PCIDLIST_ABSOLUTE pidl,
    _In_ DWORD dwFlags,
    _In_ UINT uGroupID)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;
    int i;

    /* Adding a location again only updates it */
    if (pidl)
    {
        for (i = 0; i < GetNamespaceCount(); i++)
        {
            pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
            if (SHCompareIDsFull(SHCIDS_CANONICALONLY, pns->m_pidl, pidl, NULL) == S_OK)
            {
                return pns->SetNamespace(static_cast<IShellFolder2 *>(this), pguidObject, psf, pidl,
                                         dwFlags, uGroupID);
            }
        }
    }

    pns = new CMergedFldrNamespace(!m_fCDBurn);
    if (!pns)
        return E_OUTOFMEMORY;

    hr = pns->SetNamespace(static_cast<IShellFolder2 *>(this), pguidObject, psf, pidl, dwFlags,
                           uGroupID);
    if (SUCCEEDED(hr))
        hr = SimpleAddNamespace(pns);
    if (FAILED(hr))
        delete pns;
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetNameSpaceID(
    _In_ PCUITEMID_CHILD pidl,
    _Out_ GUID *pguidOut)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    if (!pidl)
        return E_INVALIDARG;

    hr = GetSubPidl(pidl, 0, NULL, NULL, NULL, &pns);
    if (SUCCEEDED(hr))
        *pguidOut = pns->m_guid;
    return hr;
}

STDMETHODIMP
CMergedShellFolder::QueryNameSpace(
    _In_ ULONG iNamespace,
    _Out_opt_ GUID *pguidOut,
    _Outptr_opt_ IShellFolder **ppsf)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    hr = ((LONG)iNamespace < 0) ? E_INVALIDARG : GetNamespace(iNamespace, &pns);
    if (FAILED(hr))
        return hr;

    if (pguidOut)
        *pguidOut = pns->m_guid;
    if (ppsf)
    {
        *ppsf = pns->m_psf;
        if (*ppsf)
            (*ppsf)->AddRef();
    }
    return S_OK;
}

STDMETHODIMP
CMergedShellFolder::EnumNameSpace(
    _In_ ULONG iNamespace,
    _Out_ ULONG *piNamespace)
{
    /* Asking with -1 returns the count */
    if (iNamespace == (ULONG)-1)
        return (HRESULT)LOWORD(GetNamespaceCount());

    if (iNamespace >= (ULONG)GetNamespaceCount())
        return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);

    *piNamespace = iNamespace;
    return S_OK;
}

STDMETHODIMP
CMergedShellFolder::UnWrapIDList(
    _In_ PCUITEMID_CHILD pidl,
    _In_ LONG cMax,
    _Out_writes_opt_(cMax) IShellFolder **apsf,
    _Out_writes_opt_(cMax) PIDLIST_ABSOLUTE *apidlFolder,
    _Out_writes_opt_(cMax) PITEMID_CHILD *apidlItem,
    _Out_opt_ LONG *pcFetched)
{
    HRESULT hr = S_OK;
    LONG i;

    if (cMax <= 0)
        return E_INVALIDARG;

    for (i = 0; i < cMax && SUCCEEDED(hr); i++)
    {
        CMergedFldrNamespace *pns;
        PITEMID_CHILD pidlSub;

        if (FAILED(GetSubPidl(pidl, i, NULL, NULL, &pidlSub, &pns)))
            break;

        if (apsf)
        {
            apsf[i] = pns->m_psf;
            if (apsf[i])
                apsf[i]->AddRef();
        }
        if (apidlFolder)
        {
            apidlFolder[i] = pns->m_pidl ? ILClone(pns->m_pidl) : NULL;
            hr = pns->m_pidl ? (apidlFolder[i] ? S_OK : E_OUTOFMEMORY) : E_INVALIDARG;
        }
        if (apidlItem)
        {
            apidlItem[i] = NULL;
            if (SUCCEEDED(hr))
                hr = CloneFirstID(pidlSub, &apidlItem[i]);
        }
        ILFree(pidlSub);
    }

    if (FAILED(hr))
    {
        LONG j;

        for (j = 0; j < i; j++)
        {
            if (apsf && apsf[j])
            {
                apsf[j]->Release();
                apsf[j] = NULL;
            }
            if (apidlFolder)
                ILFree(apidlFolder[j]);
            if (apidlItem)
                ILFree(apidlItem[j]);
        }
        return hr;
    }

    if (pcFetched)
        *pcFetched = i;
    return (i == cMax) ? S_OK : S_FALSE;
}

STDMETHODIMP
CMergedShellFolder::QueryNameSpace2(
    _In_ ULONG iNamespace,
    _Inout_ MERGEDNAMESPACEINFO *pqnsi)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    if (pqnsi->cbSize != sizeof(*pqnsi) ||
        (pqnsi->dwMask & ~(MFQNS_FLAGS | MFQNS_FOLDER | MFQNS_GUID | MFQNS_IDLIST | MFQNS_GROUPID)))
    {
        return E_INVALIDARG;
    }

    hr = ((LONG)iNamespace < 0) ? E_INVALIDARG : GetNamespace(iNamespace, &pns);
    if (FAILED(hr))
        return hr;

    if (pqnsi->dwMask & MFQNS_IDLIST)
    {
        pqnsi->pidl = pns->m_pidl ? ILClone(pns->m_pidl) : NULL;
        if (!pns->m_pidl)
            return E_INVALIDARG;
        if (!pqnsi->pidl)
            return E_OUTOFMEMORY;
    }

    if (pqnsi->dwMask & MFQNS_FLAGS)
        pqnsi->dwFlags = pns->m_dwFlags;
    if (pqnsi->dwMask & MFQNS_FOLDER)
    {
        pqnsi->psf = pns->m_psf;
        if (pqnsi->psf)
            pqnsi->psf->AddRef();
    }
    if (pqnsi->dwMask & MFQNS_GUID)
        pqnsi->guidObject = pns->m_guid;
    if (pqnsi->dwMask & MFQNS_GROUPID)
        pqnsi->uGroupID = pns->m_uGroupID;
    return S_OK;
}

STDMETHODIMP
CMergedShellFolder::GetNameSpaceGroupID(
    _In_ PCUITEMID_CHILD pidl,
    _Out_ UINT *puGroupID)
{
    CMergedFldrNamespace *pns;
    HRESULT hr;

    if (!pidl || !puGroupID)
        return E_INVALIDARG;

    hr = GetSubPidl(pidl, 0, NULL, NULL, NULL, &pns);
    if (SUCCEEDED(hr))
        *puGroupID = pns->m_uGroupID;
    return hr;
}

STDMETHODIMP
CMergedShellFolder::GetFlagsForChild(
    _In_ PCUITEMID_CHILD pidl,
    _In_ REFGUID rguidObject,
    _Out_ DWORD *pdwFlags)
{
    CMergedFldrNamespace *pns;
    WORD wFlags;
    UINT i;

    *pdwFlags = 0;
    for (i = 0; SUCCEEDED(GetSubPidl(pidl, i, NULL, &wFlags, NULL, &pns)); i++)
    {
        if (IsEqualGUID(pns->m_guid, rguidObject))
        {
            if (wFlags & MERGEDENTRY_IMPLICIT)
                *pdwFlags |= MFCF_IMPLICIT;
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

/* IPersistFolder2 */

STDMETHODIMP
CMergedShellFolder::GetClassID(
    _Out_ CLSID *pclsid)
{
    *pclsid = m_clsid;
    return S_OK;
}

STDMETHODIMP
CMergedShellFolder::Initialize(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    ILFree(m_pidl);
    m_pidl = pidl ? ILClone(pidl) : NULL;
    return (pidl && !m_pidl) ? E_OUTOFMEMORY : S_OK;
}

STDMETHODIMP
CMergedShellFolder::GetCurFolder(
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    if (!m_pidl)
    {
        *ppidl = NULL;
        return S_FALSE;
    }

    *ppidl = ILClone(m_pidl);
    return *ppidl ? S_OK : E_OUTOFMEMORY;
}

/* IFolderType */

STDMETHODIMP
CMergedShellFolder::GetFolderType(
    _Out_ FOLDERTYPEID *pftid)
{
    *pftid = FOLDERTYPEID_Generic;
    return S_OK;
}

/* IShellService */

STDMETHODIMP
CMergedShellFolder::SetOwner(
    _In_opt_ IUnknown *punkOwner)
{
    if (m_hdpaNamespaces)
        DPA_EnumCallback(m_hdpaNamespaces, SetOwnerCB, punkOwner);
    return S_OK;
}

/* ITranslateShellChangeNotify */

BOOL
CMergedShellFolder::IsChildIDInternal(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ BOOL fImmediate,
    _Out_opt_ int *piNamespace)
{
    PCIDLIST_ABSOLUTE pidlDeepest = NULL;
    BOOL fChild = FALSE;
    int i;

    if (!pidl)
        return FALSE;

    /* With several matches the deepest namespace owns the item */
    for (i = 0; i < GetNamespaceCount() && (!fChild || piNamespace); i++)
    {
        CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);

        if (!ILIsParent(pns->m_pidl, pidl, fImmediate))
            continue;

        fChild = TRUE;
        if (!pidlDeepest || ILIsParent(pidlDeepest, pns->m_pidl, FALSE))
        {
            pidlDeepest = pns->m_pidl;
            if (piNamespace)
                *piNamespace = i;
        }
    }
    return fChild;
}

HRESULT
CMergedShellFolder::ForceParseDisplayName(
    _In_ PCIDLIST_ABSOLUTE pidlAbs,
    _In_ LPCWSTR pszName,
    _In_ BOOL fForce,
    _In_ BOOL fFolder,
    _Out_ BOOL *pfMerged,
    _Outptr_ PITEMID_CHILD *ppidlWrap)
{
    IBindCtx *pbcFileSys = NULL;
    CMergedFldrNamespace *pnsOwner = NULL;
    int iOwner = -1;
    HRESULT hr = S_OK;
    int i;

    *ppidlWrap = NULL;
    *pfMerged = FALSE;

    /* The item may be gone already, so the owner namespace parses it from bind data */
    if (fForce)
    {
        WIN32_FIND_DATAW wfd = { 0 };

        wfd.dwFileAttributes = fFolder ? FILE_ATTRIBUTE_DIRECTORY : 0;
        hr = IFileSystemBindData_Constructor(&wfd, &pbcFileSys);
    }

    if (SUCCEEDED(hr))
    {
        IsChildIDInternal(pidlAbs, FALSE, &iOwner);
        if (iOwner != -1)
            GetNamespace(iOwner, &pnsOwner);

        /* Only namespaces that can hold what the owner holds are asked */
        for (i = 0; SUCCEEDED(hr) && iOwner != -1 && i < GetNamespaceCount(); i++)
        {
            CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
            IBindCtx *pbc = NULL;
            PITEMID_CHILD pidlSub;

            if (i == iOwner)
            {
                pbc = pbcFileSys;
            }
            else if (pns != pnsOwner && m_fHasGroupedNamespaces && pnsOwner &&
                     (pnsOwner->m_dwFlags & MFNS_GROUPED) &&
                     pnsOwner->m_uGroupID != pns->m_uGroupID)
            {
                continue;
            }

            if (SUCCEEDED(ParseNameInFolder(pns->m_psf, NULL, pbc, pszName, &pidlSub)))
            {
                if (*ppidlWrap)
                    *pfMerged = TRUE;
                hr = WrapAddIDList(pidlSub, i, 0, ppidlWrap);
                ILFree(pidlSub);
            }
        }

        if (*ppidlWrap)
        {
            if (fForce)
                ((PMERGEDWRAP)*ppidlWrap)->wFlags |= MERGEDWRAP_SIMPLE;
        }
        else
        {
            hr = E_FAIL;
        }
    }

    if (pbcFileSys)
        pbcFileSys->Release();
    return hr;
}

HRESULT
CMergedShellFolder::AbsPidlToAbsWrap(
    _In_ CMergedFldrNamespace *pns,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ BOOL fForce,
    _In_ BOOL fFolder,
    _Out_ BOOL *pfMerged,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlOut)
{
    PCUIDLIST_RELATIVE pidlRel = ILIsParent(pns->m_pidl, pidl, FALSE) ?
        ILFindChild(pns->m_pidl, pidl) : NULL;
    PIDLIST_ABSOLUTE pidlResult;
    CMergedShellFolder *pLevel;
    IShellFolder *psfNamespace;
    IBindCtx *pbc;
    HRESULT hrBind = S_OK;
    HRESULT hr;

    *pfMerged = FALSE;
    *ppidlOut = NULL;
    if (!m_pidl)
        return E_INVALIDARG;

    pidlResult = ILClone(m_pidl);
    if (!pidlResult)
        return E_OUTOFMEMORY;

    hr = CreateSkipBindCtx(&pbc);
    if (FAILED(hr))
    {
        ILFree(pidlResult);
        return hr;
    }

    pLevel = this;
    pLevel->AddRef();
    psfNamespace = pns->m_psf;
    psfNamespace->AddRef();

    if (!pidlRel || !pidlRel->mkid.cb)
        *pfMerged = TRUE;

    /* Walk down the namespace path, wrapping each level in the merged folder of that level */
    while (SUCCEEDED(hr) && SUCCEEDED(hrBind) && pidlRel && pidlRel->mkid.cb)
    {
        PITEMID_CHILD pidlFirst;
        WCHAR szName[MAX_PATH];

        hr = CloneFirstID(pidlRel, &pidlFirst);
        if (FAILED(hr))
            break;

        hr = DisplayNameOfW(psfNamespace, pidlFirst, SHGDN_INFOLDER | SHGDN_FORPARSING, szName,
                            _countof(szName));
        if (SUCCEEDED(hr))
        {
            PCUIDLIST_RELATIVE pidlNext = ILGetNext(pidlRel);
            BOOL fLevelFolder = (pidlNext && pidlNext->mkid.cb) || fFolder;
            PITEMID_CHILD pidlWrap;

            hr = pLevel->ForceParseDisplayName(pidl, szName, fForce, fLevelFolder, pfMerged,
                                               &pidlWrap);
            if (SUCCEEDED(hr))
            {
                if (IsFolder(pidlWrap))
                    AddAllOtherNamespacesIfNeeded(&pidlWrap);

                hr = SHILAppend((LPITEMIDLIST)pidlWrap, (LPITEMIDLIST *)&pidlResult);
                if (SUCCEEDED(hr))
                {
                    IShellFolder *psfNext = NULL;

                    hrBind = psfNamespace->BindToObject(pidlFirst, pbc,
                                                        IID_PPV_ARG(IShellFolder, &psfNext));
                    if (SUCCEEDED(hrBind))
                        SetWrapper(psfNext, static_cast<IShellFolder2 *>(pLevel));
                    psfNamespace->Release();
                    psfNamespace = psfNext;

                    if (SUCCEEDED(hrBind))
                    {
                        CMergedShellFolder *pNext = NULL;

                        /* The merged folder class ID hands out the object itself */
                        hrBind = pLevel->BindToObject(ILFindLastID(pidlResult), pbc,
                                                      CLSID_MergedFolder, (void **)&pNext);
                        pLevel->Release();
                        pLevel = pNext;
                    }
                    pidlRel = pidlNext;
                }
            }
        }
        ILFree(pidlFirst);
    }

    if (pLevel)
        pLevel->Release();
    if (psfNamespace)
        psfNamespace->Release();
    pbc->Release();

    if (FAILED(hr))
    {
        ILFree(pidlResult);
        return hr;
    }

    *ppidlOut = pidlResult;
    return hr;
}

HRESULT
CMergedShellFolder::TransformSingleNotify(
    _In_ CMergedFldrNamespace *pns,
    _In_ UINT iNamespace,
    _In_ BOOL fFolder,
    _In_ BOOL fSecond,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Inout_ LONG *plEvent,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlOut1,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlOut2)
{
    BOOL fMerged;
    HRESULT hr;

    hr = AbsPidlToAbsWrap(pns, pidl, !fSecond, fFolder, &fMerged, ppidlOut1);
    if (FAILED(hr))
        return hr;

    if (!fMerged)
    {
        *ppidlOut2 = NULL;
        return hr;
    }

    /* Other namespaces still hold the item, so the merged child only changes its entries */
    *plEvent = fFolder ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM;
    if (fSecond)
    {
        *ppidlOut2 = *ppidlOut1;
        return WrapRemoveIDListAbs(*ppidlOut2, iNamespace, ppidlOut1);
    }
    return WrapRemoveIDListAbs(*ppidlOut1, iNamespace, ppidlOut2);
}

STDMETHODIMP
CMergedShellFolder::TranslateIDs(
    _Inout_ LONG *plEvent,
    _In_opt_ PCIDLIST_ABSOLUTE pidl1,
    _In_opt_ PCIDLIST_ABSOLUTE pidl2,
    _Out_ PIDLIST_ABSOLUTE *ppidlOut1,
    _Out_ PIDLIST_ABSOLUTE *ppidlOut2,
    _Out_ LONG *plEvent2,
    _Out_ PIDLIST_ABSOLUTE *ppidlOutEvent2Pidl1,
    _Out_ PIDLIST_ABSOLUTE *ppidlOutEvent2Pidl2)
{
    CMergedFldrNamespace *pns1 = NULL;
    CMergedFldrNamespace *pns2 = NULL;
    int iNamespace1 = -1;
    int iNamespace2 = -1;
    BOOL fChild1;
    BOOL fChild2;
    BOOL fFolder = FALSE;
    HRESULT hr = S_OK;

    if (!plEvent || !ppidlOut1 || !ppidlOut2 || !plEvent2 || !ppidlOutEvent2Pidl1 ||
        !ppidlOutEvent2Pidl2)
    {
        return E_INVALIDARG;
    }

    /* Notifications that already name merged children are not translated again */
    if ((pidl1 && GetWrap((PCUITEMID_CHILD)ILFindLastID(pidl1))) ||
        (pidl2 && GetWrap((PCUITEMID_CHILD)ILFindLastID(pidl2))))
    {
        return E_INVALIDARG;
    }

    if (!m_hdpaNamespaces)
        return E_FAIL;

    if (*plEvent == SHCNE_UPDATEIMAGE || *plEvent == SHCNE_EXTENDED_EVENT ||
        *plEvent == SHCNE_ASSOCCHANGED)
    {
        return S_OK;
    }

    *plEvent2 = -1;
    *ppidlOutEvent2Pidl1 = NULL;
    *ppidlOutEvent2Pidl2 = NULL;
    *ppidlOut1 = (PIDLIST_ABSOLUTE)pidl1;
    *ppidlOut2 = (PIDLIST_ABSOLUTE)pidl2;

    fChild1 = IsChildIDInternal(pidl1, FALSE, &iNamespace1);
    if (fChild1)
        GetNamespace(iNamespace1, &pns1);
    fChild2 = IsChildIDInternal(pidl2, FALSE, &iNamespace2);
    if (fChild2)
        GetNamespace(iNamespace2, &pns2);

    if (!fChild1 && !fChild2)
        return E_FAIL;

    switch (*plEvent)
    {
        case SHCNE_MEDIAINSERTED:
        case SHCNE_MEDIAREMOVED:
        case SHCNE_UPDATEDIR:
        case SHCNE_UPDATEITEM:
        {
            BOOL fMerged;

            if (fChild1)
                hr = AbsPidlToAbsWrap(pns1, pidl1, FALSE, FALSE, &fMerged, ppidlOut1);
            break;
        }

        case SHCNE_RENAMEFOLDER:
            fFolder = TRUE;
            /* Fall through */
        case SHCNE_RENAMEITEM:
        {
            LONG *plEventNext = plEvent;
            PIDLIST_ABSOLUTE *ppidlNext1 = ppidlOut1;
            PIDLIST_ABSOLUTE *ppidlNext2 = ppidlOut2;

            /* A rename is a delete of the old child and a create of the new one */
            if (fChild1)
            {
                *plEvent = fFolder ? SHCNE_RMDIR : SHCNE_DELETE;
                hr = TransformSingleNotify(pns1, iNamespace1, fFolder, FALSE, pidl1, plEvent,
                                           ppidlOut1, ppidlOut2);
                if (FAILED(hr))
                    break;

                plEventNext = plEvent2;
                ppidlNext1 = ppidlOutEvent2Pidl1;
                ppidlNext2 = ppidlOutEvent2Pidl2;
            }

            if (fChild2)
            {
                *plEventNext = fFolder ? SHCNE_MKDIR : SHCNE_CREATE;
                hr = TransformSingleNotify(pns2, iNamespace2, fFolder, TRUE, pidl2, plEventNext,
                                           ppidlNext1, ppidlNext2);
            }

            /* Two plain halves are one rename again */
            if (SUCCEEDED(hr) && fChild1 && fChild2 &&
                (*plEvent == SHCNE_RMDIR || *plEvent == SHCNE_DELETE) &&
                (*plEvent2 == SHCNE_MKDIR || *plEvent2 == SHCNE_CREATE))
            {
                *plEvent = fFolder ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM;
                *plEvent2 = -1;
                *ppidlOut2 = *ppidlOutEvent2Pidl1;
                *ppidlOutEvent2Pidl1 = NULL;
            }
            break;
        }

        case SHCNE_MKDIR:
            fFolder = TRUE;
            /* Fall through */
        case SHCNE_CREATE:
            if (fChild1)
            {
                hr = TransformSingleNotify(pns1, iNamespace1, fFolder, TRUE, pidl1, plEvent,
                                           ppidlOut1, ppidlOut2);
            }
            break;

        case SHCNE_RMDIR:
            fFolder = TRUE;
            /* Fall through */
        case SHCNE_DELETE:
            if (fChild1)
            {
                hr = TransformSingleNotify(pns1, iNamespace1, fFolder, FALSE, pidl1, plEvent,
                                           ppidlOut1, ppidlOut2);
            }
            break;
    }
    return hr;
}

STDMETHODIMP
CMergedShellFolder::IsChildID(
    _In_ PCIDLIST_ABSOLUTE pidlKid,
    _In_ BOOL fImmediate)
{
    return IsChildIDInternal(pidlKid, fImmediate, NULL) ? S_OK : S_FALSE;
}

STDMETHODIMP
CMergedShellFolder::IsEqualID(
    _In_opt_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ PCIDLIST_ABSOLUTE pidlParent)
{
    int i;

    for (i = 0; i < GetNamespaceCount(); i++)
    {
        CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);

        if (pidl)
        {
            if (SHCompareIDsFull(SHCIDS_CANONICALONLY, pns->m_pidl, pidl, NULL) == S_OK)
                return S_OK;
        }
        else if (pidlParent && ILIsParent(pidlParent, pns->m_pidl, FALSE))
        {
            return S_OK;
        }
    }
    return S_FALSE;
}

/* IObjectWithBackReferences */

STDMETHODIMP
CMergedShellFolder::RemoveBackReferences()
{
    return S_OK;
}

/* ILocalizableItemParent */

STDMETHODIMP
CMergedShellFolder::GetResourceDisplayName(
    _In_ PCUITEMID_CHILD pidl,
    _Out_writes_(cch) LPWSTR pszResource,
    _In_ UINT cch,
    _Out_opt_ int *piIndex)
{
    CMergedFldrNamespace *pns;
    ILocalizableItemParent *plip;
    PITEMID_CHILD pidlSub;
    HRESULT hr;

    hr = NamespaceForItem(pidl, MFNS_DEFAULT, MFNS_DEFAULT, NULL, &pidlSub, &pns, FALSE);
    if (FAILED(hr))
        return hr;

    hr = pns->m_psf->QueryInterface(IID_PPV_ARG(ILocalizableItemParent, &plip));
    if (SUCCEEDED(hr))
    {
        hr = plip->GetResourceDisplayName(pidlSub, pszResource, cch, piIndex);
        plip->Release();
    }
    ILFree(pidlSub);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::SetResourceDisplayName(
    _In_ PCUITEMID_CHILD pidl,
    _In_ LPCWSTR pszResource,
    _In_ int iIndex)
{
    CMergedFldrNamespace *pns;
    ILocalizableItemParent *plip;
    PITEMID_CHILD pidlSub;
    HRESULT hr;

    hr = NamespaceForItem(pidl, MFNS_DEFAULT, MFNS_DEFAULT, NULL, &pidlSub, &pns, FALSE);
    if (FAILED(hr))
        return hr;

    hr = pns->m_psf->QueryInterface(IID_PPV_ARG(ILocalizableItemParent, &plip));
    if (SUCCEEDED(hr))
    {
        hr = plip->SetResourceDisplayName(pidlSub, pszResource, iIndex);
        plip->Release();
    }
    ILFree(pidlSub);
    return hr;
}

STDMETHODIMP
CMergedShellFolder::RemoveResourceDisplayName(
    _In_ PCUITEMID_CHILD pidl)
{
    CMergedFldrNamespace *pns;
    ILocalizableItemParent *plip;
    PITEMID_CHILD pidlSub;
    HRESULT hr;

    hr = NamespaceForItem(pidl, MFNS_DEFAULT, MFNS_DEFAULT, NULL, &pidlSub, &pns, FALSE);
    if (FAILED(hr))
        return hr;

    hr = pns->m_psf->QueryInterface(IID_PPV_ARG(ILocalizableItemParent, &plip));
    if (SUCCEEDED(hr))
    {
        hr = plip->RemoveResourceDisplayName(pidlSub);
        plip->Release();
    }
    ILFree(pidlSub);
    return hr;
}

/* IAliasedNamespace */

STDMETHODIMP
CMergedShellFolder::GetRealLocations(
    _Outptr_ IShellItemArray **ppsia)
{
    PIDLIST_ABSOLUTE *apidl;
    UINT cItems = 0;
    HRESULT hr;
    int i;

    *ppsia = NULL;
    if (!m_pidl || m_pParent)
        return E_FAIL;

    apidl = new PIDLIST_ABSOLUTE[GetNamespaceCount() + 1];
    if (!apidl)
        return E_OUTOFMEMORY;

    /* Each namespace reports where it really lives */
    for (i = 0; i < GetNamespaceCount(); i++)
    {
        CMergedFldrNamespace *pns = (CMergedFldrNamespace *)DPA_GetPtr(m_hdpaNamespaces, i);
        IAliasedNamespace *pan;
        IShellItemArray *psia;

        if (FAILED(pns->m_psf->QueryInterface(IID_PPV_ARG(IAliasedNamespace, &pan))))
            continue;

        if (SUCCEEDED(pan->GetRealLocations(&psia)))
        {
            DWORD cLocations = 0;
            DWORD j;

            psia->GetCount(&cLocations);
            for (j = 0; j < cLocations; j++)
            {
                IShellItem *psi;
                PIDLIST_ABSOLUTE pidl;

                if (SUCCEEDED(psia->GetItemAt(j, &psi)))
                {
                    if (SUCCEEDED(SHGetIDListFromObject(psi, &pidl)))
                    {
                        PIDLIST_ABSOLUTE *apidlNew = apidl;

                        if (cItems >= (UINT)GetNamespaceCount())
                        {
                            apidlNew = new PIDLIST_ABSOLUTE[cItems * 2 + 1];
                            if (apidlNew)
                            {
                                CopyMemory(apidlNew, apidl, cItems * sizeof(*apidl));
                                delete[] apidl;
                                apidl = apidlNew;
                            }
                        }

                        if (apidlNew)
                            apidl[cItems++] = pidl;
                        else
                            ILFree(pidl);
                    }
                    psi->Release();
                }
            }
            psia->Release();
        }
        pan->Release();
    }

    hr = cItems ? SHCreateShellItemArrayFromIDLists(cItems, (PCIDLIST_ABSOLUTE_ARRAY)apidl, ppsia) :
        E_FAIL;

    for (UINT k = 0; k < cItems; k++)
        ILFree(apidl[k]);
    delete[] apidl;
    return hr;
}

HRESULT
CMergedShellFolder::TryParseNameWithNamespace(
    _In_ CMergedFldrNamespace *pns,
    _In_opt_ HWND hwnd,
    _In_opt_ IBindCtx *pbc,
    _In_ LPCWSTR pszName,
    _Outptr_ PITEMID_CHILD *ppidl)
{
    return ParseNameInFolder(pns->m_psf, hwnd, pbc, pszName, ppidl);
}

HRESULT
CMergedShellFolder_CreateInstance(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CMergedShellFolder *pmf = new CMergedShellFolder(NULL, rclsid, 0);
    HRESULT hr;

    *ppv = NULL;
    if (!pmf)
        return E_OUTOFMEMORY;

    hr = pmf->QueryInterface(riid, ppv);
    pmf->Release();
    return hr;
}

/* The ID list of a namespace class, through "shell:::{clsid}" */
static HRESULT
GetNamespaceClassIDList(
    _In_ REFCLSID rclsid,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    WCHAR szPath[sizeof("shell:::") + 39] = L"shell:::";

    *ppidl = NULL;
    StringFromGUID2(rclsid, szPath + wcslen(szPath), 39);
    return SHILCreateFromPathW(szPath, ppidl, NULL);
}

/* One location of a merged folder made from special folders */
struct MERGED_NAMESPACE_DEF
{
    int csidl;
    DWORD dwFlags;
    const GUID *pguidObject;
    UINT uGroupID;
};

static HRESULT
CreateMergedFolderHelper(
    _In_ REFCLSID rclsid,
    _In_reads_(cDefs) const MERGED_NAMESPACE_DEF *pDefs,
    _In_ UINT cDefs,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    IAugmentedMergedFolder *pamf;
    PIDLIST_ABSOLUTE pidlRoot;
    HRESULT hr;
    UINT i;

    *ppv = NULL;
    hr = CMergedShellFolder_CreateInstance(CLSID_MergedFolder,
                                           IID_PPV_ARG(IAugmentedMergedFolder, &pamf));
    if (FAILED(hr))
        return hr;

    hr = GetNamespaceClassIDList(rclsid, &pidlRoot);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < cDefs && SUCCEEDED(hr); i++)
        {
            PERSIST_FOLDER_TARGET_INFO pfti = { 0 };
            IShellFolder2 *psf;

            if ((pDefs[i].dwFlags & MFNS_COMMON) && SHRestricted(REST_NOCOMMONGROUPS))
                continue;

            /* Each location is a file system folder rooted at the merged folder */
            pfti.dwAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SYSTEM;
            pfti.csidl = pDefs[i].csidl | CSIDL_FLAG_DONT_VERIFY;
            hr = SHELL32_CoCreateInitSF(pidlRoot, &pfti, NULL, &CLSID_ShellFSFolder,
                                        IID_PPV_ARG(IShellFolder2, &psf));
            if (FAILED(hr))
                break;

            if (pDefs[i].pguidObject == &CLSID_StartMenu &&
                SHWindowsPolicyEx(POLID_NoStartMenuSubFolders, 0))
            {
                ISetFolderEnumRestriction *pser;

                if (SUCCEEDED(psf->QueryInterface(IID_ISetFolderEnumRestriction, (void **)&pser)))
                {
                    pser->SetEnumRestriction(0, SHCONTF_FOLDERS);
                    pser->Release();
                }
            }

            hr = pamf->AddNameSpace(pDefs[i].pguidObject, psf, NULL, pDefs[i].dwFlags,
                                    pDefs[i].uGroupID);
            psf->Release();
        }
        ILFree(pidlRoot);

        if (SUCCEEDED(hr))
            hr = pamf->QueryInterface(riid, ppv);
    }

    pamf->Release();
    return hr;
}

/* The user and all users Programs folders, plus the shortcuts at the top of both Start menus */
static const MERGED_NAMESPACE_DEF c_ProgramsFolderAndFastItems[] =
{
    { CSIDL_PROGRAMS | CSIDL_FLAG_CREATE, 0xFF00 | MFNS_FOLDERSLAST | MFNS_GROUPED,
      &CLSID_ProgramsFolder, 2 },
    { CSIDL_COMMON_PROGRAMS, MFNS_FOLDERSLAST | MFNS_COMMON | MFNS_GROUPED,
      &CLSID_ProgramsFolder, 2 },
    { CSIDL_STARTMENU | CSIDL_FLAG_CREATE, MFNS_FOLDERSLAST | MFNS_GROUPED, &CLSID_StartMenu, 1 },
    { CSIDL_COMMON_STARTMENU, MFNS_FOLDERSLAST | MFNS_COMMON | MFNS_GROUPED, &CLSID_StartMenu, 1 },
};

HRESULT
CProgramsFolderAndFastItems_CreateInstance(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    return CreateMergedFolderHelper(CLSID_ProgramsFolderAndFastItems, c_ProgramsFolderAndFastItems,
                                    _countof(c_ProgramsFolderAndFastItems), riid, ppv);
}
