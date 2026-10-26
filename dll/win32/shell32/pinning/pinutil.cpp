/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Path, ID list and application ID helpers for pinning
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "pinning.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* Long enough for any path the folder comparison deals with */
#define PCP_MAX_PATH    2084

/* SHGetPathFromIDListEx options */
#define PIN_GPFIDL_ALTNAME      0x1
#define PIN_GPFIDL_UNCPRINTER   0x2

/* Darwin IDs are handed around as a path that starts with this character */
#define DARWIN_ID_MARKER        L'\x0001'

/* Attributes that rule out a virtual item having its own application ID */
#define SFGAO_APPIDROOT_EXCLUDE (SFGAO_FILESYSTEM | SFGAO_FILESYSANCESTOR | SFGAO_STORAGEANCESTOR | SFGAO_STORAGE)

typedef struct _PIN_MD5_CONTEXT
{
    ULONG Count[2];
    ULONG State[4];
    UCHAR Buffer[64];
    UCHAR Digest[16];
} PIN_MD5_CONTEXT;

EXTERN_C const CLSID CLSID_StartMenuPin =
    { 0xa2a9545d, 0xa0c2, 0x42b4, { 0x97, 0x08, 0xa0, 0xb2, 0xba, 0xdd, 0x77, 0xc8 } };
EXTERN_C const CLSID CLSID_TaskbandPin =
    { 0x90aa3a4e, 0x1cba, 0x4233, { 0xb8, 0xbb, 0x53, 0x57, 0x73, 0xd4, 0x84, 0x49 } };
EXTERN_C const CLSID CLSID_StartMenuCacheAndAppResolver =
    { 0x660b90c8, 0x73a9, 0x4b58, { 0x8c, 0xae, 0x35, 0x5b, 0x7f, 0x55, 0x34, 0x1b } };
EXTERN_C const CLSID CLSID_AutomaticDestinationList =
    { 0xf0ae1542, 0xf497, 0x484b, { 0xa1, 0x75, 0xa2, 0x0d, 0xb0, 0x91, 0x44, 0xba } };

EXTERN_C VOID NTAPI MD5Init(PIN_MD5_CONTEXT *Context);
EXTERN_C VOID NTAPI MD5Update(PIN_MD5_CONTEXT *Context, const UCHAR *Buffer, ULONG Length);
EXTERN_C VOID NTAPI MD5Final(PIN_MD5_CONTEXT *Context);

EXTERN_C
HRESULT
WINAPI
SHCompareIDsFull(
    _In_ LPARAM lParam,
    _In_ PCUIDLIST_RELATIVE pidl1,
    _In_ PCUIDLIST_RELATIVE pidl2,
    _Out_opt_ int *piResult)
{
    CComPtr<IShellFolder> psfDesktop;
    HRESULT hr;

    /* Byte identical lists match without asking any folder */
    if (pidl1 == pidl2 ||
        (pidl1 && pidl2 && ILGetSize(pidl1) == ILGetSize(pidl2) &&
         !memcmp(pidl1, pidl2, ILGetSize(pidl1))))
    {
        if (piResult)
            *piResult = 0;
        return S_OK;
    }

    hr = SHGetDesktopFolder(&psfDesktop);
    if (FAILED(hr))
        return hr;

    /* The extended comparison flags only mean something to IShellFolder2 folders */
    if (HIWORD(lParam))
    {
        CComPtr<IShellFolder2> psf2;
        if (FAILED(psfDesktop->QueryInterface(IID_PPV_ARG(IShellFolder2, &psf2))))
            lParam = LOWORD(lParam);
    }

    hr = psfDesktop->CompareIDs(lParam, pidl1, pidl2);
    if (piResult)
        *piResult = SUCCEEDED(hr) ? (short)HRESULT_CODE(hr) : -1;
    return hr;
}

static
VOID
RemoveTrailingSlash(
    _Inout_ PWSTR pszPath,
    _Inout_ int *pcch)
{
    if (*pcch > 0 && pszPath[*pcch - 1] == L'\\')
    {
        *pcch -= 1;
        pszPath[*pcch] = UNICODE_NULL;
    }
}

/* pszFolder may also be a CSIDL value */
EXTERN_C
int
WINAPI
PathComparePaths(
    _In_ PCWSTR pszFolder,
    _In_ PCWSTR pszPath)
{
    WCHAR szFolder[PCP_MAX_PATH];
    WCHAR szPath[PCP_MAX_PATH];
    PCWSTR pszRest;
    int cchFolder;
    int cchPath;
    HRESULT hr;

    if (IS_INTRESOURCE(pszFolder))
        hr = SHGetFolderPathW(NULL, LOWORD(pszFolder) | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, szFolder);
    else
        hr = StringCchCopyW(szFolder, _countof(szFolder), pszFolder);
    if (FAILED(hr) || FAILED(StringCchCopyW(szPath, _countof(szPath), pszPath)))
        return PCP_UNRELATED;

    cchFolder = lstrlenW(szFolder);
    cchPath = lstrlenW(szPath);
    RemoveTrailingSlash(szFolder, &cchFolder);
    RemoveTrailingSlash(szPath, &cchPath);

    if (cchFolder > cchPath)
        return PCP_UNRELATED;

    /* A longer path only counts when the folder ends at a separator */
    if (cchFolder < cchPath)
    {
        if (szPath[cchFolder] != L'\\')
            return PCP_UNRELATED;
        szPath[cchFolder] = UNICODE_NULL;
    }

    if (CompareStringOrdinal(szPath, cchFolder, szFolder, cchFolder, TRUE) != CSTR_EQUAL)
        return PCP_UNRELATED;

    if (cchFolder == cchPath)
        return PCP_EQUAL;

    pszRest = &szPath[cchFolder + 1];
    if (!*pszRest || !StrChrW(pszRest, L'\\'))
        return PCP_CHILD | PCP_IMMEDIATE;
    return PCP_CHILD;
}

BOOL
ILIsEqualByPath(
    _In_ PCIDLIST_ABSOLUTE pidl1,
    _In_ PCIDLIST_ABSOLUTE pidl2)
{
    WCHAR szPath1[MAX_PATH];
    WCHAR szPath2[MAX_PATH];

    if (!SHGetPathFromIDListW(pidl1, szPath1) || !SHGetPathFromIDListW(pidl2, szPath2))
        return FALSE;
    return PathComparePaths(szPath1, szPath2) == PCP_EQUAL;
}

/* Like Windows, a direct child only counts when fImmediate is set and a deeper one only when it is not */
BOOL
ILIsParentByPath(
    _In_ PCIDLIST_ABSOLUTE pidlParent,
    _In_ PCIDLIST_ABSOLUTE pidlChild,
    _In_ BOOL fImmediate)
{
    WCHAR szParent[MAX_PATH];
    WCHAR szChild[MAX_PATH];
    int iCompare;

    if (!SHGetPathFromIDListEx(pidlParent, szParent, _countof(szParent), PIN_GPFIDL_UNCPRINTER) ||
        !SHGetPathFromIDListEx(pidlChild, szChild, _countof(szChild), PIN_GPFIDL_UNCPRINTER))
    {
        return FALSE;
    }

    iCompare = PathComparePaths(szParent, szChild);
    if (iCompare & PCP_IMMEDIATE)
        return fImmediate;
    if (iCompare & PCP_CHILD)
        return !fImmediate;
    return FALSE;
}

EXTERN_C
BOOL
WINAPI
SHGetPathFromIDListEx(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath,
    _In_ GPFIDL_FLAGS uOpts)
{
    WCHAR szShort[MAX_PATH];
    HRESULT hr;

    if (!cchPath)
        return FALSE;
    *pszPath = UNICODE_NULL;
    if (!pidl)
        return FALSE;

    if (ILIsEmpty(pidl))
    {
        PWSTR pszDesktop;

        hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &pszDesktop);
        if (SUCCEEDED(hr))
        {
            hr = StringCchCopyW(pszPath, cchPath, pszDesktop);
            CoTaskMemFree(pszDesktop);
        }
    }
    else
    {
        CComPtr<IShellFolder> psf;
        PCUITEMID_CHILD pidlLast;
        SFGAOF sfgao = SFGAO_FILESYSTEM;

        hr = SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
        if (FAILED(hr))
            return FALSE;

        hr = DisplayNameOfW(psf, pidlLast, SHGDN_FORPARSING, pszPath, cchPath);
        if (SUCCEEDED(hr))
            hr = psf->GetAttributesOf(1, &pidlLast, &sfgao);

        /* Only file system items have a path, except servers when the caller asks for them */
        if (SUCCEEDED(hr) && !(sfgao & SFGAO_FILESYSTEM))
        {
            CLSID clsid;
            if (!(uOpts & PIN_GPFIDL_UNCPRINTER) || FAILED(IUnknown_GetClassID(psf, &clsid)) ||
                !IsEqualCLSID(clsid, CLSID_NetworkServer))
            {
                hr = E_FAIL;
            }
        }
    }

    if (SUCCEEDED(hr) && (uOpts & PIN_GPFIDL_ALTNAME) &&
        GetShortPathNameW(pszPath, szShort, _countof(szShort)))
    {
        hr = StringCchCopyW(pszPath, cchPath, szShort);
    }

    if (FAILED(hr))
        *pszPath = UNICODE_NULL;
    return SUCCEEDED(hr);
}

VOID
ILExpungeRemovedHiddenIDs(
    _Inout_ LPITEMIDLIST pidl)
{
    LPITEMIDLIST pidlLast;
    PIDHIDDENBLOCK pBlock;

    if (!pidl)
        return;

    pidlLast = ILFindLastID(pidl);
    while ((pBlock = ILFindHiddenIDOn(pidlLast, HIDDENID_REMOVED, TRUE)) != NULL)
    {
        WORD cbBlock = pBlock->cb;
        BYTE *pAfter = (BYTE *)pBlock + cbBlock;
        BYTE *pEnd = (BYTE *)pidlLast + pidlLast->mkid.cb + sizeof(USHORT);

        /* The list terminator moves down with the rest of the item */
        MoveMemory(pBlock, pAfter, pEnd - pAfter);
        pidlLast->mkid.cb -= cbBlock;
    }
}

DWORD
SetLinkFlags(
    _In_ IShellLinkW *psl,
    _In_ DWORD dwFlags,
    _In_ DWORD dwMask)
{
    CComPtr<IShellLinkDataList> pdl;
    DWORD dwOld = 0;

    if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IShellLinkDataList, &pdl))))
    {
        if (SUCCEEDED(pdl->GetFlags(&dwOld)) && dwMask)
            pdl->SetFlags((dwFlags & dwMask) | (dwOld & ~dwMask));
    }
    return dwOld;
}

HRESULT
GetPathOrDarwinID(
    _In_ IShellLinkW *psl,
    _Out_writes_(MAX_PATH) PWSTR pszPath,
    _Outptr_opt_ PWSTR *ppszArgs)
{
    CComPtr<IShellLinkDataList> pdl;
    WCHAR szArgs[MAX_PATH] = L"";
    BOOL fHaveTarget = FALSE;
    HRESULT hr;

    if (ppszArgs)
        *ppszArgs = NULL;
    *pszPath = UNICODE_NULL;

    if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IShellLinkDataList, &pdl))))
    {
        DWORD dwFlags = 0;

        hr = pdl->GetFlags(&dwFlags);
        if (SUCCEEDED(hr))
        {
            if (ppszArgs && (dwFlags & SLDF_HAS_ARGS))
                psl->GetArguments(szArgs, _countof(szArgs));

            /* An installer shortcut has no path, its product ID stands in for it */
            if (dwFlags & SLDF_HAS_DARWINID)
            {
                EXP_DARWIN_LINK *pDarwin;

                hr = pdl->CopyDataBlock(EXP_DARWIN_ID_SIG, (void **)&pDarwin);
                if (SUCCEEDED(hr))
                {
                    pszPath[0] = DARWIN_ID_MARKER;
                    hr = StringCchCopyW(pszPath + 1, MAX_PATH - 1, pDarwin->szwDarwinID);
                    LocalFree(pDarwin);
                    fHaveTarget = SUCCEEDED(hr);
                }
            }
        }
    }

    if (!fHaveTarget)
    {
        hr = psl->GetPath(pszPath, MAX_PATH, NULL, SLGP_RAWPATH);
        if (hr == S_FALSE)
        {
            PIDLIST_ABSOLUTE pidlTarget;

            /* A link to a non file system item names it by its parsing name */
            hr = psl->GetIDList(&pidlTarget);
            if (FAILED(hr))
                return hr;
            hr = SHGetNameAndFlagsW(pidlTarget, SHGDN_FORPARSING, pszPath, MAX_PATH, NULL);
            ILFree(pidlTarget);
        }
    }

    if (SUCCEEDED(hr) && ppszArgs && szArgs[0])
        hr = SHStrDupW(szArgs, ppszArgs);
    return hr;
}

HRESULT
PathGetLongPath(
    _In_ PCWSTR pszPath,
    _Out_writes_(cchLong) PWSTR pszLong,
    _In_ DWORD cchLong)
{
    DWORD cch = GetLongPathNameW(pszPath, pszLong, cchLong);
    HRESULT hr;

    if (cch)
        return (cch > cchLong) ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : S_OK;

    hr = HRESULT_FROM_WIN32(GetLastError());
    return FAILED(hr) ? hr : E_FAIL;
}

/* Finds the real child ID for a simple one by asking its folder to parse it again */
static
HRESULT
GetRealChildIDList(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidlChild,
    _Outptr_ PITEMID_CHILD *ppidlReal)
{
    WCHAR szName[MAX_PATH];
    HRESULT hr = S_OK;

    *ppidlReal = NULL;
    if (!ILIsEmpty(pidlChild) && (SHGetAttributes(psf, pidlChild, SFGAO_FILESYSTEM) & SFGAO_FILESYSTEM))
    {
        hr = DisplayNameOfW(psf, pidlChild, SHGDN_INFOLDER | SHGDN_FORPARSING, szName, _countof(szName));
        if (SUCCEEDED(hr))
            hr = psf->ParseDisplayName(NULL, NULL, szName, NULL, (PIDLIST_RELATIVE *)ppidlReal, NULL);
        if (hr != E_NOTIMPL)
            return hr;
    }

    *ppidlReal = ILClone(pidlChild);
    return *ppidlReal ? S_OK : E_OUTOFMEMORY;
}

HRESULT
SimpleToRealIDList(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlReal)
{
    CComPtr<IShellFolder> psf;
    PCUITEMID_CHILD pidlLast;
    PITEMID_CHILD pidlChild;
    PIDLIST_ABSOLUTE pidlParent;
    HRESULT hr;

    *ppidlReal = NULL;
    hr = SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
    if (FAILED(hr))
        return hr;

    hr = GetRealChildIDList(psf, pidlLast, &pidlChild);
    if (FAILED(hr))
        return hr;

    pidlParent = ILClone(pidl);
    if (pidlParent)
    {
        ILRemoveLastID(pidlParent);
        *ppidlReal = ILCombine(pidlParent, pidlChild);
        ILFree(pidlParent);
    }
    ILFree(pidlChild);
    return *ppidlReal ? S_OK : E_OUTOFMEMORY;
}

/* Gets the one item of a data object, by ID list or else by the ANSI file name it carries */
HRESULT
PinGetIDListFromDataObject(
    _In_ IDataObject *pdo,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    *ppidl = NULL;
    if (!pdo)
        return E_INVALIDARG;

    CDataObjectHIDA cida(pdo);
    if (cida)
    {
        if (cida->cidl != 1)
            return E_FAIL;
        *ppidl = ILCombine(HIDA_GetPIDLFolder(cida), HIDA_GetPIDLItem(cida, 0));
        return *ppidl ? S_OK : E_OUTOFMEMORY;
    }

    FORMATETC fmt = { (CLIPFORMAT)RegisterClipboardFormatA(CFSTR_FILENAMEA), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    HRESULT hr = pdo->GetData(&fmt, &medium);
    if (FAILED(hr))
        return cida.hr();

    PCSTR pszName = (PCSTR)GlobalLock(medium.hGlobal);
    if (pszName)
    {
        WCHAR szName[PCP_MAX_PATH];

        SHAnsiToUnicode(pszName, szName, _countof(szName));
        GlobalUnlock(medium.hGlobal);
        hr = SHParseDisplayName(szName, NULL, ppidl, 0, NULL);
    }
    else
    {
        hr = E_UNEXPECTED;
    }
    ReleaseStgMedium(&medium);
    return hr;
}

/* Bind context parameters only need to be present, so any object stands in for the value */
class CBindParamMarker : public IUnknown
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown))
        {
            *ppv = static_cast<IUnknown *>(this);
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
};

static CBindParamMarker g_BindParamMarker;

HRESULT
CreateBindCtxWithParam(
    _In_ PCWSTR pszParam,
    _Outptr_ IBindCtx **ppbc)
{
    CComPtr<IBindCtx> pbc;
    HRESULT hr;

    *ppbc = NULL;
    hr = CreateBindCtx(0, &pbc);
    if (SUCCEEDED(hr))
        hr = pbc->RegisterObjectParam(const_cast<PWSTR>(pszParam), &g_BindParamMarker);
    if (SUCCEEDED(hr))
        *ppbc = pbc.Detach();
    return hr;
}

BOOL
IsExplorerProcess(VOID)
{
    WCHAR szModule[MAX_PATH];

    if (!GetModuleFileNameW(NULL, szModule, _countof(szModule)))
        return FALSE;
    return CompareStringOrdinal(PathFindFileNameW(szModule), -1, L"EXPLORER.EXE", -1, TRUE) == CSTR_EQUAL;
}

/* Explorer learns about its own shortcut changes through the resolver */
VOID
PinSendChangeNotify(
    _In_ LONG lEvent,
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    static LONG s_lExplorer = -1;

    if (s_lExplorer < 0)
        InterlockedExchange(&s_lExplorer, IsExplorerProcess() ? 1 : 0);

    if (s_lExplorer)
    {
        CComPtr<IStartMenuItemsCache> psmic;

        if (SUCCEEDED(CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                                       IID_PPV_ARG(IStartMenuItemsCache, &psmic))))
        {
            psmic->OnChangeNotify(7, lEvent, pidl, NULL);
        }
    }
    else
    {
        SHChangeNotify(lEvent, SHCNF_IDLIST | SHCNF_FLUSH, pidl, NULL);
    }
}

typedef struct _PIN_POLICY
{
    PCWSTR pszKey;
    PCWSTR pszValue;
} PIN_POLICY;

static const PIN_POLICY g_PinPolicies[] =
{
    { L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"NoPinningToTaskbar" },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"TaskbarNoPinnedList" },
    { L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"NoPinningToDestinations" },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoStartMenuSubFolders" },
};

/* Machine policy wins over the user's */
BOOL
PinPolicyEnabled(
    _In_ UINT uPolicy)
{
    DWORD dwValue;

    if (uPolicy >= _countof(g_PinPolicies))
        return FALSE;

    if (RegQueryDword(HKEY_LOCAL_MACHINE, g_PinPolicies[uPolicy].pszKey,
                      g_PinPolicies[uPolicy].pszValue, &dwValue) == ERROR_SUCCESS)
    {
        return dwValue != 0;
    }
    if (RegQueryDword(HKEY_CURRENT_USER, g_PinPolicies[uPolicy].pszKey,
                      g_PinPolicies[uPolicy].pszValue, &dwValue) == ERROR_SUCCESS)
    {
        return dwValue != 0;
    }
    return FALSE;
}

/* 1 for a management console file, 2 for a program, 0 for anything else */
UINT
GetExeOrMscExtension(
    _In_ PCWSTR pszPath)
{
    PCWSTR pszExt = PathFindExtensionW(pszPath);

    if (*pszExt != L'.')
        return 0;
    if (!_wcsicmp(pszExt + 1, L"msc"))
        return 1;
    if (!_wcsicmp(pszExt + 1, L"exe"))
        return 2;
    return 0;
}

/* Folders whose paths application IDs name by GUID, so the IDs survive a different install drive */
static const KNOWNFOLDERID *const g_AppIDFolders[] =
{
    &FOLDERID_ProgramFilesX86,
    &FOLDERID_ProgramFilesX64,
    &FOLDERID_SystemX86,
    &FOLDERID_System,
    &FOLDERID_Windows,
};

VOID
EncodeCommonRelativePath(
    _Out_writes_(cchOut) PWSTR pszOut,
    _In_ SIZE_T cchOut,
    _In_ PCWSTR pszPath)
{
    WCHAR szPath[PCP_MAX_PATH];

    StringCchCopyW(szPath, _countof(szPath), pszPath);

    for (UINT i = 0; i < _countof(g_AppIDFolders); i++)
    {
        PWSTR pszFolder;
        int cchFolder;
        int cchCommon;

        if (FAILED(SHGetKnownFolderPath(*g_AppIDFolders[i], KF_FLAG_DONT_VERIFY | KF_FLAG_NO_ALIAS, NULL, &pszFolder)))
            continue;

        cchCommon = PathCommonPrefixW(pszFolder, szPath, NULL);
        cchFolder = lstrlenW(pszFolder);
        CoTaskMemFree(pszFolder);

        if (cchCommon && cchCommon == cchFolder)
        {
            StringFromGUID2(*g_AppIDFolders[i], pszOut, (int)cchOut);
            StringCchCatW(pszOut, cchOut, &szPath[cchFolder]);
            return;
        }
    }

    StringCchCopyW(pszOut, cchOut, szPath);
}

/* Shortcuts with arguments get a hashed ID, everything else is named by its encoded path */
HRESULT
GenerateAppIDWorker(
    _In_ PCWSTR pszEncodedPath,
    _In_opt_ PCWSTR pszArgs,
    _In_opt_ PCWSTR pszName,
    _Outptr_ PWSTR *ppszAppID)
{
    WCHAR szPath[MAX_PATH];
    WCHAR szGuid[39];
    WCHAR szAppID[64];
    PIN_MD5_CONTEXT md5;
    CStringW strHash;
    GUID guidHash;
    HRESULT hr;

    *ppszAppID = NULL;
    if (!pszArgs || !*pszArgs)
        return SHStrDupW(pszEncodedPath, ppszAppID);

    hr = StringCchCopyW(szPath, _countof(szPath), pszEncodedPath);
    if (FAILED(hr))
        return hr;
    CharUpperBuffW(szPath, lstrlenW(szPath));

    strHash = szPath;
    strHash += pszArgs;
    if (pszName)
        strHash += pszName;

    MD5Init(&md5);
    MD5Update(&md5, (const UCHAR *)strHash.GetString(), strHash.GetLength() * sizeof(WCHAR));
    MD5Final(&md5);

    C_ASSERT(sizeof(guidHash) == sizeof(md5.Digest));
    CopyMemory(&guidHash, md5.Digest, sizeof(guidHash));
    StringFromGUID2(guidHash, szGuid, _countof(szGuid));

    hr = StringCchPrintfW(szAppID, _countof(szAppID), L"Microsoft.AutoGenerated.%s", szGuid);
    if (SUCCEEDED(hr))
        hr = SHStrDupW(szAppID, ppszAppID);
    return hr;
}

HRESULT
GenerateAppID(
    _In_ PCWSTR pszPath,
    _In_opt_ PCWSTR pszArgs,
    _In_opt_ PCWSTR pszName,
    _Outptr_ PWSTR *ppszAppID)
{
    WCHAR szEncoded[MAX_PATH];

    EncodeCommonRelativePath(szEncoded, _countof(szEncoded), pszPath);
    return GenerateAppIDWorker(szEncoded, pszArgs, pszName, ppszAppID);
}

HRESULT
GetAppPathFromShortcut(
    _In_ IShellLinkW *psl,
    _In_opt_ PCIDLIST_ABSOLUTE pidlLink,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath)
{
    WCHAR szTarget[MAX_PATH];
    HRESULT hr;

    UNREFERENCED_PARAMETER(pidlLink);

    hr = GetPathOrDarwinID(psl, szTarget, NULL);
    if (FAILED(hr))
        return hr;

    /* Installer shortcuts need the darwin link cache, which shell32 does not keep yet */
    if (szTarget[0] == DARWIN_ID_MARKER)
        return E_FAIL;

    if (ExpandEnvironmentStringsW(szTarget, pszPath, cchPath))
    {
        hr = S_OK;
        if (!PathIsNetworkPathW(pszPath))
            PathGetLongPath(pszPath, pszPath, cchPath);
    }
    else
    {
        hr = E_FAIL;
    }
    return hr;
}

HRESULT
GetAppPathFromLink(
    _In_ IUnknown *punkItem,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath)
{
    CComPtr<IShellItem> psi;
    CComPtr<IShellLinkW> psl;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    hr = punkItem->QueryInterface(IID_PPV_ARG(IShellItem, &psi));
    if (SUCCEEDED(hr))
        hr = psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IShellLinkW, &psl));
    if (FAILED(hr))
        return hr;

    hr = SHGetIDListFromObject(psi, &pidl);
    if (SUCCEEDED(hr))
    {
        hr = GetAppPathFromShortcut(psl, pidl, pszPath, cchPath);
        ILFree(pidl);
    }
    return hr;
}

HRESULT
GetStringProperty(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key,
    _Outptr_ PWSTR *ppszValue)
{
    PROPVARIANT pv;
    HRESULT hr;

    *ppszValue = NULL;
    PropVariantInit(&pv);
    hr = pps->GetValue(key, &pv);
    if (FAILED(hr))
        return hr;

    if (pv.vt == VT_LPWSTR && pv.pwszVal)
        hr = SHStrDupW(pv.pwszVal, ppszValue);
    else if (pv.vt == VT_BSTR && pv.bstrVal)
        hr = SHStrDupW(pv.bstrVal, ppszValue);
    else
        hr = E_FAIL;
    PropVariantClear(&pv);
    return hr;
}

HRESULT
GetBoolProperty(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key,
    _Out_ BOOL *pfValue)
{
    PROPVARIANT pv;
    HRESULT hr;

    *pfValue = FALSE;
    PropVariantInit(&pv);
    hr = pps->GetValue(key, &pv);
    if (FAILED(hr))
        return hr;

    switch (pv.vt)
    {
        case VT_BOOL:
            *pfValue = (pv.boolVal != VARIANT_FALSE);
            break;
        case VT_UI4:
        case VT_I4:
            *pfValue = (pv.ulVal != 0);
            break;
        case VT_EMPTY:
            break;
        default:
            hr = E_FAIL;
            break;
    }
    PropVariantClear(&pv);
    return hr;
}

VOID
GetShortcutAppProperties(
    _In_ IShellLinkW *psl,
    _Outptr_result_maybenull_ PWSTR *ppszAppID,
    _Out_ BOOL *pfPreventPinning,
    _Out_opt_ BOOL *pfExcludeFromNew)
{
    CComPtr<IPropertyStore> pps;

    *ppszAppID = NULL;
    *pfPreventPinning = FALSE;
    if (pfExcludeFromNew)
        *pfExcludeFromNew = FALSE;

    if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps))))
    {
        GetStringProperty(pps, PKEY_AppUserModel_ID, ppszAppID);
        GetBoolProperty(pps, PKEY_AppUserModel_PreventPinning, pfPreventPinning);
        if (pfExcludeFromNew)
            GetBoolProperty(pps, PKEY_AppUserModel_ExcludeFromShowInNewInstall, pfExcludeFromNew);
    }
}

BOOL
IsInterestingAppID(
    _In_ PCWSTR pszAppID)
{
    HKEY hKey;
    BOOL fInteresting = TRUE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileAssociation\\NoStartPageAppUserModelIDs",
                      0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        fInteresting = !RegValueExists(hKey, pszAppID);
        RegCloseKey(hKey);
    }
    return fInteresting;
}

/* Walks up from a virtual item to the first one that names an application ID */
EXTERN_C
HRESULT
WINAPI
GetAppIDRoot(
    _In_ IShellItem *psi,
    _In_ REFIID riid,
    _Outptr_ void **ppv,
    _Out_ PROPVARIANT *ppropvarAppID)
{
    CComPtr<IShellItem2> psi2;
    SFGAOF sfgao = 0;
    HRESULT hr;

    *ppv = NULL;
    PropVariantInit(ppropvarAppID);

    hr = psi->GetAttributes(SFGAO_APPIDROOT_EXCLUDE, &sfgao);
    if (FAILED(hr))
        return hr;
    if (sfgao)
        return E_FAIL;

    hr = psi->QueryInterface(IID_PPV_ARG(IShellItem2, &psi2));
    while (SUCCEEDED(hr))
    {
        CComPtr<IShellItem> psiParent;

        if (SUCCEEDED(psi2->GetProperty(PKEY_AppUserModel_ID, ppropvarAppID)) &&
            ppropvarAppID->vt != VT_EMPTY)
        {
            return psi2->QueryInterface(riid, ppv);
        }

        hr = psi2->GetParent(&psiParent);
        psi2.Release();
        if (SUCCEEDED(hr))
            hr = psiParent->QueryInterface(IID_PPV_ARG(IShellItem2, &psi2));
    }
    return hr;
}

HRESULT
GetAppIDFromIDList(
    _In_ IApplicationResolver *par,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfCached)
{
    CComPtr<IShellItem> psi;
    SFGAOF sfgao = 0;
    HRESULT hr;

    *ppszAppID = NULL;
    if (pfCached)
        *pfCached = FALSE;

    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi));
    if (FAILED(hr))
        return hr;

    if (SUCCEEDED(psi->GetAttributes(SFGAO_LINK, &sfgao)) && (sfgao & SFGAO_LINK))
        hr = par->GetAppIDForShortcut(psi, ppszAppID);

    if (SUCCEEDED(hr) && !*ppszAppID)
    {
        PWSTR pszPath;

        hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
        if (SUCCEEDED(hr))
        {
            /* Programs keep the ID they were pinned with */
            if (!GetExeOrMscExtension(pszPath))
            {
                hr = E_FAIL;
            }
            else if (ILGetHiddenStringAllocW(pidl, HIDDENID_APPID, ppszAppID))
            {
                if (pfCached)
                    *pfCached = TRUE;
                hr = S_OK;
            }
            else
            {
                hr = GenerateAppID(pszPath, NULL, NULL, ppszAppID);
            }
            CoTaskMemFree(pszPath);
        }
    }
    return hr;
}

/* Takes over pidl; an item without an ID comes back unchanged */
HRESULT
CacheAppIDInIDList(
    _In_ PIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlOut)
{
    CComPtr<IApplicationResolver> par;
    PWSTR pszAppID;
    HRESULT hr;

    *ppidlOut = NULL;
    hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                          IID_PPV_ARG(IApplicationResolver, &par));
    if (FAILED(hr))
    {
        ILFree(pidl);
        return hr;
    }

    if (FAILED(GetAppIDFromIDList(par, pidl, &pszAppID, NULL)))
    {
        *ppidlOut = pidl;
        return S_OK;
    }

    *ppidlOut = ILAppendHiddenStringW(pidl, HIDDENID_APPID, pszAppID);
    CoTaskMemFree(pszAppID);
    return *ppidlOut ? S_OK : E_OUTOFMEMORY;
}

HRESULT
DetermineFolderDestinationParentAppID(
    _In_ IApplicationResolver *par,
    _In_ IShellItem *psiFolder,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfExplorer)
{
    CComPtr<IShellItem2> psi2;
    SFGAOF sfgao = 0;
    HRESULT hr;

    *ppszAppID = NULL;
    if (ppsiApplication)
        *ppsiApplication = NULL;
    if (pfExplorer)
        *pfExplorer = FALSE;

    hr = psiFolder->QueryInterface(IID_PPV_ARG(IShellItem2, &psi2));
    if (FAILED(hr))
        return hr;
    hr = psiFolder->GetAttributes(SFGAO_APPIDROOT_EXCLUDE, &sfgao);
    if (FAILED(hr))
        return hr;

    if (!sfgao)
    {
        CComPtr<IShellItem2> psiRoot;
        PROPVARIANT pvAppID;
        PWSTR pszOwn;

        /* A folder with an ID of its own is an application, not a destination */
        if (SUCCEEDED(psi2->GetString(PKEY_AppUserModel_ID, &pszOwn)))
        {
            CoTaskMemFree(pszOwn);
            return E_PIN_HAS_APPID;
        }

        if (SUCCEEDED(GetAppIDRoot(psiFolder, IID_PPV_ARG(IShellItem2, &psiRoot), &pvAppID)))
        {
            PWSTR pszAppID = NULL;

            if (pvAppID.vt == VT_LPWSTR && pvAppID.pwszVal)
                hr = SHStrDupW(pvAppID.pwszVal, &pszAppID);
            else if (pvAppID.vt == VT_BSTR && pvAppID.bstrVal)
                hr = SHStrDupW(pvAppID.bstrVal, &pszAppID);
            else
                hr = E_FAIL;

            if (SUCCEEDED(hr) && ppsiApplication)
                hr = par->GenerateShortcutFromItemProperties(psiRoot, ppsiApplication);

            if (SUCCEEDED(hr))
                *ppszAppID = pszAppID;
            else
                CoTaskMemFree(pszAppID);
            PropVariantClear(&pvAppID);
            return hr;
        }
    }

    /* Everything else opens in Explorer */
    WCHAR szExplorer[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%systemroot%\\explorer.exe", szExplorer, _countof(szExplorer)))
        return E_FAIL;

    hr = par->GetBestShortcutAndAppIDForAppPath(szExplorer, ppsiApplication, ppszAppID);
    if (SUCCEEDED(hr) && pfExplorer)
        *pfExplorer = TRUE;
    return hr;
}

/* Checks that a destination belongs in the jump list of the given application */
HRESULT
MakeDestinationItem(
    _In_ IShellItem *psiDestination,
    _In_opt_ IApplicationResolver *par,
    _In_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath)
{
    CComPtr<IApplicationResolver> parCreated;
    SFGAOF sfgao = 0;
    HRESULT hr;

    hr = psiDestination->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &sfgao);
    if (FAILED(hr))
        return hr;

    if ((sfgao & SFGAO_FOLDER) && !(sfgao & SFGAO_STREAM))
    {
        PWSTR pszFolderAppID;

        if (!pszAppPath || CompareStringOrdinal(PathFindFileNameW(pszAppPath), -1, L"explorer.exe", -1, TRUE) != CSTR_EQUAL)
            return E_PIN_NOT_FOLDER_APP;

        if (!par)
        {
            hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                                  IID_PPV_ARG(IApplicationResolver, &parCreated));
            if (FAILED(hr))
                return hr;
            par = parCreated;
        }

        hr = DetermineFolderDestinationParentAppID(par, psiDestination, NULL, &pszFolderAppID, NULL);
        if (SUCCEEDED(hr))
        {
            if (CompareStringOrdinal(pszFolderAppID, -1, pszAppID, -1, TRUE) != CSTR_EQUAL)
                hr = E_PIN_NOT_FOLDER_APP;
            CoTaskMemFree(pszFolderAppID);
        }
        return hr;
    }

    /* A file belongs to the program its type opens with */
    WCHAR szHandler[MAX_PATH];
    DWORD cchHandler = _countof(szHandler);
    PWSTR pszName;

    if (!pszAppPath)
        return E_PIN_NOT_FOLDER_APP;

    hr = psiDestination->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &pszName);
    if (FAILED(hr))
        return hr;

    hr = AssocQueryStringW(ASSOCF_VERIFY, ASSOCSTR_EXECUTABLE, PathFindExtensionW(pszName), NULL,
                           szHandler, &cchHandler);
    CoTaskMemFree(pszName);
    if (FAILED(hr))
        return E_PIN_NOT_FOLDER_APP;

    if (CompareStringOrdinal(PathFindFileNameW(szHandler), -1, PathFindFileNameW(pszAppPath), -1, TRUE) != CSTR_EQUAL)
        return E_PIN_NOT_FOLDER_APP;
    return S_OK;
}
