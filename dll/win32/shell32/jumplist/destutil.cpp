/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Helpers shared by the destination lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <propvarutil.h>
#include "jumplist.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

EXTERN_C const CLSID CLSID_ApplicationDestinations =
    { 0x86c14003, 0x4d6b, 0x4ef3, { 0xa7, 0xb4, 0x05, 0x06, 0x66, 0x3b, 0x2e, 0x68 } };
EXTERN_C const CLSID CLSID_ApplicationDocumentLists =
    { 0x86bec222, 0x30f2, 0x47e0, { 0x9f, 0x25, 0x60, 0xd1, 0x1c, 0xd7, 0x5c, 0x28 } };

/* objectarray.idl is not part of the ReactOS uuid library */
EXTERN_C const IID IID_IObjectArray =
    { 0x92ca9dcd, 0x5622, 0x4bba, { 0xa8, 0x05, 0x5e, 0x9f, 0x54, 0x1b, 0xd8, 0xc9 } };
EXTERN_C const IID IID_IObjectCollection =
    { 0x5632b1a4, 0xe38a, 0x400a, { 0x92, 0x8a, 0xd4, 0xcd, 0x63, 0x23, 0x02, 0x95 } };

#define EXPLORER_ADVANCED_KEY   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"

/* Reflected form of the polynomial behind the list file names */
#define JUMPLIST_CRC64_POLY     0x92C64265D32139A4ULL

/* The counters explorer reads; same section and layout as explorer's reader */
#define SHELL_COUNTER_SECTION   L"Local\\ShellGlobalCounters"
#define SHELL_COUNTER_COUNT     13

static ULONGLONG g_rgullCrc64[256];
static BOOL g_fCrc64Ready = FALSE;

static
VOID
JumpListCrc64InitTable(VOID)
{
    if (g_fCrc64Ready)
        return;

    for (UINT i = 0; i < _countof(g_rgullCrc64); i++)
    {
        ULONGLONG ull = i;

        for (UINT iBit = 0; iBit < 8; iBit++)
            ull = (ull & 1) ? ((ull >> 1) ^ JUMPLIST_CRC64_POLY) : (ull >> 1);
        g_rgullCrc64[i] = ull;
    }
    g_fCrc64Ready = TRUE;
}

CJumpListCrc64::CJumpListCrc64() :
    m_ullValue(~0ULL)
{
}

CJumpListCrc64::CJumpListCrc64(
    _In_reads_bytes_(cb) const void *pv,
    _In_ ULONG cb) :
    m_ullValue(~0ULL)
{
    Update(pv, cb);
}

ULONGLONG
CJumpListCrc64::Update(
    _In_reads_bytes_(cb) const void *pv,
    _In_ ULONG cb)
{
    const BYTE *pb = (const BYTE *)pv;

    JumpListCrc64InitTable();
    while (cb)
    {
        m_ullValue = (m_ullValue >> 8) ^ g_rgullCrc64[(BYTE)(m_ullValue ^ *pb)];
        pb++;
        cb -= 1;
    }
    return m_ullValue;
}

HRESULT
CJumpListCrc64::ToString(
    _Out_writes_(cch) PWSTR psz,
    _In_ UINT cch) const
{
    return StringCchPrintfW(psz, cch, L"%I64x", m_ullValue);
}

/* The list file name of an application: the CRC of its upper cased ID */
HRESULT
HashAppID(
    _In_ PCWSTR pszAppID,
    _Out_writes_(cchHash) PWSTR pszHash,
    _In_ UINT cchHash)
{
    WCHAR szUpper[MAX_PATH];
    size_t cbUpper;
    HRESULT hr;

    hr = StringCchCopyW(szUpper, _countof(szUpper), pszAppID);
    if (FAILED(hr))
        return hr;

    CharUpperBuffW(szUpper, lstrlenW(szUpper));
    hr = StringCbLengthW(szUpper, sizeof(szUpper), &cbUpper);
    if (FAILED(hr))
        return hr;

    CJumpListCrc64 crc(szUpper, (ULONG)cbUpper);
    return crc.ToString(pszHash, cchHash);
}

/* The AutomaticDestinations or CustomDestinations folder, created when missing */
HRESULT
GetDestinationsPath(
    _In_ UINT uType,
    _Out_writes_(MAX_PATH) PWSTR pszPath)
{
    WCHAR szFolder[MAX_PATH];
    PWSTR pszRecent = NULL;
    PCWSTR pszSub = NULL;
    HRESULT hr;

    hr = SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY | KF_FLAG_NO_ALIAS, NULL, &pszRecent);
    if (SUCCEEDED(hr))
    {
        if (uType == DESTINATIONS_AUTOMATIC)
            pszSub = L"AutomaticDestinations";
        else if (uType == DESTINATIONS_CUSTOM)
            pszSub = L"CustomDestinations";

        if (!PathCombineW(szFolder, pszRecent, pszSub))
            hr = E_FAIL;

        if (SUCCEEDED(hr) && !CreateDirectoryW(szFolder, NULL))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                hr = S_OK;
        }

        if (SUCCEEDED(hr))
            hr = StringCchCopyW(pszPath, MAX_PATH, szFolder);
    }
    CoTaskMemFree(pszRecent);
    return hr;
}

static
UINT
ReadStartPageSetting(
    _In_ PCWSTR pszValue,
    _In_ UINT uDefault)
{
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);
    DWORD dwType = 0;

    if (SHRegGetUSValueW(EXPLORER_ADVANCED_KEY, pszValue, &dwType, &dwValue, &cbValue,
                         FALSE, NULL, 0) != ERROR_SUCCESS ||
        dwType != REG_DWORD)
    {
        return uDefault;
    }
    return dwValue;
}

BOOL
IsRecentDocsEnabled(VOID)
{
    if (SHRestricted(REST_NORECENTDOCSHISTORY))
        return FALSE;
    return ReadStartPageSetting(L"Start_TrackDocs", TRUE) != 0;
}

UINT
ReadStartPageSettingBounded(
    _In_ PCWSTR pszValue,
    _In_ UINT uDefault,
    _In_ UINT uMin,
    _In_ UINT uMax)
{
    UINT uValue = ReadStartPageSetting(pszValue, uDefault);

    if (uValue < uMin)
        uValue = uMin;
    if (uValue > uMax)
        uValue = uMax;
    return uValue;
}

/* Bumps the destination list counter so explorer drops its cached jump lists */
VOID
JumpListCounterIncrement(VOID)
{
    static LONG volatile *s_plCounters = NULL;
    HANDLE hSection;
    void *pvView;

    if (!s_plCounters)
    {
        hSection = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                      SHELL_COUNTER_COUNT * sizeof(LONG), SHELL_COUNTER_SECTION);
        if (!hSection)
            return;

        pvView = MapViewOfFile(hSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        CloseHandle(hSection);
        if (!pvView)
            return;

        if (InterlockedCompareExchangePointer((void * volatile *)&s_plCounters, pvView, NULL) != NULL)
            UnmapViewOfFile(pvView);
    }

    InterlockedIncrement(&s_plCounters[JUMPLIST_GLOBALCOUNTER]);
}

/* A string property; an unset one reads as an empty string */
HRESULT
JumpListGetString(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key,
    _Outptr_ PWSTR *ppsz)
{
    PROPVARIANT pv;
    HRESULT hr;

    *ppsz = NULL;
    PropVariantInit(&pv);
    hr = pps->GetValue(key, &pv);
    if (FAILED(hr))
        return hr;

    if (pv.vt == VT_EMPTY)
        hr = SHStrDupW(L"", ppsz);
    else
        hr = PropVariantToStringAlloc(pv, ppsz);
    PropVariantClear(&pv);
    return hr;
}

static
BOOL
GetLinkBoolProperty(
    _In_ IUnknown *punk,
    _In_ REFPROPERTYKEY key)
{
    CComPtr<IPropertyStore> pps;
    BOOL fValue = FALSE;
    PROPVARIANT pv;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps))) &&
        SUCCEEDED(pps->GetValue(key, &pv)))
    {
        PropVariantToBoolean(pv, &fValue);
        PropVariantClear(&pv);
    }
    return fValue;
}

BOOL
IsSeparator(
    _In_ IUnknown *punk)
{
    return GetLinkBoolProperty(punk, PKEY_AppUserModel_IsDestListSeparator);
}

/* Links the shell made for plain items, as opposed to links an application supplied */
BOOL
IsDestListLink(
    _In_ IUnknown *punk)
{
    return GetLinkBoolProperty(punk, PKEY_AppUserModel_IsDestListLink);
}

HRESULT
MarkDestListLink(
    _In_ IUnknown *punk)
{
    CComPtr<IPropertyStore> pps;
    PROPVARIANT pv;
    HRESULT hr;

    hr = punk->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps));
    if (FAILED(hr))
        return hr;

    PropVariantInit(&pv);
    pv.vt = VT_BOOL;
    pv.boolVal = VARIANT_TRUE;
    hr = pps->SetValue(PKEY_AppUserModel_IsDestListLink, pv);
    if (SUCCEEDED(hr))
        hr = pps->Commit();
    return hr;
}

/* Wraps an item in a shell link marked as a destination list link */
HRESULT
CreateLinkForItem(
    _In_ IUnknown *punkItem,
    _Outptr_ IShellLinkW **ppsl,
    _In_ BOOL fKeepTracking)
{
    CComPtr<IShellLinkW> psl;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    *ppsl = NULL;
    hr = SHGetIDListFromObject(punkItem, &pidl);
    if (FAILED(hr))
        return hr;

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &psl));
    if (SUCCEEDED(hr))
    {
        /* A link only used to compare items needs no tracking data */
        if (!fKeepTracking)
            SetLinkFlags(psl, SLDF_FORCE_NO_LINKINFO | SLDF_FORCE_NO_LINKTRACK,
                         SLDF_FORCE_NO_LINKINFO | SLDF_FORCE_NO_LINKTRACK);

        hr = psl->SetIDList(pidl);
        if (SUCCEEDED(hr))
            hr = MarkDestListLink(psl);
        if (SUCCEEDED(hr))
            *ppsl = psl.Detach();
    }
    CoTaskMemFree(pidl);
    return hr;
}

HRESULT
CreateItemFromLink(
    _In_ IShellLinkW *psl,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    *ppv = NULL;
    hr = psl->GetIDList(&pidl);
    if (hr != S_OK)
        return SUCCEEDED(hr) ? E_FAIL : hr;

    hr = SHCreateItemFromIDList(pidl, riid, ppv);
    CoTaskMemFree(pidl);
    return hr;
}

/* Name of an application link: the CRC of its upper cased path, its arguments and title */
HRESULT
DisplayNameFromLink(
    _In_ IShellLinkW *psl,
    _Outptr_ PWSTR *ppszName,
    _Inout_updates_z_(MAX_PATH) PWSTR pszPath)
{
    CComPtr<IPropertyStore> pps;
    CComHeapPtr<WCHAR> pszArgs;
    CComHeapPtr<WCHAR> pszTitle;
    WCHAR szHash[20];
    HRESULT hr;

    *ppszName = NULL;
    hr = psl->QueryInterface(IID_PPV_ARG(IPropertyStore, &pps));
    if (FAILED(hr))
        return hr;

    hr = JumpListGetString(pps, PKEY_Link_Arguments, &pszArgs);
    if (SUCCEEDED(hr))
        hr = JumpListGetString(pps, PKEY_Title, &pszTitle);
    if (FAILED(hr))
        return hr;

    /* Separators carry no command and never match anything */
    if (IsSeparator(psl))
        return E_INVALIDARG;

    CharUpperBuffW(pszPath, lstrlenW(pszPath));
    CharUpperBuffW(pszTitle, lstrlenW(pszTitle));

    CStringW strName(pszPath);
    strName += pszArgs;
    strName += pszTitle;

    CJumpListCrc64 crc(strName.GetString(), strName.GetLength() * sizeof(WCHAR));
    hr = crc.ToString(szHash, _countof(szHash));
    if (SUCCEEDED(hr))
        hr = SHStrDupW(szHash, ppszName);
    return hr;
}

/* DPA_Search callback over shell links; 0 when both lead to the same destination */
int
CALLBACK
DPA_CompareLinksCB(
    _In_ void *p1,
    _In_ void *p2,
    _In_ LPARAM lParam)
{
    IShellLinkW *psl1 = (IShellLinkW *)p1;
    IShellLinkW *psl2 = (IShellLinkW *)p2;
    BOOL fDestListLink = IsDestListLink(psl1);
    int iResult = -1;

    UNREFERENCED_PARAMETER(lParam);

    if (fDestListLink != IsDestListLink(psl2))
        return iResult;

    if (fDestListLink)
    {
        CComPtr<IShellItem> psiItem1;
        CComPtr<IShellItem> psiItem2;

        if (SUCCEEDED(CreateItemFromLink(psl1, IID_PPV_ARG(IShellItem, &psiItem1))) &&
            SUCCEEDED(CreateItemFromLink(psl2, IID_PPV_ARG(IShellItem, &psiItem2))))
        {
            psiItem1->Compare(psiItem2, SICHINT_CANONICAL, &iResult);
        }
        return iResult;
    }

    WCHAR szPath[MAX_PATH];
    CComHeapPtr<WCHAR> pszName1;
    CComHeapPtr<WCHAR> pszName2;

    if (psl1->GetPath(szPath, _countof(szPath), NULL, SLGP_RAWPATH) != S_OK ||
        FAILED(DisplayNameFromLink(psl1, &pszName1, szPath)))
    {
        return iResult;
    }
    if (psl2->GetPath(szPath, _countof(szPath), NULL, SLGP_RAWPATH) != S_OK ||
        FAILED(DisplayNameFromLink(psl2, &pszName2, szPath)))
    {
        return iResult;
    }

    if (CompareStringOrdinal(pszName1, -1, pszName2, -1, FALSE) == CSTR_EQUAL)
        iResult = 0;
    return iResult;
}

int
CALLBACK
DPA_ReleaseUnknownCB(
    _In_opt_ void *p,
    _In_opt_ void *pData)
{
    UNREFERENCED_PARAMETER(pData);

    if (p)
        ((IUnknown *)p)->Release();
    return TRUE;
}

static
BOOL
IsExpectedCLSID(
    _In_reads_(cclsid) const CLSID * const *rgpclsid,
    _In_ UINT cclsid,
    _In_ REFCLSID clsid)
{
    for (UINT i = 0; i < cclsid; i++)
    {
        if (IsEqualCLSID(*rgpclsid[i], clsid))
            return TRUE;
    }
    return FALSE;
}

/* Loads an object saved as its class ID and stream, from a short list of allowed classes */
HRESULT
IUnknown_LoadKnownImplFromStream(
    _In_ IStream *pstm,
    _In_reads_(cclsid) const CLSID * const *rgpclsid,
    _In_ UINT cclsid,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CComPtr<IUnknown> punk;
    CComPtr<IPersistStream> pps;
    CLSID clsid;
    HRESULT hr;

    *ppv = NULL;
    hr = IStream_Read(pstm, &clsid, sizeof(clsid));
    if (FAILED(hr))
        return hr;

    if (!IsExpectedCLSID(rgpclsid, cclsid, clsid))
        return E_INVALIDARG;

    hr = CoCreateInstance(clsid, NULL, CLSCTX_SERVER | CLSCTX_NO_CODE_DOWNLOAD, riid, (void **)&punk);
    if (FAILED(hr))
        return hr;

    hr = punk->QueryInterface(IID_PPV_ARG(IPersistStream, &pps));
    if (SUCCEEDED(hr))
        hr = pps->Load(pstm);
    if (SUCCEEDED(hr))
        hr = pps->QueryInterface(riid, ppv);
    return hr;
}

HRESULT
IUnknown_SaveKnownImplToStream(
    _In_ IStream *pstm,
    _In_reads_(cclsid) const CLSID * const *rgpclsid,
    _In_ UINT cclsid,
    _In_ IUnknown *punk)
{
    CComPtr<IPersistStream> pps;
    CLSID clsid;
    HRESULT hr;

    hr = punk->QueryInterface(IID_PPV_ARG(IPersistStream, &pps));
    if (FAILED(hr))
        return hr;

    hr = pps->GetClassID(&clsid);
    if (FAILED(hr))
        return hr;

    if (!IsExpectedCLSID(rgpclsid, cclsid, clsid))
        return E_INVALIDARG;

    hr = IStream_Write(pstm, &clsid, sizeof(clsid));
    if (SUCCEEDED(hr))
        hr = pps->Save(pstm, TRUE);
    return hr;
}

/* Strings in the list files: a 16 bit character count, then the characters */
HRESULT
JumpListStreamReadString(
    _In_ IStream *pstm,
    _Outptr_ PWSTR *ppsz)
{
    USHORT cch;
    HRESULT hr;

    *ppsz = NULL;
    hr = IStream_Read(pstm, &cch, sizeof(cch));
    if (FAILED(hr))
        return hr;

    PWSTR psz = (PWSTR)CoTaskMemAlloc((cch + 1) * sizeof(WCHAR));
    if (!psz)
        return E_OUTOFMEMORY;

    hr = IStream_Read(pstm, psz, cch * sizeof(WCHAR));
    if (FAILED(hr))
    {
        CoTaskMemFree(psz);
        return hr;
    }

    psz[cch] = UNICODE_NULL;
    *ppsz = psz;
    return S_OK;
}

HRESULT
JumpListStreamWriteString(
    _In_ IStream *pstm,
    _In_opt_ PCWSTR psz)
{
    size_t cch = psz ? wcslen(psz) : 0;
    USHORT cchWrite;
    HRESULT hr;

    if (cch > USHRT_MAX)
        return E_INVALIDARG;

    cchWrite = (USHORT)cch;
    hr = IStream_Write(pstm, &cchWrite, sizeof(cchWrite));
    if (SUCCEEDED(hr) && cchWrite)
        hr = IStream_Write(pstm, psz, cchWrite * sizeof(WCHAR));
    return hr;
}

static
HRESULT
GetProcessImagePath(
    _In_ DWORD dwProcessId,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath)
{
    HANDLE hProcess;
    BOOL fOk;

    if (dwProcessId == GetCurrentProcessId())
        return GetModuleFileNameW(NULL, pszPath, cchPath) ? S_OK : HRESULT_FROM_WIN32(GetLastError());

    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
    if (!hProcess)
        return HRESULT_FROM_WIN32(GetLastError());

    fOk = QueryFullProcessImageNameW(hProcess, 0, pszPath, &cchPath);
    CloseHandle(hProcess);
    return fOk ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

/*
 * Checks that an item may appear in an application's jump list. The application is named
 * by its ID or, without one, by the process the item was used in.
 */
HRESULT
MakeDestinationItemForApp(
    _In_ IShellItem *psi,
    _In_opt_ IApplicationResolver *par,
    _In_opt_ PCWSTR pszAppID,
    _In_ DWORD dwProcessId,
    _In_opt_ PCWSTR pszAppPath,
    _Outptr_opt_ PWSTR *ppszAppID)
{
    CComPtr<IApplicationResolver> parCreated;
    CComHeapPtr<WCHAR> pszResolved;
    WCHAR szAppPath[MAX_PATH];
    HRESULT hr;

    if (ppszAppID)
        *ppszAppID = NULL;

    if (!pszAppID)
    {
        if (!par)
        {
            hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARG(IApplicationResolver, &parCreated));
            if (FAILED(hr))
                return hr;
            par = parCreated;
        }

        hr = par->GetAppIDForProcess(dwProcessId, &pszResolved, NULL, NULL, NULL);
        if (FAILED(hr))
            return hr;
        pszAppID = pszResolved;
    }

    if (!pszAppPath && dwProcessId && SUCCEEDED(GetProcessImagePath(dwProcessId, szAppPath, _countof(szAppPath))))
        pszAppPath = szAppPath;

    hr = MakeDestinationItem(psi, par, pszAppID, pszAppPath);
    if (SUCCEEDED(hr) && ppszAppID)
        hr = SHStrDupW(pszAppID, ppszAppID);
    return hr;
}
