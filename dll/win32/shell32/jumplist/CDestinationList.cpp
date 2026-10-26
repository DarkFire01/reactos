/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Custom destination lists (categories and tasks) applications publish
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <wincrypt.h>
#include "CDestinationList.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* customDestinations-ms file version and the mark that ends every category */
#define CUSTOMDEST_VERSION      2
#define CUSTOMDEST_SIGNATURE    0xBABFFBAB

/* Removed destinations remembered at most */
#define CUSTOMDEST_MAX_REMOVED  1000

/* The list could not be fully deleted */
#define E_CUSTOMDEST_DELETE     ((HRESULT)0x80040F05)

#ifndef SLDF_DISABLE_KNOWNFOLDER_RELATIVE_TRACKING
#define SLDF_DISABLE_KNOWNFOLDER_RELATIVE_TRACKING 0x00200000
#endif

/* Classes a list file may hold */
static const CLSID * const c_rgpclsidDests[] = { &CLSID_ShellLink };
static const CLSID * const c_rgpclsidRemoved[] = { &CLSID_ShellItem, &CLSID_ShellLink };

static
HRESULT
GenRandomFileName(
    _Out_writes_(cch) PWSTR psz,
    _In_ SIZE_T cch)
{
    static const WCHAR c_szChars[] = L"0123456789abcdefghijklmnopqrstuvwxyz";
    HCRYPTPROV hProv;
    HRESULT hr = S_OK;

    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return HRESULT_FROM_WIN32(GetLastError());

    if (CryptGenRandom(hProv, (DWORD)((cch - 1) * sizeof(WCHAR)), (BYTE *)psz))
    {
        for (SIZE_T i = 0; i < cch - 1; i++)
            psz[i] = c_szChars[psz[i] % (_countof(c_szChars) - 1)];
        psz[cch - 1] = UNICODE_NULL;
    }
    else
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }

    CryptReleaseContext(hProv, 0);
    return hr;
}

static
BOOL
ValidateKnownCategory(
    _In_ int iCategory)
{
    return iCategory == KDC_FREQUENT || iCategory == KDC_RECENT;
}

CDestinationCategory::CDestinationCategory() :
    m_cRef(1),
    m_KnownCategory((KNOWNDESTCATEGORY)0),
    m_pszName(NULL),
    m_hdpaDests(NULL),
    m_Type(DCT_CUSTOM)
{
}

CDestinationCategory::~CDestinationCategory()
{
    CoTaskMemFree(m_pszName);
    if (m_hdpaDests)
        DPA_DestroyCallback(m_hdpaDests, DPA_ReleaseUnknownCB, NULL);
}

ULONG
CDestinationCategory::AddReference(VOID)
{
    return InterlockedIncrement(&m_cRef);
}

ULONG
CDestinationCategory::ReleaseReference(VOID)
{
    ULONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

UINT
CDestinationCategory::GetDestCount() const
{
    return m_hdpaDests ? DPA_GetPtrCount(m_hdpaDests) : 0;
}

IUnknown *
CDestinationCategory::GetDest(
    _In_ UINT iDest) const
{
    return (IUnknown *)DPA_GetPtr(m_hdpaDests, iDest);
}

VOID
CDestinationCategory::DeleteDest(
    _In_ UINT iDest)
{
    IUnknown *punk = (IUnknown *)DPA_DeletePtr(m_hdpaDests, iDest);

    if (punk)
        punk->Release();
}

HRESULT
CDestinationCategory::AppendLink(
    _In_ IShellLinkW *psl)
{
    if (!m_hdpaDests)
    {
        m_hdpaDests = DPA_Create(4);
        if (!m_hdpaDests)
            return E_OUTOFMEMORY;
    }

    if (DPA_AppendPtr(m_hdpaDests, psl) == -1)
        return E_OUTOFMEMORY;

    psl->AddRef();
    return S_OK;
}

/* Items are kept as links the shell makes for them */
HRESULT
CDestinationCategory::AppendDestination(
    _In_ IShellItem *psi)
{
    CComPtr<IShellLinkW> psl;
    HRESULT hr;

    hr = CreateLinkForItem(psi, &psl, TRUE);
    if (SUCCEEDED(hr))
        hr = AppendLink(psl);
    return hr;
}

/* Returns S_FALSE for a category without destinations, which the caller drops */
HRESULT
CDestinationCategory::_LoadDests(
    _In_ IStream *pstm,
    _Inout_ UINT *pcTotalDests)
{
    UINT cDests;
    HRESULT hr;

    hr = IStream_Read(pstm, &cDests, sizeof(cDests));
    if (FAILED(hr))
        return hr;

    if (cDests + *pcTotalDests >= JUMPLIST_MAX_ITEMS)
        return E_FAIL;

    hr = S_FALSE;
    if (!cDests)
        return hr;

    m_hdpaDests = DPA_Create(cDests);
    if (!m_hdpaDests)
        return E_FAIL;

    hr = S_OK;
    for (UINT i = 0; i < cDests && SUCCEEDED(hr); i++)
    {
        IShellLinkW *psl;

        hr = IUnknown_LoadKnownImplFromStream(pstm, c_rgpclsidDests, _countof(c_rgpclsidDests),
                                              IID_PPV_ARG(IShellLinkW, &psl));
        if (SUCCEEDED(hr))
        {
            hr = AppendLink(psl);
            psl->Release();
        }
    }
    return hr;
}

HRESULT
CDestinationCategory::Load(
    _In_ IStream *pstm,
    _Inout_ UINT *pcTotalDests)
{
    DWORD dwSignature;
    HRESULT hr;

    hr = IStream_Read(pstm, &m_Type, sizeof(DWORD));
    if (FAILED(hr))
        return hr;

    if (m_Type == DCT_KNOWN)
    {
        int iCategory;

        hr = IStream_Read(pstm, &iCategory, sizeof(iCategory));
        if (SUCCEEDED(hr))
        {
            if (ValidateKnownCategory(iCategory))
                m_KnownCategory = (KNOWNDESTCATEGORY)iCategory;
            else
                hr = E_FAIL;
        }
    }
    else
    {
        if (m_Type == DCT_CUSTOM)
            hr = JumpListStreamReadString(pstm, &m_pszName);
        if (SUCCEEDED(hr))
            hr = _LoadDests(pstm, pcTotalDests);
    }

    if (SUCCEEDED(hr))
    {
        HRESULT hrRead = IStream_Read(pstm, &dwSignature, sizeof(dwSignature));

        if (FAILED(hrRead))
            hr = hrRead;
        else if (dwSignature != CUSTOMDEST_SIGNATURE)
            hr = E_FAIL;
    }
    return hr;
}

HRESULT
CDestinationCategory::_WriteDests(
    _In_ IStream *pstm) const
{
    UINT cDests = GetDestCount();
    HRESULT hr;

    hr = IStream_Write(pstm, &cDests, sizeof(cDests));
    for (UINT i = 0; i < cDests && SUCCEEDED(hr); i++)
    {
        CComPtr<IShellLinkW> psl;
        IUnknown *punk = GetDest(i);

        if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl))))
        {
            SetLinkFlags(psl, SLDF_DISABLE_KNOWNFOLDER_RELATIVE_TRACKING,
                         SLDF_DISABLE_KNOWNFOLDER_RELATIVE_TRACKING);
        }
        hr = IUnknown_SaveKnownImplToStream(pstm, c_rgpclsidDests, _countof(c_rgpclsidDests), punk);
    }
    return hr;
}

HRESULT
CDestinationCategory::Save(
    _In_ IStream *pstm) const
{
    DWORD dwType = m_Type;
    DWORD dwSignature = CUSTOMDEST_SIGNATURE;
    HRESULT hr;

    hr = IStream_Write(pstm, &dwType, sizeof(dwType));
    if (FAILED(hr))
        return hr;

    if (m_Type == DCT_KNOWN)
    {
        int iCategory = m_KnownCategory;

        hr = IStream_Write(pstm, &iCategory, sizeof(iCategory));
    }
    else
    {
        if (m_Type == DCT_CUSTOM)
            hr = JumpListStreamWriteString(pstm, m_pszName);
        if (SUCCEEDED(hr))
            hr = _WriteDests(pstm);
    }

    if (SUCCEEDED(hr))
        hr = IStream_Write(pstm, &dwSignature, sizeof(dwSignature));
    return hr;
}

static
int
CALLBACK
ReleaseCategoryCB(
    _In_opt_ void *p,
    _In_opt_ void *pData)
{
    UNREFERENCED_PARAMETER(pData);

    if (p)
        ((CDestinationCategory *)p)->ReleaseReference();
    return TRUE;
}

CDestinationList::CDestinationList() :
    m_fInList(FALSE),
    m_fLoaded(FALSE),
    m_fAddedRemovedItem(FALSE),
    m_pszAppID(NULL),
    m_pszListPath(NULL),
    m_pszAppPath(NULL),
    m_hdpaCategories(NULL),
    m_hdpaRemoved(NULL)
{
}

CDestinationList::~CDestinationList()
{
    CoTaskMemFree(m_pszAppID);
    CoTaskMemFree(m_pszListPath);
    CoTaskMemFree(m_pszAppPath);
    _DestroyCategories();
    _DestroyRemoved();
}

VOID
CDestinationList::_DestroyCategories(VOID)
{
    if (m_hdpaCategories)
    {
        DPA_DestroyCallback(m_hdpaCategories, ReleaseCategoryCB, NULL);
        m_hdpaCategories = NULL;
    }
}

VOID
CDestinationList::_DestroyRemoved(VOID)
{
    if (m_hdpaRemoved)
    {
        DPA_DestroyCallback(m_hdpaRemoved, DPA_ReleaseUnknownCB, NULL);
        m_hdpaRemoved = NULL;
    }
}

UINT
CDestinationList::_GetCategoryCount() const
{
    return m_hdpaCategories ? DPA_GetPtrCount(m_hdpaCategories) : 0;
}

CDestinationCategory *
CDestinationList::_GetCategory(
    _In_ UINT iCategory) const
{
    return (CDestinationCategory *)DPA_GetPtr(m_hdpaCategories, iCategory);
}

/* The list file: the application's hash in the CustomDestinations folder */
HRESULT
CDestinationList::_GetApplicationCustomPath(
    _In_ PCWSTR pszAppID,
    _Outptr_ PWSTR *ppszPath)
{
    WCHAR szHash[20];
    PWSTR pszPath;
    HRESULT hr;

    *ppszPath = NULL;
    hr = HashAppID(pszAppID, szHash, _countof(szHash));
    if (FAILED(hr))
        return hr;

    pszPath = (PWSTR)CoTaskMemAlloc(MAX_PATH * sizeof(WCHAR));
    if (!pszPath)
        return E_OUTOFMEMORY;

    hr = GetDestinationsPath(DESTINATIONS_CUSTOM, pszPath);
    if (SUCCEEDED(hr) && !PathAddBackslashW(pszPath))
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = StringCchCatW(pszPath, MAX_PATH, szHash);
    if (SUCCEEDED(hr))
        hr = StringCchCatW(pszPath, MAX_PATH, L".customDestinations-ms");

    if (FAILED(hr))
    {
        CoTaskMemFree(pszPath);
        return hr;
    }

    *ppszPath = pszPath;
    return hr;
}

/* Without SetAppID the list belongs to the calling process */
HRESULT
CDestinationList::_EnsureAppID(VOID)
{
    HRESULT hr = S_OK;

    if (!m_pszAppID)
    {
        CComPtr<IApplicationResolver> par;
        WCHAR szExe[MAX_PATH];

        hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARG(IApplicationResolver, &par));
        if (FAILED(hr))
            return hr;

        hr = par->GetAppIDForProcess(GetCurrentProcessId(), &m_pszAppID, NULL, NULL, NULL);
        if (FAILED(hr))
            return hr;

        if (GetModuleFileNameW(NULL, szExe, _countof(szExe)))
            SHStrDupW(szExe, &m_pszAppPath);
    }

    if (!m_pszListPath && m_pszAppID)
        hr = _GetApplicationCustomPath(m_pszAppID, &m_pszListPath);
    return hr;
}

HRESULT
CDestinationList::_LoadCategories(
    _In_ IStream *pstm,
    _In_ UINT cCategories)
{
    UINT cTotalDests = 0;
    HRESULT hr = S_OK;

    m_hdpaCategories = DPA_Create(cCategories);
    if (!m_hdpaCategories)
        return E_OUTOFMEMORY;

    for (UINT i = 0; i < cCategories && SUCCEEDED(hr); i++)
    {
        CDestinationCategory *pCategory = new CDestinationCategory;

        if (!pCategory)
            return E_OUTOFMEMORY;

        hr = pCategory->Load(pstm, &cTotalDests);
        if (hr == S_OK)
        {
            if (DPA_AppendPtr(m_hdpaCategories, pCategory) == -1)
                hr = E_OUTOFMEMORY;
            else
                pCategory->AddReference();
        }
        pCategory->ReleaseReference();
    }
    return hr;
}

HRESULT
CDestinationList::_LoadRemovedItems(
    _In_ IStream *pstm,
    _In_ UINT cRemoved)
{
    HRESULT hr = S_OK;

    m_hdpaRemoved = DPA_Create(cRemoved);
    if (!m_hdpaRemoved)
        return E_OUTOFMEMORY;

    for (UINT i = 0; i < cRemoved && SUCCEEDED(hr); i++)
    {
        IUnknown *punk;

        hr = IUnknown_LoadKnownImplFromStream(pstm, c_rgpclsidRemoved, _countof(c_rgpclsidRemoved),
                                              IID_PPV_ARG(IUnknown, &punk));
        if (SUCCEEDED(hr))
        {
            if (DPA_AppendPtr(m_hdpaRemoved, punk) == -1)
                hr = E_OUTOFMEMORY;
            else
                punk->AddRef();
            punk->Release();
        }
    }
    return hr;
}

/* A missing file is an empty list; an unreadable one is deleted */
HRESULT
CDestinationList::_LoadList(
    _In_opt_ PCWSTR pszListPath)
{
    CComPtr<IStream> pstm;
    HRESULT hr;

    hr = SHCreateStreamOnFileW(pszListPath, STGM_READ | STGM_SHARE_DENY_WRITE, &pstm);
    if (SUCCEEDED(hr))
    {
        DWORD dwVersion;
        UINT cCategories;
        UINT cRemoved;

        hr = IStream_Read(pstm, &dwVersion, sizeof(dwVersion));
        if (SUCCEEDED(hr))
            hr = IStream_Read(pstm, &cCategories, sizeof(cCategories));
        if (SUCCEEDED(hr))
            hr = IStream_Read(pstm, &cRemoved, sizeof(cRemoved));
        if (SUCCEEDED(hr) &&
            (dwVersion != CUSTOMDEST_VERSION ||
             cCategories > JUMPLIST_MAX_CATEGORIES ||
             cRemoved > CUSTOMDEST_MAX_REMOVED))
        {
            hr = E_FAIL;
        }

        if (FAILED(hr))
        {
            pstm.Release();
            DeleteFileW(pszListPath);
            hr = S_OK;
        }
        else
        {
            hr = _LoadCategories(pstm, cCategories);
            if (SUCCEEDED(hr))
                hr = _LoadRemovedItems(pstm, cRemoved);
        }
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        hr = S_OK;

    if (FAILED(hr))
    {
        _DestroyCategories();
        _DestroyRemoved();
    }
    else
    {
        m_fLoaded = TRUE;
    }
    return hr;
}

HRESULT
CDestinationList::_EnsureLoaded(VOID)
{
    if (m_fLoaded)
        return S_OK;
    return _LoadList(m_pszListPath);
}

HRESULT
CDestinationList::_WriteListToFile(
    _In_ IStream *pstm)
{
    DWORD dwVersion = CUSTOMDEST_VERSION;
    UINT cCategories = _GetCategoryCount();
    UINT cRemoved = m_hdpaRemoved ? DPA_GetPtrCount(m_hdpaRemoved) : 0;
    HRESULT hr;

    hr = IStream_Write(pstm, &dwVersion, sizeof(dwVersion));
    if (SUCCEEDED(hr))
        hr = IStream_Write(pstm, &cCategories, sizeof(cCategories));
    if (SUCCEEDED(hr))
        hr = IStream_Write(pstm, &cRemoved, sizeof(cRemoved));

    for (UINT i = 0; i < cCategories && SUCCEEDED(hr); i++)
        hr = _GetCategory(i)->Save(pstm);

    for (UINT i = 0; i < cRemoved && SUCCEEDED(hr); i++)
    {
        IUnknown *punk = (IUnknown *)DPA_GetPtr(m_hdpaRemoved, i);

        hr = IUnknown_SaveKnownImplToStream(pstm, c_rgpclsidRemoved, _countof(c_rgpclsidRemoved), punk);
    }

    if (SUCCEEDED(hr))
        hr = pstm->Commit(STGC_DEFAULT);
    return hr;
}

/* Writes a temporary file next to the list and swaps it in */
HRESULT
CDestinationList::_WriteList(VOID)
{
    WCHAR szFolder[MAX_PATH];
    WCHAR szTemp[MAX_PATH];
    WCHAR szRandom[21];
    HRESULT hr;

    szTemp[0] = UNICODE_NULL;
    hr = GetDestinationsPath(DESTINATIONS_CUSTOM, szFolder);
    if (SUCCEEDED(hr))
        hr = GenRandomFileName(szRandom, _countof(szRandom));
    if (SUCCEEDED(hr))
    {
        if (!PathCombineW(szTemp, szFolder, szRandom))
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            hr = StringCchCatW(szTemp, _countof(szTemp), L".temp");
        if (FAILED(hr))
            szTemp[0] = UNICODE_NULL;
    }
    if (FAILED(hr))
        return hr;

    {
        CComPtr<IStream> pstm;

        hr = SHCreateStreamOnFileW(szTemp, STGM_CREATE | STGM_SHARE_EXCLUSIVE | STGM_WRITE, &pstm);
        if (SUCCEEDED(hr))
            hr = _WriteListToFile(pstm);
    }
    if (FAILED(hr))
        return hr;

    BOOL fOk;
    if (PathFileExistsW(m_pszListPath))
        fOk = ReplaceFileW(m_pszListPath, szTemp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);
    else
        fOk = MoveFileW(szTemp, m_pszListPath);

    if (!fOk)
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr))
        DeleteFileW(szTemp);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::SetAppID(
    _In_ PCWSTR pszAppID)
{
    if (m_fInList)
        return E_UNEXPECTED;
    return SetApplicationID(pszAppID);
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::BeginList(
    _Out_ UINT *pcMinSlots,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IObjectArray> poaRemoved;
    HRESULT hr;

    *pcMinSlots = 0;
    *ppv = NULL;
    if (m_fInList)
        return E_UNEXPECTED;

    m_fAddedRemovedItem = FALSE;
    hr = GetRemovedDestinations(IID_PPV_ARG(IObjectArray, &poaRemoved));
    if (FAILED(hr))
    {
        hr = CreateEnumerableObjectCollection(0, IID_PPV_ARG(IObjectArray, &poaRemoved));
        if (FAILED(hr))
            return hr;
    }

    _DestroyCategories();
    m_hdpaCategories = DPA_Create(4);
    if (!m_hdpaCategories)
        return E_OUTOFMEMORY;

    hr = poaRemoved->QueryInterface(riid, ppv);
    if (SUCCEEDED(hr))
    {
        /* Windows 7 also returns 20 for applications with the jump list compatibility flag */
        *pcMinSlots = ReadStartPageSettingBounded(L"Start_JumpListItems", JUMPLIST_DEFAULT_ITEMS, 0,
                                                  JUMPLIST_MAX_SETTING_ITEMS);
        m_fInList = TRUE;
    }
    return hr;
}

/* A destination the user removed may not come back until the application sees the removal */
HRESULT
CDestinationList::_EnsureAddedItemIsNotRemoved(
    _In_ IUnknown *punk)
{
    CComPtr<IShellItem> psi;
    CComPtr<IShellLinkW> psl;
    UINT cRemoved = m_hdpaRemoved ? DPA_GetPtrCount(m_hdpaRemoved) : 0;
    HRESULT hr;

    if (!cRemoved)
        return S_OK;

    hr = punk->QueryInterface(IID_PPV_ARG(IShellItem, &psi));
    if (FAILED(hr))
        hr = punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl));

    for (UINT i = 0; i < cRemoved && SUCCEEDED(hr); i++)
    {
        IUnknown *punkRemoved = (IUnknown *)DPA_GetPtr(m_hdpaRemoved, i);

        if (psi)
        {
            CComPtr<IShellItem> psiRemoved;
            int iOrder;

            if (SUCCEEDED(punkRemoved->QueryInterface(IID_PPV_ARG(IShellItem, &psiRemoved))) &&
                psi->Compare(psiRemoved, SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL,
                             &iOrder) == S_OK)
            {
                hr = E_FAIL;
            }
        }
        else
        {
            CComPtr<IShellLinkW> pslRemoved;

            if (SUCCEEDED(punkRemoved->QueryInterface(IID_PPV_ARG(IShellLinkW, &pslRemoved))) &&
                DPA_CompareLinksCB(psl, pslRemoved, 0) == 0)
            {
                hr = E_FAIL;
            }
        }
    }

    if (FAILED(hr))
        m_fAddedRemovedItem = TRUE;
    return hr;
}

HRESULT
CDestinationList::_AppendCategory(
    _In_opt_ PCWSTR pszCategory,
    _In_ IObjectArray *poa,
    _In_ BOOL fTasks)
{
    CDestinationCategory *pCategory;
    UINT cObjects;
    HRESULT hr;

    if (!fTasks && !IsRecentDocsEnabled())
        return E_ACCESSDENIED;

    if (!m_fInList)
        return E_UNEXPECTED;

    if (_GetCategoryCount() + 1 > JUMPLIST_MAX_CATEGORIES)
        return E_INVALIDARG;

    hr = poa->GetCount(&cObjects);
    if (FAILED(hr))
        return hr;
    if (!cObjects)
        return E_INVALIDARG;

    pCategory = new CDestinationCategory;
    if (!pCategory)
        return E_OUTOFMEMORY;

    if (fTasks)
    {
        pCategory->m_Type = DCT_TASKS;
    }
    else
    {
        pCategory->m_Type = DCT_CUSTOM;
        hr = SHStrDupW(pszCategory, &pCategory->m_pszName);
    }

    for (UINT i = 0; i < cObjects && SUCCEEDED(hr); i++)
    {
        CComPtr<IUnknown> punk;
        CComPtr<IShellItem> psi;
        CComPtr<IShellLinkW> psl;

        hr = poa->GetAt(i, IID_PPV_ARG(IUnknown, &punk));
        if (FAILED(hr))
            continue;

        if (!fTasks)
        {
            hr = _EnsureAddedItemIsNotRemoved(punk);
            if (FAILED(hr))
                continue;
        }

        /* Items must open with this application */
        if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IShellItem, &psi))))
        {
            hr = MakeDestinationItemForApp(psi, NULL, m_pszAppID, 0, m_pszAppPath, NULL);
            if (SUCCEEDED(hr))
                hr = pCategory->AppendDestination(psi);
            continue;
        }

        hr = punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl));
        if (FAILED(hr))
            continue;

        /* Separators belong to tasks; any other link needs a title */
        if (IsSeparator(psl))
        {
            if (!fTasks)
            {
                hr = E_INVALIDARG;
                continue;
            }
        }
        else
        {
            CComPtr<IPropertyStore> pps;
            BOOL fHasTitle = FALSE;

            if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps))))
            {
                PROPVARIANT pv;

                if (SUCCEEDED(pps->GetValue(PKEY_Title, &pv)))
                {
                    fHasTitle = (pv.vt != VT_EMPTY);
                    PropVariantClear(&pv);
                }
            }

            if (!fHasTitle)
            {
                hr = E_INVALIDARG;
                continue;
            }
        }

        hr = pCategory->AppendLink(psl);
    }

    if (SUCCEEDED(hr))
    {
        if (DPA_AppendPtr(m_hdpaCategories, pCategory) == -1)
            hr = E_OUTOFMEMORY;
        else
            pCategory->AddReference();
    }
    pCategory->ReleaseReference();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::AppendCategory(
    _In_ PCWSTR pszCategory,
    _In_ IObjectArray *poa)
{
    return _AppendCategory(pszCategory, poa, FALSE);
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::AppendKnownCategory(
    _In_ KNOWNDESTCATEGORY category)
{
    CDestinationCategory *pCategory;
    HRESULT hr;

    if (!m_fInList)
        return E_UNEXPECTED;

    if (!ValidateKnownCategory(category))
        return E_FAIL;

    pCategory = new CDestinationCategory;
    if (!pCategory)
        return E_OUTOFMEMORY;

    pCategory->m_Type = DCT_KNOWN;
    pCategory->m_KnownCategory = category;

    hr = E_OUTOFMEMORY;
    if (DPA_AppendPtr(m_hdpaCategories, pCategory) != -1)
    {
        pCategory->AddReference();
        hr = S_OK;
    }
    pCategory->ReleaseReference();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::AddUserTasks(
    _In_ IObjectArray *poa)
{
    for (UINT i = 0; i < _GetCategoryCount(); i++)
    {
        if (_GetCategory(i)->m_Type == DCT_TASKS)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    return _AppendCategory(NULL, poa, TRUE);
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::CommitList()
{
    HRESULT hr;

    if (!m_fInList)
        return E_UNEXPECTED;

    hr = _EnsureAppID();
    if (SUCCEEDED(hr))
    {
        /* The application has seen the removed items unless it tried to add one back */
        if (!m_fAddedRemovedItem)
            _DestroyRemoved();
        hr = _WriteList();
    }

    _DestroyCategories();
    _DestroyRemoved();
    m_fInList = FALSE;
    m_fLoaded = FALSE;

    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::GetRemovedDestinations(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IObjectCollection> poc;
    UINT cRemoved;
    HRESULT hr;

    *ppv = NULL;
    hr = _EnsureAppID();
    if (SUCCEEDED(hr))
        hr = _EnsureLoaded();
    if (FAILED(hr))
        return hr;

    cRemoved = m_hdpaRemoved ? DPA_GetPtrCount(m_hdpaRemoved) : 0;
    hr = CreateEnumerableObjectCollection(cRemoved, IID_PPV_ARG(IObjectCollection, &poc));
    for (UINT i = 0; i < cRemoved && SUCCEEDED(hr); i++)
        hr = poc->AddObject((IUnknown *)DPA_GetPtr(m_hdpaRemoved, i));
    if (SUCCEEDED(hr))
        hr = poc->QueryInterface(riid, ppv);
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::DeleteList(
    _In_opt_ PCWSTR pszAppID)
{
    CComHeapPtr<WCHAR> pszProcessAppID;
    HRESULT hr;

    if (m_fInList)
        return E_UNEXPECTED;

    if (!pszAppID)
    {
        CComPtr<IApplicationResolver> par;

        hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARG(IApplicationResolver, &par));
        if (SUCCEEDED(hr))
            hr = par->GetAppIDForProcess(GetCurrentProcessId(), &pszProcessAppID, NULL, NULL, NULL);
        if (FAILED(hr))
            return hr;
        pszAppID = pszProcessAppID;
    }

    hr = s_DeleteList(pszAppID, NULL, CDOPT_DELETECUSTOM | CDOPT_DELETETASKS);
    if (SUCCEEDED(hr))
    {
        _DestroyRemoved();
        m_fLoaded = FALSE;
    }
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::AbortList()
{
    if (!m_fInList)
        return E_UNEXPECTED;

    m_fInList = FALSE;
    m_fLoaded = FALSE;
    _DestroyCategories();
    _DestroyRemoved();
    return S_OK;
}

/* Deletes the categories uOptions selects; the file goes away once nothing is left */
HRESULT
CDestinationList::s_DeleteList(
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszListPath,
    _In_ UINT uOptions)
{
    CComPtr<IInternalCustomDestinationList> picdl;
    CDestinationList *pList;
    BOOL fFailed = FALSE;
    UINT cCategories;
    HRESULT hr = S_OK;

    if (!(uOptions & (CDOPT_DELETECUSTOM | CDOPT_DELETETASKS | CDOPT_REMOVEDESTINATIONS)))
    {
        JumpListCounterIncrement();
        return hr;
    }

    hr = ShellObjectCreator<CDestinationList>(IID_PPV_ARG(IInternalCustomDestinationList, &picdl));
    if (FAILED(hr))
        return hr;
    pList = static_cast<CDestinationList *>(picdl.p);

    if (pszAppID)
        hr = pList->SetApplicationID(pszAppID);
    else
        hr = SHStrDupW(pszListPath, &pList->m_pszListPath);
    if (SUCCEEDED(hr))
        hr = pList->GetCategoryCount(&cCategories);
    if (FAILED(hr))
        return hr;

    for (int i = (int)cCategories - 1; i >= 0; i -= 1)
    {
        APPDESTCATEGORY Category;

        hr = pList->GetCategory(i, GETCATFLAG_NONE, &Category);
        if (SUCCEEDED(hr))
        {
            UINT uNeeded = (Category.cattype == DCT_TASKS) ? CDOPT_DELETETASKS : CDOPT_DELETECUSTOM;

            if (uOptions & uNeeded)
                hr = pList->DeleteCategory(i, (uOptions & CDOPT_REMOVEDESTINATIONS) != 0);
        }
        if (FAILED(hr))
            fFailed = TRUE;
    }

    if (!fFailed && !(uOptions & CDOPT_REMOVEDESTINATIONS) && (uOptions & CDOPT_DELETECUSTOM))
        pList->_DestroyRemoved();

    BOOL fEmpty = (!pList->m_hdpaRemoved || !DPA_GetPtrCount(pList->m_hdpaRemoved)) &&
                  !pList->_GetCategoryCount();

    if (fFailed)
    {
        hr = E_CUSTOMDEST_DELETE;
        if (!fEmpty)
            hr = pList->_WriteList();
    }
    else if (fEmpty)
    {
        hr = S_OK;
        if (!DeleteFileW(pList->m_pszListPath))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                hr = S_OK;
        }
    }
    else
    {
        hr = pList->_WriteList();
    }

    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::SetMinItems(
    _In_ UINT cMinItems)
{
    DWORD dwValue = cMinItems;

    if (cMinItems > JUMPLIST_MAX_SETTING_ITEMS)
        return E_INVALIDARG;

    /* Windows 7 hands the registry status back as is */
    return (HRESULT)SHSetValueW(HKEY_CURRENT_USER,
                                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                                L"Start_JumpListItems", REG_DWORD, &dwValue, sizeof(dwValue));
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::SetApplicationID(
    _In_ PCWSTR pszAppID)
{
    HRESULT hr;

    CoTaskMemFree(m_pszAppID);
    m_pszAppID = NULL;
    CoTaskMemFree(m_pszListPath);
    m_pszListPath = NULL;
    _DestroyCategories();
    _DestroyRemoved();
    m_fLoaded = FALSE;

    hr = SHStrDupW(pszAppID, &m_pszAppID);
    if (SUCCEEDED(hr))
        hr = _GetApplicationCustomPath(m_pszAppID, &m_pszListPath);
    return hr;
}

/* Every category takes a slot for its header plus one per destination */
HRESULT
STDMETHODCALLTYPE
CDestinationList::GetSlotCount(
    _Out_ UINT *pcSlots)
{
    UINT cCategories;
    UINT cDests = 0;
    HRESULT hr;

    *pcSlots = 0;
    hr = GetCategoryCount(&cCategories);
    if (FAILED(hr))
        return hr;

    for (UINT i = 0; i < cCategories; i++)
        cDests += _GetCategory(i)->GetDestCount();

    *pcSlots = cCategories + cDests;
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::GetCategoryCount(
    _Out_ UINT *pcCategories)
{
    HRESULT hr;

    *pcCategories = 0;
    hr = _EnsureLoaded();
    if (SUCCEEDED(hr))
        *pcCategories = _GetCategoryCount();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::GetCategory(
    _In_ UINT iCategory,
    _In_ GETCATFLAG flags,
    _Out_ APPDESTCATEGORY *pCategory)
{
    CDestinationCategory *pCat;
    UINT cDests;
    UINT cItems = 0;
    UINT cSeparators = 0;
    BOOL fItemSinceSeparator = FALSE;
    HRESULT hr;

    hr = _EnsureLoaded();
    if (FAILED(hr))
        return hr;

    pCat = _GetCategory(iCategory);
    if (!pCat)
        return E_FAIL;

    pCategory->cattype = pCat->m_Type;
    cDests = pCat->GetDestCount();
    for (UINT i = 0; i < cDests; i++)
    {
        if (!IsSeparator(pCat->GetDest(i)))
            cItems++;
    }
    pCategory->cItems = cItems;

    /* A separator counts when something came before it and it is not the last entry */
    for (UINT i = 0; i < cDests; i++)
    {
        IUnknown *punk = pCat->GetDest(i);

        if (!punk)
            continue;

        if (IsSeparator(punk) && fItemSinceSeparator && i != cDests - 1)
        {
            cSeparators++;
            fItemSinceSeparator = FALSE;
        }
        else
        {
            fItemSinceSeparator = TRUE;
        }
    }
    pCategory->cSeparators = cSeparators;

    if (pCat->m_Type == DCT_KNOWN)
    {
        pCategory->category = pCat->m_KnownCategory;
    }
    else if (pCat->m_Type == DCT_CUSTOM)
    {
        pCategory->pszCategoryName = NULL;
        if (flags & GETCATFLAG_GETNAME)
        {
            WCHAR szName[MAX_PATH];

            hr = pCat->m_pszName ? StringCchCopyW(szName, _countof(szName), pCat->m_pszName) : E_UNEXPECTED;
            if (SUCCEEDED(hr))
                hr = SHStrDupW(szName, &pCategory->pszCategoryName);
        }
    }
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::DeleteCategory(
    _In_ UINT iCategory,
    _In_ BOOL fDeleteDestinations)
{
    HRESULT hr;

    hr = _EnsureLoaded();
    if (FAILED(hr))
        return hr;

    if (!fDeleteDestinations)
    {
        CDestinationCategory *pCat = (CDestinationCategory *)DPA_DeletePtr(m_hdpaCategories, iCategory);

        if (pCat)
            pCat->ReleaseReference();
        return hr;
    }

    /* Removing every destination one by one also drops the emptied category */
    CComPtr<IObjectCollection> poc;
    UINT cDests;

    hr = EnumerateCategoryDestinations(iCategory, IID_PPV_ARG(IObjectCollection, &poc));
    if (FAILED(hr))
        return E_CUSTOMDEST_DELETE;

    hr = poc->GetCount(&cDests);
    for (UINT i = 0; SUCCEEDED(hr) && i < cDests; i++)
    {
        CComPtr<IUnknown> punk;

        hr = poc->GetAt(i, IID_PPV_ARG(IUnknown, &punk));
        if (FAILED(hr))
        {
            hr = S_OK;
            continue;
        }

        hr = RemoveDestination(punk);
    }

    return FAILED(hr) ? E_CUSTOMDEST_DELETE : hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::EnumerateCategoryDestinations(
    _In_ UINT iCategory,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IObjectCollection> poc;
    CDestinationCategory *pCat;
    UINT cDests;
    HRESULT hr;

    *ppv = NULL;
    hr = _EnsureLoaded();
    if (FAILED(hr))
        return hr;

    pCat = _GetCategory(iCategory);
    if (!pCat)
        return E_FAIL;

    cDests = pCat->GetDestCount();
    hr = CreateEnumerableObjectCollection(cDests, IID_PPV_ARG(IObjectCollection, &poc));
    for (UINT i = 0; i < cDests && SUCCEEDED(hr); i++)
    {
        CComPtr<IShellLinkW> psl;
        IUnknown *punk = pCat->GetDest(i);

        hr = punk ? punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl)) : E_FAIL;
        if (FAILED(hr))
            break;

        /* Links the shell made for items come back as the items */
        if (IsDestListLink(psl))
        {
            CComPtr<IShellItem> psi;

            hr = CreateItemFromLink(psl, IID_PPV_ARG(IShellItem, &psi));
            if (SUCCEEDED(hr))
                hr = poc->AddObject(psi);
        }
        else
        {
            hr = poc->AddObject(psl);
        }
    }

    if (SUCCEEDED(hr))
        hr = poc->QueryInterface(riid, ppv);
    return hr;
}

/* The user removed a destination: drop it from the custom categories and remember it */
HRESULT
STDMETHODCALLTYPE
CDestinationList::RemoveDestination(
    _In_ IUnknown *punk)
{
    CComPtr<IShellLinkW> pslFind;
    BOOL fChanged = FALSE;
    BOOL fHasCustom = FALSE;
    HRESULT hr;

    hr = _EnsureLoaded();
    if (FAILED(hr))
        return hr;

    if (FAILED(punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &pslFind))))
    {
        CComPtr<IShellItem> psi;

        hr = punk->QueryInterface(IID_PPV_ARG(IShellItem, &psi));
        if (SUCCEEDED(hr))
            hr = CreateLinkForItem(psi, &pslFind, FALSE);
        if (FAILED(hr))
            return hr;
    }

    for (int i = (int)_GetCategoryCount() - 1; i >= 0 && SUCCEEDED(hr); i -= 1)
    {
        CDestinationCategory *pCat = _GetCategory(i);
        int iFound;

        if (!pCat->GetDestCount() || pCat->m_Type != DCT_CUSTOM)
            continue;

        fHasCustom = TRUE;
        iFound = DPA_Search(pCat->m_hdpaDests, (IShellLinkW *)pslFind, 0, DPA_CompareLinksCB, 0, 0);
        if (iFound == -1)
            continue;

        hr = E_FAIL;
        if (pCat->GetDest(iFound))
        {
            pCat->DeleteDest(iFound);
            hr = S_OK;
        }

        if (SUCCEEDED(hr))
        {
            fChanged = TRUE;
            if (!pCat->GetDestCount())
            {
                DPA_DeletePtr(m_hdpaCategories, i);
                pCat->ReleaseReference();
            }
        }
    }

    if (SUCCEEDED(hr) && fHasCustom)
    {
        if (!m_hdpaRemoved)
            m_hdpaRemoved = DPA_Create(4);

        if (!m_hdpaRemoved || DPA_AppendPtr(m_hdpaRemoved, punk) == -1)
        {
            hr = E_OUTOFMEMORY;
        }
        else
        {
            punk->AddRef();
            fChanged = TRUE;

            /* Only the newest removals are kept */
            if ((UINT)DPA_GetPtrCount(m_hdpaRemoved) > CUSTOMDEST_MAX_REMOVED)
                ((IUnknown *)DPA_DeletePtr(m_hdpaRemoved, 0))->Release();
        }
    }

    if (SUCCEEDED(hr) && fChanged)
        hr = _WriteList();

    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

HRESULT
STDMETHODCALLTYPE
CDestinationList::ResolveDestination(
    _In_opt_ HWND hwnd,
    _In_ DWORD dwFlags,
    _In_ IShellItem *psi,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IShellLinkW> pslFind;
    CComPtr<IShellLinkW> psl;
    CComPtr<IPersistStream> pps;
    HRESULT hr;

    *ppv = NULL;
    hr = _EnsureLoaded();
    if (SUCCEEDED(hr))
        hr = CreateLinkForItem(psi, &pslFind, FALSE);
    if (FAILED(hr))
        return hr;

    /* The first custom or task link that leads to the item */
    hr = E_FAIL;
    for (UINT i = 0; i < _GetCategoryCount(); i++)
    {
        CDestinationCategory *pCat = _GetCategory(i);
        int iFound;

        if (!pCat->GetDestCount() || (pCat->m_Type != DCT_CUSTOM && pCat->m_Type != DCT_TASKS))
            continue;

        iFound = DPA_Search(pCat->m_hdpaDests, (IShellLinkW *)pslFind, 0, DPA_CompareLinksCB, 0, 0);
        if (iFound == -1)
            continue;

        IUnknown *punk = pCat->GetDest(iFound);
        if (punk)
            hr = punk->QueryInterface(IID_PPV_ARG(IShellLinkW, &psl));
        break;
    }
    if (FAILED(hr))
        return hr;

    hr = psl->Resolve(hwnd, dwFlags);
    if (hr == S_FALSE)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (hr != S_OK)
        return hr;

    hr = psl->QueryInterface(IID_PPV_ARG(IPersistStream, &pps));
    if (FAILED(hr))
        return hr;

    /* Resolving that changed nothing does not count */
    if (pps->IsDirty() != S_OK)
        return E_FAIL;

    _WriteList();
    return CreateItemFromLink(psl, riid, ppv);
}
