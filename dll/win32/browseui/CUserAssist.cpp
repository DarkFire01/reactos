/*
 * PROJECT:     ReactOS browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IUserAssist implementation
 * COPYRIGHT:   Copyright 2020 Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 */
// See http://www.geoffchappell.com/studies/windows/ie/browseui/interfaces/iuserassist.htm

#include "precomp.h"
#include <comctl32_undoc.h>

const IID IID_IShellUserAssist =
    { 0x90d75131, 0x43a6, 0x4664, { 0x9a, 0xf8, 0xdc, 0xce, 0xb8, 0x5a, 0x74, 0x62 } };

/* Registered callbacks, called back to front on every event */
typedef struct _UA_NOTIFY_ENTRY
{
    PFNUANOTIFY pfnNotify;
    void *pvContext;
} UA_NOTIFY_ENTRY;

/* Set while tracking is off by policy or by Enable(FALSE) */
static BOOL g_fNoUA = FALSE;

/* References the store keeps for the lifetime of the process */
static CUserAssistLog *s_rgpGroupLogs[UA_GROUP_COUNT];

CUserAssist::CUserAssist() :
    m_hMutex(NULL),
    m_hdsaNotify(NULL)
{
}

CUserAssist::~CUserAssist()
{
    if (m_hMutex)
        CloseHandle(m_hMutex);

    if (m_hdsaNotify)
        DSA_Destroy(m_hdsaNotify);
}

HRESULT
CUserAssist::FinalConstruct()
{
    HKEY hkey;
    HRESULT hr;

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                      0,
                      KEY_READ,
                      &hkey) == ERROR_SUCCESS)
    {
        DWORD dwValue;
        DWORD cbValue = sizeof(dwValue);

        if (SHGetValueW(hkey, L"UserAssist\\Settings", L"SessionTime", NULL, &dwValue, &cbValue) ==
            ERROR_SUCCESS)
        {
            g_dwUASessionTime = dwValue;
        }

        cbValue = sizeof(dwValue);
        if (SHGetValueW(hkey, L"UserAssist\\Settings", L"CleanupTime", NULL, &dwValue, &cbValue) ==
            ERROR_SUCCESS)
        {
            g_dwUACleanupTime = dwValue;
        }

        if ((LONG)g_dwUASessionTime < 0)
            g_dwUASessionTime = 172800000;

        cbValue = sizeof(dwValue);
        if (SHGetValueW(hkey, L"UserAssist\\Settings", L"TestLogEnabled", NULL, &dwValue, &cbValue) ==
            ERROR_SUCCESS)
        {
            g_fUATestLog = (dwValue != 0);
        }

        RegCloseKey(hkey);
    }

    g_fNoUA = !UAIsProgramTrackingEnabled();

    m_hMutex = CreateMutexW(NULL, FALSE, L"_SHuassist.mtx");
    if (!m_hMutex)
        return E_FAIL;

    hr = CUserAssistLog::GetLogger(UA_GROUP_APPLICATIONS, &s_rgpGroupLogs[UA_GROUP_APPLICATIONS]);
    if (SUCCEEDED(hr))
        hr = CUserAssistLog::GetLogger(UA_GROUP_SHORTCUTS, &s_rgpGroupLogs[UA_GROUP_SHORTCUTS]);

    if (SUCCEEDED(hr))
    {
        if (g_fUATestLog)
        {
            hr = CUserAssistLog::GetLogger(UA_GROUP_TEST, &s_rgpGroupLogs[UA_GROUP_TEST]);
        }
        else
        {
            BOOL fExpired;

            s_rgpGroupLogs[UA_GROUP_APPLICATIONS]->GarbageCollect(&fExpired);
            s_rgpGroupLogs[UA_GROUP_SHORTCUTS]->GarbageCollect(NULL);
            if (fExpired)
                s_rgpGroupLogs[UA_GROUP_SHORTCUTS]->ScheduleSnapshot();
        }
    }

    return hr;
}

/* Another process may be writing the store; then the update is skipped */
BOOL
CUserAssist::AcquireStore()
{
    DWORD dwWait = WaitForSingleObject(m_hMutex, 0);

    return dwWait == WAIT_OBJECT_0 || dwWait == WAIT_ABANDONED;
}

// *** IUserAssist methods ***
STDMETHODIMP CUserAssist::FireEvent(GUID const *guid, INT param1, ULONG param2, WPARAM wparam, LPARAM lparam)
{
    TRACE("(%u, %d, %d, %p, %p)\n", this, guid, param1, param2, wparam, lparam);
    return E_NOTIMPL;
}

STDMETHODIMP CUserAssist::QueryEvent(GUID const *guid, INT param, WPARAM wparam, LPARAM lparam, PVOID ptr)
{
    TRACE("(%u, %d, %p, %p, %p)\n", this, guid, param, wparam, lparam, ptr);
    return E_NOTIMPL;
}

STDMETHODIMP CUserAssist::SetEvent(GUID const *guid, INT param, WPARAM wparam, LPARAM lparam, PVOID ptr)
{
    TRACE("(%u, %d, %p, %p, %p)\n", this, guid, param, wparam, lparam, ptr);
    return E_NOTIMPL;
}

STDMETHODIMP CUserAssist::Enable(BOOL bEnable)
{
    g_fNoUA = !bEnable;
    return S_OK;
}

// *** IShellUserAssist methods ***
STDMETHODIMP
CUserAssist::FireEvent(
    _In_ REFGUID rguidGroup,
    _In_ UA_EVENT eEvent,
    _In_ PCWSTR pszEntry,
    _In_ UINT uParam)
{
    WCHAR szEntry[UA_CCH_ENTRY];
    CUserAssistLog *pLog;

    if (g_fNoUA ||
        !CUserAssistLog::GetLogAndEncodedPath(rguidGroup, pszEntry, szEntry, _countof(szEntry), &pLog) ||
        !AcquireStore())
    {
        return S_OK;
    }

    BOOL fExpired;

    switch (eEvent)
    {
        case UAEVENT_LAUNCH:
            pLog->IncCount(szEntry, UA_COUNT_LAUNCHES);
            if (IsEqualGUID(rguidGroup, UAIID_SHORTCUTS))
                pLog->AddUserTime(szEntry, 1, &fExpired);
            break;

        case UAEVENT_SWITCH:
            pLog->IncCount(szEntry, UA_COUNT_SWITCHES);
            break;

        case UAEVENT_TIME:
            pLog->AddUserTime(szEntry, uParam, &fExpired);
            if (fExpired && IsEqualGUID(rguidGroup, UAIID_APPLICATIONS))
                CUserAssistLog::FromGroup(UAIID_SHORTCUTS)->ScheduleSnapshot();
            break;

        case UAEVENT_SESSION:
            CUserAssistLog::FromGroup(UAIID_APPLICATIONS)->InvalidateSession();
            CUserAssistLog::FromGroup(UAIID_SHORTCUTS)->InvalidateSession();
            break;

        case UAEVENT_EXCLUDEAPP:
            pLog->DeleteEntry(szEntry, TRUE);
            break;
    }

    if (m_hdsaNotify)
    {
        for (int i = DSA_GetItemCount(m_hdsaNotify) - 1; i >= 0; i -= 1)
        {
            UA_NOTIFY_ENTRY *pEntry = (UA_NOTIFY_ENTRY *)DSA_GetItemPtr(m_hdsaNotify, i);

            if (pEntry && pEntry->pfnNotify)
                pEntry->pfnNotify(pEntry->pvContext, rguidGroup, eEvent);
        }
    }

    ReleaseMutex(m_hMutex);
    return S_OK;
}

STDMETHODIMP
CUserAssist::QueryEntry(
    _In_ REFGUID rguidGroup,
    _In_ PCWSTR pszEntry,
    _Inout_ UA_ENTRY_INFO *pInfo)
{
    WCHAR szEntry[UA_CCH_ENTRY];
    CUserAssistLog *pLog;

    if (g_fNoUA ||
        !CUserAssistLog::GetLogAndEncodedPath(rguidGroup, pszEntry, szEntry, _countof(szEntry), &pLog))
    {
        return S_OK;
    }

    if (pInfo->dwMask & UAEI_LAUNCHES)
        pInfo->cLaunches = pLog->GetCount(szEntry, UA_COUNT_LAUNCHES);

    if (pInfo->dwMask & UAEI_SWITCHES)
        pInfo->cSwitches = pLog->GetCount(szEntry, UA_COUNT_SWITCHES);

    if (pInfo->dwMask & UAEI_USERTIME)
    {
        CUserAssistCount count;

        pLog->ReadCount(szEntry, TRUE, &count);
        pInfo->dwUserTime = count.m_Data.dwUserTime;
    }

    if (pInfo->dwMask & UAEI_LASTRUN)
    {
        CUserAssistCount count;

        pLog->ReadCount(szEntry, FALSE, &count);
        pInfo->ftLastRun = count.m_Data.ftLastRun;
    }

    if (pInfo->dwMask & UAEI_RELEVANCE)
    {
        CUserAssistCount count;

        pLog->ReadCount(szEntry, FALSE, &count);
        pInfo->flRelevance = count.GetRelevance();
    }

    if (pInfo->dwMask & UAEI_EXCLUDED)
    {
        CUserAssistCount count;

        pLog->ReadCount(szEntry, TRUE, &count);
        pInfo->fExcluded = count.m_Data.fExcluded;
    }

    return S_OK;
}

STDMETHODIMP
CUserAssist::SetEntry(
    _In_ REFGUID rguidGroup,
    _In_ PCWSTR pszEntry,
    _Inout_ UA_ENTRY_INFO *pInfo)
{
    WCHAR szEntry[UA_CCH_ENTRY];
    CUserAssistLog *pLog;

    if (g_fNoUA ||
        !CUserAssistLog::GetLogAndEncodedPath(rguidGroup, pszEntry, szEntry, _countof(szEntry), &pLog))
    {
        return S_OK;
    }

    pInfo->dwMask &= UAEI_SETTABLE;
    if (!pInfo->dwMask || !AcquireStore())
        return S_OK;

    if (pInfo->dwMask & UAEI_LASTRUN)
        pLog->SetLastRun(szEntry, &pInfo->ftLastRun);

    if (pInfo->dwMask & UAEI_LAUNCHES)
        pLog->SetCount(szEntry, UA_COUNT_LAUNCHES, pInfo->cLaunches);

    if (pInfo->dwMask & UAEI_SWITCHES)
        pLog->SetCount(szEntry, UA_COUNT_SWITCHES, pInfo->cSwitches);

    if (pInfo->dwMask & UAEI_USERTIME)
    {
        BOOL fExpired;

        pLog->SetUserTime(szEntry, pInfo->dwUserTime, &fExpired);
        if (fExpired && IsEqualGUID(rguidGroup, UAIID_APPLICATIONS))
            CUserAssistLog::FromGroup(UAIID_SHORTCUTS)->ScheduleSnapshot();
    }

    if (pInfo->dwMask & UAEI_EXCLUDED)
        pLog->SetExcluded(szEntry, pInfo->fExcluded);

    ReleaseMutex(m_hMutex);
    return S_OK;
}

STDMETHODIMP
CUserAssist::RenameEntry(
    _In_ REFGUID rguidGroup,
    _In_ PCWSTR pszFrom,
    _In_ PCWSTR pszTo)
{
    WCHAR szFrom[UA_CCH_ENTRY];
    WCHAR szTo[UA_CCH_ENTRY];
    CUserAssistLog *pLog;

    if (!g_fNoUA &&
        CUserAssistLog::GetLogAndEncodedPath(rguidGroup, pszFrom, szFrom, _countof(szFrom), &pLog))
    {
        CUserAssistLog::EncodePath(pszTo, szTo, _countof(szTo));
        pLog->RenameEntry(szFrom, szTo);
    }

    return S_OK;
}

STDMETHODIMP
CUserAssist::DeleteEntry(
    _In_ REFGUID rguidGroup,
    _In_ PCWSTR pszEntry)
{
    WCHAR szEntry[UA_CCH_ENTRY];
    CUserAssistLog *pLog;

    if (!g_fNoUA &&
        CUserAssistLog::GetLogAndEncodedPath(rguidGroup, pszEntry, szEntry, _countof(szEntry), &pLog))
    {
        pLog->DeleteEntry(szEntry, FALSE);
    }

    return S_OK;
}

STDMETHODIMP
CUserAssist::RegisterNotify(
    _In_ PFNUANOTIFY pfnNotify,
    _In_opt_ void *pvContext,
    _In_ BOOL fRegister)
{
    HRESULT hr;
    DWORD dwWait;

    dwWait = WaitForSingleObject(m_hMutex, INFINITE);
    if (dwWait == WAIT_OBJECT_0)
        hr = S_OK;
    else if (dwWait == WAIT_ABANDONED)
        hr = S_FALSE;
    else
        return E_FAIL;

    if (!m_hdsaNotify)
    {
        m_hdsaNotify = DSA_Create(sizeof(UA_NOTIFY_ENTRY), 4);
        if (!m_hdsaNotify)
            hr = E_OUTOFMEMORY;
    }

    if (SUCCEEDED(hr))
    {
        if (fRegister)
        {
            UA_NOTIFY_ENTRY entry;

            entry.pfnNotify = pfnNotify;
            entry.pvContext = pvContext;
            if (DSA_AppendItem(m_hdsaNotify, &entry) == -1)
                hr = E_OUTOFMEMORY;
            else
                hr = S_OK;
        }
        else
        {
            hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            for (int i = DSA_GetItemCount(m_hdsaNotify) - 1; i >= 0; i -= 1)
            {
                UA_NOTIFY_ENTRY *pEntry = (UA_NOTIFY_ENTRY *)DSA_GetItemPtr(m_hdsaNotify, i);

                if (pEntry && pEntry->pfnNotify == pfnNotify && pEntry->pvContext == pvContext)
                {
                    pEntry->pfnNotify = NULL;
                    DSA_DeleteItem(m_hdsaNotify, i);
                    hr = S_OK;
                    break;
                }
            }
        }
    }

    ReleaseMutex(m_hMutex);
    return hr;
}
