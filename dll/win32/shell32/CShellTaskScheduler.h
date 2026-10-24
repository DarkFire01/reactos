/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Background and shared task schedulers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifndef ITSSFLAG_KILL_ON_DESTROY
#define ITSSFLAG_COMPLETE_ON_DESTROY    0x0000
#define ITSSFLAG_KILL_ON_DESTROY        0x0001
#endif
#ifndef ITSS_THREAD_TIMEOUT_NO_CHANGE
#define ITSS_THREAD_TIMEOUT_NO_CHANGE   (INFINITE - 1)
#endif
#ifndef ITSAT_DEFAULT_LPARAM
#define ITSAT_DEFAULT_LPARAM            ((DWORD_PTR)-1)
#endif
#ifndef ITSAT_MAX_PRIORITY
#define ITSAT_MAX_PRIORITY              0x7FFFFFFF
#endif

typedef enum _SCHEDULER_FLAGS
{
    SCHEDULER_DEDICATED_THREADS = 0x1,
    SCHEDULER_NO_LIBRARY_REFS = 0x2
} SCHEDULER_FLAGS;

/* AddTask2 and MoveTask placement flags */
#define TASK_PLACE_AHEAD    0x1
#define TASK_PLACE_BEHIND   0x2
#define TASK_PLACE_MASK     0x3
#define TASK_PURGE_MATCHING 0x4

struct DECLSPEC_UUID("3E24A11C-15B2-4F71-B81E-008F77998E9F") DECLSPEC_NOVTABLE
IShellTaskScheduler2 : public IShellTaskScheduler
{
    STDMETHOD(AddTask2)(
        _In_ IRunnableTask *pTask,
        _In_ REFGUID rtoid,
        _In_ DWORD_PTR lParam,
        _In_ DWORD dwPriority,
        _In_ DWORD dwFlags) PURE;
    STDMETHOD(MoveTask)(
        _In_ REFGUID rtoid,
        _In_ DWORD_PTR lParam,
        _In_ DWORD dwPriority,
        _In_ DWORD dwFlags) PURE;
};

struct DECLSPEC_UUID("4BC6CE0A-2B39-4F63-89C1-3BEAA7BDE02A") DECLSPEC_NOVTABLE
IShellTaskSchedulerSettings : public IUnknown
{
    STDMETHOD(SetWorkerThreadCountMax)(
        _In_ LONG cMaxWorkers) PURE;
    STDMETHOD(SetWorkerThreadPriority)(
        _In_ INT nPriority) PURE;
    STDMETHOD(SetFlags)(
        _In_ DWORD dwMask,
        _In_ DWORD dwFlags) PURE;
    STDMETHOD(GetFlags)(
        _Out_ DWORD *pdwFlags) PURE;
};

class CScheduledTask;
struct TASK_WORKER;

class CShellTaskScheduler :
    public CComCoClass<CShellTaskScheduler, &CLSID_ShellTaskScheduler>,
    public IShellTaskScheduler2,
    public IShellTaskSchedulerSettings
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_opt_ PVOID pUnkOuter,
        _In_ REFIID riid,
        _Outptr_ PVOID *ppv);

    typedef CShellTaskScheduler _CreatorClass;

    static void WINAPI ObjectMain(bool) {}
    static const struct _ATL_CATMAP_ENTRY *GetCategoryMap() { return NULL; }

    DECLARE_REGISTRY_RESOURCEID(IDR_SHELLTASKSCHEDULER)

    // *** IUnknown methods ***
    STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // *** IShellTaskScheduler methods ***
    STDMETHODIMP AddTask(IRunnableTask *pTask, REFGUID rtoid, DWORD_PTR lParam, DWORD dwPriority) override;
    STDMETHODIMP RemoveTasks(REFGUID rtoid, DWORD_PTR lParam, BOOL fWaitIfRunning) override;
    STDMETHODIMP_(UINT) CountTasks(REFGUID rtoid) override;
    STDMETHODIMP Status(DWORD dwReleaseStatus, DWORD dwThreadTimeout) override;

    // *** IShellTaskScheduler2 methods ***
    STDMETHODIMP AddTask2(IRunnableTask *pTask, REFGUID rtoid, DWORD_PTR lParam, DWORD dwPriority, DWORD dwFlags) override;
    STDMETHODIMP MoveTask(REFGUID rtoid, DWORD_PTR lParam, DWORD dwPriority, DWORD dwFlags) override;

    // *** IShellTaskSchedulerSettings methods ***
    STDMETHODIMP SetWorkerThreadCountMax(LONG cMaxWorkers) override;
    STDMETHODIMP SetWorkerThreadPriority(INT nPriority) override;
    STDMETHODIMP SetFlags(DWORD dwMask, DWORD dwFlags) override;
    STDMETHODIMP GetFlags(DWORD *pdwFlags) override;

private:
    CShellTaskScheduler();
    ~CShellTaskScheduler();

    HRESULT Initialize();
    void ReleaseHold();

    HRESULT QueuePendingLocked(_In_ CScheduledTask *pEntry, _In_ BOOL bAhead);
    HRESULT StartWorkerLocked(_In_ DWORD dwPriority);
    INT MarkRunningForKillLocked(_In_ REFGUID rtoid, _In_ DWORD_PTR lParam);
    void KillMarkedRunning();
    INT DetachPendingLocked(_In_ REFGUID rtoid, _In_ DWORD_PTR lParam, _Out_ HDPA *phdpaDetached);
    void FinishDetached(_In_ INT cDetached, _In_opt_ HDPA hdpaDetached);
    void WaitOnRunning(_In_ REFGUID rtoid, _In_ DWORD_PTR lParam);

    BOOL WaitForWork(_In_ TASK_WORKER *pWorker, _In_ BOOL bFirstPass);
    void RetireWorker(_In_ TASK_WORKER *pWorker);
    void RunWorker(_In_ TASK_WORKER *pWorker);
    void LeaveRunning(_In_ CScheduledTask *pEntry, _In_ BOOL bSuspended);

    static DWORD WINAPI WorkerThreadProc(_In_ PVOID Parameter);

    LONG m_cRef;
    LONG m_cHolds;
    IUnknown *m_pMarshaler;
    BOOL m_bReady;
    HDPA m_hdpaPending;
    HDPA m_hdpaRunning;
    HDPA m_hdpaWorkers;
    HANDLE m_hWorkAvailable;
    HANDLE m_hShutdown;
    LONG m_cMaxWorkers;
    INT m_nWorkerPriority;
    DWORD m_dwFlags;
    DWORD m_dwReleaseStatus;
    DWORD m_dwWorkerTimeout;
    CRITICAL_SECTION m_Lock;
};

class CSharedTaskScheduler :
    public CComCoClass<CSharedTaskScheduler, &CLSID_SharedTaskScheduler>
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_opt_ PVOID pUnkOuter,
        _In_ REFIID riid,
        _Outptr_ PVOID *ppv);

    typedef CSharedTaskScheduler _CreatorClass;

    static void WINAPI ObjectMain(bool) {}
    static const struct _ATL_CATMAP_ENTRY *GetCategoryMap() { return NULL; }

    DECLARE_REGISTRY_RESOURCEID(IDR_SHAREDTASKSCHEDULER)

    static void Shutdown();
};
