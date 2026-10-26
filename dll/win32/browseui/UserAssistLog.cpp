/*
 * PROJECT:     ReactOS browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Registry backed usage store behind the user assist object
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <initguid.h>
#include <knownfolders.h>

extern const GUID UAIID_APPLICATIONS =
    { 0xcebff5cd, 0xace2, 0x4f4f, { 0x91, 0x78, 0x99, 0x26, 0xf4, 0x17, 0x49, 0xea } };
extern const GUID UAIID_SHORTCUTS =
    { 0xf4e57c4b, 0x2036, 0x45f0, { 0xa9, 0xab, 0x44, 0x3b, 0xcf, 0xe3, 0x3d, 0x9f } };
extern const GUID UAIID_AUTOMATIC =
    { 0x6b2fc80b, 0xbe21, 0x4a6a, { 0x97, 0xc4, 0x11, 0x79, 0xb2, 0x95, 0x03, 0x6e } };
static const GUID UAIID_TEST =
    { 0xb267e3ad, 0xa825, 0x4a09, { 0x82, 0xb9, 0xee, 0xc2, 0x2a, 0xa3, 0xb8, 0x47 } };

/* Registry key of each group, in UA_GROUP_* order */
static const GUID *const c_rgpguidGroups[UA_GROUP_COUNT] =
{
    &UAIID_APPLICATIONS,
    &UAIID_SHORTCUTS,
    &UAIID_TEST,
};

/* Entry names start with one of these folders as a known folder ID when they can */
static const KNOWNFOLDERID *const c_rgpkfidEncode[] =
{
    &FOLDERID_Programs,
    &FOLDERID_CommonPrograms,
    &FOLDERID_Favorites,
    &FOLDERID_UserPinned,
};

/* Weight of each older session relevance, the newest first */
static const FLOAT c_rgflHistoryWeight[UA_HISTORY_COUNT] =
{
    0.95f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f
};

/* Slots compared against before the overall one */
static const UINT c_rgiTopCheck[] = { UA_TOP_SWITCHES, UA_TOP_LAUNCHES };

static const WCHAR c_szSessionValue[] = L"UEME_CTLSESSION";
static const WCHAR c_szDefaultCount[] = L"UEME_CTLCUACount:ctor";
static const WCHAR c_szExplorerKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";

#define UA_STORE_VERSION        5
#define UA_MIN_DEAD_ENTRIES     15

/* Operations a busy store runs once its long lock is free */
#define UA_PENDING_DELETE       0
#define UA_PENDING_RENAME       1
#define UA_PENDING_EXCLUDE      2

typedef struct _UA_PENDING_OP
{
    UINT uOp;
    WCHAR szName[MAX_PATH];
    WCHAR szNewName[MAX_PATH];
} UA_PENDING_OP;

typedef struct _UA_DEAD_ENTRY
{
    PWSTR pszName;
    FILETIME ftLastRun;
} UA_DEAD_ENTRY;

DWORD g_dwUASessionTime = 172800000;
DWORD g_dwUACleanupTime = 16;
BOOL g_fUATestLog = FALSE;

static CUserAssistLog *s_rgpLogs[UA_GROUP_COUNT];

/* Value names are stored with every letter rotated by 13 */
static
PCWSTR
UARotateName(
    _In_ PCWSTR pszIn,
    _Out_writes_(cchOut) PWSTR pszOut,
    _In_ UINT cchOut)
{
    UINT i;

    for (i = 0; i + 1 < cchOut && pszIn[i]; i++)
    {
        WCHAR ch = pszIn[i];

        if (ch >= L'a' && ch <= L'z')
            ch = L'a' + (ch - L'a' + 13) % 26;
        else if (ch >= L'A' && ch <= L'Z')
            ch = L'A' + (ch - L'A' + 13) % 26;

        pszOut[i] = ch;
    }

    pszOut[i] = UNICODE_NULL;
    return pszOut;
}

static
BOOL
UAIsSameName(
    _In_ PCWSTR psz1,
    _In_ PCWSTR psz2)
{
    return StrCmpIW(psz1, psz2) == 0;
}

/* 1 when the key was missing or too old and got deleted, 0 when it is current */
static
HRESULT
UACheckStoreVersion(
    _In_ HKEY hkey,
    _In_ PCWSTR pszSubKey)
{
    BOOL fOutdated = TRUE;
    HRESULT hr = S_OK;
    DWORD dwVersion;
    DWORD cbVersion = sizeof(dwVersion);
    HKEY hkeySub;

    if (RegOpenKeyExW(hkey, pszSubKey, 0, KEY_QUERY_VALUE, &hkeySub) != ERROR_SUCCESS)
        return S_FALSE;

    if (SHGetValueW(hkeySub, NULL, L"Version", NULL, &dwVersion, &cbVersion) == ERROR_SUCCESS)
    {
        if (dwVersion == UA_STORE_VERSION)
        {
            fOutdated = FALSE;
        }
        else if (dwVersion > UA_STORE_VERSION)
        {
            fOutdated = FALSE;
            hr = E_FAIL;
        }
    }

    RegCloseKey(hkeySub);

    if (fOutdated)
        hr = (SHDeleteKeyW(hkey, pszSubKey) == ERROR_SUCCESS) ? S_FALSE : E_FAIL;

    return hr;
}

BOOL
UAIsProgramTrackingEnabled(VOID)
{
    static const WCHAR c_szAdvanced[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    const HKEY rghkeyRoots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };

    for (UINT i = 0; i < _countof(rghkeyRoots); i++)
    {
        DWORD dwValue;
        DWORD cbValue = sizeof(dwValue);

        if (SHGetValueW(rghkeyRoots[i], c_szAdvanced, L"Start_TrackProgs", NULL, &dwValue, &cbValue) ==
            ERROR_SUCCESS)
        {
            return dwValue != 0;
        }
    }

    return TRUE;
}

static
BOOL
UAIsInstrumentationDisabled(VOID)
{
    static const WCHAR c_szPolicies[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);

    if (SHGetValueW(HKEY_LOCAL_MACHINE, c_szPolicies, L"NoInstrumentation", NULL, &dwValue, &cbValue) !=
        ERROR_SUCCESS)
    {
        cbValue = sizeof(dwValue);
        SHGetValueW(HKEY_CURRENT_USER, c_szPolicies, L"NoInstrumentation", NULL, &dwValue, &cbValue);
    }

    return dwValue != 0;
}

/* Session record */

CUserAssistSession::CUserAssistSession() :
    m_dwFlags(0)
{
    ZeroMemory(&m_Data, sizeof(m_Data));
}

void
CUserAssistSession::StartNew()
{
    DWORD dwNext = m_Data.dwSession + 1;

    ZeroMemory(&m_Data, sizeof(m_Data));
    m_Data.dwSession = dwNext;
    m_dwFlags |= UASF_DIRTY;
}

void
CUserAssistSession::AddLaunches(
    _In_ LONG cDelta)
{
    m_Data.cLaunches += cDelta;
    m_dwFlags |= UASF_DIRTY;
}

void
CUserAssistSession::AddSwitches(
    _In_ LONG cDelta)
{
    m_Data.cSwitches += cDelta;
    m_dwFlags |= UASF_DIRTY;
}

void
CUserAssistSession::AddUserTime(
    _In_ LONG dwDelta)
{
    m_Data.dwUserTime += dwDelta;
    m_dwFlags |= UASF_DIRTY;
}

/* Takes an entry's usage out of the totals; a top slot it held gets rebuilt later */
void
CUserAssistSession::RemoveValues(
    _In_ const UA_TOP_ENTRY *pEntry)
{
    m_Data.dwUserTime -= pEntry->dwUserTime;
    m_Data.cLaunches -= pEntry->cLaunches;
    m_Data.cSwitches -= pEntry->cSwitches;

    for (UINT i = 0; i < UA_TOP_COUNT; i++)
    {
        if (UAIsSameName(pEntry->szName, m_Data.rgTop[i].szName))
        {
            m_dwFlags |= UASF_REBUILD;
            ZeroMemory(&m_Data.rgTop[i], sizeof(m_Data.rgTop[i]));
        }
    }
}

void
CUserAssistSession::SetTop(
    _In_ UINT iTop,
    _In_ const UA_TOP_ENTRY *pEntry)
{
    m_Data.rgTop[iTop] = *pEntry;
    m_dwFlags |= UASF_DIRTY;
}

void
CUserAssistSession::GetTop(
    _In_ UINT iTop,
    _Out_ UA_TOP_ENTRY *pEntry) const
{
    *pEntry = m_Data.rgTop[iTop];
}

/* Usage in time units: launches and switches count as an average share of the session time */
FLOAT
CUserAssistSession::Weigh(
    _In_ DWORD cLaunches,
    _In_ DWORD cSwitches,
    _In_ DWORD dwUserTime) const
{
    FLOAT flTotal = (FLOAT)(m_Data.dwUserTime ? m_Data.dwUserTime : 1);
    FLOAT flPerLaunch = 0.0f;
    FLOAT flPerSwitch = 0.0f;

    if (m_Data.cSwitches)
        flPerSwitch = flTotal / (FLOAT)m_Data.cSwitches;
    if (m_Data.cLaunches)
        flPerLaunch = flTotal / (FLOAT)m_Data.cLaunches;

    return (FLOAT)cLaunches * flPerLaunch + (FLOAT)dwUserTime + (FLOAT)cSwitches * flPerSwitch;
}

/* Count record */

CUserAssistCount::CUserAssistCount() :
    m_pSession(NULL),
    m_dwFlags(0)
{
    ZeroMemory(&m_Data, sizeof(m_Data));
    m_Data.iHistory = -1;
    for (UINT i = 0; i < UA_HISTORY_COUNT; i++)
        m_Data.rgflHistory[i] = -1.0f;
}

void
CUserAssistCount::Attach(
    _In_ CUserAssistSession *pSession)
{
    m_pSession = pSession;

    if (!(m_dwFlags & UACF_LOADED))
    {
        m_Data.cLaunches = 0;
        m_Data.cSwitches = 0;
        m_Data.dwUserTime = 0;
        m_Data.fExcluded = FALSE;
        m_Data.lSession = -1;
        m_dwFlags |= UACF_LOADED;
    }

    /* A record that never belonged to a session joins the current one */
    if (m_Data.lSession == -1)
        m_dwFlags |= UACF_DIRTY;
    if (m_Data.lSession < 0)
        m_Data.lSession = (LONG)pSession->GetSessionId();
}

/* This session's usage against the most used entry, at most 1 */
FLOAT
CUserAssistCount::GetSessionRelevance() const
{
    UA_TOP_ENTRY best;
    FLOAT flEntry;
    FLOAT flBest;
    FLOAT flRatio = 0.0f;

    flEntry = m_pSession->Weigh(m_Data.cLaunches, m_Data.cSwitches, m_Data.dwUserTime);
    m_pSession->GetTop(UA_TOP_OVERALL, &best);
    flBest = m_pSession->Weigh(best.cLaunches, best.cSwitches, best.dwUserTime);
    if (flBest != 0.0f)
        flRatio = flEntry / flBest;

    return (flRatio < 1.0f) ? flRatio : 1.0f;
}

/* The current relevance, scaled by how far the session got, averaged with the older ones */
FLOAT
CUserAssistCount::GetRelevance() const
{
    FLOAT flSessionPart;
    FLOAT flSum;
    FLOAT flWeights = 1.0f;
    LONG iSlot;

    flSessionPart = (FLOAT)m_pSession->m_Data.dwUserTime / (FLOAT)g_dwUASessionTime;
    if (flSessionPart > 1.0f)
        flSessionPart = 1.0f;

    flSum = flSessionPart * GetSessionRelevance();
    if (m_Data.iHistory < 0)
        return flSum / flWeights;

    iSlot = m_Data.iHistory;
    for (UINT i = 0; i < UA_HISTORY_COUNT; i++)
    {
        FLOAT flOld = m_Data.rgflHistory[iSlot];

        if (flOld < 0.0f)
            break;

        flWeights += c_rgflHistoryWeight[i];
        flSum += c_rgflHistoryWeight[i] * flOld;

        iSlot = (iSlot == 0) ? UA_HISTORY_COUNT - 1 : iSlot - 1;
    }

    return (flWeights != 0.0f) ? flSum / flWeights : flSum;
}

/* Store */

CUserAssistLog::CUserAssistLog() :
    m_cRef(1),
    m_hkey(NULL),
    m_fWritable(FALSE),
    m_fReloadSession(FALSE),
    m_fSnapshotQueued(FALSE),
    m_hdpaPending(NULL)
{
    InitializeCriticalSection(&m_csLong);
    InitializeCriticalSection(&m_csShort);
}

static
int
CALLBACK
UAFreePendingCB(
    _In_ PVOID p,
    _In_opt_ PVOID pData)
{
    UNREFERENCED_PARAMETER(pData);
    delete (UA_PENDING_OP *)p;
    return TRUE;
}

CUserAssistLog::~CUserAssistLog()
{
    DeleteCriticalSection(&m_csShort);
    DeleteCriticalSection(&m_csLong);

    if (m_hkey)
        RegCloseKey(m_hkey);

    if (m_hdpaPending)
        DPA_DestroyCallback(m_hdpaPending, UAFreePendingCB, NULL);
}

ULONG
CUserAssistLog::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG
CUserAssistLog::Release()
{
    ULONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;

    return cRef;
}

HRESULT
CUserAssistLog::SetRoot(
    _In_ HKEY hkey,
    _In_ BOOL fWritable)
{
    LSTATUS lError;

    if (m_hkey)
    {
        RegCloseKey(m_hkey);
        m_hkey = NULL;
        m_fWritable = FALSE;
    }

    m_fWritable = fWritable;
    lError = RegOpenKeyExW(hkey,
                           NULL,
                           0,
                           fWritable ? (KEY_READ | KEY_SET_VALUE | KEY_CREATE_SUB_KEY) : KEY_READ,
                           &m_hkey);
    if (lError != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(lError);

    m_fReloadSession = TRUE;
    return S_OK;
}

/* Moves the store one key down, creating it when the store may write */
HRESULT
CUserAssistLog::ChangeKey(
    _In_ PCWSTR pszSubKey)
{
    HKEY hkeyNew;
    LSTATUS lError;

    if (!m_hkey)
        return E_UNEXPECTED;

    if (m_fWritable)
    {
        lError = RegCreateKeyExW(m_hkey,
                                 pszSubKey,
                                 0,
                                 NULL,
                                 0,
                                 KEY_READ | KEY_WRITE | DELETE,
                                 NULL,
                                 &hkeyNew,
                                 NULL);
    }
    else
    {
        lError = RegOpenKeyExW(m_hkey, pszSubKey, 0, KEY_QUERY_VALUE, &hkeyNew);
    }

    if (lError != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(lError);

    RegCloseKey(m_hkey);
    m_hkey = hkeyNew;
    m_fReloadSession = TRUE;
    return S_OK;
}

HRESULT
CUserAssistLog::GetLogger(
    _In_ UINT iGroup,
    _Outptr_ CUserAssistLog **ppLog)
{
    if (!s_rgpLogs[iGroup])
    {
        CUserAssistLog *pLog = NULL;
        HKEY hkeyExplorer;

        if (RegOpenKeyExW(HKEY_CURRENT_USER, c_szExplorerKey, 0, KEY_READ, &hkeyExplorer) ==
            ERROR_SUCCESS)
        {
            pLog = new CUserAssistLog();
            if (pLog && FAILED(pLog->SetRoot(hkeyExplorer, TRUE)))
            {
                pLog->Release();
                pLog = NULL;
            }

            RegCloseKey(hkeyExplorer);
        }

        if (pLog)
        {
            WCHAR szGroup[39];
            HRESULT hrVersion = S_OK;
            HRESULT hr;

            StringFromGUID2(*c_rgpguidGroups[iGroup], szGroup, _countof(szGroup));

            hr = pLog->ChangeKey(L"UserAssist");
            if (SUCCEEDED(hr))
            {
                hrVersion = UACheckStoreVersion(pLog->m_hkey, szGroup);
                if (FAILED(hrVersion))
                    hr = hrVersion;
            }

            if (SUCCEEDED(hr))
                hr = pLog->ChangeKey(szGroup);

            if (SUCCEEDED(hr) && hrVersion == S_FALSE)
            {
                DWORD dwVersion = UA_STORE_VERSION;

                if (RegSetValueExW(pLog->m_hkey,
                                   L"Version",
                                   0,
                                   REG_DWORD,
                                   (const BYTE *)&dwVersion,
                                   sizeof(dwVersion)) != ERROR_SUCCESS)
                {
                    hr = E_FAIL;
                }
            }

            if (SUCCEEDED(hr))
                hr = pLog->ChangeKey(L"Count");

            if (FAILED(hr))
            {
                pLog->Release();
                pLog = NULL;
            }

            if (pLog && InterlockedCompareExchangePointer((PVOID *)&s_rgpLogs[iGroup], pLog, NULL))
                pLog->Release();
        }
    }

    *ppLog = s_rgpLogs[iGroup];
    if (!*ppLog)
        return E_FAIL;

    (*ppLog)->AddRef();
    return S_OK;
}

CUserAssistLog *
CUserAssistLog::FromGroup(
    _In_ REFGUID rguidGroup)
{
    if (IsEqualGUID(rguidGroup, UAIID_SHORTCUTS))
        return s_rgpLogs[UA_GROUP_SHORTCUTS];
    if (IsEqualGUID(rguidGroup, UAIID_APPLICATIONS))
        return s_rgpLogs[UA_GROUP_APPLICATIONS];
    if (g_fUATestLog && IsEqualGUID(rguidGroup, UAIID_TEST))
        return s_rgpLogs[UA_GROUP_TEST];

    return NULL;
}

void
CUserAssistLog::EncodePath(
    _In_ PCWSTR pszPath,
    _Out_writes_(cchEncoded) PWSTR pszEncoded,
    _In_ UINT cchEncoded)
{
    WCHAR szPath[UA_CCH_ENTRY];
    int cchPrefix = 0;
    const KNOWNFOLDERID *pkfidFound = NULL;

    StringCchCopyW(szPath, _countof(szPath), pszPath);

    for (UINT i = 0; i < _countof(c_rgpkfidEncode) && !pkfidFound; i++)
    {
        PWSTR pszFolder;

        if (SUCCEEDED(SHGetKnownFolderPath(*c_rgpkfidEncode[i],
                                           KF_FLAG_DONT_VERIFY | KF_FLAG_NO_ALIAS,
                                           NULL,
                                           &pszFolder)))
        {
            int cchCommon = PathCommonPrefixW(pszFolder, szPath, NULL);

            if (cchCommon && cchCommon == lstrlenW(pszFolder))
            {
                cchPrefix = cchCommon;
                pkfidFound = c_rgpkfidEncode[i];
            }

            CoTaskMemFree(pszFolder);
        }
    }

    if (!pkfidFound)
    {
        StringCchCopyW(pszEncoded, cchEncoded, szPath);
        return;
    }

    StringFromGUID2(*pkfidFound, pszEncoded, cchEncoded);
    StringCchCatW(pszEncoded, cchEncoded, &szPath[cchPrefix]);
}

/* Picks the store for a group, an automatic group goes by whether the path is a link */
BOOL
CUserAssistLog::GetLogAndEncodedPath(
    _In_ REFGUID rguidGroup,
    _In_ PCWSTR pszPath,
    _Out_writes_(cchEncoded) PWSTR pszEncoded,
    _In_ UINT cchEncoded,
    _Outptr_result_maybenull_ CUserAssistLog **ppLog)
{
    const GUID *pguidGroup = &rguidGroup;

    *ppLog = NULL;

    if (UAIsInstrumentationDisabled())
        return FALSE;

    if (IsEqualGUID(rguidGroup, UAIID_AUTOMATIC))
    {
        PIDLIST_ABSOLUTE pidl;
        SFGAOF sfgao;

        pguidGroup = &UAIID_APPLICATIONS;
        if (SUCCEEDED(SHParseDisplayName(pszPath, NULL, &pidl, SFGAO_LINK, &sfgao)))
        {
            if (sfgao & SFGAO_LINK)
                pguidGroup = &UAIID_SHORTCUTS;
            CoTaskMemFree(pidl);
        }
    }

    *ppLog = FromGroup(*pguidGroup);
    if (!*ppLog)
        return FALSE;

    EncodePath(pszPath, pszEncoded, cchEncoded);
    return TRUE;
}

HRESULT
CUserAssistLog::ReadValue(
    _In_ PCWSTR pszName,
    _Out_writes_bytes_(cb) void *pv,
    _In_ DWORD cb)
{
    WCHAR szValue[UA_CCH_ENTRY];
    DWORD dwType;

    UARotateName(pszName, szValue, _countof(szValue));
    if (SHQueryValueExW(m_hkey, szValue, NULL, &dwType, pv, &cb) != ERROR_SUCCESS)
        return E_FAIL;

    return S_OK;
}

HRESULT
CUserAssistLog::WriteValue(
    _In_ PCWSTR pszName,
    _In_reads_bytes_(cb) const void *pv,
    _In_ DWORD cb)
{
    WCHAR szValue[UA_CCH_ENTRY];

    UARotateName(pszName, szValue, _countof(szValue));
    if (RegSetValueExW(m_hkey, szValue, 0, REG_BINARY, (const BYTE *)pv, cb) != ERROR_SUCCESS)
        return E_FAIL;

    return S_OK;
}

/* A value that cannot be deleted gets the record written instead */
HRESULT
CUserAssistLog::DeleteValue(
    _In_ PCWSTR pszName,
    _In_reads_bytes_(cb) const void *pv,
    _In_ DWORD cb)
{
    WCHAR szValue[UA_CCH_ENTRY];

    UARotateName(pszName, szValue, _countof(szValue));
    if (SHDeleteValueW(m_hkey, NULL, szValue) == ERROR_SUCCESS)
        return S_OK;

    return WriteValue(pszName, pv, cb);
}

HRESULT
CUserAssistLog::SaveCount(
    _In_ PCWSTR pszName,
    _Inout_ CUserAssistCount *pCount,
    _In_ BOOL fForce)
{
    HRESULT hr;

    if (!fForce && !(pCount->m_dwFlags & (UACF_DIRTY | UACF_DELETE)))
        return S_FALSE;

    if (pCount->m_dwFlags & UACF_DELETE)
    {
        hr = DeleteValue(pszName, &pCount->m_Data, sizeof(pCount->m_Data));
        if (FAILED(hr))
            return hr;

        pCount->m_dwFlags &= ~UACF_DELETE;
    }
    else
    {
        hr = WriteValue(pszName, &pCount->m_Data, sizeof(pCount->m_Data));
    }

    if (SUCCEEDED(hr))
        pCount->m_dwFlags &= ~UACF_DIRTY;

    return hr;
}

/* Collects the decoded names of every entry, leaving out the session and default records */
HRESULT
CUserAssistLog::EnumEntries(
    _Inout_ HDPA *phdpa)
{
    WCHAR szSession[32];
    WCHAR szDefault[44];
    WCHAR szValue[MAX_PATH];
    WCHAR szName[MAX_PATH];
    HRESULT hr = S_OK;
    DWORD iValue = 0;

    if (!*phdpa)
    {
        *phdpa = DPA_Create(4);
        if (!*phdpa)
            return E_OUTOFMEMORY;
    }

    UARotateName(c_szSessionValue, szSession, _countof(szSession));
    UARotateName(c_szDefaultCount, szDefault, _countof(szDefault));

    for (;;)
    {
        DWORD cchValue = _countof(szValue);
        PWSTR pszName;

        if (SHEnumValueW(m_hkey, iValue++, szValue, &cchValue, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (!StrCmpW(szValue, szSession) || !StrCmpW(szValue, szDefault))
            continue;

        pszName = StrDupW(UARotateName(szValue, szName, _countof(szName)));
        if (!pszName)
        {
            hr = E_OUTOFMEMORY;
            break;
        }

        if (DPA_AppendPtr(*phdpa, pszName) == -1)
        {
            LocalFree(pszName);
            hr = E_OUTOFMEMORY;
            break;
        }
    }

    return hr;
}

static
void
UAFreeNames(
    _In_opt_ HDPA hdpa)
{
    if (hdpa)
    {
        for (int i = 0; i < DPA_GetPtrCount(hdpa); i++)
            LocalFree(DPA_FastGetPtr(hdpa, i));

        DPA_Destroy(hdpa);
    }
}

void
CUserAssistLog::LoadSession()
{
    if (!m_fReloadSession)
        return;

    if (FAILED(ReadValue(c_szSessionValue, &m_Session.m_Data, sizeof(m_Session.m_Data))))
    {
        /* Nothing stored yet: start from an empty session that gets written */
        if (!(m_Session.m_dwFlags & UASF_LOADED))
        {
            ZeroMemory(&m_Session.m_Data, sizeof(m_Session.m_Data));
            m_Session.m_dwFlags |= UASF_LOADED | UASF_DIRTY;
        }
    }
    else
    {
        m_Session.m_dwFlags |= UASF_LOADED;
    }

    m_fReloadSession = FALSE;
}

HRESULT
CUserAssistLog::SaveSession()
{
    HRESULT hr = S_FALSE;

    if (m_Session.m_dwFlags & UASF_REBUILD)
    {
        m_Session.m_dwFlags &= ~UASF_REBUILD;
        QueueTask(s_RebuildTask);
    }

    if (m_Session.m_dwFlags & UASF_DIRTY)
    {
        hr = WriteValue(c_szSessionValue, &m_Session.m_Data, sizeof(m_Session.m_Data));
        if (SUCCEEDED(hr))
            m_Session.m_dwFlags &= ~UASF_DIRTY;
    }

    return hr;
}

/* S_OK when the entry exists, S_FALSE when the record is a new one */
HRESULT
CUserAssistLog::ReadCount(
    _In_ PCWSTR pszName,
    _In_ BOOL fUseDefault,
    _Inout_ CUserAssistCount *pCount)
{
    HRESULT hr;

    LoadSession();

    hr = ReadValue(pszName, &pCount->m_Data, sizeof(pCount->m_Data));
    if (SUCCEEDED(hr))
    {
        pCount->m_dwFlags |= UACF_LOADED;
    }
    else if (fUseDefault)
    {
        if (SUCCEEDED(ReadValue(c_szDefaultCount, &pCount->m_Data, sizeof(pCount->m_Data))))
        {
            pCount->m_dwFlags |= UACF_LOADED;
        }
        else
        {
            pCount->Attach(&m_Session);
            pCount->m_Data.lSession = -1;
            SaveCount(c_szDefaultCount, pCount, TRUE);
        }
    }

    pCount->Attach(&m_Session);
    return SUCCEEDED(hr) ? S_OK : S_FALSE;
}

/* Keeps the most launched, most switched and overall most used entries of the session */
HRESULT
CUserAssistLog::UpdateTop(
    _In_ PCWSTR pszName,
    _In_ const CUserAssistCount *pCount)
{
    UA_TOP_ENTRY entryThis;
    UA_TOP_ENTRY entryBest;
    FLOAT flThis;
    FLOAT flBest;
    BOOL fBestChanged = FALSE;
    HRESULT hr;

    m_Session.GetTop(UA_TOP_OVERALL, &entryBest);

    entryThis.cLaunches = pCount->m_Data.cLaunches;
    entryThis.cSwitches = pCount->m_Data.cSwitches;
    entryThis.dwUserTime = pCount->m_Data.dwUserTime;
    hr = StringCchCopyW(entryThis.szName, _countof(entryThis.szName), pszName);
    if (FAILED(hr))
        return hr;

    flThis = m_Session.Weigh(entryThis.cLaunches, entryThis.cSwitches, entryThis.dwUserTime);
    flBest = m_Session.Weigh(entryBest.cLaunches, entryBest.cSwitches, entryBest.dwUserTime);

    for (UINT i = 0; i < _countof(c_rgiTopCheck); i++)
    {
        UINT iTop = c_rgiTopCheck[i];
        UA_TOP_ENTRY entry;
        FLOAT flEntry;
        BOOL fMore;
        BOOL fSame;

        m_Session.GetTop(iTop, &entry);
        flEntry = m_Session.Weigh(entry.cLaunches, entry.cSwitches, entry.dwUserTime);

        if (iTop == UA_TOP_LAUNCHES)
        {
            fMore = entryThis.cLaunches > entry.cLaunches;
            fSame = entryThis.cLaunches == entry.cLaunches;
        }
        else
        {
            fMore = entryThis.cSwitches > entry.cSwitches;
            fSame = entryThis.cSwitches == entry.cSwitches;
        }

        if (fMore || (fSame && flThis > flEntry) || UAIsSameName(entryThis.szName, entry.szName))
        {
            m_Session.SetTop(iTop, &entryThis);
            entry = entryThis;
            flEntry = flThis;
        }

        if (flEntry > flBest)
        {
            fBestChanged = TRUE;
            flBest = flEntry;
            m_Session.SetTop(UA_TOP_OVERALL, &entry);
        }
    }

    if ((!fBestChanged && UAIsSameName(entryThis.szName, entryBest.szName)) || flThis > flBest)
        m_Session.SetTop(UA_TOP_OVERALL, &entryThis);

    return S_OK;
}

HRESULT
CUserAssistLog::RenameTop(
    _In_ PCWSTR pszFrom,
    _In_ PCWSTR pszTo)
{
    for (UINT i = 0; i < UA_TOP_COUNT; i++)
    {
        UA_TOP_ENTRY entry;

        m_Session.GetTop(i, &entry);
        if (UAIsSameName(entry.szName, pszFrom))
        {
            HRESULT hr = StringCchCopyW(entry.szName, _countof(entry.szName), pszTo);

            if (FAILED(hr))
                return hr;

            m_Session.SetTop(i, &entry);
        }
    }

    return S_OK;
}

/* Takes the short lock unless a background pass holds the long one */
BOOL
CUserAssistLog::TryLockShort()
{
    EnterCriticalSection(&m_csShort);
    if (TryEnterCriticalSection(&m_csLong))
    {
        LeaveCriticalSection(&m_csLong);
        return TRUE;
    }

    LeaveCriticalSection(&m_csShort);
    return FALSE;
}

void
CUserAssistLog::LockLong()
{
    EnterCriticalSection(&m_csLong);
    EnterCriticalSection(&m_csShort);
    LeaveCriticalSection(&m_csShort);
}

/* Runs the deletes and renames that came in while the long lock was held */
void
CUserAssistLog::UnlockLong()
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&m_csShort);
    LeaveCriticalSection(&m_csLong);

    if (m_hdpaPending)
    {
        for (int i = 0; i < DPA_GetPtrCount(m_hdpaPending) && SUCCEEDED(hr); i++)
        {
            UA_PENDING_OP *pOp = (UA_PENDING_OP *)DPA_FastGetPtr(m_hdpaPending, i);

            if (pOp->uOp == UA_PENDING_DELETE)
                hr = DeleteEntry(pOp->szName, FALSE);
            else if (pOp->uOp == UA_PENDING_RENAME)
                hr = RenameEntry(pOp->szName, pOp->szNewName);
            else if (pOp->uOp == UA_PENDING_EXCLUDE)
                hr = DeleteEntry(pOp->szName, TRUE);
        }

        DPA_DestroyCallback(m_hdpaPending, UAFreePendingCB, NULL);
        m_hdpaPending = NULL;
    }

    LeaveCriticalSection(&m_csShort);
}

HRESULT
CUserAssistLog::QueuePending(
    _In_ UINT uOp,
    _In_ PCWSTR pszName,
    _In_opt_ PCWSTR pszNewName)
{
    UA_PENDING_OP *pOp;
    HRESULT hr;

    pOp = new UA_PENDING_OP;
    if (!pOp)
        return E_OUTOFMEMORY;

    ZeroMemory(pOp, sizeof(*pOp));
    pOp->uOp = uOp;
    hr = StringCchCopyW(pOp->szName, _countof(pOp->szName), pszName);
    if (SUCCEEDED(hr) && uOp == UA_PENDING_RENAME)
        hr = StringCchCopyW(pOp->szNewName, _countof(pOp->szNewName), pszNewName);

    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&m_csShort);
        if (!m_hdpaPending)
            m_hdpaPending = DPA_Create(1);

        hr = E_OUTOFMEMORY;
        if (m_hdpaPending && DPA_AppendPtr(m_hdpaPending, pOp) != -1)
        {
            pOp = NULL;
            hr = S_OK;
        }
        LeaveCriticalSection(&m_csShort);
    }

    delete pOp;
    return hr;
}

HRESULT
CUserAssistLog::QueueTask(
    _In_ LPTHREAD_START_ROUTINE pfnTask)
{
    AddRef();
    if (!QueueUserWorkItem(pfnTask, this, WT_EXECUTEDEFAULT))
    {
        Release();
        return E_FAIL;
    }

    return S_OK;
}

DWORD
CUserAssistLog::GetCount(
    _In_ PCWSTR pszName,
    _In_ UINT iCounter)
{
    CUserAssistCount count;

    ReadCount(pszName, TRUE, &count);
    if (iCounter == UA_COUNT_LAUNCHES)
        return count.m_Data.cLaunches;
    if (iCounter == UA_COUNT_SWITCHES)
        return count.m_Data.cSwitches;

    return 0;
}

HRESULT
CUserAssistLog::SetCount(
    _In_ PCWSTR pszName,
    _In_ UINT iCounter,
    _In_ DWORD cValue)
{
    CUserAssistCount count;
    HRESULT hr;

    if (!TryLockShort())
        return S_OK;

    hr = ReadCount(pszName, FALSE, &count);
    if (SUCCEEDED(hr))
    {
        if (iCounter == UA_COUNT_SWITCHES)
        {
            m_Session.AddSwitches((LONG)(cValue - count.m_Data.cSwitches));
            count.m_Data.cSwitches = cValue;
        }
        else if (iCounter == UA_COUNT_LAUNCHES)
        {
            m_Session.AddLaunches((LONG)(cValue - count.m_Data.cLaunches));
            count.m_Data.cLaunches = cValue;
        }

        UpdateTop(pszName, &count);
        hr = SaveCount(pszName, &count, TRUE);
        if (SUCCEEDED(hr))
            SaveSession();
    }

    LeaveCriticalSection(&m_csShort);
    return hr;
}

HRESULT
CUserAssistLog::IncCount(
    _In_ PCWSTR pszName,
    _In_ UINT iCounter)
{
    CUserAssistCount count;
    HRESULT hr;

    if (!TryLockShort())
        return S_OK;

    ReadCount(pszName, TRUE, &count);
    if (iCounter == UA_COUNT_LAUNCHES)
    {
        SYSTEMTIME st;

        count.m_Data.cLaunches++;
        m_Session.AddLaunches(1);

        GetSystemTime(&st);
        SystemTimeToFileTime(&st, &count.m_Data.ftLastRun);
    }
    else if (iCounter == UA_COUNT_SWITCHES)
    {
        count.m_Data.cSwitches++;
        m_Session.AddSwitches(1);
    }

    UpdateTop(pszName, &count);
    hr = SaveCount(pszName, &count, TRUE);
    if (SUCCEEDED(hr))
        SaveSession();

    LeaveCriticalSection(&m_csShort);
    return hr;
}

HRESULT
CUserAssistLog::SetUserTime(
    _In_ PCWSTR pszName,
    _In_ DWORD dwUserTime,
    _Out_ BOOL *pfExpired)
{
    CUserAssistCount count;
    HRESULT hr;

    *pfExpired = FALSE;
    if (!TryLockShort())
        return S_OK;

    hr = ReadCount(pszName, FALSE, &count);
    if (SUCCEEDED(hr))
    {
        m_Session.AddUserTime((LONG)(dwUserTime - count.m_Data.dwUserTime));
        count.m_Data.dwUserTime = dwUserTime;

        UpdateTop(pszName, &count);
        hr = SaveCount(pszName, &count, TRUE);
        if (SUCCEEDED(hr))
            SaveSession();
    }

    *pfExpired = CheckSessionExpired();
    LeaveCriticalSection(&m_csShort);
    return hr;
}

HRESULT
CUserAssistLog::AddUserTime(
    _In_ PCWSTR pszName,
    _In_ DWORD dwDelta,
    _Out_ BOOL *pfExpired)
{
    CUserAssistCount count;
    HRESULT hr;

    *pfExpired = FALSE;
    if (!TryLockShort())
        return S_OK;

    ReadCount(pszName, TRUE, &count);
    count.m_Data.dwUserTime += dwDelta;
    m_Session.AddUserTime((LONG)dwDelta);

    UpdateTop(pszName, &count);
    hr = SaveCount(pszName, &count, TRUE);
    if (SUCCEEDED(hr))
        SaveSession();

    *pfExpired = CheckSessionExpired();
    LeaveCriticalSection(&m_csShort);
    return hr;
}

HRESULT
CUserAssistLog::SetLastRun(
    _In_ PCWSTR pszName,
    _In_ const FILETIME *pft)
{
    CUserAssistCount count;
    HRESULT hr;

    if (!TryLockShort())
        return S_OK;

    hr = ReadCount(pszName, FALSE, &count);
    if (SUCCEEDED(hr))
    {
        count.m_Data.ftLastRun = *pft;
        hr = SaveCount(pszName, &count, TRUE);
        if (SUCCEEDED(hr))
            SaveSession();
    }

    LeaveCriticalSection(&m_csShort);
    return hr;
}

/* Only an unused entry keeps the flag, one with any usage is dropped instead */
HRESULT
CUserAssistLog::SetExcluded(
    _In_ PCWSTR pszName,
    _In_ BOOL fExcluded)
{
    BOOL fDrop = FALSE;
    HRESULT hr;

    if (TryLockShort())
    {
        CUserAssistCount count;

        hr = ReadCount(pszName, FALSE, &count);
        if (SUCCEEDED(hr))
        {
            if (count.m_Data.cLaunches || count.m_Data.cSwitches || count.m_Data.dwUserTime)
                fDrop = TRUE;

            count.m_Data.fExcluded = fExcluded;
            hr = SaveCount(pszName, &count, TRUE);
            if (SUCCEEDED(hr))
                SaveSession();
        }

        LeaveCriticalSection(&m_csShort);
    }
    else
    {
        hr = QueuePending(fExcluded ? UA_PENDING_EXCLUDE : UA_PENDING_DELETE, pszName, NULL);
    }

    if (fDrop)
        hr = DeleteEntry(pszName, FALSE);

    return hr;
}

/* fExclude keeps the record but marks it, otherwise the value goes away */
HRESULT
CUserAssistLog::DeleteEntry(
    _In_ PCWSTR pszName,
    _In_ BOOL fExclude)
{
    CUserAssistCount count;
    HRESULT hr;

    if (!TryLockShort())
        return QueuePending(fExclude ? UA_PENDING_EXCLUDE : UA_PENDING_DELETE, pszName, NULL);

    hr = ReadCount(pszName, fExclude, &count);
    if (SUCCEEDED(hr) && (fExclude || hr == S_OK))
    {
        UA_TOP_ENTRY entry;

        if (fExclude)
            count.m_Data.fExcluded = TRUE;
        else
            count.m_dwFlags |= UACF_DELETE;

        entry.cLaunches = count.m_Data.cLaunches;
        entry.cSwitches = count.m_Data.cSwitches;
        entry.dwUserTime = count.m_Data.dwUserTime;
        hr = StringCchCopyW(entry.szName, _countof(entry.szName), pszName);
        if (SUCCEEDED(hr))
        {
            m_Session.RemoveValues(&entry);
            hr = SaveCount(pszName, &count, TRUE);
            if (SUCCEEDED(hr))
                SaveSession();
        }
    }

    LeaveCriticalSection(&m_csShort);
    return hr;
}

HRESULT
CUserAssistLog::RenameEntry(
    _In_ PCWSTR pszFrom,
    _In_ PCWSTR pszTo)
{
    CUserAssistCount count;
    HRESULT hr;

    if (!TryLockShort())
        return QueuePending(UA_PENDING_RENAME, pszFrom, pszTo);

    if (ReadCount(pszFrom, FALSE, &count) != S_OK)
    {
        hr = E_FAIL;
    }
    else
    {
        hr = SaveCount(pszTo, &count, TRUE);
        if (SUCCEEDED(hr))
        {
            count.m_dwFlags |= UACF_DELETE;
            hr = SaveCount(pszFrom, &count, TRUE);
            if (SUCCEEDED(hr))
            {
                hr = RenameTop(pszFrom, pszTo);
                if (SUCCEEDED(hr))
                    SaveSession();
            }
        }
    }

    LeaveCriticalSection(&m_csShort);
    return hr;
}

/* A session ends once enough focus time went by; its relevance then gets saved away */
BOOL
CUserAssistLog::CheckSessionExpired()
{
    LoadSession();
    if (m_Session.m_Data.dwUserTime >= g_dwUASessionTime)
    {
        ScheduleSnapshot();
        return TRUE;
    }

    return FALSE;
}

void
CUserAssistLog::GarbageCollect(
    _Out_opt_ BOOL *pfExpired)
{
    if (pfExpired)
        *pfExpired = CheckSessionExpired();

    /* Unused entries are cleaned up every few sessions only */
    if (g_dwUACleanupTime)
    {
        LoadSession();
        if (m_Session.GetSessionId() % g_dwUACleanupTime)
            return;
    }

    QueueTask(s_CollectTask);
}

void
CUserAssistLog::ScheduleSnapshot()
{
    if (InterlockedCompareExchange(&m_fSnapshotQueued, TRUE, FALSE))
        return;

    if (FAILED(QueueTask(s_SnapshotTask)))
        InterlockedExchange(&m_fSnapshotQueued, FALSE);
}

static
int
CALLBACK
UACompareDeadEntries(
    _In_ PVOID p1,
    _In_ PVOID p2,
    _In_ LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    return CompareFileTime(&((UA_DEAD_ENTRY *)p1)->ftLastRun, &((UA_DEAD_ENTRY *)p2)->ftLastRun);
}

static
int
CALLBACK
UAFreeDeadEntryCB(
    _In_ PVOID p,
    _In_opt_ PVOID pData)
{
    UNREFERENCED_PARAMETER(pData);
    delete (UA_DEAD_ENTRY *)p;
    return TRUE;
}

/* Drops the oldest entries without relevance, keeping at least as many as the MFU list shows */
HRESULT
CUserAssistLog::CollectSlow()
{
    DWORD cKeep = 0;
    DWORD cbKeep = sizeof(cKeep);
    HDPA hdpaNames = NULL;
    HRESULT hr;

    LockLong();
    LoadSession();

    SHGetValueW(HKEY_CURRENT_USER,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"Start_MinMFU",
                NULL,
                &cKeep,
                &cbKeep);
    if (cKeep <= UA_MIN_DEAD_ENTRIES)
        cKeep = UA_MIN_DEAD_ENTRIES;

    hr = EnumEntries(&hdpaNames);
    if (SUCCEEDED(hr))
    {
        HDPA hdpaDead = DPA_Create(cKeep);

        if (hdpaDead)
        {
            for (int i = 0; i < DPA_GetPtrCount(hdpaNames) && SUCCEEDED(hr); i++)
            {
                PWSTR pszName = (PWSTR)DPA_FastGetPtr(hdpaNames, i);
                CUserAssistCount count;

                hr = ReadValue(pszName, &count.m_Data, sizeof(count.m_Data));
                if (FAILED(hr))
                    break;

                count.m_dwFlags |= UACF_LOADED;
                count.Attach(&m_Session);
                if (count.GetRelevance() == 0.0f)
                {
                    UA_DEAD_ENTRY *pDead = new UA_DEAD_ENTRY;

                    hr = E_OUTOFMEMORY;
                    if (pDead)
                    {
                        pDead->pszName = pszName;
                        pDead->ftLastRun = count.m_Data.ftLastRun;
                        if (DPA_AppendPtr(hdpaDead, pDead) != -1)
                            hr = S_OK;
                        else
                            delete pDead;
                    }
                }
            }

            if (SUCCEEDED(hr) &&
                (DWORD)DPA_GetPtrCount(hdpaDead) > cKeep &&
                DPA_Sort(hdpaDead, UACompareDeadEntries, 0))
            {
                DWORD cDrop = DPA_GetPtrCount(hdpaDead) - cKeep;

                for (DWORD i = 0; i < cDrop; i++)
                {
                    UA_DEAD_ENTRY *pDead = (UA_DEAD_ENTRY *)DPA_FastGetPtr(hdpaDead, i);

                    DeleteEntry(pDead->pszName, FALSE);
                }
            }

            DPA_DestroyCallback(hdpaDead, UAFreeDeadEntryCB, NULL);
        }
    }

    UAFreeNames(hdpaNames);
    UnlockLong();
    return S_OK;
}

/* Works the top slots out again from every entry */
HRESULT
CUserAssistLog::RebuildSessionSlow()
{
    UA_TOP_ENTRY empty;
    HDPA hdpaNames = NULL;
    HRESULT hr;

    LockLong();
    LoadSession();

    ZeroMemory(&empty, sizeof(empty));
    for (UINT i = 0; i < UA_TOP_COUNT; i++)
        m_Session.SetTop(i, &empty);

    hr = EnumEntries(&hdpaNames);
    if (SUCCEEDED(hr))
    {
        for (int i = 0; i < DPA_GetPtrCount(hdpaNames) && SUCCEEDED(hr); i++)
        {
            PWSTR pszName = (PWSTR)DPA_FastGetPtr(hdpaNames, i);
            CUserAssistCount count;

            hr = ReadValue(pszName, &count.m_Data, sizeof(count.m_Data));
            if (SUCCEEDED(hr))
            {
                count.m_dwFlags |= UACF_LOADED;
                count.Attach(&m_Session);
                hr = UpdateTop(pszName, &count);
            }
        }

        if (SUCCEEDED(hr))
            SaveSession();
    }

    UAFreeNames(hdpaNames);
    UnlockLong();
    return hr;
}

/* Moves every entry's relevance into its history and starts the next session */
HRESULT
CUserAssistLog::SnapshotSlow()
{
    HDPA hdpaNames = NULL;
    HRESULT hr;

    LockLong();
    LoadSession();

    hr = EnumEntries(&hdpaNames);
    if (SUCCEEDED(hr))
    {
        for (int i = 0; i < DPA_GetPtrCount(hdpaNames) && SUCCEEDED(hr); i++)
        {
            PWSTR pszName = (PWSTR)DPA_FastGetPtr(hdpaNames, i);
            CUserAssistCount count;

            hr = ReadValue(pszName, &count.m_Data, sizeof(count.m_Data));
            if (FAILED(hr))
                break;

            count.m_dwFlags |= UACF_LOADED;
            count.Attach(&m_Session);

            DWORD dwNext = m_Session.GetSessionId() + 1;
            if ((DWORD)count.m_Data.lSession < dwNext || !dwNext)
            {
                LONG iSlot = (count.m_Data.iHistory + 1) % UA_HISTORY_COUNT;

                count.m_Data.rgflHistory[iSlot] = count.GetSessionRelevance();
                count.m_Data.cLaunches = 0;
                count.m_Data.cSwitches = 0;
                count.m_Data.dwUserTime = 0;
                count.m_Data.fExcluded = FALSE;
                count.m_dwFlags |= UACF_DIRTY;
                count.m_Data.iHistory = iSlot;
                count.m_Data.lSession = (LONG)dwNext;
            }

            hr = SaveCount(pszName, &count, FALSE);
        }

        if (SUCCEEDED(hr))
        {
            m_Session.StartNew();
            SaveSession();
        }
    }

    UAFreeNames(hdpaNames);
    UnlockLong();
    return hr;
}

DWORD
WINAPI
CUserAssistLog::s_CollectTask(
    _In_ PVOID pvLog)
{
    CUserAssistLog *pLog = (CUserAssistLog *)pvLog;

    pLog->CollectSlow();
    pLog->Release();
    return 0;
}

DWORD
WINAPI
CUserAssistLog::s_RebuildTask(
    _In_ PVOID pvLog)
{
    CUserAssistLog *pLog = (CUserAssistLog *)pvLog;

    pLog->RebuildSessionSlow();
    pLog->Release();
    return 0;
}

DWORD
WINAPI
CUserAssistLog::s_SnapshotTask(
    _In_ PVOID pvLog)
{
    CUserAssistLog *pLog = (CUserAssistLog *)pvLog;

    pLog->SnapshotSlow();
    InterlockedExchange(&pLog->m_fSnapshotQueued, FALSE);
    pLog->Release();
    return 0;
}
