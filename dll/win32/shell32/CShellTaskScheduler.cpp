/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Background and shared task schedulers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#ifndef TPS_LONGEXECTIME
#define TPS_LONGEXECTIME 0x00000008
#endif

/* CTF_NOADDREFLIB, which the headers only expose for Windows 7 targets */
#define THREAD_NO_LIBRARY_REF 0x2000

static const IID IID_IShellTaskScheduler2Private = __uuidof(IShellTaskScheduler2);
static const IID IID_IShellTaskSchedulerSettingsPrivate = __uuidof(IShellTaskSchedulerSettings);

static HRESULT
LastErrorResult(VOID)
{
    DWORD dwError = GetLastError();

    return dwError ? HRESULT_FROM_WIN32(dwError) : E_FAIL;
}

typedef enum _TASK_STATE
{
    TaskPending = 1,
    TaskRunning,
    TaskSuspended,
    TaskFinished
} TASK_STATE;

class CScheduledTask
{
public:
    LONG m_cRef;
    IRunnableTask *m_pTask;
    IRunnableTask *m_pKillTarget;
    GUID m_rtoid;
    DWORD_PTR m_lParam;
    DWORD m_dwPriority;
    TASK_STATE m_State;
    BOOL m_bKillPending;
    BOOL m_bKilled;
    BOOL m_bDropped;
    DWORD m_dwRunThread;
    DWORD m_dwKillThread;
    DWORD m_dwDropThread;
    HANDLE m_hDone;

    CScheduledTask() :
        m_cRef(1),
        m_pTask(NULL),
        m_pKillTarget(NULL),
        m_rtoid(GUID_NULL),
        m_lParam(0),
        m_dwPriority(0),
        m_State(TaskPending),
        m_bKillPending(FALSE),
        m_bKilled(FALSE),
        m_bDropped(FALSE),
        m_dwRunThread(0),
        m_dwKillThread(0),
        m_dwDropThread(0),
        m_hDone(NULL)
    {
    }

    HRESULT
    Initialize(
        _In_ IRunnableTask *pTask,
        _In_ REFGUID rtoid,
        _In_ DWORD_PTR lParam,
        _In_ DWORD dwPriority)
    {
        m_hDone = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!m_hDone)
            return LastErrorResult();

        pTask->AddRef();
        m_pTask = pTask;
        pTask->AddRef();
        m_pKillTarget = pTask;
        m_rtoid = rtoid;
        m_lParam = lParam;
        m_dwPriority = dwPriority;
        return S_OK;
    }

    void AddRef()
    {
        InterlockedIncrement(&m_cRef);
    }

    void Release()
    {
        if (InterlockedDecrement(&m_cRef))
            return;

        if (m_pTask)
            m_pTask->Release();
        if (m_pKillTarget)
            m_pKillTarget->Release();
        if (m_hDone)
            CloseHandle(m_hDone);
        delete this;
    }

    BOOL Matches(
        _In_ REFGUID rtoid,
        _In_ DWORD_PTR lParam) const
    {
        if (!IsEqualGUID(rtoid, GUID_NULL) && !IsEqualGUID(rtoid, m_rtoid))
            return FALSE;
        return (lParam == ITSAT_DEFAULT_LPARAM || lParam == m_lParam);
    }

    void MarkKill()
    {
        m_bKillPending = TRUE;
        m_dwKillThread = GetCurrentThreadId();
    }

    void MarkDropped()
    {
        m_bDropped = TRUE;
        m_dwDropThread = GetCurrentThreadId();
    }

    BOOL IsKillMine() const
    {
        return (m_bKillPending && m_dwKillThread == GetCurrentThreadId());
    }

    void Kill()
    {
        IRunnableTask *pTarget = m_pKillTarget;

        m_pKillTarget = NULL;
        pTarget->Kill(FALSE);
        pTarget->Release();
        m_bKilled = TRUE;
        m_bKillPending = FALSE;
    }

    /* The kill reference stays behind while another thread still owes the task a Kill */
    void Abandon()
    {
        if (!m_bKillPending && m_pKillTarget)
        {
            m_pKillTarget->Release();
            m_pKillTarget = NULL;
        }
        if (m_pTask)
        {
            m_pTask->Release();
            m_pTask = NULL;
        }
        m_State = TaskFinished;
        SetEvent(m_hDone);
    }

    void Retire()
    {
        if (IsKillMine())
            Kill();
        Abandon();
    }
};

struct TASK_WORKER
{
    CShellTaskScheduler *pScheduler;
};

static inline CScheduledTask *
TaskAt(
    _In_ HDPA hdpa,
    _In_ INT Index)
{
    return (CScheduledTask *)DPA_FastGetPtr(hdpa, Index);
}

static BOOL
IsApartmentMessage(
    _In_ const MSG *pMsg)
{
    WCHAR szClass[32];

    if (pMsg->message < WM_USER || pMsg->message > WM_USER + 7)
        return FALSE;
    if (!GetClassNameW(pMsg->hwnd, szClass, _countof(szClass)))
        return FALSE;
    return (wcscmp(szClass, L"OleMainThreadWndClass") == 0);
}

/* Returns the signaled handle index, pumping posted and sent messages while it waits */
static DWORD
WaitPumpingMessages(
    _In_reads_(2) HANDLE *phHandles,
    _In_ DWORD dwTimeout)
{
    DWORD dwStart = GetTickCount();
    DWORD dwLeft = dwTimeout;
    DWORD dwResult;
    MSG msg;

    for (;;)
    {
        dwResult = MsgWaitForMultipleObjects(2, phHandles, FALSE, dwLeft, QS_POSTMESSAGE | QS_SENDMESSAGE);
        if (dwResult != WAIT_OBJECT_0 + 2)
            break;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);

        if (dwTimeout != INFINITE)
        {
            DWORD dwElapsed = GetTickCount() - dwStart;
            dwLeft = (dwElapsed > dwTimeout) ? 0 : dwTimeout - dwElapsed;
        }
    }

    if (dwResult == WAIT_ABANDONED_0 || dwResult == WAIT_ABANDONED_0 + 1)
        dwResult -= WAIT_ABANDONED_0;
    return dwResult;
}

static INT
FindPriorityIndex(
    _In_ HDPA hdpa,
    _In_ DWORD dwPriority,
    _In_ BOOL bAhead)
{
    INT cItems = DPA_GetPtrCount(hdpa);
    INT Index;

    for (Index = 0; Index < cItems; Index++)
    {
        DWORD dwOther = TaskAt(hdpa, Index)->m_dwPriority;

        if (dwOther < dwPriority || (dwOther == dwPriority && bAhead))
            break;
    }
    return Index;
}

static HRESULT
InsertByPriority(
    _In_ HDPA hdpa,
    _In_ CScheduledTask *pEntry,
    _In_ BOOL bAhead)
{
    INT Index = FindPriorityIndex(hdpa, pEntry->m_dwPriority, bAhead);

    if (DPA_InsertPtr(hdpa, Index, pEntry) == -1)
        return E_OUTOFMEMORY;

    pEntry->AddRef();
    return S_OK;
}

static void
RemoveEntry(
    _In_ HDPA hdpa,
    _In_ PVOID pItem)
{
    INT Index = DPA_GetPtrIndex(hdpa, pItem);

    if (Index != -1)
        DPA_DeletePtr(hdpa, Index);
}

CShellTaskScheduler::CShellTaskScheduler() :
    m_cRef(1),
    m_cHolds(1),
    m_pMarshaler(NULL),
    m_bReady(FALSE),
    m_hdpaPending(NULL),
    m_hdpaRunning(NULL),
    m_hdpaWorkers(NULL),
    m_hWorkAvailable(NULL),
    m_hShutdown(NULL),
    m_cMaxWorkers(1),
    m_nWorkerPriority(THREAD_PRIORITY_NORMAL),
    m_dwFlags(0),
    m_dwReleaseStatus(ITSSFLAG_COMPLETE_ON_DESTROY),
    m_dwWorkerTimeout(10 * 1000)
{
}

CShellTaskScheduler::~CShellTaskScheduler()
{
    if (m_hdpaPending)
        DPA_Destroy(m_hdpaPending);
    if (m_hdpaRunning)
        DPA_Destroy(m_hdpaRunning);
    if (m_hdpaWorkers)
        DPA_Destroy(m_hdpaWorkers);
    if (m_hWorkAvailable)
        CloseHandle(m_hWorkAvailable);
    if (m_hShutdown)
        CloseHandle(m_hShutdown);
    if (m_bReady)
        DeleteCriticalSection(&m_Lock);
    if (m_pMarshaler)
        m_pMarshaler->Release();
}

HRESULT
CShellTaskScheduler::Initialize()
{
    m_hdpaPending = DPA_Create(24);
    m_hdpaRunning = DPA_Create(2);
    m_hdpaWorkers = DPA_Create(2);
    if (!m_hdpaPending || !m_hdpaRunning || !m_hdpaWorkers)
        return E_OUTOFMEMORY;

    m_hWorkAvailable = CreateSemaphoreW(NULL, 0, 0xFFFF, NULL);
    if (!m_hWorkAvailable)
        return LastErrorResult();

    m_hShutdown = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!m_hShutdown)
        return E_OUTOFMEMORY;

    InitializeCriticalSection(&m_Lock);
    m_bReady = TRUE;
    return S_OK;
}

HRESULT WINAPI
CShellTaskScheduler::CreateInstance(
    _In_opt_ PVOID pUnkOuter,
    _In_ REFIID riid,
    _Outptr_ PVOID *ppv)
{
    CShellTaskScheduler *pScheduler;
    HRESULT hr;

    *ppv = NULL;
    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    pScheduler = new CShellTaskScheduler();
    if (!pScheduler)
        return E_OUTOFMEMORY;

    hr = pScheduler->Initialize();
    if (SUCCEEDED(hr))
        hr = pScheduler->QueryInterface(riid, ppv);
    pScheduler->Release();
    return hr;
}

void
CShellTaskScheduler::ReleaseHold()
{
    if (!InterlockedDecrement(&m_cHolds))
        delete this;
}

STDMETHODIMP
CShellTaskScheduler::QueryInterface(
    REFIID riid,
    PVOID *ppv)
{
    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IShellTaskScheduler) ||
        IsEqualIID(riid, IID_IShellTaskScheduler2Private))
    {
        *ppv = static_cast<IShellTaskScheduler2 *>(this);
    }
    else if (IsEqualIID(riid, IID_IShellTaskSchedulerSettingsPrivate))
    {
        *ppv = static_cast<IShellTaskSchedulerSettings *>(this);
    }
    else if (IsEqualIID(riid, IID_IMarshal))
    {
        if (!m_pMarshaler)
        {
            IUnknown *pMarshaler;
            HRESULT hr;

            hr = CoCreateFreeThreadedMarshaler(static_cast<IShellTaskScheduler2 *>(this), &pMarshaler);
            if (FAILED(hr))
            {
                *ppv = NULL;
                return hr;
            }
            if (InterlockedCompareExchangePointer((PVOID *)&m_pMarshaler, pMarshaler, NULL))
                pMarshaler->Release();
        }
        return m_pMarshaler->QueryInterface(riid, ppv);
    }
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG)
CShellTaskScheduler::AddRef()
{
    InterlockedIncrement(&m_cHolds);
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CShellTaskScheduler::Release()
{
    ULONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef && m_bReady)
    {
        SetEvent(m_hShutdown);
        if (m_dwReleaseStatus == ITSSFLAG_KILL_ON_DESTROY)
        {
            EnterCriticalSection(&m_Lock);
            MarkRunningForKillLocked(GUID_NULL, ITSAT_DEFAULT_LPARAM);
            LeaveCriticalSection(&m_Lock);
            KillMarkedRunning();
        }
    }

    ReleaseHold();
    return cRef;
}

HRESULT
CShellTaskScheduler::QueuePendingLocked(
    _In_ CScheduledTask *pEntry,
    _In_ BOOL bAhead)
{
    HRESULT hr = InsertByPriority(m_hdpaPending, pEntry, bAhead);

    if (FAILED(hr))
        return hr;

    if (!ReleaseSemaphore(m_hWorkAvailable, 1, NULL))
    {
        hr = LastErrorResult();
        RemoveEntry(m_hdpaPending, pEntry);
        pEntry->Release();
    }
    return hr;
}

HRESULT
CShellTaskScheduler::StartWorkerLocked(
    _In_ DWORD dwPriority)
{
    INT cWorkers = DPA_GetPtrCount(m_hdpaWorkers);
    INT cIdle = cWorkers - DPA_GetPtrCount(m_hdpaRunning);
    TASK_WORKER *pWorker;
    BOOL bStarted;
    HRESULT hr;

    /* The top priority may go past the worker limit so it never waits behind other tasks */
    if (cWorkers >= m_cMaxWorkers && dwPriority != ITSAT_MAX_PRIORITY)
        return S_OK;
    if (cIdle >= DPA_GetPtrCount(m_hdpaPending))
        return S_OK;

    pWorker = new TASK_WORKER;
    if (!pWorker)
        return E_OUTOFMEMORY;

    pWorker->pScheduler = this;
    InterlockedIncrement(&m_cHolds);

    if (DPA_AppendPtr(m_hdpaWorkers, pWorker) == -1)
    {
        hr = E_OUTOFMEMORY;
        goto Cleanup;
    }

    if (m_dwFlags & SCHEDULER_DEDICATED_THREADS)
    {
        DWORD dwThreadFlags = CTF_FREELIBANDEXIT;

        if (m_dwFlags & SCHEDULER_NO_LIBRARY_REFS)
            dwThreadFlags |= THREAD_NO_LIBRARY_REF;
        bStarted = SHCreateThread(WorkerThreadProc, pWorker, dwThreadFlags, NULL);
    }
    else
    {
        bStarted = SHQueueUserWorkItem(WorkerThreadProc, pWorker, 0, 0, NULL, NULL, TPS_LONGEXECTIME);
    }

    if (bStarted)
        return S_OK;

    hr = LastErrorResult();
    DPA_DeletePtr(m_hdpaWorkers, DPA_GetPtrCount(m_hdpaWorkers) - 1);

Cleanup:
    delete pWorker;
    ReleaseHold();
    return hr;
}

INT
CShellTaskScheduler::MarkRunningForKillLocked(
    _In_ REFGUID rtoid,
    _In_ DWORD_PTR lParam)
{
    DWORD dwThread = GetCurrentThreadId();
    INT cMarked = 0;
    INT Index;

    for (Index = DPA_GetPtrCount(m_hdpaRunning) - 1; Index >= 0; Index--)
    {
        CScheduledTask *pEntry = TaskAt(m_hdpaRunning, Index);

        if (!pEntry->Matches(rtoid, lParam))
            continue;
        if (pEntry->m_bKillPending || pEntry->m_bKilled || pEntry->m_dwRunThread == dwThread)
            continue;

        pEntry->MarkKill();
        cMarked++;
    }
    return cMarked;
}

void
CShellTaskScheduler::KillMarkedRunning()
{
    for (;;)
    {
        CScheduledTask *pVictim = NULL;
        INT cRunning;
        INT Index;

        EnterCriticalSection(&m_Lock);
        cRunning = DPA_GetPtrCount(m_hdpaRunning);
        for (Index = 0; Index < cRunning; Index++)
        {
            if (TaskAt(m_hdpaRunning, Index)->IsKillMine())
            {
                pVictim = TaskAt(m_hdpaRunning, Index);
                pVictim->AddRef();
                break;
            }
        }
        LeaveCriticalSection(&m_Lock);

        if (!pVictim)
            break;

        pVictim->Kill();
        pVictim->Release();
    }
}

/* Detached tasks go into a list for FinishDetached; ones that cannot be moved stay marked in place */
INT
CShellTaskScheduler::DetachPendingLocked(
    _In_ REFGUID rtoid,
    _In_ DWORD_PTR lParam,
    _Out_ HDPA *phdpaDetached)
{
    HDPA hdpaDetached = DPA_Create(DPA_GetPtrCount(m_hdpaPending) + 1);
    INT cDetached = 0;
    INT Index;

    for (Index = DPA_GetPtrCount(m_hdpaPending) - 1; Index >= 0; Index--)
    {
        CScheduledTask *pEntry = TaskAt(m_hdpaPending, Index);

        if (pEntry->m_bDropped || !pEntry->Matches(rtoid, lParam))
            continue;

        if (hdpaDetached && DPA_InsertPtr(hdpaDetached, 0, pEntry) != -1)
            DPA_DeletePtr(m_hdpaPending, Index);

        if (pEntry->m_State == TaskSuspended)
            pEntry->MarkKill();
        pEntry->MarkDropped();
        cDetached++;
    }

    *phdpaDetached = hdpaDetached;
    return cDetached;
}

void
CShellTaskScheduler::FinishDetached(
    _In_ INT cDetached,
    _In_opt_ HDPA hdpaDetached)
{
    DWORD dwThread = GetCurrentThreadId();
    INT Index;

    if (hdpaDetached)
    {
        INT cMoved = DPA_GetPtrCount(hdpaDetached);

        for (Index = 0; Index < cMoved; Index++)
        {
            CScheduledTask *pEntry = TaskAt(hdpaDetached, Index);

            pEntry->Retire();
            pEntry->Release();
        }
        DPA_Destroy(hdpaDetached);
        cDetached -= cMoved;
    }

    while (cDetached-- > 0)
    {
        CScheduledTask *pEntry = NULL;

        EnterCriticalSection(&m_Lock);
        for (Index = DPA_GetPtrCount(m_hdpaPending) - 1; Index >= 0; Index--)
        {
            CScheduledTask *pCandidate = TaskAt(m_hdpaPending, Index);

            if (pCandidate->m_bDropped && pCandidate->m_dwDropThread == dwThread)
            {
                pEntry = pCandidate;
                DPA_DeletePtr(m_hdpaPending, Index);
                break;
            }
        }
        LeaveCriticalSection(&m_Lock);

        if (!pEntry)
            break;

        pEntry->Retire();
        pEntry->Release();
    }
}

void
CShellTaskScheduler::WaitOnRunning(
    _In_ REFGUID rtoid,
    _In_ DWORD_PTR lParam)
{
    DWORD dwThread = GetCurrentThreadId();

    for (;;)
    {
        CScheduledTask *pBusy = NULL;
        INT Index;

        EnterCriticalSection(&m_Lock);
        for (Index = DPA_GetPtrCount(m_hdpaRunning) - 1; Index >= 0; Index--)
        {
            CScheduledTask *pEntry = TaskAt(m_hdpaRunning, Index);

            if (pEntry->Matches(rtoid, lParam) && pEntry->m_dwRunThread != dwThread)
            {
                pBusy = pEntry;
                pBusy->AddRef();
                break;
            }
        }
        LeaveCriticalSection(&m_Lock);

        if (!pBusy)
            break;

        SHWaitForSendMessageThread(pBusy->m_hDone, INFINITE);
        pBusy->Release();
    }
}

STDMETHODIMP
CShellTaskScheduler::AddTask(
    IRunnableTask *pTask,
    REFGUID rtoid,
    DWORD_PTR lParam,
    DWORD dwPriority)
{
    return AddTask2(pTask, rtoid, lParam, dwPriority, TASK_PLACE_BEHIND);
}

STDMETHODIMP
CShellTaskScheduler::AddTask2(
    IRunnableTask *pTask,
    REFGUID rtoid,
    DWORD_PTR lParam,
    DWORD dwPriority,
    DWORD dwFlags)
{
    DWORD dwThread = GetCurrentThreadId();
    CScheduledTask *pEntry;
    HRESULT hr;
    INT Index;

    if (!pTask || IsEqualGUID(rtoid, GUID_NULL) || !(dwFlags & TASK_PLACE_MASK))
        return E_INVALIDARG;

    if (dwFlags & TASK_PURGE_MATCHING)
        RemoveTasks(rtoid, lParam, FALSE);

    pEntry = new CScheduledTask();
    if (!pEntry)
        return E_OUTOFMEMORY;

    hr = pEntry->Initialize(pTask, rtoid, lParam, dwPriority);
    if (FAILED(hr))
    {
        pEntry->Release();
        return hr;
    }

    EnterCriticalSection(&m_Lock);

    /* A task that is being killed may not queue more work from its own thread */
    for (Index = DPA_GetPtrCount(m_hdpaRunning) - 1; Index >= 0; Index--)
    {
        CScheduledTask *pCurrent = TaskAt(m_hdpaRunning, Index);

        if (pCurrent->m_dwRunThread != dwThread)
            continue;
        if (pCurrent->m_bKillPending || pCurrent->m_bKilled)
            hr = E_FAIL;
        break;
    }

    if (SUCCEEDED(hr))
        hr = QueuePendingLocked(pEntry, (dwFlags & TASK_PLACE_AHEAD) != 0);

    if (SUCCEEDED(hr))
    {
        hr = StartWorkerLocked(dwPriority);
        if (FAILED(hr))
        {
            RemoveEntry(m_hdpaPending, pEntry);
            pEntry->Release();
            pEntry->MarkDropped();
        }
        else
        {
            /* Make room by suspending the least important task that runs below the new one */
            for (Index = DPA_GetPtrCount(m_hdpaRunning) - 1; Index >= 0; Index--)
            {
                CScheduledTask *pLower = TaskAt(m_hdpaRunning, Index);

                if (pLower->m_dwPriority >= dwPriority || pLower->m_bKillPending || pLower->m_bKilled)
                    break;
                if (SUCCEEDED(pLower->m_pTask->Suspend()))
                    break;
            }
        }
    }

    LeaveCriticalSection(&m_Lock);

    if (pEntry->m_bDropped && pEntry->m_dwDropThread == dwThread)
        pEntry->Abandon();
    pEntry->Release();
    return hr;
}

STDMETHODIMP
CShellTaskScheduler::RemoveTasks(
    REFGUID rtoid,
    DWORD_PTR lParam,
    BOOL fWaitIfRunning)
{
    HDPA hdpaDetached;
    INT cMarked = 0;
    INT cDetached;

    EnterCriticalSection(&m_Lock);
    if (m_dwReleaseStatus == ITSSFLAG_KILL_ON_DESTROY)
        cMarked = MarkRunningForKillLocked(rtoid, lParam);
    cDetached = DetachPendingLocked(rtoid, lParam, &hdpaDetached);
    LeaveCriticalSection(&m_Lock);

    if (cMarked > 0)
        KillMarkedRunning();
    FinishDetached(cDetached, hdpaDetached);

    if (fWaitIfRunning)
        WaitOnRunning(rtoid, lParam);
    return S_OK;
}

STDMETHODIMP_(UINT)
CShellTaskScheduler::CountTasks(
    REFGUID rtoid)
{
    HDPA hdpaLists[] = { m_hdpaPending, m_hdpaRunning };
    UINT cTasks = 0;
    SIZE_T List;

    EnterCriticalSection(&m_Lock);
    for (List = 0; List < _countof(hdpaLists); List++)
    {
        INT cItems = DPA_GetPtrCount(hdpaLists[List]);
        INT Index;

        if (IsEqualGUID(rtoid, GUID_NULL))
        {
            cTasks += cItems;
            continue;
        }

        for (Index = 0; Index < cItems; Index++)
        {
            if (IsEqualGUID(TaskAt(hdpaLists[List], Index)->m_rtoid, rtoid))
                cTasks++;
        }
    }
    LeaveCriticalSection(&m_Lock);
    return cTasks;
}

STDMETHODIMP
CShellTaskScheduler::Status(
    DWORD dwReleaseStatus,
    DWORD dwThreadTimeout)
{
    m_dwReleaseStatus = dwReleaseStatus & 3;
    if (dwThreadTimeout != ITSS_THREAD_TIMEOUT_NO_CHANGE)
        m_dwWorkerTimeout = dwThreadTimeout;
    return S_OK;
}

/* Matching pending tasks take the new priority and sit together where that priority belongs */
STDMETHODIMP
CShellTaskScheduler::MoveTask(
    REFGUID rtoid,
    DWORD_PTR lParam,
    DWORD dwPriority,
    DWORD dwFlags)
{
    HDPA hdpaMoved;
    HRESULT hr = S_OK;
    INT cMoved;
    INT Target;
    INT Index;

    EnterCriticalSection(&m_Lock);

    hdpaMoved = DPA_Create(4);
    if (!hdpaMoved)
    {
        LeaveCriticalSection(&m_Lock);
        return E_OUTOFMEMORY;
    }

    for (Index = 0; Index < DPA_GetPtrCount(m_hdpaPending);)
    {
        CScheduledTask *pEntry = TaskAt(m_hdpaPending, Index);

        if (pEntry->Matches(rtoid, lParam) && DPA_AppendPtr(hdpaMoved, pEntry) != -1)
        {
            DPA_DeletePtr(m_hdpaPending, Index);
            continue;
        }
        Index++;
    }

    cMoved = DPA_GetPtrCount(hdpaMoved);
    Target = FindPriorityIndex(m_hdpaPending, dwPriority, (dwFlags & TASK_PLACE_AHEAD) != 0);
    for (Index = 0; Index < cMoved; Index++)
    {
        CScheduledTask *pEntry = TaskAt(hdpaMoved, Index);

        pEntry->m_dwPriority = dwPriority;
        if (DPA_InsertPtr(m_hdpaPending, Target, pEntry) == -1)
        {
            hr = E_OUTOFMEMORY;
            DPA_AppendPtr(m_hdpaPending, pEntry);
        }
        else
        {
            Target++;
        }
    }

    LeaveCriticalSection(&m_Lock);
    DPA_Destroy(hdpaMoved);
    return hr;
}

STDMETHODIMP
CShellTaskScheduler::SetWorkerThreadCountMax(
    LONG cMaxWorkers)
{
    if (cMaxWorkers <= 0)
        return E_INVALIDARG;

    m_cMaxWorkers = cMaxWorkers;
    return S_OK;
}

STDMETHODIMP
CShellTaskScheduler::SetWorkerThreadPriority(
    INT nPriority)
{
    if (nPriority != THREAD_PRIORITY_IDLE &&
        nPriority != THREAD_PRIORITY_TIME_CRITICAL &&
        (nPriority < THREAD_PRIORITY_LOWEST || nPriority > THREAD_PRIORITY_HIGHEST))
    {
        return E_INVALIDARG;
    }

    m_nWorkerPriority = nPriority;
    return S_OK;
}

STDMETHODIMP
CShellTaskScheduler::SetFlags(
    DWORD dwMask,
    DWORD dwFlags)
{
    m_dwFlags = (m_dwFlags & ~dwMask) | (dwFlags & dwMask);
    return S_OK;
}

STDMETHODIMP
CShellTaskScheduler::GetFlags(
    DWORD *pdwFlags)
{
    *pdwFlags = m_dwFlags;
    return S_OK;
}

void
CShellTaskScheduler::RetireWorker(
    _In_ TASK_WORKER *pWorker)
{
    HDPA hdpaDetached = NULL;
    INT cDetached = 0;

    EnterCriticalSection(&m_Lock);
    RemoveEntry(m_hdpaWorkers, pWorker);
    if (!DPA_GetPtrCount(m_hdpaWorkers))
        cDetached = DetachPendingLocked(GUID_NULL, ITSAT_DEFAULT_LPARAM, &hdpaDetached);
    LeaveCriticalSection(&m_Lock);

    FinishDetached(cDetached, hdpaDetached);
}

/* TRUE means the worker owns one unit of pending work; FALSE means it has left the worker list */
BOOL
CShellTaskScheduler::WaitForWork(
    _In_ TASK_WORKER *pWorker,
    _In_ BOOL bFirstPass)
{
    HANDLE hWait[2] = { m_hShutdown, m_hWorkAvailable };
    DWORD dwResult;

    if (!bFirstPass)
    {
        BOOL bSurplus;

        EnterCriticalSection(&m_Lock);
        bSurplus = (DPA_GetPtrCount(m_hdpaWorkers) > m_cMaxWorkers);
        if (bSurplus)
            RemoveEntry(m_hdpaWorkers, pWorker);
        LeaveCriticalSection(&m_Lock);

        if (bSurplus)
            return FALSE;
    }

    dwResult = WaitPumpingMessages(hWait, m_dwWorkerTimeout);
    if (dwResult == WAIT_OBJECT_0 + 1)
        return TRUE;

    if (dwResult == WAIT_TIMEOUT)
    {
        /* Recheck under the lock so a task queued right at the timeout still finds this worker */
        EnterCriticalSection(&m_Lock);
        dwResult = WaitForMultipleObjects(2, hWait, FALSE, 0);
        if (dwResult == WAIT_TIMEOUT)
            RemoveEntry(m_hdpaWorkers, pWorker);
        LeaveCriticalSection(&m_Lock);

        if (dwResult == WAIT_OBJECT_0 + 1)
            return TRUE;
        if (dwResult == WAIT_TIMEOUT)
            return FALSE;
    }

    RetireWorker(pWorker);
    return FALSE;
}

void
CShellTaskScheduler::LeaveRunning(
    _In_ CScheduledTask *pEntry,
    _In_ BOOL bSuspended)
{
    CScheduledTask *pFinished = pEntry;
    BOOL bClean = TRUE;
    MSG msg;

    /* Only COM's own apartment traffic survives; anything else means the thread cannot host a suspended task */
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (!IsWindow(msg.hwnd))
            continue;
        if (IsApartmentMessage(&msg))
            DispatchMessageW(&msg);
        else
            bClean = FALSE;
    }

    EnterCriticalSection(&m_Lock);
    RemoveEntry(m_hdpaRunning, pEntry);
    pEntry->Release();

    if (bSuspended && !pEntry->m_bKillPending && !pEntry->m_bKilled)
    {
        pEntry->m_State = TaskSuspended;
        if (bClean && SUCCEEDED(QueuePendingLocked(pEntry, TRUE)))
            pFinished = NULL;
        else
            pEntry->MarkKill();
    }

    if (pFinished)
        pFinished->MarkDropped();
    LeaveCriticalSection(&m_Lock);

    if (pFinished)
        pFinished->Retire();
}

void
CShellTaskScheduler::RunWorker(
    _In_ TASK_WORKER *pWorker)
{
    HANDLE hThread = GetCurrentThread();
    INT nOriginal = GetThreadPriority(hThread);
    BOOL bFirstPass = TRUE;
    MSG msg;

    if (nOriginal != THREAD_PRIORITY_ERROR_RETURN && nOriginal != m_nWorkerPriority)
        SetThreadPriority(hThread, m_nWorkerPriority);

    while (WaitForWork(pWorker, bFirstPass))
    {
        CScheduledTask *pTaken = NULL;
        CScheduledTask *pRun = NULL;
        BOOL bResume = FALSE;
        INT cPending;
        INT Index;

        bFirstPass = FALSE;

        EnterCriticalSection(&m_Lock);
        cPending = DPA_GetPtrCount(m_hdpaPending);
        for (Index = 0; Index < cPending; Index++)
        {
            if (!TaskAt(m_hdpaPending, Index)->m_bDropped)
            {
                pTaken = (CScheduledTask *)DPA_DeletePtr(m_hdpaPending, Index);
                break;
            }
        }

        if (pTaken)
        {
            bResume = (pTaken->m_State == TaskSuspended);
            pTaken->m_State = TaskRunning;
            pTaken->m_dwRunThread = GetCurrentThreadId();

            if (SUCCEEDED(InsertByPriority(m_hdpaRunning, pTaken, TRUE)))
            {
                pRun = pTaken;
                pRun->AddRef();
            }
            else
            {
                if (bResume)
                    pTaken->MarkKill();
                pTaken->MarkDropped();
            }
        }
        LeaveCriticalSection(&m_Lock);

        if (pTaken)
        {
            if (!pRun)
                pTaken->Retire();
            pTaken->Release();
        }

        if (pRun)
        {
            INT nNow = GetThreadPriority(hThread);
            HRESULT hr;

            if (nOriginal != THREAD_PRIORITY_ERROR_RETURN &&
                nNow != THREAD_PRIORITY_ERROR_RETURN &&
                nNow != m_nWorkerPriority)
            {
                SetThreadPriority(hThread, m_nWorkerPriority);
            }

            hr = bResume ? pRun->m_pTask->Resume() : pRun->m_pTask->Run();
            LeaveRunning(pRun, hr == E_PENDING);
            pRun->Release();
        }

        /* Leftover messages mean the task touched COM state; start the apartment over */
        if (PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE))
        {
            CoUninitialize();
            CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        }
    }

    if (nOriginal != THREAD_PRIORITY_ERROR_RETURN && GetThreadPriority(hThread) != nOriginal)
        SetThreadPriority(hThread, nOriginal);
}

DWORD WINAPI
CShellTaskScheduler::WorkerThreadProc(
    _In_ PVOID Parameter)
{
    TASK_WORKER *pWorker = (TASK_WORKER *)Parameter;
    CShellTaskScheduler *pScheduler = pWorker->pScheduler;

    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
        pScheduler->RunWorker(pWorker);
        CoUninitialize();
    }
    else
    {
        pScheduler->RetireWorker(pWorker);
    }

    delete pWorker;
    pScheduler->ReleaseHold();
    return 0;
}

static IShellTaskScheduler *g_pSharedScheduler;

HRESULT WINAPI
CSharedTaskScheduler::CreateInstance(
    _In_opt_ PVOID pUnkOuter,
    _In_ REFIID riid,
    _Outptr_ PVOID *ppv)
{
    IShellTaskScheduler *pScheduler;
    HRESULT hr;

    *ppv = NULL;
    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    if (!g_pSharedScheduler)
    {
        hr = CShellTaskScheduler::CreateInstance(NULL, IID_PPV_ARG(IShellTaskScheduler, &pScheduler));
        if (FAILED(hr))
            return hr;

        pScheduler->Status(ITSSFLAG_KILL_ON_DESTROY, 60 * 1000);
        if (InterlockedCompareExchangePointer((PVOID *)&g_pSharedScheduler, pScheduler, NULL))
            pScheduler->Release();
    }

    pScheduler = g_pSharedScheduler;
    if (!pScheduler)
        return E_FAIL;
    return pScheduler->QueryInterface(riid, ppv);
}

void
CSharedTaskScheduler::Shutdown()
{
    IShellTaskScheduler *pScheduler;

    pScheduler = (IShellTaskScheduler *)InterlockedExchangePointer((PVOID *)&g_pSharedScheduler, NULL);
    if (pScheduler)
    {
        pScheduler->RemoveTasks(GUID_NULL, ITSAT_DEFAULT_LPARAM, FALSE);
        pScheduler->Release();
    }
}
