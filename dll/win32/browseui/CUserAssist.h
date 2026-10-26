/*
 * PROJECT:     ReactOS browseui
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IUserAssist implementation
 * COPYRIGHT:   Copyright 2020 Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 */
// See https://www.geoffchappell.com/studies/windows/ie/browseui/classes/userassist.htm

#pragma once

#include "UserAssistLog.h"

/* Events reported through IShellUserAssist::FireEvent */
typedef enum _UA_EVENT
{
    UAEVENT_LAUNCH = 0,
    UAEVENT_SWITCH = 1,
    UAEVENT_TIME = 2,
    UAEVENT_SESSION = 3,
    UAEVENT_EXCLUDEAPP = 4
} UA_EVENT;

/* Usage of one entry as IShellUserAssist reads and writes it */
typedef struct _UA_ENTRY_INFO
{
    UINT cbSize;
    DWORD dwMask;
    FLOAT flRelevance;
    UINT cLaunches;
    UINT cSwitches;
    DWORD dwUserTime;
    FILETIME ftLastRun;
    BOOL fExcluded;
} UA_ENTRY_INFO;

/* UA_ENTRY_INFO::dwMask */
#define UAEI_LASTRUN        0x01
#define UAEI_LAUNCHES       0x02
#define UAEI_SWITCHES       0x04
#define UAEI_USERTIME       0x08
#define UAEI_RELEVANCE      0x10
#define UAEI_EXCLUDED       0x20
#define UAEI_SETTABLE       (UAEI_LASTRUN | UAEI_LAUNCHES | UAEI_SWITCHES | UAEI_USERTIME | UAEI_EXCLUDED)

typedef BOOL (CALLBACK *PFNUANOTIFY)(
    _In_opt_ void *pvContext,
    _In_ REFGUID rguidGroup,
    _In_ UA_EVENT eEvent);

extern const IID IID_IShellUserAssist;

struct DECLSPEC_NOVTABLE
IShellUserAssist : public IUnknown
{
    STDMETHOD(FireEvent)(
        _In_ REFGUID rguidGroup,
        _In_ UA_EVENT eEvent,
        _In_ PCWSTR pszEntry,
        _In_ UINT uParam) PURE;
    STDMETHOD(QueryEntry)(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszEntry,
        _Inout_ UA_ENTRY_INFO *pInfo) PURE;
    STDMETHOD(SetEntry)(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszEntry,
        _Inout_ UA_ENTRY_INFO *pInfo) PURE;
    STDMETHOD(RenameEntry)(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszFrom,
        _In_ PCWSTR pszTo) PURE;
    STDMETHOD(DeleteEntry)(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszEntry) PURE;
    STDMETHOD(Enable)(
        _In_ BOOL fEnable) PURE;
    STDMETHOD(RegisterNotify)(
        _In_ PFNUANOTIFY pfnNotify,
        _In_opt_ void *pvContext,
        _In_ BOOL fRegister) PURE;
};

class CUserAssist :
    public CComCoClass<CUserAssist, &CLSID_UserAssist>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IUserAssist,
    public IShellUserAssist
{
private:
    HANDLE m_hMutex;
    HDSA m_hdsaNotify;

    BOOL AcquireStore();

public:
    CUserAssist();
    ~CUserAssist();

    HRESULT FinalConstruct();

    // *** IUserAssist methods ***
    STDMETHODIMP FireEvent(GUID const *guid, INT param1, ULONG param2, WPARAM wparam, LPARAM lparam) override;
    // FIXME: PVOID should point to undocumented UEMINFO structure.
    STDMETHODIMP QueryEvent(GUID const *guid, INT param, WPARAM wparam, LPARAM lparam, PVOID ptr) override;
    STDMETHODIMP SetEvent(GUID const *guid, INT param, WPARAM wparam, LPARAM lparam, PVOID ptr) override;
    STDMETHODIMP Enable(BOOL bEnable) override;

    // *** IShellUserAssist methods ***
    STDMETHODIMP FireEvent(
        _In_ REFGUID rguidGroup,
        _In_ UA_EVENT eEvent,
        _In_ PCWSTR pszEntry,
        _In_ UINT uParam) override;
    STDMETHODIMP QueryEntry(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszEntry,
        _Inout_ UA_ENTRY_INFO *pInfo) override;
    STDMETHODIMP SetEntry(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszEntry,
        _Inout_ UA_ENTRY_INFO *pInfo) override;
    STDMETHODIMP RenameEntry(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszFrom,
        _In_ PCWSTR pszTo) override;
    STDMETHODIMP DeleteEntry(
        _In_ REFGUID rguidGroup,
        _In_ PCWSTR pszEntry) override;
    STDMETHODIMP RegisterNotify(
        _In_ PFNUANOTIFY pfnNotify,
        _In_opt_ void *pvContext,
        _In_ BOOL fRegister) override;

public:

    DECLARE_REGISTRY_RESOURCEID(IDR_USERASSIST)
    DECLARE_NOT_AGGREGATABLE(CUserAssist)
    DECLARE_CLASSFACTORY_SINGLETON(CUserAssist)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CUserAssist)
        COM_INTERFACE_ENTRY_IID(IID_IShellUserAssist, IShellUserAssist)
        COM_INTERFACE_ENTRY_IID(IID_IUserAssist, IUserAssist)
    END_COM_MAP()
};
