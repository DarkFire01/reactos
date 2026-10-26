/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Start menu program cache and application resolver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <ndk/psfuncs.h>
#include "CAppResolver.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define FILEASSOC_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileAssociation"

/* Folder changes the resolver needs to hear about */
#define RESOLVER_NOTIFY_EVENTS  (SHCNE_RENAMEITEM | SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | \
                                 SHCNE_ATTRIBUTES | SHCNE_UPDATEDIR | SHCNE_UPDATEITEM | SHCNE_RENAMEFOLDER)

/* The taskbar learns about renamed and deleted shortcuts through this message */
#define TWM_SHORTCUTCHANGED     (WM_USER + 229)

/* GetPathOrDarwinID puts this in front of an installer product ID */
#define DARWIN_MARKER           L'\x0001'

/* Root that only Explorer's own shortcut changes are reported under */
#define RESOLVER_ROOT_PINNED    7

/* Hidden ID block that carries the start menu's private data about a shortcut */
#define HIDDENID_STARTPANE      0xBEEF000B
#define STARTPANE_VERSION       5

/* Root folder flags */
#define ROOTF_NOSUBFOLDERS      0x1     /* Only the root itself is read */
#define ROOTF_SKIPIFNESTED      0x4     /* Already read through the root before it */
#define ROOTF_SUBFOLDERPOLICY   0x8     /* Subfolders follow the start menu policy */

typedef struct _RESOLVER_ROOT_INFO
{
    const KNOWNFOLDERID *pkfid;
    DWORD dwKFFlags;
    DWORD dwRootFlags;
    BOOL fCommon;
} RESOLVER_ROOT_INFO;

static const RESOLVER_ROOT_INFO g_ResolverRoots[] =
{
    { &FOLDERID_StartMenu,          0,                  ROOTF_SUBFOLDERPOLICY,  FALSE },
    { &FOLDERID_Programs,           0,                  ROOTF_SKIPIFNESTED,     FALSE },
    { &FOLDERID_CommonStartMenu,    0,                  ROOTF_SUBFOLDERPOLICY,  TRUE },
    { &FOLDERID_CommonPrograms,     0,                  ROOTF_SKIPIFNESTED,     TRUE },
    { &FOLDERID_Desktop,            KF_FLAG_NO_ALIAS,   ROOTF_NOSUBFOLDERS,     FALSE },
    { &FOLDERID_PublicDesktop,      KF_FLAG_NO_ALIAS,   ROOTF_NOSUBFOLDERS,     TRUE },
    { &FOLDERID_Games,              0,                  0,                      FALSE },
    { &FOLDERID_UserPinned,         0,                  0,                      FALSE },
};

#define RESOLVER_ROOT_COUNT     _countof(g_ResolverRoots)
#define RESOLVER_FIRST_DESKTOP  4
#define RESOLVER_LAST_DESKTOP   5

/* User assist store, kept by the shell for the start menu's usage counts */
struct DECLSPEC_UUID("90D75131-43A6-4664-9AF8-DCCEB85A7462") DECLSPEC_NOVTABLE
IShellUserAssist : public IUnknown
{
    STDMETHOD(FireEvent)(REFGUID rguid, DWORD dwEvent, PCWSTR pszEntry, DWORD dwParam) PURE;
    STDMETHOD(QueryEntry)(REFGUID rguid, PCWSTR pszEntry, UA_USAGE *pUsage) PURE;
};

static const IID IID_IShellUserAssistPrivate = __uuidof(IShellUserAssist);
static const CLSID CLSID_UserAssistStore =
    { 0xdd313e04, 0xfeff, 0x11d1, { 0x8e, 0xcd, 0x00, 0x00, 0xf8, 0x7a, 0x47, 0x0c } };
static const GUID UA_GROUP_APPLICATIONS =
    { 0xcebff5cd, 0xace2, 0x4f4f, { 0x91, 0x78, 0x99, 0x26, 0xf4, 0x17, 0x49, 0xea } };

struct APP_RECORD;

/* A shortcut found in one of the root folders */
struct APP_SHORTCUT
{
    PIDLIST_ABSOLUTE pidlFolder;
    PITEMID_CHILD pidlItem;
    APP_RECORD *pApp;
    PWSTR pszArgs;
    BOOL fInteresting;
    BOOL fOnDesktop;
};

/* One application, however many shortcuts start it */
struct APP_RECORD
{
    PWSTR pszEncodedPath;
    PWSTR pszAppID;
    LONG cShortcuts;            /* The record is stale while no shortcut points at it */
    APP_SHORTCUT *pBest;
    UA_USAGE Usage;
};

struct RESOLVER_ROOT
{
    PIDLIST_ABSOLUTE pidl;
    HDPA hdpaShortcuts;
    BOOL fScanned;
    BOOL fDirty;
    BOOL fRegistered;
};

/* Kept once per process, like the shortcuts it describes */
class CAppShortcutCache
{
public:
    CAppShortcutCache();

    VOID Lock() { EnterCriticalSection(&m_cs); }
    VOID Unlock() { LeaveCriticalSection(&m_cs); }

    VOID EnsureScanned();
    VOID MarkDirty(
        _In_ UINT uRoot);

    BOOL PathIsInterestingExe(
        _In_ PCWSTR pszPath,
        _In_ BOOL fHasArgs);
    BOOL PathIsInterestingLink(
        _In_ PCWSTR pszLinkName,
        _In_ BOOL fInteresting);

    APP_RECORD *FindRecordByAppID(
        _In_ PCWSTR pszAppID);

    int GetRecordCount() const { return DPA_GetPtrCount(m_hdpaApps); }
    APP_RECORD *GetRecord(_In_ int i) { return (APP_RECORD *)DPA_FastGetPtr(m_hdpaApps, i); }

    IAppResolverNotify *m_pARNotify;
    IStartMenuItemsCacheNotify *m_pSMNotify;
    RESOLVER_ROOT m_Roots[RESOLVER_ROOT_COUNT];

private:
    VOID InitStringLists();
    VOID ScanRoot(
        _In_ UINT uRoot,
        _In_ IShellFolder *psfDesktop);
    VOID FreeRootShortcuts(
        _In_ UINT uRoot);
    VOID ChooseBestShortcuts();
    VOID FillFolder(
        _In_ UINT uRoot,
        _In_ PCIDLIST_ABSOLUTE pidlFolder,
        _In_ BOOL fRecurse);
    VOID AddShortcut(
        _In_ UINT uRoot,
        _In_ PCIDLIST_ABSOLUTE pidlFolder,
        _In_ IShellFolder *psf,
        _In_ PCUITEMID_CHILD pidlChild);
    APP_RECORD *GetRecord(
        _In_ PCWSTR pszAppID,
        _In_ PCWSTR pszEncodedPath);
    BOOL IsExcludedExe(
        _In_ PCWSTR pszPath,
        _In_ BOOL fHasArgs);
    BOOL IsInterestingDirectory(
        _In_ PCIDLIST_ABSOLUTE pidlFolder);

    CRITICAL_SECTION m_cs;
    HDPA m_hdpaApps;
    HDPA m_hdpaKillExes;        /* Programs that never show, by file name */
    HDPA m_hdpaHostExes;        /* Hosts that only show when started with arguments */
    HDPA m_hdpaKillNames;       /* Shortcut names that hide a shortcut */
    HDPA m_hdpaKeepNames;       /* Shortcut names that show however they look */
    HDPA m_hdpaExcludedDirs;    /* Startup folders */
};

static CAppShortcutCache *g_pAppShortcutCache;

static
CAppShortcutCache *
GetAppShortcutCache(VOID)
{
    if (!g_pAppShortcutCache)
    {
        CAppShortcutCache *pCache = new CAppShortcutCache();

        if (pCache && InterlockedCompareExchangePointer((PVOID *)&g_pAppShortcutCache, pCache, NULL))
            delete pCache;
    }
    return g_pAppShortcutCache;
}

static
VOID
AppendStringCopy(
    _Inout_ HDPA hdpa,
    _In_ PCWSTR psz)
{
    PWSTR pszCopy = StrDupW(psz);

    if (pszCopy && DPA_AppendPtr(hdpa, pszCopy) < 0)
        LocalFree(pszCopy);
}

/* Reads a value holding a list of names split by semicolons */
static
VOID
ReadStringList(
    _In_ HKEY hKey,
    _In_ PCWSTR pszValue,
    _Inout_ HDPA hdpa)
{
    DWORD cbData = 0;
    PWSTR pszData;

    if (RegQueryValueExW(hKey, pszValue, NULL, NULL, NULL, &cbData) != ERROR_SUCCESS)
        return;

    cbData += sizeof(WCHAR);
    pszData = (PWSTR)LocalAlloc(LPTR, cbData);
    if (!pszData)
        return;

    if (SHGetValueW(hKey, NULL, pszValue, NULL, pszData, &cbData) == ERROR_SUCCESS)
    {
        PWSTR pszNext = pszData;

        while (pszNext)
        {
            PWSTR pszSeparator = StrChrW(pszNext, L';');

            if (pszSeparator)
                *pszSeparator = UNICODE_NULL;
            if (*pszNext)
                AppendStringCopy(hdpa, pszNext);
            pszNext = pszSeparator ? pszSeparator + 1 : NULL;
        }
    }
    LocalFree(pszData);
}

CAppShortcutCache::CAppShortcutCache() :
    m_pARNotify(NULL),
    m_pSMNotify(NULL)
{
    InitializeCriticalSection(&m_cs);
    ZeroMemory(m_Roots, sizeof(m_Roots));
    m_hdpaApps = DPA_Create(16);
    m_hdpaKillExes = DPA_Create(4);
    m_hdpaHostExes = DPA_Create(4);
    m_hdpaKillNames = DPA_Create(4);
    m_hdpaKeepNames = DPA_Create(4);
    m_hdpaExcludedDirs = DPA_Create(4);
    InitStringLists();
}

VOID
CAppShortcutCache::InitStringLists()
{
    static const int s_ExcludedCsidls[] =
    {
        CSIDL_ALTSTARTUP,
        CSIDL_STARTUP,
        CSIDL_COMMON_ALTSTARTUP,
        CSIDL_COMMON_STARTUP,
    };
    static const HKEY s_KeepRoots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    WCHAR szPath[MAX_PATH];
    HKEY hKey;

    for (UINT i = 0; i < _countof(s_ExcludedCsidls); i++)
    {
        if (SUCCEEDED(SHGetFolderPathW(NULL, s_ExcludedCsidls[i], NULL, SHGFP_TYPE_CURRENT, szPath)))
            AppendStringCopy(m_hdpaExcludedDirs, szPath);
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FILEASSOC_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        ReadStringList(hKey, L"AddRemoveApps", m_hdpaKillExes);
        ReadStringList(hKey, L"HostApps", m_hdpaHostExes);
        ReadStringList(hKey, L"AddRemoveNames", m_hdpaKillNames);
        RegCloseKey(hKey);
    }

    for (UINT i = 0; i < _countof(s_KeepRoots); i++)
    {
        if (RegOpenKeyExW(s_KeepRoots[i], FILEASSOC_KEY L"\\StartMenu", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            WCHAR szName[MAX_PATH];
            DWORD cchName;

            for (DWORD iValue = 0; ; iValue++)
            {
                cchName = _countof(szName);
                if (RegEnumValueW(hKey, iValue, szName, &cchName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                AppendStringCopy(m_hdpaKeepNames, szName);
            }
            RegCloseKey(hKey);
        }
    }
}

/* Programs listed for removal never show, hosts only when given something to host */
BOOL
CAppShortcutCache::IsExcludedExe(
    _In_ PCWSTR pszPath,
    _In_ BOOL fHasArgs)
{
    PCWSTR pszName = PathFindFileNameW(pszPath);
    WCHAR szKey[MAX_PATH];
    HKEY hKey;
    BOOL fExcluded = FALSE;

    for (int i = 0; i < DPA_GetPtrCount(m_hdpaKillExes); i++)
    {
        if (!StrCmpIW(pszName, (PCWSTR)DPA_FastGetPtr(m_hdpaKillExes, i)))
            return TRUE;
    }

    if (!fHasArgs)
    {
        for (int i = 0; i < DPA_GetPtrCount(m_hdpaHostExes); i++)
        {
            if (!StrCmpIW(pszName, (PCWSTR)DPA_FastGetPtr(m_hdpaHostExes, i)))
                return TRUE;
        }
    }

    if (FAILED(StringCchPrintfW(szKey, _countof(szKey), L"Applications\\%s", pszName)))
        return FALSE;

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, szKey, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        fExcluded = RegValueExists(hKey, L"NoStartPage") ||
                    (!fHasArgs && RegValueExists(hKey, L"IsHostApp"));
        RegCloseKey(hKey);
    }
    return fExcluded;
}

BOOL
CAppShortcutCache::PathIsInterestingExe(
    _In_ PCWSTR pszPath,
    _In_ BOOL fHasArgs)
{
    switch (GetExeOrMscExtension(pszPath))
    {
        case 1:
            return TRUE;
        case 2:
            return !IsExcludedExe(pszPath, fHasArgs);
        default:
            return FALSE;
    }
}

/* Uninstall and help shortcuts hide by name unless listed to stay */
BOOL
CAppShortcutCache::PathIsInterestingLink(
    _In_ PCWSTR pszLinkName,
    _In_ BOOL fInteresting)
{
    if (fInteresting)
    {
        for (int i = 0; i < DPA_GetPtrCount(m_hdpaKillNames); i++)
        {
            if (StrStrIW(pszLinkName, (PCWSTR)DPA_FastGetPtr(m_hdpaKillNames, i)))
            {
                fInteresting = FALSE;
                break;
            }
        }
    }

    if (!fInteresting)
    {
        for (int i = 0; i < DPA_GetPtrCount(m_hdpaKeepNames); i++)
        {
            if (!StrCmpIW(pszLinkName, (PCWSTR)DPA_FastGetPtr(m_hdpaKeepNames, i)))
                return TRUE;
        }
    }
    return fInteresting;
}

BOOL
CAppShortcutCache::IsInterestingDirectory(
    _In_ PCIDLIST_ABSOLUTE pidlFolder)
{
    WCHAR szFolder[MAX_PATH];

    if (FAILED(SHGetNameAndFlagsW(pidlFolder, SHGDN_FORPARSING, szFolder, _countof(szFolder), NULL)))
        return TRUE;

    for (int i = 0; i < DPA_GetPtrCount(m_hdpaExcludedDirs); i++)
    {
        if (!lstrcmpiW((PCWSTR)DPA_FastGetPtr(m_hdpaExcludedDirs, i), szFolder))
            return FALSE;
    }
    return TRUE;
}

APP_RECORD *
CAppShortcutCache::FindRecordByAppID(
    _In_ PCWSTR pszAppID)
{
    for (int i = 0; i < GetRecordCount(); i++)
    {
        APP_RECORD *pApp = GetRecord(i);

        if (pApp->cShortcuts && CompareStringOrdinal(pApp->pszAppID, -1, pszAppID, -1, TRUE) == CSTR_EQUAL)
            return pApp;
    }
    return NULL;
}

APP_RECORD *
CAppShortcutCache::GetRecord(
    _In_ PCWSTR pszAppID,
    _In_ PCWSTR pszEncodedPath)
{
    APP_RECORD *pFree = NULL;

    for (int i = 0; i < GetRecordCount(); i++)
    {
        APP_RECORD *pApp = GetRecord(i);

        if (CompareStringOrdinal(pApp->pszAppID, -1, pszAppID, -1, TRUE) == CSTR_EQUAL)
            return pApp;
        if (!pApp->cShortcuts && !pFree)
            pFree = pApp;
    }

    /* Stale records are reused before the list grows */
    if (pFree)
    {
        LocalFree(pFree->pszAppID);
        LocalFree(pFree->pszEncodedPath);
    }
    else
    {
        pFree = (APP_RECORD *)LocalAlloc(LPTR, sizeof(*pFree));
        if (!pFree)
            return NULL;
        if (DPA_AppendPtr(m_hdpaApps, pFree) < 0)
        {
            LocalFree(pFree);
            return NULL;
        }
    }

    ZeroMemory(pFree, sizeof(*pFree));
    pFree->pszAppID = StrDupW(pszAppID);
    pFree->pszEncodedPath = StrDupW(pszEncodedPath);
    if (!pFree->pszAppID || !pFree->pszEncodedPath)
        return NULL;
    return pFree;
}

static
INT
CALLBACK
FreeShortcutCallback(
    _In_ PVOID p,
    _In_opt_ PVOID pData)
{
    APP_SHORTCUT *pShortcut = (APP_SHORTCUT *)p;

    UNREFERENCED_PARAMETER(pData);
    if (pShortcut->pApp)
    {
        if (pShortcut->pApp->pBest == pShortcut)
            pShortcut->pApp->pBest = NULL;
        pShortcut->pApp->cShortcuts -= 1;
    }
    ILFree(pShortcut->pidlFolder);
    ILFree(pShortcut->pidlItem);
    LocalFree(pShortcut->pszArgs);
    LocalFree(pShortcut);
    return TRUE;
}

VOID
CAppShortcutCache::FreeRootShortcuts(
    _In_ UINT uRoot)
{
    RESOLVER_ROOT *pRoot = &m_Roots[uRoot];

    if (pRoot->hdpaShortcuts)
    {
        DPA_DestroyCallback(pRoot->hdpaShortcuts, FreeShortcutCallback, NULL);
        pRoot->hdpaShortcuts = NULL;
    }
}

/* Start menu shortcuts beat desktop ones, then the one with the shortest arguments */
VOID
CAppShortcutCache::ChooseBestShortcuts()
{
    for (int i = 0; i < GetRecordCount(); i++)
        GetRecord(i)->pBest = NULL;

    for (UINT r = 0; r < RESOLVER_ROOT_COUNT; r++)
    {
        HDPA hdpa = m_Roots[r].hdpaShortcuts;

        for (int i = 0; hdpa && i < DPA_GetPtrCount(hdpa); i++)
        {
            APP_SHORTCUT *pShortcut = (APP_SHORTCUT *)DPA_FastGetPtr(hdpa, i);
            APP_SHORTCUT *pBest = pShortcut->pApp->pBest;

            if (!pShortcut->fInteresting)
                continue;

            if (!pBest ||
                (pBest->fOnDesktop && !pShortcut->fOnDesktop) ||
                (pBest->fOnDesktop == pShortcut->fOnDesktop &&
                 lstrlenW(pBest->pszArgs) > lstrlenW(pShortcut->pszArgs)))
            {
                pShortcut->pApp->pBest = pShortcut;
            }
        }
    }
}

VOID
CAppShortcutCache::AddShortcut(
    _In_ UINT uRoot,
    _In_ PCIDLIST_ABSOLUTE pidlFolder,
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidlChild)
{
    CComPtr<IShellLinkW> psl;
    WCHAR szTarget[MAX_PATH];
    WCHAR szPath[MAX_PATH];
    WCHAR szEncoded[MAX_PATH];
    WCHAR szName[MAX_PATH];
    WCHAR szLinkName[MAX_PATH];
    PWSTR pszArgs = NULL;
    PWSTR pszAppID = NULL;
    BOOL fPreventPinning;
    BOOL fExplicit;
    APP_SHORTCUT *pShortcut;
    APP_RECORD *pApp;
    WORD wHotkey = 0;

    if (FAILED(psf->GetUIObjectOf(NULL, 1, &pidlChild, IID_IShellLinkW, NULL, (void **)&psl)))
        return;

    /* Installer shortcuts need the darwin cache shell32 does not keep yet */
    if (FAILED(GetPathOrDarwinID(psl, szTarget, &pszArgs)) || szTarget[0] == DARWIN_MARKER)
        goto Cleanup;

    if (!ExpandEnvironmentStringsW(szTarget, szPath, _countof(szPath)))
        goto Cleanup;
    EncodeCommonRelativePath(szEncoded, _countof(szEncoded), szPath);

    /* A shortcut that names its own ID keeps it, any other is named after what it starts */
    GetShortcutAppProperties(psl, &pszAppID, &fPreventPinning, NULL);
    fExplicit = (pszAppID != NULL);
    if (FAILED(DisplayNameOfW(psf, pidlChild, SHGDN_INFOLDER, szName, _countof(szName))))
        szName[0] = UNICODE_NULL;
    if (!fExplicit && FAILED(GenerateAppIDWorker(szEncoded, pszArgs, szName, &pszAppID)))
        goto Cleanup;

    pApp = GetRecord(pszAppID, szEncoded);
    if (!pApp)
        goto Cleanup;

    pShortcut = (APP_SHORTCUT *)LocalAlloc(LPTR, sizeof(*pShortcut));
    if (!pShortcut)
        goto Cleanup;

    pShortcut->pidlFolder = ILClone(pidlFolder);
    pShortcut->pidlItem = (PITEMID_CHILD)ILClone(pidlChild);
    pShortcut->fOnDesktop = (uRoot >= RESOLVER_FIRST_DESKTOP && uRoot <= RESOLVER_LAST_DESKTOP);
    if (pszArgs && *pszArgs)
        pShortcut->pszArgs = StrDupW(pszArgs);

    if (!pShortcut->pidlFolder || !pShortcut->pidlItem ||
        (pszArgs && *pszArgs && !pShortcut->pszArgs) ||
        DPA_AppendPtr(m_Roots[uRoot].hdpaShortcuts, pShortcut) < 0)
    {
        ILFree(pShortcut->pidlFolder);
        ILFree(pShortcut->pidlItem);
        LocalFree(pShortcut->pszArgs);
        LocalFree(pShortcut);
        goto Cleanup;
    }

    pShortcut->pApp = pApp;
    pApp->cShortcuts += 1;

    /* Programs, folders and names on the hide lists do not count, nor IDs listed as hidden */
    if (fPreventPinning)
    {
        pShortcut->fInteresting = FALSE;
    }
    else if (fExplicit)
    {
        pShortcut->fInteresting = TRUE;
    }
    else
    {
        BOOL fInteresting = PathIsInterestingExe(szPath, pShortcut->pszArgs != NULL) &&
                            IsInterestingDirectory(pidlFolder);

        if (SUCCEEDED(DisplayNameOfW(psf, pidlChild, SHGDN_INFOLDER | SHGDN_FORPARSING,
                                     szLinkName, _countof(szLinkName))))
        {
            fInteresting = PathIsInterestingLink(szLinkName, fInteresting);
        }
        pShortcut->fInteresting = fInteresting;
    }
    if (pShortcut->fInteresting)
        pShortcut->fInteresting = IsInterestingAppID(pApp->pszAppID);

    if (m_pARNotify && SUCCEEDED(psl->GetHotkey(&wHotkey)) && wHotkey)
        m_pARNotify->AddHotkey(wHotkey, pidlFolder, pidlChild, TRUE);

Cleanup:
    CoTaskMemFree(pszArgs);
    CoTaskMemFree(pszAppID);
}

VOID
CAppShortcutCache::FillFolder(
    _In_ UINT uRoot,
    _In_ PCIDLIST_ABSOLUTE pidlFolder,
    _In_ BOOL fRecurse)
{
    CComPtr<IShellFolder> psf;
    CComPtr<IEnumIDList> penum;
    PITEMID_CHILD pidlChild;
    HDPA hdpaFolders;

    if (ILIsEmpty(pidlFolder))
    {
        if (FAILED(SHGetDesktopFolder(&psf)))
            return;
    }
    else if (FAILED(SHBindToObject(NULL, pidlFolder, NULL, IID_PPV_ARG(IShellFolder, &psf))))
    {
        return;
    }

    if (psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum) != S_OK)
        return;

    hdpaFolders = DPA_Create(4);
    if (!hdpaFolders)
        return;

    while (penum->Next(1, &pidlChild, NULL) == S_OK)
    {
        SFGAOF sfgao = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_LINK;

        if (FAILED(psf->GetAttributesOf(1, (PCUITEMID_CHILD_ARRAY)&pidlChild, &sfgao)))
        {
            ILFree(pidlChild);
            continue;
        }

        if (sfgao & SFGAO_FOLDER)
        {
            /* Real subfolders only; linked folders could lead anywhere */
            if (fRecurse && (sfgao & SFGAO_FILESYSTEM) && !(sfgao & SFGAO_LINK) &&
                DPA_AppendPtr(hdpaFolders, pidlChild) >= 0)
            {
                continue;
            }
        }
        else
        {
            AddShortcut(uRoot, pidlFolder, psf, pidlChild);
        }
        ILFree(pidlChild);
    }

    for (int i = 0; i < DPA_GetPtrCount(hdpaFolders); i++)
    {
        PITEMID_CHILD pidlSub = (PITEMID_CHILD)DPA_FastGetPtr(hdpaFolders, i);
        PIDLIST_ABSOLUTE pidlFull = ILCombine(pidlFolder, pidlSub);

        if (pidlFull)
        {
            FillFolder(uRoot, pidlFull, TRUE);
            ILFree(pidlFull);
        }
        ILFree(pidlSub);
    }
    DPA_Destroy(hdpaFolders);
}

VOID
CAppShortcutCache::ScanRoot(
    _In_ UINT uRoot,
    _In_ IShellFolder *psfDesktop)
{
    const RESOLVER_ROOT_INFO *pInfo = &g_ResolverRoots[uRoot];
    RESOLVER_ROOT *pRoot = &m_Roots[uRoot];
    DWORD dwRootFlags = pInfo->dwRootFlags;
    BOOL fSubFolderPolicy = PinPolicyEnabled(PIN_POLICY_NOSTARTMENUSUBFOLDERS);

    UNREFERENCED_PARAMETER(psfDesktop);

    FreeRootShortcuts(uRoot);
    pRoot->fScanned = TRUE;
    pRoot->fDirty = FALSE;

    if (!pRoot->pidl &&
        FAILED(SHGetKnownFolderIDList(*pInfo->pkfid, pInfo->dwKFFlags, NULL, &pRoot->pidl)))
    {
        return;
    }

    if (pInfo->fCommon && SHRestricted(REST_NOCOMMONGROUPS))
        return;

    /* Programs sit inside the start menu, which already read them */
    if ((dwRootFlags & ROOTF_SKIPIFNESTED) && !fSubFolderPolicy && uRoot > 0 && m_Roots[uRoot - 1].pidl &&
        ILIsParent(m_Roots[uRoot - 1].pidl, pRoot->pidl, FALSE))
    {
        return;
    }
    if ((dwRootFlags & ROOTF_SUBFOLDERPOLICY) && fSubFolderPolicy)
        dwRootFlags |= ROOTF_NOSUBFOLDERS;

    pRoot->hdpaShortcuts = DPA_Create(8);
    if (!pRoot->hdpaShortcuts)
        return;

    /* Explorer watches the root for us and calls back through OnChangeNotify */
    if (m_pARNotify && !pRoot->fRegistered)
    {
        pRoot->fRegistered = TRUE;
        m_pARNotify->RegisterNotification(uRoot, RESOLVER_NOTIFY_EVENTS, pRoot->pidl, TRUE);
    }

    FillFolder(uRoot, pRoot->pidl, !(dwRootFlags & ROOTF_NOSUBFOLDERS));
}

VOID
CAppShortcutCache::EnsureScanned()
{
    CComPtr<IShellFolder> psfDesktop;
    BOOL fScanned = FALSE;

    if (FAILED(SHGetDesktopFolder(&psfDesktop)))
        return;

    for (UINT i = 0; i < RESOLVER_ROOT_COUNT; i++)
    {
        if (!m_Roots[i].fScanned || m_Roots[i].fDirty)
        {
            ScanRoot(i, psfDesktop);
            fScanned = TRUE;
        }
    }

    if (fScanned)
        ChooseBestShortcuts();
}

VOID
CAppShortcutCache::MarkDirty(
    _In_ UINT uRoot)
{
    m_Roots[uRoot].fDirty = TRUE;
}

static
HRESULT
CreateShortcutItem(
    _In_ const APP_SHORTCUT *pShortcut,
    _Outptr_ IShellItem **ppsi)
{
    PIDLIST_ABSOLUTE pidl = ILCombine(pShortcut->pidlFolder, pShortcut->pidlItem);
    HRESULT hr;

    *ppsi = NULL;
    if (!pidl)
        return E_OUTOFMEMORY;
    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, ppsi));
    ILFree(pidl);
    return hr;
}

static
BOOL
IsSameShortcut(
    _In_ const APP_SHORTCUT *pShortcut,
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    PIDLIST_ABSOLUTE pidlShortcut = ILCombine(pShortcut->pidlFolder, pShortcut->pidlItem);
    BOOL fSame;

    if (!pidlShortcut)
        return FALSE;
    fSame = SHCompareIDsFull(SHCIDS_CANONICALONLY, pidlShortcut, pidl, NULL) == S_OK ||
            ILIsEqualByPath(pidlShortcut, pidl);
    ILFree(pidlShortcut);
    return fSame;
}

/* Start menu data hidden in shortcut ID lists */

typedef struct _STARTPANE_DATA
{
    WORD wHotkey;
    PWSTR rgpsz[6];             /* Path, target, display name, ID, arguments, encoded path */
    FILETIME rgft[2];
    DWORD rgdw[4];
} STARTPANE_DATA;

#define STARTPANE_ALTNAME   2

static
VOID
ClearStartPaneData(
    _Inout_ STARTPANE_DATA *pData)
{
    for (UINT i = 0; i < _countof(pData->rgpsz); i++)
        CoTaskMemFree(pData->rgpsz[i]);
    ZeroMemory(pData, sizeof(*pData));
}

static
const BYTE *
ReadStartPaneString(
    _In_opt_ const BYTE *pb,
    _In_ const BYTE *pEnd,
    _Outptr_opt_ PWSTR *ppsz)
{
    const BYTE *pScan = pb;
    SIZE_T cb;
    WCHAR ch;

    if (!pb)
        return NULL;

    for (;;)
    {
        if (pScan + sizeof(WCHAR) > pEnd)
            return NULL;
        CopyMemory(&ch, pScan, sizeof(ch));
        pScan += sizeof(WCHAR);
        if (!ch)
            break;
    }

    cb = pScan - pb;
    if (ppsz)
    {
        *ppsz = (PWSTR)CoTaskMemAlloc(cb);
        if (!*ppsz)
            return NULL;
        CopyMemory(*ppsz, pb, cb);
    }
    return pScan;
}

static
const BYTE *
ReadStartPaneBytes(
    _In_opt_ const BYTE *pb,
    _In_ const BYTE *pEnd,
    _Out_writes_bytes_(cb) PVOID pv,
    _In_ SIZE_T cb)
{
    if (!pb || pb + cb > pEnd)
        return NULL;
    CopyMemory(pv, pb, cb);
    return pb + cb;
}

static
BOOL
GetStartPaneData(
    _In_ PCUIDLIST_RELATIVE pidl,
    _Out_ STARTPANE_DATA *pData)
{
    PIDHIDDENBLOCK pBlock = ILFindHiddenIDOn((LPCITEMIDLIST)pidl, HIDDENID_STARTPANE, TRUE);
    const BYTE *pEnd;
    const BYTE *pb;
    WORD wPad;

    ZeroMemory(pData, sizeof(*pData));
    if (!pBlock || pBlock->cb < sizeof(*pBlock) + sizeof(DWORD) + sizeof(WORD))
        return FALSE;

    pEnd = (const BYTE *)pBlock + pBlock->cb;
    pb = (const BYTE *)(pBlock + 1) + sizeof(DWORD);
    pb = ReadStartPaneBytes(pb, pEnd, &pData->wHotkey, sizeof(pData->wHotkey));
    for (UINT i = 0; i < 3; i++)
        pb = ReadStartPaneString(pb, pEnd, &pData->rgpsz[i]);

    if (pBlock->wVersion >= 1)
        pb = ReadStartPaneBytes(pb, pEnd, &wPad, sizeof(wPad));
    if (pBlock->wVersion >= 2)
    {
        pb = ReadStartPaneString(pb, pEnd, &pData->rgpsz[3]);
        pb = ReadStartPaneString(pb, pEnd, &pData->rgpsz[4]);
        pb = ReadStartPaneBytes(pb, pEnd, pData->rgft, sizeof(pData->rgft));
        pb = ReadStartPaneBytes(pb, pEnd, pData->rgdw, 2 * sizeof(DWORD));
    }
    if (pBlock->wVersion >= 3)
        pb = ReadStartPaneBytes(pb, pEnd, &pData->rgdw[2], sizeof(DWORD));
    if (pBlock->wVersion >= 4)
        pb = ReadStartPaneString(pb, pEnd, &pData->rgpsz[5]);
    if (pBlock->wVersion >= 5)
        pb = ReadStartPaneBytes(pb, pEnd, &pData->rgdw[3], sizeof(DWORD));

    if (!pb)
    {
        ClearStartPaneData(pData);
        return FALSE;
    }
    return TRUE;
}

static
SIZE_T
StartPaneStringSize(
    _In_opt_ PCWSTR psz)
{
    return (psz ? (lstrlenW(psz) + 1) : 1) * sizeof(WCHAR);
}

static
BYTE *
WriteStartPaneString(
    _Out_ BYTE *pb,
    _In_opt_ PCWSTR psz)
{
    SIZE_T cb = StartPaneStringSize(psz);

    if (psz)
        CopyMemory(pb, psz, cb);
    else
        ZeroMemory(pb, cb);
    return pb + cb;
}

/* Takes over pidl and gives back the list carrying the data, or NULL */
static
LPITEMIDLIST
SetStartPaneData(
    _In_ LPITEMIDLIST pidl,
    _In_ const STARTPANE_DATA *pData)
{
    PIDHIDDENBLOCK pBlock;
    LPITEMIDLIST pidlNew;
    SIZE_T cb = sizeof(IDHIDDENBLOCK) + sizeof(DWORD) + sizeof(WORD) + sizeof(WORD) +
                sizeof(pData->rgft) + sizeof(pData->rgdw);
    BYTE *pb;

    for (UINT i = 0; i < _countof(pData->rgpsz); i++)
        cb += StartPaneStringSize(pData->rgpsz[i]);
    if (cb > MAXWORD)
    {
        ILFree(pidl);
        return NULL;
    }

    pBlock = (PIDHIDDENBLOCK)LocalAlloc(LPTR, cb);
    if (!pBlock)
    {
        ILFree(pidl);
        return NULL;
    }

    pBlock->cb = (WORD)cb;
    pBlock->wVersion = STARTPANE_VERSION;
    pBlock->dwId = HIDDENID_STARTPANE;
    pb = (BYTE *)(pBlock + 1) + sizeof(DWORD);
    CopyMemory(pb, &pData->wHotkey, sizeof(WORD));
    pb += sizeof(WORD);
    for (UINT i = 0; i < 3; i++)
        pb = WriteStartPaneString(pb, pData->rgpsz[i]);
    pb += sizeof(WORD);
    pb = WriteStartPaneString(pb, pData->rgpsz[3]);
    pb = WriteStartPaneString(pb, pData->rgpsz[4]);
    CopyMemory(pb, pData->rgft, sizeof(pData->rgft));
    pb += sizeof(pData->rgft);
    CopyMemory(pb, pData->rgdw, 3 * sizeof(DWORD));
    pb += 3 * sizeof(DWORD);
    pb = WriteStartPaneString(pb, pData->rgpsz[5]);
    CopyMemory(pb, &pData->rgdw[3], sizeof(DWORD));

    if (ILRemoveHiddenID(pidl, HIDDENID_STARTPANE))
        ILExpungeRemovedHiddenIDs(pidl);
    pidlNew = ILAppendHiddenID(pidl, pBlock);
    LocalFree(pBlock);
    return pidlNew;
}

/* Hands out a fixed set of start menu programs; the caller copies what it keeps */
class CEnumProgramItems :
    public CPinZeroAlloc,
    public IEnumStartMenuItems
{
public:
    CEnumProgramItems(
        _In_ HDSA hdsa) : m_cRef(1), m_iNext(0), m_hdsa(hdsa) {}

    virtual ~CEnumProgramItems()
    {
        if (m_hdsa)
            DSA_DestroyCallback(m_hdsa, FreeItemCallback, NULL);
    }

    static INT CALLBACK FreeItemCallback(
        _In_ PVOID p,
        _In_opt_ PVOID pData)
    {
        PSM_PROGRAM_ITEM pItem = (PSM_PROGRAM_ITEM)p;

        UNREFERENCED_PARAMETER(pData);
        ILFree(pItem->pidlFolder);
        ILFree(pItem->pidlItem);
        CoTaskMemFree(pItem->pszAppID);
        return TRUE;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumStartMenuItems))
        {
            *ppv = static_cast<IEnumStartMenuItems *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (!cRef)
            delete this;
        return cRef;
    }

    STDMETHODIMP Next(ULONG celt, SM_PROGRAM_ITEM *rgelt, ULONG *pceltFetched) override
    {
        ULONG cFetched = 0;

        while (cFetched < celt && m_hdsa && m_iNext < DSA_GetItemCount(m_hdsa))
        {
            CopyMemory(&rgelt[cFetched], DSA_GetItemPtr(m_hdsa, m_iNext), sizeof(*rgelt));
            m_iNext++;
            cFetched++;
        }
        if (pceltFetched)
            *pceltFetched = cFetched;
        return (cFetched == celt) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG celt) override
    {
        m_iNext += celt;
        return (m_hdsa && m_iNext <= DSA_GetItemCount(m_hdsa)) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override
    {
        m_iNext = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumStartMenuItems **ppenum) override
    {
        *ppenum = NULL;
        return E_NOTIMPL;
    }

private:
    LONG m_cRef;
    int m_iNext;
    HDSA m_hdsa;
};

/* New programs are not tracked yet, so this list is always empty */
class CEnumNoStrings :
    public CPinZeroAlloc,
    public IEnumString
{
public:
    CEnumNoStrings() : m_cRef(1) {}
    virtual ~CEnumNoStrings() {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumString))
        {
            *ppv = static_cast<IEnumString *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (!cRef)
            delete this;
        return cRef;
    }

    STDMETHODIMP Next(ULONG celt, LPOLESTR *rgelt, ULONG *pceltFetched) override
    {
        if (pceltFetched)
            *pceltFetched = 0;
        return celt ? S_FALSE : S_OK;
    }

    STDMETHODIMP Skip(ULONG celt) override { return celt ? S_FALSE : S_OK; }
    STDMETHODIMP Reset() override { return S_OK; }

    STDMETHODIMP Clone(IEnumString **ppenum) override
    {
        *ppenum = new CEnumNoStrings();
        return *ppenum ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG m_cRef;
};

static
HRESULT
AddProgramItem(
    _In_ HDSA hdsa,
    _In_opt_ PCIDLIST_ABSOLUTE pidlFolder,
    _In_opt_ PCUIDLIST_RELATIVE pidlItem,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ const UA_USAGE *pUsage,
    _In_ int iPinIndex)
{
    SM_PROGRAM_ITEM item;

    ZeroMemory(&item, sizeof(item));
    item.iPinIndex = iPinIndex;
    if (pUsage)
        item.Usage = *pUsage;

    if (iPinIndex != SM_PROGRAM_SEPARATOR)
    {
        if (FAILED(SHStrDupW(pszAppID ? pszAppID : L"", &item.pszAppID)) ||
            (pidlFolder && !(item.pidlFolder = ILClone(pidlFolder))) ||
            !pidlItem || !(item.pidlItem = ILClone(pidlItem)))
        {
            CEnumProgramItems::FreeItemCallback(&item, NULL);
            return E_OUTOFMEMORY;
        }
    }

    if (DSA_AppendItem(hdsa, &item) < 0)
    {
        CEnumProgramItems::FreeItemCallback(&item, NULL);
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

static
int
CompareUsage(
    _In_ const UA_USAGE *pUsage1,
    _In_ const UA_USAGE *pUsage2)
{
    if (pUsage1->flScore > pUsage2->flScore)
        return -1;
    if (pUsage1->flScore < pUsage2->flScore)
        return 1;
    return -CompareFileTime(&pUsage1->ftLastUsed, &pUsage2->ftLastUsed);
}

static
int
CALLBACK
CompareRecordsByUsage(
    _In_ PVOID p1,
    _In_ PVOID p2,
    _In_ LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    return CompareUsage(&((APP_RECORD *)p1)->Usage, &((APP_RECORD *)p2)->Usage);
}

/* The resolver */

CStartMenuCacheAndAppResolver::CStartMenuCacheAndAppResolver() :
    m_pplStartMenu(NULL),
    m_pplTaskband(NULL)
{
}

CStartMenuCacheAndAppResolver::~CStartMenuCacheAndAppResolver()
{
    if (m_pplStartMenu)
        m_pplStartMenu->Release();
    if (m_pplTaskband)
        m_pplTaskband->Release();
}

HRESULT
CStartMenuCacheAndAppResolver::FinalConstruct()
{
    HRESULT hr;

    hr = CoCreateInstance(CLSID_StartMenuPin, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IPinnedList, &m_pplStartMenu));
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_TaskbandPin, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IPinnedList, &m_pplTaskband));
    if (SUCCEEDED(hr) && !GetAppShortcutCache())
        hr = E_OUTOFMEMORY;
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::OnChangeNotify(
    _In_ UINT uId,
    _In_ LONG lEvent,
    _In_opt_ PCIDLIST_ABSOLUTE pidl1,
    _In_opt_ PCIDLIST_ABSOLUTE pidl2)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    WCHAR szPath[MAX_PATH];

    /* The taskbar follows its pinned shortcuts when they are renamed or deleted */
    if ((lEvent == SHCNE_RENAMEITEM || lEvent == SHCNE_DELETE) && pidl1 &&
        SHGetPathFromIDListEx(pidl1, szPath, _countof(szPath), 2) &&
        PathMatchSpecW(szPath, L"*.lnk;*.url"))
    {
        static HWND s_hwndTray;

        if (!s_hwndTray || !IsWindow(s_hwndTray))
            s_hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
        SendMessageW(s_hwndTray, TWM_SHORTCUTCHANGED, (WPARAM)pidl1,
                     (LPARAM)(lEvent == SHCNE_DELETE ? NULL : pidl2));
    }

    if (uId < RESOLVER_ROOT_COUNT && pCache)
    {
        pCache->Lock();
        pCache->MarkDirty(uId);
        if (pCache->m_pARNotify)
        {
            /* The root registers again when it is read next */
            if (uId != RESOLVER_ROOT_PINNED)
            {
                pCache->m_pARNotify->UnregisterNotification(uId);
                pCache->m_Roots[uId].fRegistered = FALSE;
            }
            if (pCache->m_pSMNotify)
                pCache->m_pSMNotify->ChangeNotify(0);
        }
        pCache->Unlock();
    }
    return S_OK;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::PinListChanged()
{
    return S_OK;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetPinnedItemsCount(
    _Out_ int *pcPinned)
{
    CComPtr<IEnumFullIDList> penum;
    PIDLIST_ABSOLUTE pidl;

    *pcPinned = 0;
    if (SHRestricted(REST_NOSMPINNEDLIST) || FAILED(m_pplStartMenu->EnumObjects(&penum)))
        return S_OK;

    while (penum->Next(1, &pidl, NULL) == S_OK)
    {
        *pcPinned += 1;
        ILFree(pidl);
    }
    return S_OK;
}

/* Pinned programs first, a separator, then the most used ones */
STDMETHODIMP
CStartMenuCacheAndAppResolver::GetStartMenuMFUList(
    _In_ UINT cMax,
    _Outptr_ IEnumStartMenuItems **ppenumItems,
    _Outptr_ IEnumString **ppenumNewApps,
    _Out_ FILETIME *pftNewest)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    CComPtr<IShellUserAssist> pua;
    CComPtr<IEnumFullIDList> penum;
    CEnumProgramItems *pEnumItems;
    CEnumNoStrings *pEnumNew;
    PIDLIST_ABSOLUTE pidl;
    UINT cPinned = 0;
    HDSA hdsa;

    *ppenumItems = NULL;
    *ppenumNewApps = NULL;
    ZeroMemory(pftNewest, sizeof(*pftNewest));

    hdsa = DSA_Create(sizeof(SM_PROGRAM_ITEM), 8);
    if (!hdsa)
        return E_OUTOFMEMORY;

    if (!SHRestricted(REST_NOSMPINNEDLIST) && SUCCEEDED(m_pplStartMenu->EnumObjects(&penum)))
    {
        while ((UINT)DSA_GetItemCount(hdsa) < cMax && penum->Next(1, &pidl, NULL) == S_OK)
        {
            PWSTR pszAppID = NULL;

            ILGetHiddenStringAllocW(pidl, HIDDENID_APPID, &pszAppID);
            if (SUCCEEDED(AddProgramItem(hdsa, NULL, pidl, pszAppID, NULL, cPinned)))
                cPinned++;
            CoTaskMemFree(pszAppID);
            ILFree(pidl);
        }
        if (cPinned && (UINT)DSA_GetItemCount(hdsa) < cMax)
            AddProgramItem(hdsa, NULL, NULL, NULL, NULL, SM_PROGRAM_SEPARATOR);
    }

    /* Usage counts come from the user assist store when there is one */
    if (pCache && !SHRestricted(REST_NOSMMFUPROGRAMS) &&
        SUCCEEDED(CoCreateInstance(CLSID_UserAssistStore, NULL, CLSCTX_INPROC_SERVER,
                                   IID_IShellUserAssistPrivate, (void **)&pua)))
    {
        HDPA hdpaUsed = DPA_Create(16);

        pCache->Lock();
        pCache->EnsureScanned();
        for (int i = 0; hdpaUsed && i < pCache->GetRecordCount(); i++)
        {
            APP_RECORD *pApp = pCache->GetRecord(i);
            PIDLIST_ABSOLUTE pidlBest;

            if (!pApp->cShortcuts || !pApp->pBest)
                continue;

            ZeroMemory(&pApp->Usage, sizeof(pApp->Usage));
            pApp->Usage.cbSize = sizeof(pApp->Usage);
            pApp->Usage.dwMask = UA_USAGE_LASTUSED | UA_USAGE_SCORE | UA_USAGE_EXCLUDED;
            if (FAILED(pua->QueryEntry(UA_GROUP_APPLICATIONS, pApp->pszAppID, &pApp->Usage)) ||
                pApp->Usage.fExcluded ||
                (!pApp->Usage.ftLastUsed.dwLowDateTime && !pApp->Usage.ftLastUsed.dwHighDateTime))
            {
                continue;
            }

            /* Pinned programs already show above */
            pidlBest = ILCombine(pApp->pBest->pidlFolder, pApp->pBest->pidlItem);
            if (pidlBest)
            {
                if (m_pplStartMenu->IsPinned(pidlBest) != S_OK && m_pplTaskband->IsPinned(pidlBest) != S_OK)
                    DPA_AppendPtr(hdpaUsed, pApp);
                ILFree(pidlBest);
            }
        }

        if (hdpaUsed)
        {
            DPA_Sort(hdpaUsed, CompareRecordsByUsage, 0);
            for (int i = 0; i < DPA_GetPtrCount(hdpaUsed) && (UINT)DSA_GetItemCount(hdsa) < cMax; i++)
            {
                APP_RECORD *pApp = (APP_RECORD *)DPA_FastGetPtr(hdpaUsed, i);

                AddProgramItem(hdsa, pApp->pBest->pidlFolder, pApp->pBest->pidlItem, pApp->pszAppID,
                               &pApp->Usage, -1);
            }
            DPA_Destroy(hdpaUsed);
        }
        pCache->Unlock();
    }

    pEnumItems = new CEnumProgramItems(hdsa);
    if (!pEnumItems)
    {
        DSA_DestroyCallback(hdsa, CEnumProgramItems::FreeItemCallback, NULL);
        return E_OUTOFMEMORY;
    }

    pEnumNew = new CEnumNoStrings();
    if (!pEnumNew)
    {
        pEnumItems->Release();
        return E_OUTOFMEMORY;
    }

    *ppenumItems = pEnumItems;
    *ppenumNewApps = pEnumNew;
    return S_OK;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::RegisterSMNotify(
    _In_opt_ IStartMenuItemsCacheNotify *pNotify)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();

    if (!pCache)
        return E_UNEXPECTED;

    pCache->Lock();
    pCache->m_pSMNotify = pNotify;
    pCache->Unlock();
    return S_OK;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::RegisterARNotify(
    _In_opt_ IAppResolverNotify *pNotify)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();

    if (!pCache)
        return E_UNEXPECTED;

    pCache->Lock();
    if (pCache->m_pARNotify)
    {
        for (UINT i = 0; i < RESOLVER_ROOT_COUNT; i++)
        {
            pCache->m_Roots[i].fRegistered = FALSE;
            pCache->m_pARNotify->UnregisterNotification(i);
        }
    }

    /* A new sink wants the roots and hotkeys again, so read everything once more */
    pCache->m_pARNotify = pNotify;
    for (UINT i = 0; i < RESOLVER_ROOT_COUNT; i++)
        pCache->MarkDirty(i);
    pCache->Unlock();
    return S_OK;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::SetAltName(
    _In_ PCUIDLIST_RELATIVE pidl,
    _In_ PCWSTR pszName,
    _Outptr_ PIDLIST_RELATIVE *ppidlNew)
{
    STARTPANE_DATA data;
    LPITEMIDLIST pidlCopy;

    *ppidlNew = NULL;
    pidlCopy = ILClone(pidl);
    if (!pidlCopy)
        return E_OUTOFMEMORY;

    GetStartPaneData(pidl, &data);
    CoTaskMemFree(data.rgpsz[STARTPANE_ALTNAME]);
    data.rgpsz[STARTPANE_ALTNAME] = NULL;
    if (FAILED(SHStrDupW(pszName, &data.rgpsz[STARTPANE_ALTNAME])))
    {
        ClearStartPaneData(&data);
        ILFree(pidlCopy);
        return E_OUTOFMEMORY;
    }

    *ppidlNew = SetStartPaneData(pidlCopy, &data);
    ClearStartPaneData(&data);
    return *ppidlNew ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetAltName(
    _In_ PCUIDLIST_RELATIVE pidl,
    _Outptr_ PWSTR *ppszName)
{
    STARTPANE_DATA data;

    *ppszName = NULL;
    if (!GetStartPaneData(pidl, &data))
        return E_FAIL;

    *ppszName = data.rgpsz[STARTPANE_ALTNAME];
    data.rgpsz[STARTPANE_ALTNAME] = NULL;
    ClearStartPaneData(&data);
    return *ppszName ? S_OK : E_FAIL;
}

BOOL
CStartMenuCacheAndAppResolver::IsShortcutInteresting(
    _In_ IShellItem *psi,
    _In_ PCWSTR pszTarget,
    _In_ BOOL fHasArgs)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    PWSTR pszLinkName;
    BOOL fInteresting;

    if (!pCache)
        return FALSE;

    fInteresting = pCache->PathIsInterestingExe(PathFindFileNameW(pszTarget), fHasArgs);
    if (FAILED(psi->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &pszLinkName)))
        return FALSE;

    fInteresting = pCache->PathIsInterestingLink(pszLinkName, fInteresting);
    CoTaskMemFree(pszLinkName);
    return fInteresting;
}

/* Prefers what the shortcut says, then a known shortcut to the same file, then its target */
STDMETHODIMP
CStartMenuCacheAndAppResolver::GetAppIDForShortcut(
    _In_ IShellItem *psi,
    _Outptr_ PWSTR *ppszAppID)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    CComPtr<IShellLinkW> psl;
    WCHAR szTarget[MAX_PATH];
    WCHAR szEncoded[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    BOOL fPreventPinning;
    BOOL fFound = FALSE;
    HRESULT hr;

    *ppszAppID = NULL;
    hr = psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IShellLinkW, &psl));
    if (FAILED(hr))
        return hr;

    GetShortcutAppProperties(psl, ppszAppID, &fPreventPinning, NULL);
    if (fPreventPinning || (*ppszAppID && !IsInterestingAppID(*ppszAppID)))
    {
        CoTaskMemFree(*ppszAppID);
        *ppszAppID = NULL;
        return E_APP_NOT_INTERESTING;
    }
    if (*ppszAppID)
        return S_OK;

    hr = SHGetIDListFromObject(psi, &pidl);
    if (FAILED(hr))
        return hr;

    hr = GetAppPathFromShortcut(psl, pidl, szTarget, _countof(szTarget));
    if (SUCCEEDED(hr) && pCache)
    {
        EncodeCommonRelativePath(szEncoded, _countof(szEncoded), szTarget);

        pCache->Lock();
        pCache->EnsureScanned();
        for (UINT r = 0; !fFound && SUCCEEDED(hr) && r < RESOLVER_ROOT_COUNT; r++)
        {
            HDPA hdpa = pCache->m_Roots[r].hdpaShortcuts;

            for (int i = 0; !fFound && hdpa && i < DPA_GetPtrCount(hdpa); i++)
            {
                APP_SHORTCUT *pShortcut = (APP_SHORTCUT *)DPA_FastGetPtr(hdpa, i);

                if (pShortcut->fInteresting &&
                    CompareStringOrdinal(pShortcut->pApp->pszEncodedPath, -1, szEncoded, -1, TRUE) == CSTR_EQUAL &&
                    IsSameShortcut(pShortcut, pidl))
                {
                    hr = SHStrDupW(pShortcut->pApp->pszAppID, ppszAppID);
                    fFound = SUCCEEDED(hr);
                }
            }
        }
        pCache->Unlock();

        /* A shortcut the cache does not know gets the ID it would have had there */
        if (!fFound && SUCCEEDED(hr))
        {
            WCHAR szArgs[MAX_PATH] = L"";

            if (!GetExeOrMscExtension(szTarget))
            {
                hr = E_APP_NO_EXTENSION;
            }
            else
            {
                psl->GetArguments(szArgs, _countof(szArgs));
                if (!IsShortcutInteresting(psi, szTarget, szArgs[0] != UNICODE_NULL))
                {
                    hr = E_APP_NOT_INTERESTING;
                }
                else
                {
                    PWSTR pszName;

                    hr = psi->GetDisplayName(SIGDN_PARENTRELATIVE, &pszName);
                    if (SUCCEEDED(hr))
                    {
                        hr = GenerateAppID(szTarget, szArgs, pszName, ppszAppID);
                        CoTaskMemFree(pszName);
                    }
                }
            }
        }
    }

    ILFree(pidl);
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetAppIDForWindow(
    _In_ HWND hwnd,
    _Outptr_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfPinningPrevented,
    _Out_opt_ BOOL *pfExplicitAppID,
    _Out_opt_ BOOL *pfEmbeddedShortcutValid)
{
    CComPtr<IPropertyStore> pps;
    DWORD dwProcessId = 0;
    HRESULT hr;

    *ppszAppID = NULL;
    if (pfPinningPrevented)
        *pfPinningPrevented = FALSE;
    if (pfExplicitAppID)
        *pfExplicitAppID = FALSE;
    if (pfEmbeddedShortcutValid)
        *pfEmbeddedShortcutValid = FALSE;

    hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARG(IPropertyStore, &pps));
    if (SUCCEEDED(hr))
    {
        PROPVARIANT pv;

        PropVariantInit(&pv);
        hr = pps->GetValue(PKEY_AppUserModel_ID, &pv);
        if (SUCCEEDED(hr))
        {
            /* A window that cleared its ID on purpose does not fall back to its process */
            if (pv.vt == VT_EMPTY)
            {
                hr = E_FAIL;
            }
            else if (pv.vt == VT_NULL)
            {
                hr = E_PENDING;
            }
            else if (pv.vt == VT_LPWSTR && pv.pwszVal)
            {
                hr = SHStrDupW(pv.pwszVal, ppszAppID);
                if (SUCCEEDED(hr))
                {
                    if (pfPinningPrevented)
                        GetBoolProperty(pps, PKEY_AppUserModel_PreventPinning, pfPinningPrevented);
                    if (pfExplicitAppID)
                        *pfExplicitAppID = TRUE;
                }
            }
            else
            {
                hr = E_FAIL;
            }
            PropVariantClear(&pv);
        }
    }

    if (FAILED(hr) && hr != E_PENDING)
    {
        GetWindowThreadProcessId(hwnd, &dwProcessId);
        hr = GetAppIDForProcess(dwProcessId, ppszAppID, pfPinningPrevented, pfExplicitAppID,
                                pfEmbeddedShortcutValid);
    }
    return hr;
}

typedef struct _PIN_PROCESS_WINDOW_INFO
{
    ULONG WindowFlags;
    USHORT WindowTitleLength;
    WCHAR WindowTitle[MAX_PATH];
} PIN_PROCESS_WINDOW_INFO;

#define PIN_PROCESS_WINDOW_INFO_CLASS   ((PROCESSINFOCLASS)50)

static
BOOL
IsTokenBelowMediumIL(
    _In_ HANDLE hProcess)
{
    BYTE rgbLabel[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    TOKEN_MANDATORY_LABEL *pLabel = (TOKEN_MANDATORY_LABEL *)rgbLabel;
    HANDLE hToken;
    DWORD cb;
    BOOL fLow = FALSE;

    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &hToken))
        return FALSE;

    if (GetTokenInformation(hToken, TokenIntegrityLevel, pLabel, sizeof(rgbLabel), &cb))
    {
        PUCHAR pcSub = GetSidSubAuthorityCount(pLabel->Label.Sid);
        DWORD dwLevel = *GetSidSubAuthority(pLabel->Label.Sid, *pcSub - 1);
        fLow = (dwLevel < SECURITY_MANDATORY_MEDIUM_RID);
    }
    CloseHandle(hToken);
    return fLow;
}

/* A process started from a shortcut or with an ID carries it in its startup window title */
HRESULT
CStartMenuCacheAndAppResolver::GetShortcutPathOrAppIDFromPid(
    _In_ DWORD dwProcessId,
    _Out_writes_(MAX_PATH) PWSTR pszValue,
    _Out_ BOOL *pfAppID,
    _Out_opt_ BOOL *pfPinningPrevented)
{
    PIN_PROCESS_WINDOW_INFO info;
    NTSTATUS Status;
    HANDLE hProcess;
    HRESULT hr = E_FAIL;

    *pfAppID = FALSE;
    if (pfPinningPrevented)
        *pfPinningPrevented = FALSE;
    *pszValue = UNICODE_NULL;

    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
    if (!hProcess)
        return E_FAIL;

    ZeroMemory(&info, sizeof(info));
    Status = NtQueryInformationProcess(hProcess, PIN_PROCESS_WINDOW_INFO_CLASS, &info, sizeof(info), NULL);
    if (NT_SUCCESS(Status) && (info.WindowFlags & (STARTF_TITLEISLINKNAME | STARTF_TITLEISAPPID)) &&
        info.WindowTitleLength)
    {
        UINT cch = min(info.WindowTitleLength / sizeof(WCHAR), _countof(info.WindowTitle) - 1);

        info.WindowTitle[cch] = UNICODE_NULL;
        *pfAppID = (info.WindowFlags & STARTF_TITLEISAPPID) != 0;
        if (pfPinningPrevented)
        {
            *pfPinningPrevented = (info.WindowFlags & STARTF_PREVENTPINNING) ||
                                  (*pfAppID && !IsInterestingAppID(info.WindowTitle));
        }

        /* A low integrity process may only name a shortcut it could have written itself */
        if (*pfAppID || !IsTokenBelowMediumIL(hProcess))
        {
            hr = StringCchCopyW(pszValue, MAX_PATH, info.WindowTitle);
        }
        else
        {
            hr = E_ACCESSDENIED;
        }
    }
    CloseHandle(hProcess);
    return hr;
}

/* Picks the shortcut whose arguments match the ones the process was started with */
HRESULT
CStartMenuCacheAndAppResolver::GetBestAppIDForProcess(
    _In_ PCWSTR pszExePath,
    _In_ HANDLE hProcess,
    _Outptr_ PWSTR *ppszAppID)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    PROCESS_BASIC_INFORMATION pbi;
    PEB peb;
    RTL_USER_PROCESS_PARAMETERS params;
    WCHAR szCmdLine[2084];
    WCHAR szEncoded[MAX_PATH];
    PCWSTR pszArgs;
    HRESULT hr = E_FAIL;
    SIZE_T cbRead;

    *ppszAppID = NULL;
    if (!pCache)
        return E_FAIL;

    if (!NT_SUCCESS(NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL)) ||
        !ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), NULL) ||
        !ReadProcessMemory(hProcess, peb.ProcessParameters, &params, sizeof(params), NULL))
    {
        return E_FAIL;
    }

    ZeroMemory(szCmdLine, sizeof(szCmdLine));
    cbRead = min(params.CommandLine.Length, sizeof(szCmdLine) - sizeof(WCHAR));
    if (cbRead && !ReadProcessMemory(hProcess, params.CommandLine.Buffer, szCmdLine, cbRead, NULL))
        return E_FAIL;

    PathRemoveBlanksW(szCmdLine);
    pszArgs = PathGetArgsW(szCmdLine);
    EncodeCommonRelativePath(szEncoded, _countof(szEncoded), pszExePath);

    pCache->Lock();
    pCache->EnsureScanned();
    for (UINT r = 0; FAILED(hr) && r < RESOLVER_ROOT_COUNT; r++)
    {
        HDPA hdpa = pCache->m_Roots[r].hdpaShortcuts;

        for (int i = 0; FAILED(hr) && hdpa && i < DPA_GetPtrCount(hdpa); i++)
        {
            APP_SHORTCUT *pShortcut = (APP_SHORTCUT *)DPA_FastGetPtr(hdpa, i);

            if (!pShortcut->fInteresting ||
                CompareStringOrdinal(pShortcut->pApp->pszEncodedPath, -1, szEncoded, -1, TRUE) != CSTR_EQUAL)
            {
                continue;
            }

            if (pShortcut->pszArgs ? (CompareStringOrdinal(pShortcut->pszArgs, -1, pszArgs, -1, FALSE) == CSTR_EQUAL)
                                   : !*pszArgs)
            {
                hr = SHStrDupW(pShortcut->pApp->pszAppID, ppszAppID);
            }
        }
    }
    pCache->Unlock();
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetAppIDForProcess(
    _In_ DWORD dwProcessId,
    _Outptr_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfPinningPrevented,
    _Out_opt_ BOOL *pfExplicitAppID,
    _Out_opt_ BOOL *pfEmbeddedShortcutValid)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    WCHAR szValue[MAX_PATH];
    BOOL fAppID;
    HRESULT hr;

    *ppszAppID = NULL;
    if (pfPinningPrevented)
        *pfPinningPrevented = FALSE;
    if (pfExplicitAppID)
        *pfExplicitAppID = FALSE;
    if (pfEmbeddedShortcutValid)
        *pfEmbeddedShortcutValid = FALSE;

    hr = GetShortcutPathOrAppIDFromPid(dwProcessId, szValue, &fAppID, pfPinningPrevented);
    if (SUCCEEDED(hr))
    {
        if (fAppID)
        {
            if (pfExplicitAppID)
                *pfExplicitAppID = TRUE;
            return SHStrDupW(szValue, ppszAppID);
        }

        /* Started from a shortcut, so the shortcut decides */
        CComPtr<IBindCtx> pbc;
        CComPtr<IShellItem> psi;

        hr = CreateBindCtxWithParam(PIN_BIND_NO_ALIAS, &pbc);
        if (SUCCEEDED(hr))
            hr = SHCreateItemFromParsingName(szValue, pbc, IID_PPV_ARG(IShellItem, &psi));
        if (SUCCEEDED(hr))
        {
            hr = GetAppIDForShortcut(psi, ppszAppID);
            if (pfEmbeddedShortcutValid)
                *pfEmbeddedShortcutValid = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr))
            return hr;
    }

    if (hr == E_ACCESSDENIED || hr == E_APP_NOT_INTERESTING)
        return hr;

    /* Otherwise the program itself names the application */
    WCHAR szExe[MAX_PATH];
    DWORD cchExe = _countof(szExe);
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);

    if (!hProcess)
        return HRESULT_FROM_WIN32(GetLastError());
    hr = QueryFullProcessImageNameW(hProcess, 0, szExe, &cchExe) ? S_OK : E_FAIL;
    CloseHandle(hProcess);
    if (FAILED(hr))
        return hr;

    if (!PathIsNetworkPathW(szExe))
        PathGetLongPath(szExe, szExe, _countof(szExe));

    /* Reading the command line needs more access, without it only the program counts */
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwProcessId);
    if (hProcess)
    {
        hr = GetBestAppIDForProcess(szExe, hProcess, ppszAppID);
        CloseHandle(hProcess);
        if (SUCCEEDED(hr))
            return hr;
    }

    if (!pCache || !pCache->PathIsInterestingExe(szExe, FALSE))
        return E_APP_NOT_INTERESTING;

    hr = GetBestShortcutAndAppIDForAppPath(szExe, NULL, ppszAppID);
    if (FAILED(hr))
        hr = GenerateAppID(szExe, NULL, NULL, ppszAppID);
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetShortcutForProcess(
    _In_ DWORD dwProcessId,
    _Outptr_ IShellItem **ppsi)
{
    CComPtr<IBindCtx> pbc;
    WCHAR szValue[MAX_PATH];
    BOOL fAppID;
    HRESULT hr;

    *ppsi = NULL;
    hr = GetShortcutPathOrAppIDFromPid(dwProcessId, szValue, &fAppID, NULL);
    if (FAILED(hr))
        return hr;
    if (fAppID)
        return E_FAIL;

    hr = CreateBindCtxWithParam(PIN_BIND_NO_ALIAS, &pbc);
    if (SUCCEEDED(hr))
        hr = SHCreateItemFromParsingName(szValue, pbc, IID_PPV_ARG(IShellItem, ppsi));
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetBestShortcutForAppID(
    _In_ PCWSTR pszAppID,
    _Outptr_ IShellItem **ppsi)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    APP_RECORD *pApp;
    HRESULT hr = E_FAIL;

    *ppsi = NULL;
    if (!pCache)
        return E_FAIL;

    pCache->Lock();
    pCache->EnsureScanned();
    pApp = pCache->FindRecordByAppID(pszAppID);
    if (pApp && pApp->pBest)
        hr = CreateShortcutItem(pApp->pBest, ppsi);
    pCache->Unlock();
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetBestShortcutAndAppIDForAppPath(
    _In_ PCWSTR pszAppPath,
    _Outptr_opt_ IShellItem **ppsi,
    _Outptr_opt_ PWSTR *ppszAppID)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    WCHAR szEncoded[MAX_PATH];
    APP_RECORD *pChosen = NULL;
    HRESULT hr = E_FAIL;

    if (ppsi)
        *ppsi = NULL;
    if (ppszAppID)
        *ppszAppID = NULL;
    if (!pCache)
        return E_FAIL;

    EncodeCommonRelativePath(szEncoded, _countof(szEncoded), pszAppPath);

    pCache->Lock();
    pCache->EnsureScanned();
    for (int i = 0; i < pCache->GetRecordCount(); i++)
    {
        APP_RECORD *pApp = pCache->GetRecord(i);

        if (!pApp->cShortcuts || !pApp->pBest ||
            CompareStringOrdinal(pApp->pszEncodedPath, -1, szEncoded, -1, TRUE) != CSTR_EQUAL)
        {
            continue;
        }

        /* The most used one wins, and then the one with the shortest arguments */
        if (!pChosen ||
            CompareUsage(&pApp->Usage, &pChosen->Usage) < 0 ||
            (CompareUsage(&pApp->Usage, &pChosen->Usage) == 0 &&
             lstrlenW(pApp->pBest->pszArgs) < lstrlenW(pChosen->pBest->pszArgs)))
        {
            pChosen = pApp;
        }
    }

    if (pChosen)
    {
        hr = ppsi ? CreateShortcutItem(pChosen->pBest, ppsi) : S_OK;
        if (SUCCEEDED(hr) && ppszAppID)
        {
            hr = SHStrDupW(pChosen->pszAppID, ppszAppID);
            if (FAILED(hr) && ppsi && *ppsi)
            {
                (*ppsi)->Release();
                *ppsi = NULL;
            }
        }
    }
    pCache->Unlock();
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::CanPinApp(
    _In_ IShellItem *psi)
{
    CAppShortcutCache *pCache = GetAppShortcutCache();
    SFGAOF sfgao = 0;
    PWSTR psz;
    HRESULT hr;

    hr = psi->GetAttributes(SFGAO_LINK, &sfgao);
    if (FAILED(hr))
        return hr;

    if (sfgao & SFGAO_LINK)
    {
        hr = GetAppIDForShortcut(psi, &psz);
        if (SUCCEEDED(hr))
            CoTaskMemFree(psz);
        return hr;
    }

    hr = psi->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &psz);
    if (FAILED(hr))
        return hr;

    if (!pCache || !pCache->PathIsInterestingExe(psz, FALSE) || !pCache->PathIsInterestingLink(psz, TRUE))
        hr = E_APP_NOT_INTERESTING;
    CoTaskMemFree(psz);
    return hr;
}

HRESULT
CStartMenuCacheAndAppResolver::GetRelaunchPropertiesFromStore(
    _In_ IPropertyStore *pps,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Outptr_opt_ PWSTR *ppszCmdLine,
    _Outptr_opt_ PWSTR *ppszIconResource,
    _Outptr_opt_ PWSTR *ppszDisplayNameResource,
    _Out_opt_ BOOL *pfPinnable)
{
    struct
    {
        PWSTR *ppsz;
        const PROPERTYKEY *pkey;
    } fields[] =
    {
        { ppszAppID, &PKEY_AppUserModel_ID },
        { ppszCmdLine, &PKEY_AppUserModel_RelaunchCommand },
        { ppszDisplayNameResource, &PKEY_AppUserModel_RelaunchDisplayNameResource },
        { ppszIconResource, &PKEY_AppUserModel_RelaunchIconResource },
    };
    HRESULT hr = S_OK;

    for (UINT i = 0; i < _countof(fields); i++)
    {
        if (fields[i].ppsz)
            *fields[i].ppsz = NULL;
    }
    if (pfPinnable)
        *pfPinnable = TRUE;

    for (UINT i = 0; SUCCEEDED(hr) && i < _countof(fields); i++)
    {
        PROPVARIANT pv;

        if (!fields[i].ppsz)
            continue;

        PropVariantInit(&pv);
        hr = pps->GetValue(*fields[i].pkey, &pv);
        if (SUCCEEDED(hr) && pv.vt != VT_EMPTY)
        {
            if (pv.vt == VT_LPWSTR && pv.pwszVal)
                hr = SHStrDupW(pv.pwszVal, fields[i].ppsz);
            else
                hr = E_INVALIDARG;
        }
        PropVariantClear(&pv);
    }

    if (SUCCEEDED(hr) && pfPinnable)
    {
        BOOL fPrevent = FALSE;

        if (SUCCEEDED(GetBoolProperty(pps, PKEY_AppUserModel_PreventPinning, &fPrevent)))
            *pfPinnable = !fPrevent;
    }

    if (FAILED(hr))
    {
        for (UINT i = 0; i < _countof(fields); i++)
        {
            if (fields[i].ppsz)
            {
                CoTaskMemFree(*fields[i].ppsz);
                *fields[i].ppsz = NULL;
            }
        }
    }
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GetRelaunchProperties(
    _In_ HWND hwnd,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Outptr_opt_ PWSTR *ppszCmdLine,
    _Outptr_opt_ PWSTR *ppszIconResource,
    _Outptr_opt_ PWSTR *ppszDisplayNameResource,
    _Out_opt_ BOOL *pfPinnable)
{
    CComPtr<IPropertyStore> pps;
    HRESULT hr;

    if (ppszAppID)
        *ppszAppID = NULL;
    if (ppszCmdLine)
        *ppszCmdLine = NULL;
    if (ppszIconResource)
        *ppszIconResource = NULL;
    if (ppszDisplayNameResource)
        *ppszDisplayNameResource = NULL;

    hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARG(IPropertyStore, &pps));
    if (FAILED(hr))
        return hr;

    return GetRelaunchPropertiesFromStore(pps, ppszAppID, ppszCmdLine, ppszIconResource,
                                          ppszDisplayNameResource, pfPinnable);
}

/* Names the folder for a generated shortcut after the application ID */
static
HRESULT
GetImplicitShortcutFolderName(
    _In_ PCWSTR pszAppID,
    _Out_writes_(cchName) PWSTR pszName,
    _In_ SIZE_T cchName)
{
    WCHAR szUpper[MAX_PATH];
    ULONGLONG ullHash = 0xcbf29ce484222325ULL;
    HRESULT hr;

    hr = StringCchCopyW(szUpper, _countof(szUpper), pszAppID);
    if (FAILED(hr))
        return hr;
    CharUpperBuffW(szUpper, lstrlenW(szUpper));

    for (PCWSTR pch = szUpper; *pch; pch++)
    {
        ullHash ^= *pch;
        ullHash *= 0x100000001b3ULL;
    }
    return StringCchPrintfW(pszName, cchName, L"%016I64x", ullHash);
}

HRESULT
CStartMenuCacheAndAppResolver::CreateShortcutFromProperties(
    _In_ PCWSTR pszAppID,
    _In_ PCWSTR pszCmdLine,
    _In_opt_ PCWSTR pszIconResource,
    _In_ PCWSTR pszDisplayNameResource,
    _Outptr_ IShellItem **ppsi)
{
    CComPtr<IShellLinkW> psl;
    CComPtr<IPersistFile> ppf;
    WCHAR szFolderName[32];
    WCHAR szLink[MAX_PATH];
    WCHAR szExe[MAX_PATH];
    WCHAR szIcon[MAX_PATH];
    PWSTR pszShortcuts;
    PWSTR pszArgs;
    BOOL fResourceName = (pszDisplayNameResource[0] == L'@');
    HRESULT hr;

    *ppsi = NULL;
    hr = GetImplicitShortcutFolderName(pszAppID, szFolderName, _countof(szFolderName));
    if (FAILED(hr))
        return hr;

    hr = SHGetKnownFolderPath(FOLDERID_ImplicitAppShortcuts, KF_FLAG_CREATE | KF_FLAG_NO_ALIAS, NULL, &pszShortcuts);
    if (FAILED(hr))
        return hr;

    hr = StringCchCopyW(szLink, _countof(szLink), pszShortcuts);
    CoTaskMemFree(pszShortcuts);
    if (SUCCEEDED(hr) && !PathAppendW(szLink, szFolderName))
        hr = E_FAIL;
    if (FAILED(hr))
        return hr;

    if (!CreateDirectoryW(szLink, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());

    /* A resource name is resolved through the localized name, so the file name does not matter */
    if (fResourceName)
    {
        if (!PathAppendW(szLink, L"pinned.lnk"))
            return E_FAIL;
    }
    else if (!PathAppendW(szLink, pszDisplayNameResource) ||
             FAILED(StringCchCatW(szLink, _countof(szLink), L".lnk")))
    {
        return E_FAIL;
    }

    /* An existing shortcut is reused as it is */
    if (SUCCEEDED(SHCreateItemFromParsingName(szLink, NULL, IID_PPV_ARG(IShellItem, ppsi))))
        return S_OK;

    hr = StringCchCopyW(szExe, _countof(szExe), pszCmdLine);
    if (FAILED(hr))
        return hr;
    pszArgs = PathGetArgsW(szExe);
    if (*pszArgs)
        pszArgs[-1] = UNICODE_NULL;
    PathRemoveBlanksW(szExe);
    PathUnquoteSpacesW(szExe);

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC, IID_PPV_ARG(IShellLinkW, &psl));
    if (SUCCEEDED(hr))
        hr = psl->SetPath(szExe);
    if (SUCCEEDED(hr) && *pszArgs)
        hr = psl->SetArguments(pszArgs);
    if (SUCCEEDED(hr))
    {
        WCHAR szWorkDir[MAX_PATH];

        StringCchCopyW(szWorkDir, _countof(szWorkDir), szExe);
        PathRemoveFileSpecW(szWorkDir);
        hr = psl->SetWorkingDirectory(szWorkDir);
    }
    if (SUCCEEDED(hr) && pszIconResource)
    {
        int iIcon;

        hr = StringCchCopyW(szIcon, _countof(szIcon), pszIconResource);
        if (SUCCEEDED(hr))
        {
            iIcon = PathParseIconLocationW(szIcon);
            hr = (iIcon > 0) ? E_FAIL : psl->SetIconLocation(szIcon, iIcon);
        }
    }
    if (FAILED(hr))
        return hr;

    /* Links without a property store cannot carry the ID; the generated folder name still ties them */
    {
        CComPtr<IPropertyStore> pps;

        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps))))
        {
            PROPVARIANT pv;

            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<PWSTR>(pszAppID);
            hr = pps->SetValue(PKEY_AppUserModel_ID, pv);
            if (SUCCEEDED(hr))
                hr = pps->Commit();
            if (FAILED(hr))
                return hr;
        }
    }

    hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
    if (SUCCEEDED(hr))
        hr = ppf->Save(szLink, TRUE);
    if (FAILED(hr))
        return hr;

    if (fResourceName)
    {
        int iName;

        hr = StringCchCopyW(szIcon, _countof(szIcon), pszDisplayNameResource + 1);
        if (FAILED(hr))
            return hr;
        iName = -PathParseIconLocationW(szIcon);
        if (iName <= 0)
            return E_FAIL;
        hr = SHSetLocalizedName(szLink, szIcon, iName);
        if (FAILED(hr))
            return hr;
    }

    CComPtr<IBindCtx> pbc;
    hr = CreateBindCtxWithParam(PIN_BIND_NO_ALIAS, &pbc);
    if (SUCCEEDED(hr))
        hr = SHCreateItemFromParsingName(szLink, pbc, IID_PPV_ARG(IShellItem, ppsi));
    return hr;
}

static
BOOL
IsCallerProcessBelowMediumIL(
    _In_ HWND hwnd)
{
    DWORD dwProcessId = 0;
    HANDLE hProcess;
    BOOL fLow;

    if (!GetWindowThreadProcessId(hwnd, &dwProcessId))
        return TRUE;
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, TRUE, dwProcessId);
    if (!hProcess)
        return TRUE;
    fLow = IsTokenBelowMediumIL(hProcess);
    CloseHandle(hProcess);
    return fLow;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GenerateShortcutFromWindowProperties(
    _In_ HWND hwnd,
    _Outptr_ IShellItem **ppsi)
{
    PWSTR pszAppID = NULL;
    PWSTR pszCmdLine = NULL;
    PWSTR pszIcon = NULL;
    PWSTR pszName = NULL;
    BOOL fPinnable = FALSE;
    HRESULT hr;

    *ppsi = NULL;
    if (IsCallerProcessBelowMediumIL(hwnd))
        return E_ACCESSDENIED;

    hr = GetRelaunchProperties(hwnd, &pszAppID, &pszCmdLine, &pszIcon, &pszName, &fPinnable);
    if (SUCCEEDED(hr))
    {
        if (!fPinnable)
            hr = E_APP_NOT_INTERESTING;
        else if (!pszAppID || !pszCmdLine || !pszName)
            hr = E_FAIL;
        else
            hr = CreateShortcutFromProperties(pszAppID, pszCmdLine, pszIcon, pszName, ppsi);
    }

    CoTaskMemFree(pszAppID);
    CoTaskMemFree(pszCmdLine);
    CoTaskMemFree(pszIcon);
    CoTaskMemFree(pszName);
    return hr;
}

STDMETHODIMP
CStartMenuCacheAndAppResolver::GenerateShortcutFromItemProperties(
    _In_ IShellItem2 *psi,
    _Outptr_ IShellItem **ppsi)
{
    static const PROPERTYKEY s_Keys[] =
    {
        PKEY_AppUserModel_ID,
        PKEY_AppUserModel_RelaunchCommand,
        PKEY_AppUserModel_RelaunchDisplayNameResource,
        PKEY_AppUserModel_RelaunchIconResource,
    };
    CComPtr<IPropertyStore> pps;
    PWSTR pszAppID = NULL;
    PWSTR pszCmdLine = NULL;
    PWSTR pszIcon = NULL;
    PWSTR pszName = NULL;
    BOOL fPinnable = FALSE;
    HRESULT hr;

    *ppsi = NULL;
    hr = psi->GetPropertyStoreForKeys(s_Keys, _countof(s_Keys), GPS_DEFAULT, IID_PPV_ARG(IPropertyStore, &pps));
    if (FAILED(hr))
        return hr;

    hr = GetRelaunchPropertiesFromStore(pps, &pszAppID, &pszCmdLine, &pszIcon, &pszName, &fPinnable);
    if (SUCCEEDED(hr))
    {
        if (!fPinnable)
            hr = E_APP_NOT_INTERESTING;
        else if (!pszAppID || !pszCmdLine || !pszName)
            hr = E_FAIL;
        else
            hr = CreateShortcutFromProperties(pszAppID, pszCmdLine, pszIcon, pszName, ppsi);
    }

    CoTaskMemFree(pszAppID);
    CoTaskMemFree(pszCmdLine);
    CoTaskMemFree(pszIcon);
    CoTaskMemFree(pszName);
    return hr;
}
