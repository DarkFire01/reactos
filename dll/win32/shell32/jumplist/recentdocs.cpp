/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     SHAddToRecentDocs: the automatic destination lists, the recent folder and its MRU lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "jumplist.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

static const WCHAR c_szRecentDocsKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RecentDocs";

/* Programs and scripts never count as documents */
static const WCHAR c_szProgramExts[] = L".cmd\0.bat\0.pif\0.scf\0.exe\0.com\0.scr\0";

/* IMruDataList::InitData flag: entries match on their leading name, ignoring case */
static const UINT c_uMruMatchName = 1;

/* The parent folders that never show up as recent folders */
static const KNOWNFOLDERID * const c_rgpfidNotRecent[] =
{
    &FOLDERID_Documents,
    &FOLDERID_Desktop,
    &FOLDERID_PublicDesktop,
    &FOLDERID_NetHood,
    &FOLDERID_Favorites,
};

/* The last file sent to the recent folder; reopening it within a minute leaves the folder alone */
static CComAutoCriticalSection g_csLastFile;
static PWSTR g_pszLastFile;
static ULONGLONG g_ullLastFileTime;

/* One minute in FILETIME units */
#define LAST_FILE_TIMEOUT   (60ULL * 10000000ULL)

/* One use of an item, counted in the automatic list of the given application */
HRESULT
AddPointToItem(
    _In_ IUnknown *punk,
    _In_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath)
{
    CComPtr<IAutomaticDestinationList> padl;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_AutomaticDestinationList, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IAutomaticDestinationList, &padl));
    if (SUCCEEDED(hr))
        hr = padl->Initialize(pszAppID, pszAppPath, NULL);
    if (SUCCEEDED(hr))
        hr = padl->AddUsagePoint(punk);
    return hr;
}

static
BOOL
IsNoRemoteDestinationsPolicy(VOID)
{
    static const PCWSTR c_pszKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);

    if (SHGetValueW(HKEY_CURRENT_USER, c_pszKey, L"NoRemoteDestinations", NULL, &dwValue, &cbValue) == ERROR_SUCCESS &&
        dwValue)
    {
        return TRUE;
    }

    cbValue = sizeof(dwValue);
    return SHGetValueW(HKEY_LOCAL_MACHINE, c_pszKey, L"NoRemoteDestinations", NULL, &dwValue, &cbValue) == ERROR_SUCCESS &&
           dwValue;
}

/*
 * The folder Explorer keeps for a used document. Windows 7 also switches to the library
 * or search the file was found through; ReactOS has neither, so it is the parent.
 */
static
HRESULT
AdjustForAutoDestList(
    _In_ IShellItem *psiParent,
    _Outptr_ IShellItem **ppsi)
{
    *ppsi = psiParent;
    psiParent->AddRef();
    return S_OK;
}

/*
 * Counts a document in the list of the application that used it, and its folder in
 * Explorer's list. pfExplorer tells whether the folder went to Explorer.
 */
HRESULT
AddToAutoDestList(
    _In_ DWORD dwProcessId,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath,
    _Out_ BOOL *pfExplorer)
{
    CComPtr<IApplicationResolver> par;
    CComPtr<IShellItem> psi;
    WCHAR szPath[MAX_PATH];
    HRESULT hr;

    *pfExplorer = FALSE;
    if (IsNoRemoteDestinationsPolicy() &&
        SHGetPathFromIDListEx(pidl, szPath, _countof(szPath), GPFIDL_UNCPRINTER) &&
        PathIsNetworkPathW(szPath))
    {
        return S_OK;
    }

    hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IApplicationResolver, &par));
    if (FAILED(hr))
        return hr;

    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi));
    if (SUCCEEDED(hr))
    {
        CComPtr<IShellItem> psiParent;
        PWSTR pszDestAppID;
        SFGAOF sfgao;

        hr = MakeDestinationItemForApp(psi, par, pszAppID, dwProcessId, pszAppPath, &pszDestAppID);
        if (SUCCEEDED(hr))
        {
            hr = AddPointToItem(psi, pszDestAppID, pszAppPath);
            CoTaskMemFree(pszDestAppID);
        }

        /* The folder of a used file shows up in Explorer's jump list */
        if (SUCCEEDED(psi->GetAttributes(SFGAO_FOLDER, &sfgao)) &&
            !(sfgao & SFGAO_FOLDER) &&
            SUCCEEDED(psi->GetParent(&psiParent)))
        {
            CComPtr<IShellItem> psiFolder;

            if (SUCCEEDED(AdjustForAutoDestList(psiParent, &psiFolder)))
            {
                PWSTR pszFolderAppID;

                if (SUCCEEDED(DetermineFolderDestinationParentAppID(par, psiFolder, NULL, &pszFolderAppID,
                                                                    pfExplorer)))
                {
                    if (*pfExplorer)
                        AddPointToItem(psiFolder, pszFolderAppID, NULL);
                    CoTaskMemFree(pszFolderAppID);
                }
            }
        }
    }

    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
    return hr;
}

/* Counts an application supplied link, rebuilt from its parts */
VOID
AddLinkToAutoDestList(
    _In_ DWORD dwProcessId,
    _In_ PCWSTR pszPath,
    _In_opt_ PCWSTR pszArgs,
    _In_opt_ PCWSTR pszTitle,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszDescription,
    _In_opt_ PCWSTR pszIconPath,
    _In_ int iIcon)
{
    CComPtr<IApplicationResolver> par;
    CComPtr<IShellLinkW> psl;
    CComPtr<IPropertyStore> pps;
    CComHeapPtr<WCHAR> pszProcessAppID;
    PROPVARIANT pv;
    HRESULT hr;

    if (IsNoRemoteDestinationsPolicy() && PathIsNetworkPathW(pszPath))
        return;

    hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IApplicationResolver, &par));
    if (FAILED(hr))
        return;

    if (!pszAppID)
    {
        hr = par->GetAppIDForProcess(dwProcessId, &pszProcessAppID, NULL, NULL, NULL);
        if (FAILED(hr))
            return;
        pszAppID = pszProcessAppID;
    }

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &psl));
    if (SUCCEEDED(hr))
        hr = psl->SetPath(pszPath);
    if (SUCCEEDED(hr) && pszArgs)
        hr = psl->SetArguments(pszArgs);
    if (SUCCEEDED(hr))
        hr = psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps));
    if (FAILED(hr))
        return;

    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    hr = SHStrDupW(pszTitle, &pv.pwszVal);
    if (FAILED(hr))
        PropVariantInit(&pv);

    if (SUCCEEDED(hr))
        hr = pps->SetValue(PKEY_Title, pv);
    if (SUCCEEDED(hr))
        hr = pps->Commit();
    if (SUCCEEDED(hr) && pszDescription)
        hr = psl->SetDescription(pszDescription);
    if (SUCCEEDED(hr) && pszIconPath)
        hr = psl->SetIconLocation(pszIconPath, iIcon);
    if (SUCCEEDED(hr))
        hr = AddPointToItem(psl, pszAppID, NULL);
    PropVariantClear(&pv);

    if (SUCCEEDED(hr))
        JumpListCounterIncrement();
}

/* The parts of an application link SHARD_LINK passes on */
static
HRESULT
GetLinkValues(
    _In_ IShellLinkW *psl,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ UINT cchPath,
    _Outptr_ PWSTR *ppszTitle,
    _Outptr_ PWSTR *ppszArgs,
    _Outptr_result_maybenull_ PWSTR *ppszDescription,
    _Outptr_result_maybenull_ PWSTR *ppszIconPath,
    _Out_ int *piIcon)
{
    CComPtr<IPropertyStore> pps;
    CComHeapPtr<WCHAR> pszArgs;
    CComHeapPtr<WCHAR> pszTitle;
    CComHeapPtr<WCHAR> pszDescription;
    CComHeapPtr<WCHAR> pszIconPath;
    WCHAR szText[MAX_PATH];
    HRESULT hr;

    *ppszTitle = NULL;
    *ppszArgs = NULL;
    *ppszDescription = NULL;
    *ppszIconPath = NULL;
    *piIcon = 0;

    if (psl->GetPath(pszPath, cchPath, NULL, SLGP_RAWPATH) != S_OK)
        return E_INVALIDARG;

    hr = psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps));
    if (FAILED(hr))
        return hr;

    if (IsSeparator(psl))
        return E_INVALIDARG;

    hr = JumpListGetString(pps, PKEY_Link_Arguments, &pszArgs);
    if (SUCCEEDED(hr))
        hr = JumpListGetString(pps, PKEY_Title, &pszTitle);
    if (FAILED(hr))
        return hr;

    if (SUCCEEDED(psl->GetDescription(szText, _countof(szText))))
    {
        hr = SHStrDupW(szText, &pszDescription);
        if (FAILED(hr))
            return hr;
    }

    if (SUCCEEDED(psl->GetIconLocation(szText, _countof(szText), piIcon)))
    {
        hr = SHStrDupW(szText, &pszIconPath);
        if (FAILED(hr))
            return hr;
    }

    *ppszArgs = pszArgs.Detach();
    *ppszTitle = pszTitle.Detach();
    *ppszDescription = pszDescription.Detach();
    *ppszIconPath = pszIconPath.Detach();
    return S_OK;
}

static
VOID
AddLinkFromShard(
    _In_ IShellLinkW *psl,
    _In_opt_ PCWSTR pszAppID)
{
    WCHAR szPath[MAX_PATH];
    PWSTR pszTitle;
    PWSTR pszArgs;
    PWSTR pszDescription;
    PWSTR pszIconPath;
    int iIcon;

    if (FAILED(GetLinkValues(psl, szPath, _countof(szPath), &pszTitle, &pszArgs, &pszDescription,
                             &pszIconPath, &iIcon)))
    {
        return;
    }

    AddLinkToAutoDestList(GetCurrentProcessId(), szPath, pszArgs, pszTitle, pszAppID, pszDescription,
                          pszIconPath, iIcon);

    CoTaskMemFree(pszTitle);
    CoTaskMemFree(pszArgs);
    CoTaskMemFree(pszDescription);
    CoTaskMemFree(pszIconPath);
}

static
BOOL
IsProgramExtension(
    _In_ PCWSTR pszExt)
{
    for (PCWSTR psz = c_szProgramExts; *psz; psz += wcslen(psz) + 1)
    {
        if (!_wcsicmp(psz, pszExt))
            return TRUE;
    }
    return FALSE;
}

/* TRUE when this file just went to the recent folder; otherwise it becomes the last file */
static
BOOL
CheckLastRecentFile(
    _In_ PCWSTR pszPath)
{
    FILETIME ft;
    ULONGLONG ullNow;
    BOOL fCached;

    GetSystemTimeAsFileTime(&ft);
    ullNow = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    g_csLastFile.Lock();
    fCached = g_pszLastFile && !wcscmp(g_pszLastFile, pszPath) &&
              ullNow - g_ullLastFileTime <= LAST_FILE_TIMEOUT;
    if (!fCached)
    {
        CoTaskMemFree(g_pszLastFile);
        g_pszLastFile = NULL;
        if (SUCCEEDED(SHStrDupW(pszPath, &g_pszLastFile)))
            g_ullLastFileTime = ullNow;
    }
    g_csLastFile.Unlock();
    return fCached;
}

static
VOID
ForgetLastRecentFile(VOID)
{
    g_csLastFile.Lock();
    CoTaskMemFree(g_pszLastFile);
    g_pszLastFile = NULL;
    g_csLastFile.Unlock();
}

/* Items that cannot be linked to, or whose type opts out, stay out of the recent folder */
static
BOOL
ShouldAddToRecentDocs(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    CComPtr<IShellFolder> psf;
    CComPtr<IQueryAssociations> pqa;
    PCUITEMID_CHILD pidlChild;
    SFGAOF sfgao = SFGAO_CANLINK;
    DWORD dwEditFlags = 0;
    DWORD cbEditFlags = sizeof(dwEditFlags);

    if (FAILED(SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psf), &pidlChild)))
        return TRUE;

    if (FAILED(psf->GetAttributesOf(1, &pidlChild, &sfgao)) || !(sfgao & SFGAO_CANLINK))
        return FALSE;

    if (FAILED(psf->GetUIObjectOf(NULL, 1, &pidlChild, IID_IQueryAssociations, NULL, (void **)&pqa)))
    {
        WCHAR szName[MAX_PATH];
        PCWSTR pszExt;

        /* File system folders hand out no association object, so go by the file type */
        if (FAILED(SHGetNameAndFlagsW(pidl, SHGDN_INFOLDER | SHGDN_FORPARSING, szName, _countof(szName), NULL)))
            return TRUE;
        pszExt = PathFindExtensionW(szName);
        if (!*pszExt ||
            FAILED(AssocCreate(CLSID_QueryAssociations, IID_PPV_ARG(IQueryAssociations, &pqa))) ||
            FAILED(pqa->Init(0, pszExt, NULL, NULL)))
        {
            return TRUE;
        }
    }

    if (SUCCEEDED(pqa->GetData(0, ASSOCDATA_EDITFLAGS, NULL, &dwEditFlags, &cbEditFlags)) &&
        (dwEditFlags & FTA_NoRecentDocs))
    {
        return FALSE;
    }

    /* The NoRecentDocs value on the type counts as the edit flag */
    return pqa->GetData(0, ASSOCDATA_VALUE, L"NoRecentDocs", NULL, NULL) != S_OK;
}

static
BOOL
IsFolderNeverRecent(
    _In_ PCWSTR pszPath)
{
    for (UINT i = 0; i < _countof(c_rgpfidNotRecent); i++)
    {
        PWSTR pszFolder;
        BOOL fMatch;

        if (FAILED(SHGetKnownFolderPath(*c_rgpfidNotRecent[i], KF_FLAG_DONT_VERIFY, NULL, &pszFolder)))
            continue;
        fMatch = !_wcsicmp(pszFolder, pszPath);
        CoTaskMemFree(pszFolder);
        if (fMatch)
            return TRUE;
    }
    return FALSE;
}

/*
 * The RecentDocs list for all documents (pszType NULL), for one extension, or for
 * folders. Entries hold the name followed by the ID of the link in the recent folder.
 */
static
HRESULT
OpenRecentList(
    _In_opt_ PCWSTR pszType,
    _Out_ UINT *pcMax,
    _Outptr_ IMruDataList **ppmru)
{
    CComPtr<IMruDataList> pmru;
    HKEY hKey;
    UINT cMax;
    HRESULT hr;

    *ppmru = NULL;
    if (!pszType)
    {
        cMax = SHRestricted(REST_MaxRecentDocs);
        if (!cMax)
            cMax = 150;
    }
    else if (!_wcsicmp(pszType, L"Folder"))
    {
        cMax = 30;
    }
    else
    {
        /* Types marked as major documents keep more entries */
        cMax = SHGetValueW(HKEY_CLASSES_ROOT, pszType, L"MajorDoc", NULL, NULL, NULL) == ERROR_SUCCESS ? 20 : 10;
    }
    *pcMax = cMax;

    /* Older builds kept a comctl32 list with another layout here */
    if (!pszType &&
        SHGetValueW(HKEY_CURRENT_USER, c_szRecentDocsKey, L"MRUList", NULL, NULL, NULL) == ERROR_SUCCESS &&
        SHGetValueW(HKEY_CURRENT_USER, c_szRecentDocsKey, L"MRUListEx", NULL, NULL, NULL) != ERROR_SUCCESS)
    {
        SHDeleteKeyW(HKEY_CURRENT_USER, c_szRecentDocsKey);
    }

    if (RegCreateKeyExW(HKEY_CURRENT_USER, c_szRecentDocsKey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey,
                        NULL) != ERROR_SUCCESS)
    {
        return E_FAIL;
    }

    hr = CoCreateInstance(CLSID_MruLongList, NULL, CLSCTX_INPROC_SERVER, IID_IMruDataList, (void **)&pmru);
    if (SUCCEEDED(hr))
        hr = pmru->InitData(cMax, c_uMruMatchName, hKey, pszType, NULL);
    RegCloseKey(hKey);

    if (SUCCEEDED(hr))
        *ppmru = pmru.Detach();
    return hr;
}

/* Reads one entry; the name and link ID point into the returned buffer */
static
HRESULT
ReadRecentEntry(
    _In_ IMruDataList *pmru,
    _In_ UINT iSlot,
    _Outptr_ BYTE **ppbData,
    _Outptr_ PCWSTR *ppszName,
    _Outptr_ PCUITEMID_CHILD *ppidlLink)
{
    CHeapPtr<BYTE, CLocalAllocator> pbData;
    DWORD cbData;
    SIZE_T cbName, cbLeft;
    PCWSTR pszName;
    PCUITEMID_CHILD pidl;
    HRESULT hr;

    *ppbData = NULL;
    *ppszName = NULL;
    *ppidlLink = NULL;

    hr = pmru->QueryInfo(iSlot, NULL, &cbData);
    if (FAILED(hr))
        return hr;
    if (!pbData.Allocate(cbData))
        return E_OUTOFMEMORY;
    hr = pmru->GetData(iSlot, pbData, cbData);
    if (FAILED(hr))
        return hr;

    /* Entries written by older builds use another layout, so check the bounds */
    pszName = (PCWSTR)(BYTE *)pbData;
    cbName = 0;
    while (cbName + sizeof(WCHAR) <= cbData && pszName[cbName / sizeof(WCHAR)])
        cbName += sizeof(WCHAR);
    cbName += sizeof(WCHAR);
    if (cbName + sizeof(USHORT) > cbData)
        return E_UNEXPECTED;

    /* The rest is the ID of one link and its terminator */
    pidl = (PCUITEMID_CHILD)(pbData + cbName);
    cbLeft = cbData - cbName;
    if (cbLeft < 2 * sizeof(USHORT) || pidl->mkid.cb != cbLeft - sizeof(USHORT) ||
        pbData[cbData - 2] || pbData[cbData - 1])
    {
        return E_UNEXPECTED;
    }

    *ppszName = pszName;
    *ppidlLink = pidl;
    *ppbData = pbData.Detach();
    return S_OK;
}

/*
 * Drops the entry a new one replaces: the one with the same name, or the oldest when
 * fByName is FALSE or the name is new. Its link goes away and so does its entry in
 * pmruOther.
 */
static
VOID
DropRecentEntry(
    _In_ IMruDataList *pmru,
    _In_ UINT cMax,
    _In_ PCWSTR pszName,
    _In_ BOOL fByName,
    _In_ PCIDLIST_ABSOLUTE pidlRecent,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ IMruDataList *pmruOther)
{
    PCUITEMID_CHILD pidlChild;
    PCWSTR pszOldName;
    BYTE *pbData;
    UINT iSlot;

    if (!fByName || FAILED(pmru->FindData(pszName, (DWORD)((wcslen(pszName) + 1) * sizeof(WCHAR)), &iSlot)))
        iSlot = cMax - 1;

    if (FAILED(ReadRecentEntry(pmru, iSlot, &pbData, &pszOldName, &pidlChild)))
        return;

    PIDLIST_ABSOLUTE pidlLink = ILCombine(pidlRecent, pidlChild);
    if (pidlLink)
    {
        if (!ILIsEqual(pidlLink, pidl))
        {
            WCHAR szLink[MAX_PATH];
            UINT iOther;

            if (SHGetPathFromIDListW(pidlLink, szLink) && DeleteFileW(szLink))
                SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, szLink, NULL);

            if (pmruOther &&
                SUCCEEDED(pmruOther->FindData(pszOldName, (DWORD)((wcslen(pszOldName) + 1) * sizeof(WCHAR)), &iOther)))
            {
                pmruOther->Delete(iOther);
            }
        }
        ILFree(pidlLink);
    }

    pmru->Delete(iSlot);
    LocalFree(pbData);
}

/* Makes the link the recent folder shows for an item; a link is copied instead */
static
HRESULT
CreateRecentLink(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ PCWSTR pszRecentDir,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlLink)
{
    WCHAR szLink[MAX_PATH];
    WCHAR szTarget[MAX_PATH];
    BOOL fMustCopy;
    HRESULT hr;

    *ppidlLink = NULL;
    if (!SHGetNewLinkInfoW((PCWSTR)pidl, pszRecentDir, szLink, &fMustCopy, SHGNLI_PIDL))
        return E_FAIL;

    if (fMustCopy)
    {
        if (!SHGetPathFromIDListW(pidl, szTarget) || !CopyFileW(szTarget, szLink, TRUE))
            return E_FAIL;
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, szLink, NULL);
    }
    else
    {
        CComPtr<IShellLinkW> psl;
        CComPtr<IShellLinkDataList> pdl;
        CComPtr<IPersistFile> ppf;
        DWORD dwAttributes = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STREAM;
        DWORD dwFlags;

        hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &psl));
        if (SUCCEEDED(hr))
            hr = psl->SetIDList(pidl);
        if (FAILED(hr))
            return hr;

        /* A file starts in its own folder */
        if (SUCCEEDED(SHGetNameAndFlagsW(pidl, SHGDN_FORPARSING, szTarget, _countof(szTarget), &dwAttributes)) &&
            (dwAttributes & (SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STREAM)) == (SFGAO_FILESYSTEM | SFGAO_STREAM) &&
            PathRemoveFileSpecW(szTarget))
        {
            psl->SetWorkingDirectory(szTarget);
        }

        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IShellLinkDataList, &pdl))) &&
            SUCCEEDED(pdl->GetFlags(&dwFlags)))
        {
            pdl->SetFlags(dwFlags | SLDF_DISABLE_KNOWNFOLDER_RELATIVE_TRACKING);
        }

        hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
        if (SUCCEEDED(hr))
            hr = ppf->Save(szLink, TRUE);
        if (FAILED(hr))
            return hr;
    }

    *ppidlLink = ILCreateFromPathW(szLink);
    return *ppidlLink ? S_OK : E_FAIL;
}

/* Puts an item in the recent folder, the list of all documents and the list for its type */
static
BOOL
AddToRecentLists(
    _In_ IMruDataList *pmruAll,
    _In_ UINT cAll,
    _In_ PCIDLIST_ABSOLUTE pidlRecent,
    _In_ PCWSTR pszRecentDir,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ PCWSTR pszName,
    _In_ PCWSTR pszType)
{
    CComPtr<IMruDataList> pmruType;
    CHeapPtr<BYTE, CLocalAllocator> pbData;
    PIDLIST_ABSOLUTE pidlLink;
    PCUITEMID_CHILD pidlChild;
    SIZE_T cbName, cbChild;
    UINT cType = 0;

    OpenRecentList(pszType, &cType, &pmruType);
    DropRecentEntry(pmruAll, cAll, pszName, TRUE, pidlRecent, pidl, pmruType);

    if (FAILED(CreateRecentLink(pidl, pszRecentDir, &pidlLink)))
        return FALSE;

    pidlChild = ILFindLastID(pidlLink);
    cbName = (wcslen(pszName) + 1) * sizeof(WCHAR);
    cbChild = ILGetSize(pidlChild);
    if (!pbData.Allocate(cbName + cbChild))
    {
        ILFree(pidlLink);
        return FALSE;
    }
    CopyMemory(pbData, pszName, cbName);
    CopyMemory(pbData + cbName, pidlChild, cbChild);
    ILFree(pidlLink);

    pmruAll->AddData(pbData, (DWORD)(cbName + cbChild), NULL);
    if (pmruType)
    {
        DropRecentEntry(pmruType, cType, pszName, FALSE, pidlRecent, pidl, pmruAll);
        pmruType->AddData(pbData, (DWORD)(cbName + cbChild), NULL);
    }
    return TRUE;
}

/*
 * The recent folder part of a used document. The document goes in when its type is
 * registered, and its folder goes in too when that folder went to Explorer's jump list.
 */
static
VOID
AddToRecentFolder(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ PCWSTR pszPath,
    _In_ BOOL fExplorer)
{
    CComPtr<IMruDataList> pmruAll;
    WCHAR szName[MAX_PATH];
    WCHAR szRecent[MAX_PATH];
    PCWSTR pszName;
    UINT cAll;

    pszName = PathFindFileNameW(pszPath);
    if (UrlIsW(pszPath, URLIS_URL))
    {
        StringCchCopyW(szName, _countof(szName), pszName);
        UrlUnescapeW(szName, NULL, NULL, URL_UNESCAPE_INPLACE);
        pszName = szName;
    }

    PIDLIST_ABSOLUTE pidlRecent = SHCloneSpecialIDList(NULL, CSIDL_RECENT, TRUE);
    if (!pidlRecent)
        return;

    if (SHGetPathFromIDListW(pidlRecent, szRecent) && SUCCEEDED(OpenRecentList(NULL, &cAll, &pmruAll)))
    {
        if (ShouldAddToRecentDocs(pidl))
        {
            PCWSTR pszExt = PathFindExtensionW(pszName);
            HKEY hKey;

            if (*pszExt && SUCCEEDED(AssocQueryKeyW(0, ASSOCKEY_SHELLEXECCLASS, pszExt, NULL, &hKey)))
            {
                RegCloseKey(hKey);
                AddToRecentLists(pmruAll, cAll, pidlRecent, szRecent, pidl, pszName, pszExt);
            }
        }

        if (fExplorer)
        {
            PIDLIST_ABSOLUTE pidlFolder = ILClone(pidl);
            WCHAR szFolder[MAX_PATH];
            WCHAR szFolderName[MAX_PATH];

            if (pidlFolder && ILRemoveLastID(pidlFolder) &&
                SUCCEEDED(SHGetNameAndFlagsW(pidlFolder, SHGDN_FORPARSING, szFolder, _countof(szFolder), NULL)) &&
                !IsFolderNeverRecent(szFolder))
            {
                if (FAILED(SHGetNameAndFlagsW(pidlFolder, SHGDN_NORMAL, szFolderName, _countof(szFolderName), NULL)))
                    StringCchCopyW(szFolderName, _countof(szFolderName), PathFindFileNameW(szFolder));
                AddToRecentLists(pmruAll, cAll, pidlRecent, szRecent, pidlFolder, szFolderName, L"Folder");
            }
            ILFree(pidlFolder);
        }
    }
    ILFree(pidlRecent);
}

/* Counts a used document in the jump lists and the recent folder */
static
VOID
AddRecentDoc(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath)
{
    WCHAR szExe[MAX_PATH];
    PWSTR pszPath;
    BOOL fExplorer;

    if (FAILED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &pszPath)))
        return;

    /* Temporary files and items without a name are not remembered */
    PCWSTR pszFile = PathFindFileNameW(pszPath);
    if (!IsProgramExtension(PathFindExtensionW(pszPath)) && pszFile && *pszFile && !PathIsTemporaryW(pszPath))
    {
        if (!pszAppPath && GetModuleFileNameW(NULL, szExe, _countof(szExe)) && szExe[0])
            pszAppPath = szExe;

        AddToAutoDestList(GetCurrentProcessId(), pidl, pszAppID, pszAppPath, &fExplorer);
        if (!CheckLastRecentFile(pszPath))
            AddToRecentFolder(pidl, pszPath, fExplorer);
    }
    CoTaskMemFree(pszPath);
}

/* Paths that do not exist yet still get a simple ID list */
static
PIDLIST_ABSOLUTE
ParseRecentDoc(
    _In_ PCWSTR pszPath)
{
    PIDLIST_ABSOLUTE pidl;

    if (SUCCEEDED(SHParseDisplayName(pszPath, NULL, &pidl, 0, NULL)))
        return pidl;
    return SHSimpleIDListFromPathW(pszPath);
}

/* SHAddToRecentDocs with a NULL item: empties the recent folder and its lists */
static
VOID
CleanRecentDocs(VOID)
{
    WCHAR szDir[MAX_PATH];
    WCHAR szFile[MAX_PATH];
    WIN32_FIND_DATAW find;
    HANDLE hFind;

    SHDeleteKeyW(HKEY_CURRENT_USER, c_szRecentDocsKey);

    if (SHGetSpecialFolderPathW(NULL, szDir, CSIDL_RECENT, FALSE) &&
        SUCCEEDED(StringCchCopyW(szFile, _countof(szFile), szDir)) &&
        PathAppendW(szFile, L"*.lnk"))
    {
        hFind = FindFirstFileW(szFile, &find);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (SUCCEEDED(StringCchCopyW(szFile, _countof(szFile), szDir)) && PathAppendW(szFile, find.cFileName))
                    DeleteFileW(szFile);
            } while (FindNextFileW(hFind, &find));
            FindClose(hFind);
        }
    }

    SHChangeNotify(0, SHCNF_FLUSH, NULL, NULL);
    ForgetLastRecentFile();
}

/*
 * SHAddToRecentDocs. Windows 7 hands the document to Explorer, which does this work on
 * behalf of the calling process; ReactOS does it in the caller. pszAppPath names the
 * program that opened the document when that is not the calling process.
 */
EXTERN_C
VOID
SHELL_AddToRecentDocs(
    _In_ UINT uFlags,
    _In_opt_ LPCVOID pv,
    _In_opt_ PCWSTR pszAppPath)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    PCWSTR pszAppID = NULL;
    HRESULT hrInit;

    if (!pv)
    {
        CleanRecentDocs();
        return;
    }

    if (!IsRecentDocsEnabled())
        return;

    /* The caller need not have set up COM */
    hrInit = CoInitialize(NULL);

    switch (uFlags)
    {
        case SHARD_PIDL:
            AddRecentDoc((PCIDLIST_ABSOLUTE)pv, NULL, pszAppPath);
            break;

        case SHARD_PATHA:
        {
            WCHAR szPath[MAX_PATH];

            SHAnsiToUnicode((PCSTR)pv, szPath, _countof(szPath));
            pidl = ParseRecentDoc(szPath);
            break;
        }

        case SHARD_PATHW:
            pidl = ParseRecentDoc((PCWSTR)pv);
            break;

        case SHARD_APPIDINFO:
        {
            const SHARDAPPIDINFO *pInfo = (const SHARDAPPIDINFO *)pv;

            if (FAILED(SHGetIDListFromObject(pInfo->psi, &pidl)))
                pidl = NULL;
            pszAppID = pInfo->pszAppID;
            break;
        }

        case SHARD_APPIDINFOIDLIST:
        {
            const SHARDAPPIDINFOIDLIST *pInfo = (const SHARDAPPIDINFOIDLIST *)pv;

            AddRecentDoc(pInfo->pidl, pInfo->pszAppID, pszAppPath);
            break;
        }

        /* Application links only go to the jump lists */
        case SHARD_LINK:
            AddLinkFromShard((IShellLinkW *)pv, NULL);
            break;

        case SHARD_APPIDINFOLINK:
        {
            const SHARDAPPIDINFOLINK *pInfo = (const SHARDAPPIDINFOLINK *)pv;

            AddLinkFromShard(pInfo->psl, pInfo->pszAppID);
            break;
        }

        case SHARD_SHELLITEM:
            if (FAILED(SHGetIDListFromObject((IShellItem *)pv, &pidl)))
                pidl = NULL;
            break;

        default:
            FIXME("Unsupported flags: %u\n", uFlags);
            break;
    }

    if (pidl)
    {
        AddRecentDoc(pidl, pszAppID, pszAppPath);
        ILFree(pidl);
    }

    if (SUCCEEDED(hrInit))
        CoUninitialize();
}

/* The app ID Explorer gives the program at pszAppPath */
static
HRESULT
GetAppIDForAppPath(
    _In_ IApplicationResolver *par,
    _In_ PCWSTR pszAppPath,
    _Outptr_ PWSTR *ppszAppID)
{
    CComPtr<IShellItem> psi;
    HRESULT hr;

    *ppszAppID = NULL;
    hr = par->GetBestShortcutAndAppIDForAppPath(pszAppPath, NULL, ppszAppID);
    if (SUCCEEDED(hr))
        return hr;

    hr = SHCreateItemFromParsingName(pszAppPath, NULL, IID_PPV_ARG(IShellItem, &psi));
    if (SUCCEEDED(hr))
        hr = par->CanPinApp(psi);
    if (SUCCEEDED(hr))
        hr = GenerateAppID(pszAppPath, NULL, NULL, ppszAppID);
    return hr;
}

/*
 * A document a verb or the Open With dialog opened with the program at pszAppPath. It is
 * credited to that program, under pszAppID when given; nothing is added when the program
 * has no app ID.
 */
VOID
AddToRecentDocsForApp(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ PCWSTR pszAppID,
    _In_ PCWSTR pszAppPath)
{
    CComHeapPtr<WCHAR> pszResolved;
    SHARDAPPIDINFOIDLIST info;
    HRESULT hrInit;

    if (!IsRecentDocsEnabled())
        return;

    hrInit = CoInitialize(NULL);
    if (!pszAppID)
    {
        CComPtr<IApplicationResolver> par;

        if (SUCCEEDED(CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARG(IApplicationResolver, &par))) &&
            SUCCEEDED(GetAppIDForAppPath(par, pszAppPath, &pszResolved)))
        {
            pszAppID = pszResolved;
        }
    }

    if (pszAppID)
    {
        info.pidl = pidl;
        info.pszAppID = pszAppID;
        SHELL_AddToRecentDocs(SHARD_APPIDINFOIDLIST, &info, pszAppPath);
    }

    if (SUCCEEDED(hrInit))
        CoUninitialize();
}
