/*
 * PROJECT:     ReactOS browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Registry backed usage store behind the user assist object
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Store groups, in the order the loggers are created */
#define UA_GROUP_APPLICATIONS   0
#define UA_GROUP_SHORTCUTS      1
#define UA_GROUP_TEST           2
#define UA_GROUP_COUNT          3

#define UA_HISTORY_COUNT        10
#define UA_TOP_COUNT            3

/* Slots of the most used entries kept in the session record */
#define UA_TOP_LAUNCHES         0
#define UA_TOP_SWITCHES         1
#define UA_TOP_OVERALL          2

/* Which counter IncCount, GetCount and SetCount work on */
#define UA_COUNT_LAUNCHES       0
#define UA_COUNT_SWITCHES       1

/* Longest encoded entry name */
#define UA_CCH_ENTRY            2084

/* Per entry value, 72 bytes like the version 5 store */
typedef struct _UA_COUNT_RECORD
{
    LONG lSession;
    DWORD cLaunches;
    DWORD cSwitches;
    DWORD dwUserTime;
    FLOAT rgflHistory[UA_HISTORY_COUNT];
    LONG iHistory;
    FILETIME ftLastRun;
    BOOL fExcluded;
} UA_COUNT_RECORD;

typedef struct _UA_TOP_ENTRY
{
    DWORD cLaunches;
    DWORD cSwitches;
    DWORD dwUserTime;
    WCHAR szName[MAX_PATH];
} UA_TOP_ENTRY;

/* Session value, totals of the current session and its most used entries */
typedef struct _UA_SESSION_RECORD
{
    DWORD dwSession;
    DWORD cLaunches;
    DWORD cSwitches;
    DWORD dwUserTime;
    UA_TOP_ENTRY rgTop[UA_TOP_COUNT];
} UA_SESSION_RECORD;

C_ASSERT(sizeof(UA_COUNT_RECORD) == 72);
C_ASSERT(sizeof(UA_SESSION_RECORD) == 1612);

/* CUserAssistSession::m_dwFlags */
#define UASF_LOADED             0x1
#define UASF_DIRTY              0x2
#define UASF_REBUILD            0x4

/* CUserAssistCount::m_dwFlags */
#define UACF_LOADED             0x1
#define UACF_DIRTY              0x2
#define UACF_DELETE             0x4

extern DWORD g_dwUASessionTime;
extern DWORD g_dwUACleanupTime;
extern BOOL g_fUATestLog;

extern const GUID UAIID_APPLICATIONS;
extern const GUID UAIID_SHORTCUTS;
extern const GUID UAIID_AUTOMATIC;

class CUserAssistSession
{
public:
    UA_SESSION_RECORD m_Data;
    DWORD m_dwFlags;

    CUserAssistSession();

    void StartNew();
    DWORD GetSessionId() const { return m_Data.dwSession; }
    void AddLaunches(
        _In_ LONG cDelta);
    void AddSwitches(
        _In_ LONG cDelta);
    void AddUserTime(
        _In_ LONG dwDelta);
    void RemoveValues(
        _In_ const UA_TOP_ENTRY *pEntry);
    void SetTop(
        _In_ UINT iTop,
        _In_ const UA_TOP_ENTRY *pEntry);
    void GetTop(
        _In_ UINT iTop,
        _Out_ UA_TOP_ENTRY *pEntry) const;
    FLOAT Weigh(
        _In_ DWORD cLaunches,
        _In_ DWORD cSwitches,
        _In_ DWORD dwUserTime) const;
};

class CUserAssistCount
{
public:
    UA_COUNT_RECORD m_Data;
    CUserAssistSession *m_pSession;
    DWORD m_dwFlags;

    CUserAssistCount();

    void Attach(
        _In_ CUserAssistSession *pSession);
    FLOAT GetSessionRelevance() const;
    FLOAT GetRelevance() const;
};

class CUserAssistLog
{
public:
    static HRESULT GetLogger(
        _In_ UINT iGroup,
        _Outptr_ CUserAssistLog **ppLog);
    static CUserAssistLog *FromGroup(
        _In_ REFGUID rguidGroup);
    static BOOL GetLogAndEncodedPath(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszPath,
        _Out_writes_(cchEncoded) PWSTR pszEncoded,
        _In_ UINT cchEncoded,
        _Outptr_result_maybenull_ CUserAssistLog **ppLog);
    static void EncodePath(
        _In_ PCWSTR pszPath,
        _Out_writes_(cchEncoded) PWSTR pszEncoded,
        _In_ UINT cchEncoded);

    ULONG AddRef();
    ULONG Release();

    HRESULT ReadCount(
        _In_ PCWSTR pszName,
        _In_ BOOL fUseDefault,
        _Inout_ CUserAssistCount *pCount);
    DWORD GetCount(
        _In_ PCWSTR pszName,
        _In_ UINT iCounter);
    HRESULT SetCount(
        _In_ PCWSTR pszName,
        _In_ UINT iCounter,
        _In_ DWORD cValue);
    HRESULT IncCount(
        _In_ PCWSTR pszName,
        _In_ UINT iCounter);
    HRESULT SetUserTime(
        _In_ PCWSTR pszName,
        _In_ DWORD dwUserTime,
        _Out_ BOOL *pfExpired);
    HRESULT AddUserTime(
        _In_ PCWSTR pszName,
        _In_ DWORD dwDelta,
        _Out_ BOOL *pfExpired);
    HRESULT SetLastRun(
        _In_ PCWSTR pszName,
        _In_ const FILETIME *pft);
    HRESULT SetExcluded(
        _In_ PCWSTR pszName,
        _In_ BOOL fExcluded);
    HRESULT DeleteEntry(
        _In_ PCWSTR pszName,
        _In_ BOOL fExclude);
    HRESULT RenameEntry(
        _In_ PCWSTR pszFrom,
        _In_ PCWSTR pszTo);

    void InvalidateSession() { m_fReloadSession = TRUE; }
    BOOL CheckSessionExpired();
    void GarbageCollect(
        _Out_opt_ BOOL *pfExpired);
    void ScheduleSnapshot();

private:
    LONG m_cRef;
    HKEY m_hkey;
    BOOL m_fWritable;
    BOOL m_fReloadSession;
    LONG m_fSnapshotQueued;
    CRITICAL_SECTION m_csLong;
    CRITICAL_SECTION m_csShort;
    HDPA m_hdpaPending;
    CUserAssistSession m_Session;

    CUserAssistLog();
    ~CUserAssistLog();

    HRESULT SetRoot(
        _In_ HKEY hkey,
        _In_ BOOL fWritable);
    HRESULT ChangeKey(
        _In_ PCWSTR pszSubKey);

    HRESULT ReadValue(
        _In_ PCWSTR pszName,
        _Out_writes_bytes_(cb) void *pv,
        _In_ DWORD cb);
    HRESULT WriteValue(
        _In_ PCWSTR pszName,
        _In_reads_bytes_(cb) const void *pv,
        _In_ DWORD cb);
    HRESULT DeleteValue(
        _In_ PCWSTR pszName,
        _In_reads_bytes_(cb) const void *pv,
        _In_ DWORD cb);
    HRESULT SaveCount(
        _In_ PCWSTR pszName,
        _Inout_ CUserAssistCount *pCount,
        _In_ BOOL fForce);
    HRESULT EnumEntries(
        _Inout_ HDPA *phdpa);

    void LoadSession();
    HRESULT SaveSession();
    HRESULT UpdateTop(
        _In_ PCWSTR pszName,
        _In_ const CUserAssistCount *pCount);
    HRESULT RenameTop(
        _In_ PCWSTR pszFrom,
        _In_ PCWSTR pszTo);

    BOOL TryLockShort();
    void LockLong();
    void UnlockLong();
    HRESULT QueuePending(
        _In_ UINT uOp,
        _In_ PCWSTR pszName,
        _In_opt_ PCWSTR pszNewName);
    HRESULT QueueTask(
        _In_ LPTHREAD_START_ROUTINE pfnTask);

    HRESULT CollectSlow();
    HRESULT RebuildSessionSlow();
    HRESULT SnapshotSlow();

    static DWORD WINAPI s_CollectTask(
        _In_ PVOID pvLog);
    static DWORD WINAPI s_RebuildTask(
        _In_ PVOID pvLog);
    static DWORD WINAPI s_SnapshotTask(
        _In_ PVOID pvLog);
};

BOOL
UAIsProgramTrackingEnabled(VOID);
