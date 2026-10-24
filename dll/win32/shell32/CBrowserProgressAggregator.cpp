/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Browser progress aggregator and its session connections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell);

static const IID IID_IInfoBarHostPrivate = __uuidof(IInfoBarHost);
static const IID IID_IBrowserProgressConnectionPrivate = __uuidof(IBrowserProgressConnection);
static const IID IID_IBrowserProgressSessionProviderPrivate = __uuidof(IBrowserProgressSessionProvider);
static const IID IID_IBrowserProgressAggregatorPrivate = __uuidof(IBrowserProgressAggregator);

#define PROGRESS_STATE_BUSY     1
#define PROGRESS_STATE_DONE     2

/* The browser receives progress as parts of this scale */
#define PROGRESS_SCALE          10000
#define PROGRESS_DEFAULT_TOTAL  1000

/* Sources younger than this do not count yet, so quick operations never show progress */
#define PROGRESS_SETTLE_MS      1500
/* A busy source closes half of its remaining gap every this many milliseconds */
#define PROGRESS_HALF_LIFE_MS   20000.0

#define TIMER_BUSY_TICK         1
#define TIMER_STARTUP_DELAY     2
#define BUSY_TICK_MS            250
#define STARTUP_DELAY_MS        1000

#define WM_WORKER_REFRESH       (WM_APP + 0)
#define WM_WORKER_INFOBARS      (WM_APP + 1)

typedef struct _PROGRESS_SOURCE
{
    GUID guidSource;
    UINT uInstance;
    INT nCompleted;
    INT nTotal;
    INT nState;
    INT nSeenCompleted;
    INT nSeenTotal;
    INT nBusyStartCompleted;
    DWORD dwBusyStartTick;
    DWORD dwRegisterTick;
} PROGRESS_SOURCE, *PPROGRESS_SOURCE;

typedef struct _PROGRESS_SESSION
{
    CProgressConnection *pConnection;
    DWORD dwCookie;
} PROGRESS_SESSION, *PPROGRESS_SESSION;

static INT CALLBACK
ReleaseInformCallback(
    _In_ PVOID pItem,
    _In_opt_ PVOID pData)
{
    ((IInfoBarMessage *)pItem)->Release();
    return TRUE;
}

static void
DestroyInformList(
    _In_opt_ HDPA hdpaInforms)
{
    if (hdpaInforms)
        DPA_DestroyCallback(hdpaInforms, ReleaseInformCallback, NULL);
}

/* Drops queued messages that carry this ID; stops at the first message that cannot report one */
static HRESULT
RemoveInformsWithId(
    _In_ HDPA hdpaInforms,
    _In_ REFGUID guidMessage)
{
    HRESULT hr = S_OK;
    INT Index;

    for (Index = DPA_GetPtrCount(hdpaInforms) - 1; Index >= 0 && SUCCEEDED(hr); Index--)
    {
        IInfoBarMessage *pMessage = (IInfoBarMessage *)DPA_GetPtr(hdpaInforms, Index);
        GUID guidId;
        INT nInstance;

        hr = pMessage->GetMessageID(&guidId, &nInstance);
        if (SUCCEEDED(hr) && IsEqualGUID(guidId, guidMessage))
        {
            DPA_DeletePtr(hdpaInforms, Index);
            pMessage->Release();
        }
    }
    return hr;
}

static HRESULT
GitRegister(
    _In_ IUnknown *punk,
    _Out_ DWORD *pdwCookie)
{
    IGlobalInterfaceTable *pGit;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_StdGlobalInterfaceTable, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IGlobalInterfaceTable, &pGit));
    if (FAILED(hr))
        return hr;

    hr = pGit->RegisterInterfaceInGlobal(punk, IID_IUnknown, pdwCookie);
    pGit->Release();
    return hr;
}

static void
GitRevoke(
    _In_ DWORD dwCookie)
{
    IGlobalInterfaceTable *pGit;

    if (SUCCEEDED(CoCreateInstance(CLSID_StdGlobalInterfaceTable, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARG(IGlobalInterfaceTable, &pGit))))
    {
        pGit->RevokeInterfaceFromGlobal(dwCookie);
        pGit->Release();
    }
}

class CProgressConnection :
    public IBrowserProgressConnection,
    public IInfoBarHost
{
public:
    CProgressConnection() :
        m_cRef(1),
        m_pMarshaler(NULL),
        m_bActive(FALSE),
        m_dwCookie(0),
        m_pAggregator(NULL),
        m_hdpaInforms(NULL),
        m_hdsaCancels(NULL)
    {
        InitializeCriticalSection(&m_Lock);
    }

    ~CProgressConnection()
    {
        DeleteCriticalSection(&m_Lock);
        if (m_pMarshaler)
            m_pMarshaler->Release();
        if (m_pAggregator)
            m_pAggregator->Release();
        if (m_hdsaCancels)
            DSA_Destroy(m_hdsaCancels);
        DestroyInformList(m_hdpaInforms);
    }

    HRESULT
    Initialize(
        _In_ CBrowserProgressAggregator *pAggregator)
    {
        HRESULT hr;

        hr = CoCreateFreeThreadedMarshaler(static_cast<IBrowserProgressConnection *>(this), &m_pMarshaler);
        if (FAILED(hr))
            return hr;

        m_pAggregator = pAggregator;
        pAggregator->AddRef();
        return S_OK;
    }

    /* Hands the work queued while the session waited in the background to the aggregator */
    HRESULT
    Activate()
    {
        CBrowserProgressAggregator *pAggregator;
        HDPA hdpaInforms;
        HDSA hdsaCancels;
        HRESULT hr = S_OK;
        INT Index;

        EnterCriticalSection(&m_Lock);
        pAggregator = m_pAggregator;
        m_bActive = TRUE;
        pAggregator->AddRef();
        hdsaCancels = m_hdsaCancels;
        m_hdsaCancels = NULL;
        hdpaInforms = m_hdpaInforms;
        m_hdpaInforms = NULL;
        LeaveCriticalSection(&m_Lock);

        if (hdpaInforms)
        {
            for (Index = DPA_GetPtrCount(hdpaInforms) - 1; Index >= 0 && SUCCEEDED(hr); Index--)
            {
                IInfoBarMessage *pMessage = (IInfoBarMessage *)DPA_GetPtr(hdpaInforms, Index);

                hr = pAggregator->QueueInform(pMessage);
                DPA_DeletePtr(hdpaInforms, Index);
                pMessage->Release();
            }
        }

        if (hdsaCancels)
        {
            for (Index = DSA_GetItemCount(hdsaCancels) - 1; Index >= 0 && SUCCEEDED(hr); Index--)
            {
                GUID guidMessage;

                if (DSA_GetItem(hdsaCancels, Index, &guidMessage))
                    hr = pAggregator->QueueCancelInform(guidMessage);
                else
                    hr = E_FAIL;
                DSA_DeleteItem(hdsaCancels, Index);
            }
            DSA_Destroy(hdsaCancels);
        }

        pAggregator->Release();
        DestroyInformList(hdpaInforms);
        return hr;
    }

    // *** IUnknown methods ***
    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        PVOID *ppv) override
    {
        if (!ppv)
            return E_POINTER;

        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IBrowserProgressConnectionPrivate))
        {
            *ppv = static_cast<IBrowserProgressConnection *>(this);
        }
        else if (IsEqualIID(riid, IID_IInfoBarHostPrivate))
        {
            *ppv = static_cast<IInfoBarHost *>(this);
        }
        else if (IsEqualIID(riid, IID_IMarshal) && m_pMarshaler)
        {
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
    AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        ULONG cRef = InterlockedDecrement(&m_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    // *** IBrowserProgressConnection methods ***
    STDMETHODIMP
    Register(
        REFGUID guidSource,
        UINT *puInstance) override
    {
        BOOL bCurrent;
        HRESULT hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);

        if (SUCCEEDED(hr) && bCurrent)
            m_pAggregator->RegisterSource(guidSource, puInstance);
        return hr;
    }

    STDMETHODIMP
    Unregister(
        REFGUID guidSource,
        UINT uInstance) override
    {
        BOOL bCurrent;
        HRESULT hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);

        if (SUCCEEDED(hr) && bCurrent)
            m_pAggregator->UnregisterSource(guidSource, uInstance);
        return hr;
    }

    STDMETHODIMP
    UpdateProgress(
        REFGUID guidSource,
        UINT uInstance,
        UINT uCompleted,
        UINT uTotal) override
    {
        BOOL bCurrent;
        HRESULT hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);

        if (SUCCEEDED(hr) && bCurrent)
            m_pAggregator->UpdateSourceProgress(guidSource, uInstance, uCompleted, uTotal);
        return hr;
    }

    STDMETHODIMP
    UpdateState(
        REFGUID guidSource,
        UINT uInstance,
        INT nState) override
    {
        BOOL bCurrent;
        HRESULT hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);

        if (SUCCEEDED(hr) && bCurrent)
            m_pAggregator->UpdateSourceState(guidSource, uInstance, nState);
        return hr;
    }

    STDMETHODIMP
    CancelProgressUI() override
    {
        BOOL bCurrent;
        HRESULT hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);

        if (SUCCEEDED(hr) && bCurrent)
            m_pAggregator->CancelProgress();
        return hr;
    }

    // *** IInfoBarHost methods ***
    STDMETHODIMP
    Inform(
        IInfoBarMessage *pMessage) override
    {
        BOOL bCurrent;
        HRESULT hr;

        EnterCriticalSection(&m_Lock);
        hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);
        if (SUCCEEDED(hr))
        {
            if (m_bActive && bCurrent)
            {
                m_pAggregator->QueueInform(pMessage);
            }
            else
            {
                if (!m_hdpaInforms)
                    m_hdpaInforms = DPA_Create(10);

                if (!m_hdpaInforms)
                    hr = E_FAIL;
                else if (DPA_AppendPtr(m_hdpaInforms, pMessage) == -1)
                    hr = E_OUTOFMEMORY;
                else
                    pMessage->AddRef();
            }
        }
        LeaveCriticalSection(&m_Lock);
        return hr;
    }

    STDMETHODIMP
    CancelInform(
        REFGUID guidMessage) override
    {
        BOOL bCurrent;
        HRESULT hr;

        EnterCriticalSection(&m_Lock);
        hr = m_pAggregator->IsCurrentSession(m_dwCookie, &bCurrent);
        if (SUCCEEDED(hr))
        {
            if (m_bActive && bCurrent)
            {
                m_pAggregator->QueueCancelInform(guidMessage);
            }
            else
            {
                if (!m_hdsaCancels)
                    m_hdsaCancels = DSA_Create(sizeof(GUID), 10);

                if (!m_hdsaCancels)
                    hr = E_FAIL;
                else if (DSA_AppendItem(m_hdsaCancels, (PVOID)&guidMessage) == -1)
                    hr = E_OUTOFMEMORY;
                else if (m_hdpaInforms)
                    hr = RemoveInformsWithId(m_hdpaInforms, guidMessage);
            }
        }
        LeaveCriticalSection(&m_Lock);
        return hr;
    }

    LONG m_cRef;
    CRITICAL_SECTION m_Lock;
    IUnknown *m_pMarshaler;
    BOOL m_bActive;
    DWORD m_dwCookie;
    CBrowserProgressAggregator *m_pAggregator;
    HDPA m_hdpaInforms;
    HDSA m_hdsaCancels;
};

CBrowserProgressAggregator::CBrowserProgressAggregator() :
    m_cRef(1),
    m_pMarshaler(NULL),
    m_hwndWorker(NULL),
    m_bBusyTimer(FALSE),
    m_cBusy(0),
    m_bStartupDelay(FALSE),
    m_bStarted(FALSE),
    m_bCancelled(FALSE),
    m_hwndTarget(NULL),
    m_uMsgProgress(0),
    m_hdsaSources(NULL),
    m_flProgress(0.0f),
    m_punkSite(NULL),
    m_hdpaInforms(NULL),
    m_hdsaCancels(NULL),
    m_hdsaSessions(NULL)
{
    InitializeCriticalSection(&m_Lock);
}

CBrowserProgressAggregator::~CBrowserProgressAggregator()
{
    INT Index;

    if (m_hwndWorker)
        DestroyWindow(m_hwndWorker);
    Reset();

    if (m_hdsaSessions)
    {
        for (Index = 0; Index < DSA_GetItemCount(m_hdsaSessions); Index++)
        {
            PPROGRESS_SESSION pSession = (PPROGRESS_SESSION)DSA_GetItemPtr(m_hdsaSessions, Index);

            pSession->pConnection->Release();
            GitRevoke(pSession->dwCookie);
        }
        DSA_Destroy(m_hdsaSessions);
    }

    DeleteCriticalSection(&m_Lock);
    if (m_pMarshaler)
        m_pMarshaler->Release();
    if (m_hdsaSources)
        DSA_Destroy(m_hdsaSources);
    if (m_hdsaCancels)
        DSA_Destroy(m_hdsaCancels);
    DestroyInformList(m_hdpaInforms);
}

HRESULT WINAPI
CBrowserProgressAggregator::CreateInstance(
    _In_opt_ PVOID pUnkOuter,
    _In_ REFIID riid,
    _Outptr_ PVOID *ppv)
{
    CBrowserProgressAggregator *pAggregator;
    HRESULT hr;

    *ppv = NULL;
    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    pAggregator = new CBrowserProgressAggregator();
    if (!pAggregator)
        return E_OUTOFMEMORY;

    hr = pAggregator->QueryInterface(riid, ppv);
    pAggregator->Release();
    return hr;
}

STDMETHODIMP
CBrowserProgressAggregator::QueryInterface(
    REFIID riid,
    PVOID *ppv)
{
    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IBrowserProgressSessionProviderPrivate))
    {
        *ppv = static_cast<IBrowserProgressSessionProvider *>(this);
    }
    else if (IsEqualIID(riid, IID_IBrowserProgressAggregatorPrivate))
    {
        *ppv = static_cast<IBrowserProgressAggregator *>(this);
    }
    else if (IsEqualIID(riid, IID_IMarshal) && m_pMarshaler)
    {
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
CBrowserProgressAggregator::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CBrowserProgressAggregator::Release()
{
    ULONG cRef = InterlockedDecrement(&m_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

void
CBrowserProgressAggregator::PostToWorker(
    _In_ UINT uMsg)
{
    if (m_hwndWorker)
        PostMessageW(m_hwndWorker, uMsg, 0, 0);
}

HRESULT
CBrowserProgressAggregator::IsCurrentSession(
    _In_ DWORD dwCookie,
    _Out_ BOOL *pbCurrent)
{
    DWORD dwCurrent;
    HRESULT hr;

    *pbCurrent = FALSE;
    hr = GetCurrentSession(&dwCurrent);
    if (SUCCEEDED(hr))
        *pbCurrent = (dwCurrent == dwCookie);
    return hr;
}

INT
CBrowserProgressAggregator::FindSource(
    _In_ REFGUID guidSource,
    _In_ UINT uInstance)
{
    INT Index;

    if (!m_hdsaSources)
        return -1;

    for (Index = 0; Index < DSA_GetItemCount(m_hdsaSources); Index++)
    {
        PPROGRESS_SOURCE pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);

        if (IsEqualGUID(pSource->guidSource, guidSource) && pSource->uInstance == uInstance)
            return Index;
    }
    return -1;
}

void
CBrowserProgressAggregator::UpdateBusyTimer()
{
    if (m_cBusy && !m_bBusyTimer)
    {
        SetTimer(m_hwndWorker, TIMER_BUSY_TICK, BUSY_TICK_MS, NULL);
        m_bBusyTimer = TRUE;
    }
    else if (!m_cBusy && m_bBusyTimer)
    {
        KillTimer(m_hwndWorker, TIMER_BUSY_TICK);
        m_bBusyTimer = FALSE;
    }
}

HRESULT
CBrowserProgressAggregator::RegisterSource(
    _In_ REFGUID guidSource,
    _Out_opt_ UINT *puInstance)
{
    PROGRESS_SOURCE Source;
    UINT uHighest = 0;
    HRESULT hr;
    INT Index;

    EnterCriticalSection(&m_Lock);

    /* The first source starts a quiet period before any progress is shown */
    if (!m_bStarted)
    {
        m_bStartupDelay = TRUE;
        SetTimer(m_hwndWorker, TIMER_STARTUP_DELAY, STARTUP_DELAY_MS, NULL);
        m_bStarted = TRUE;
    }

    if (!m_hdsaSources)
        m_hdsaSources = DSA_Create(sizeof(PROGRESS_SOURCE), 10);

    if (!m_hdsaSources)
    {
        hr = E_FAIL;
    }
    else
    {
        for (Index = 0; Index < DSA_GetItemCount(m_hdsaSources); Index++)
        {
            PPROGRESS_SOURCE pOther = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);

            if (IsEqualGUID(pOther->guidSource, guidSource) && pOther->uInstance >= uHighest)
                uHighest = pOther->uInstance;
        }

        ZeroMemory(&Source, sizeof(Source));
        Source.guidSource = guidSource;
        Source.nTotal = PROGRESS_DEFAULT_TOTAL;
        Source.dwRegisterTick = GetTickCount();
        if (puInstance)
        {
            Source.uInstance = uHighest + 1;
            *puInstance = Source.uInstance;
        }

        hr = (DSA_AppendItem(m_hdsaSources, &Source) == -1) ? E_OUTOFMEMORY : S_OK;
    }

    LeaveCriticalSection(&m_Lock);
    return hr;
}

HRESULT
CBrowserProgressAggregator::UnregisterSource(
    _In_ REFGUID guidSource,
    _In_ UINT uInstance)
{
    HRESULT hr = E_INVALIDARG;
    INT Index;

    EnterCriticalSection(&m_Lock);
    Index = FindSource(guidSource, uInstance);
    if (Index != -1)
    {
        PPROGRESS_SOURCE pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);

        if (pSource->nState == PROGRESS_STATE_BUSY)
        {
            m_cBusy--;
            UpdateBusyTimer();
        }
        DSA_DeleteItem(m_hdsaSources, Index);
        PostToWorker(WM_WORKER_REFRESH);
        hr = S_OK;
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

void
CBrowserProgressAggregator::UpdateSourceProgress(
    _In_ REFGUID guidSource,
    _In_ UINT uInstance,
    _In_ UINT uCompleted,
    _In_ UINT uTotal)
{
    INT Index;

    EnterCriticalSection(&m_Lock);
    Index = FindSource(guidSource, uInstance);
    if (Index != -1)
    {
        PPROGRESS_SOURCE pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);

        pSource->nCompleted = (INT)uCompleted;
        pSource->nTotal = (INT)uTotal;
        PostToWorker(WM_WORKER_REFRESH);
    }
    LeaveCriticalSection(&m_Lock);
}

HRESULT
CBrowserProgressAggregator::UpdateSourceState(
    _In_ REFGUID guidSource,
    _In_ UINT uInstance,
    _In_ INT nState)
{
    PPROGRESS_SOURCE pSource;
    HRESULT hr = E_INVALIDARG;
    INT nOldState;
    INT Index;

    EnterCriticalSection(&m_Lock);
    Index = FindSource(guidSource, uInstance);
    if (Index != -1)
    {
        pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);
        nOldState = pSource->nState;
        pSource->nState = nState;

        if (nState == PROGRESS_STATE_BUSY && nOldState != PROGRESS_STATE_BUSY)
        {
            /* Busy sources creep toward done on their own from where they stand now */
            m_cBusy++;
            pSource->dwBusyStartTick = GetTickCount();
            pSource->nBusyStartCompleted = pSource->nCompleted;
        }
        else
        {
            if (nOldState == PROGRESS_STATE_BUSY && nState != PROGRESS_STATE_BUSY)
                m_cBusy--;
            if (nState == PROGRESS_STATE_DONE)
                pSource->nCompleted = pSource->nTotal;
        }

        UpdateBusyTimer();
        PostToWorker(WM_WORKER_REFRESH);
        hr = S_OK;
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

void
CBrowserProgressAggregator::CancelProgress()
{
    EnterCriticalSection(&m_Lock);
    m_bCancelled = (m_hdsaSources && DSA_GetItemCount(m_hdsaSources) != 0);
    if (m_hwndTarget)
        PostMessageW(m_hwndTarget, m_uMsgProgress, PROGRESS_SCALE, PROGRESS_SCALE);
    PostToWorker(WM_WORKER_REFRESH);
    m_bStarted = FALSE;
    LeaveCriticalSection(&m_Lock);
}

HRESULT
CBrowserProgressAggregator::QueueInform(
    _In_ IInfoBarMessage *pMessage)
{
    HRESULT hr;

    EnterCriticalSection(&m_Lock);
    if (!m_hdpaInforms)
        m_hdpaInforms = DPA_Create(10);

    if (!m_hdpaInforms)
    {
        hr = E_FAIL;
    }
    else if (DPA_AppendPtr(m_hdpaInforms, pMessage) == -1)
    {
        hr = E_OUTOFMEMORY;
    }
    else
    {
        pMessage->AddRef();
        hr = S_OK;

        /* With progress running, messages go out with the next progress update instead */
        if (!m_hdsaSources || !DSA_GetItemCount(m_hdsaSources))
            PostToWorker(WM_WORKER_INFOBARS);
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

HRESULT
CBrowserProgressAggregator::QueueCancelInform(
    _In_ REFGUID guidMessage)
{
    HRESULT hr;

    EnterCriticalSection(&m_Lock);
    if (!m_hdsaCancels)
        m_hdsaCancels = DSA_Create(sizeof(GUID), 10);

    if (!m_hdsaCancels)
    {
        hr = E_FAIL;
    }
    else if (DSA_AppendItem(m_hdsaCancels, (PVOID)&guidMessage) == -1)
    {
        hr = E_OUTOFMEMORY;
    }
    else
    {
        hr = m_hdpaInforms ? RemoveInformsWithId(m_hdpaInforms, guidMessage) : S_OK;
        if (SUCCEEDED(hr) && (!m_hdsaSources || !DSA_GetItemCount(m_hdsaSources)))
            PostToWorker(WM_WORKER_INFOBARS);
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

void
CBrowserProgressAggregator::ClearProgress()
{
    INT Index;

    if (m_hdsaSources)
    {
        for (Index = 0; Index < DSA_GetItemCount(m_hdsaSources); Index++)
        {
            PPROGRESS_SOURCE pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);

            pSource->nCompleted = pSource->nTotal;
        }
    }

    KillTimer(m_hwndWorker, TIMER_STARTUP_DELAY);
    m_bStartupDelay = FALSE;
    m_flProgress = 0.0f;
    m_bStarted = FALSE;
    m_bCancelled = FALSE;
}

void
CBrowserProgressAggregator::ClearInforms()
{
    INT Index;

    if (m_hdsaCancels)
        DSA_DeleteAllItems(m_hdsaCancels);

    if (m_hdpaInforms)
    {
        for (Index = 0; Index < DPA_GetPtrCount(m_hdpaInforms); Index++)
            ((IInfoBarMessage *)DPA_GetPtr(m_hdpaInforms, Index))->Release();
        DPA_DeleteAllPtrs(m_hdpaInforms);
    }
}

/* Progress from the session that lost the foreground does not carry over */
void
CBrowserProgressAggregator::DropSourcesForNewSession()
{
    INT Index;

    if (!m_hdsaSources || DSA_GetItemCount(m_hdsaSources) <= 0)
        return;

    if (!m_bStartupDelay)
        CancelProgress();

    for (Index = DSA_GetItemCount(m_hdsaSources) - 1; Index >= 0; Index--)
    {
        if (((PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index))->nState == PROGRESS_STATE_BUSY)
            m_cBusy--;
        DSA_DeleteItem(m_hdsaSources, Index);
    }
    UpdateBusyTimer();
    ClearProgress();
}

/*
 * Each settled source adds the share of the outstanding work it finished since the last pass.
 * Shares combine as independent fractions, so the bar only moves forward and stops short of done.
 */
void
CBrowserProgressAggregator::Recompute(
    _Out_ HDSA *phdsaCancels,
    _Out_ HDPA *phdpaInforms)
{
    INT cSources = m_hdsaSources ? DSA_GetItemCount(m_hdsaSources) : 0;
    INT Index;

    *phdsaCancels = NULL;
    *phdpaInforms = NULL;

    if (!m_bCancelled)
    {
        DWORD dwNow;
        INT nDoneSum = 0;
        INT nTotalSum = 0;

        if (m_bStartupDelay)
            return;

        dwNow = GetTickCount();
        for (Index = 0; Index < cSources; Index++)
        {
            PPROGRESS_SOURCE pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);

            if (dwNow - pSource->dwRegisterTick < PROGRESS_SETTLE_MS)
                continue;

            nDoneSum += pSource->nCompleted;
            nTotalSum += pSource->nTotal;

            if (pSource->nState == PROGRESS_STATE_BUSY)
            {
                INT nGap = pSource->nTotal - pSource->nBusyStartCompleted;
                double dfElapsed = (double)(GetTickCount() - pSource->dwBusyStartTick);
                double dfLeft = pow(0.5, dfElapsed / PROGRESS_HALF_LIFE_MS);

                pSource->nCompleted = pSource->nTotal - (INT)ceil(dfLeft * nGap);
            }
        }

        for (Index = 0; Index < cSources; Index++)
        {
            PPROGRESS_SOURCE pSource = (PPROGRESS_SOURCE)DSA_GetItemPtr(m_hdsaSources, Index);
            FLOAT flBefore;
            FLOAT flNow;

            if (dwNow - pSource->dwRegisterTick < PROGRESS_SETTLE_MS)
                continue;

            flBefore = pSource->nSeenTotal ? (FLOAT)pSource->nSeenCompleted / (FLOAT)pSource->nSeenTotal : 0.0f;
            flNow = pSource->nTotal ? (FLOAT)pSource->nCompleted / (FLOAT)pSource->nTotal : 0.0f;

            if (flNow > flBefore)
            {
                FLOAT flGained = (flNow - flBefore) * (FLOAT)pSource->nTotal;
                FLOAT flOutstanding = (FLOAT)(nTotalSum + pSource->nCompleted - nDoneSum) -
                                      (FLOAT)pSource->nTotal * flBefore;
                FLOAT flShare = 0.0f;

                if (flGained > 0.0f && flOutstanding > 0.0f && flOutstanding >= flGained)
                    flShare = flGained / flOutstanding;

                m_flProgress = 1.0f - (1.0f - flShare) * (1.0f - m_flProgress);
            }

            pSource->nSeenTotal = pSource->nTotal;
            pSource->nSeenCompleted = pSource->nCompleted;
        }
    }

    *phdsaCancels = m_hdsaCancels;
    m_hdsaCancels = NULL;
    *phdpaInforms = m_hdpaInforms;
    m_hdpaInforms = NULL;

    if (!cSources || m_bCancelled)
    {
        if (m_hwndTarget)
            PostMessageW(m_hwndTarget, m_uMsgProgress, PROGRESS_SCALE, PROGRESS_SCALE);
        if (!cSources)
            Reset();
    }
    else if (m_hwndTarget)
    {
        FLOAT flShown = floorf((FLOAT)MulDiv(PROGRESS_SCALE, 99, 100) * m_flProgress);

        PostMessageW(m_hwndTarget, m_uMsgProgress, (WPARAM)(INT)flShown, PROGRESS_SCALE);
    }
}

void
CBrowserProgressAggregator::DeliverInfoBars(
    _In_opt_ HDSA hdsaCancels,
    _In_opt_ HDPA hdpaInforms)
{
    IInfoBarHost *pHost;
    HRESULT hr;
    INT cItems;
    INT Item;

    hr = IUnknown_QueryService(m_punkSite, IID_IInfoBarHostPrivate, IID_IInfoBarHostPrivate, (PVOID *)&pHost);
    if (FAILED(hr))
        return;

    cItems = hdsaCancels ? DSA_GetItemCount(hdsaCancels) : 0;
    for (Item = 0; Item < cItems && SUCCEEDED(hr); Item++)
    {
        GUID guidMessage;

        hr = E_FAIL;
        if (DSA_GetItem(hdsaCancels, 0, &guidMessage))
        {
            hr = pHost->CancelInform(guidMessage);
            if (SUCCEEDED(hr) && !DSA_DeleteItem(hdsaCancels, 0))
                hr = E_FAIL;
        }
    }

    cItems = hdpaInforms ? DPA_GetPtrCount(hdpaInforms) : 0;
    for (Item = 0; Item < cItems && SUCCEEDED(hr); Item++)
    {
        IInfoBarMessage *pMessage = (IInfoBarMessage *)DPA_DeletePtr(hdpaInforms, 0);

        hr = pHost->Inform(pMessage);
        pMessage->Release();
    }

    pHost->Release();
}

LRESULT CALLBACK
CBrowserProgressAggregator::WorkerWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    CBrowserProgressAggregator *pThis;
    HDSA hdsaCancels = NULL;
    HDPA hdpaInforms = NULL;

    if (uMsg != WM_TIMER && uMsg != WM_WORKER_REFRESH && uMsg != WM_WORKER_INFOBARS)
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);

    pThis = (CBrowserProgressAggregator *)GetWindowLongPtrW(hwnd, 0);
    if (!pThis)
        return 0;

    pThis->AddRef();
    EnterCriticalSection(&pThis->m_Lock);

    if (uMsg == WM_WORKER_INFOBARS)
    {
        hdsaCancels = pThis->m_hdsaCancels;
        pThis->m_hdsaCancels = NULL;
        hdpaInforms = pThis->m_hdpaInforms;
        pThis->m_hdpaInforms = NULL;
    }
    else if (uMsg == WM_WORKER_REFRESH || wParam == TIMER_BUSY_TICK)
    {
        pThis->Recompute(&hdsaCancels, &hdpaInforms);
    }
    else if (wParam == TIMER_STARTUP_DELAY)
    {
        /* The quiet period is over: show an empty bar and start reporting */
        KillTimer(hwnd, TIMER_STARTUP_DELAY);
        pThis->m_bStartupDelay = FALSE;
        if (pThis->m_hwndTarget)
            PostMessageW(pThis->m_hwndTarget, pThis->m_uMsgProgress, 0, PROGRESS_SCALE);
        pThis->Recompute(&hdsaCancels, &hdpaInforms);
    }

    LeaveCriticalSection(&pThis->m_Lock);
    pThis->DeliverInfoBars(hdsaCancels, hdpaInforms);
    pThis->Release();

    if (hdsaCancels)
        DSA_Destroy(hdsaCancels);
    DestroyInformList(hdpaInforms);
    return 0;
}

STDMETHODIMP
CBrowserProgressAggregator::BeginSession(
    DWORD *pdwCookie)
{
    CProgressConnection *pConnection;
    PROGRESS_SESSION Session;
    DWORD dwCookie;
    HRESULT hr;

    EnterCriticalSection(&m_Lock);
    *pdwCookie = 0;

    pConnection = new CProgressConnection();
    if (!pConnection)
    {
        LeaveCriticalSection(&m_Lock);
        return E_OUTOFMEMORY;
    }

    hr = pConnection->Initialize(this);
    if (SUCCEEDED(hr))
        hr = GitRegister(static_cast<IBrowserProgressConnection *>(pConnection), &dwCookie);

    if (SUCCEEDED(hr))
    {
        pConnection->m_dwCookie = dwCookie;
        Session.pConnection = pConnection;
        Session.dwCookie = dwCookie;

        /* The newest session is the current one and sits at the front */
        if (DSA_InsertItem(m_hdsaSessions, 0, &Session) == -1)
        {
            hr = E_OUTOFMEMORY;
            GitRevoke(dwCookie);
        }
        else
        {
            *pdwCookie = dwCookie;
            pConnection->AddRef();
            DropSourcesForNewSession();
        }
    }

    pConnection->Release();
    LeaveCriticalSection(&m_Lock);
    return hr;
}

STDMETHODIMP
CBrowserProgressAggregator::EndSession(
    DWORD dwCookie)
{
    BOOL bCurrent;
    HRESULT hr;
    INT Index;

    if (!dwCookie)
        return E_FAIL;

    EnterCriticalSection(&m_Lock);
    hr = IsCurrentSession(dwCookie, &bCurrent);
    if (SUCCEEDED(hr))
    {
        hr = E_INVALIDARG;
        for (Index = 0; Index < DSA_GetItemCount(m_hdsaSessions); Index++)
        {
            PPROGRESS_SESSION pSession = (PPROGRESS_SESSION)DSA_GetItemPtr(m_hdsaSessions, Index);

            if (pSession->dwCookie == dwCookie)
            {
                pSession->pConnection->Release();
                GitRevoke(dwCookie);
                DSA_DeleteItem(m_hdsaSessions, Index);
                hr = S_OK;
                break;
            }
        }

        if (SUCCEEDED(hr) && bCurrent)
        {
            DropSourcesForNewSession();
            ClearInforms();
            if (DSA_GetItemCount(m_hdsaSessions) > 0)
                ((PPROGRESS_SESSION)DSA_GetItemPtr(m_hdsaSessions, 0))->pConnection->Activate();
        }
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

STDMETHODIMP
CBrowserProgressAggregator::GetCurrentSession(
    DWORD *pdwCookie)
{
    HRESULT hr = E_UNEXPECTED;

    EnterCriticalSection(&m_Lock);
    *pdwCookie = 0;
    if (m_hdsaSessions && DSA_GetItemCount(m_hdsaSessions) > 0)
    {
        *pdwCookie = ((PPROGRESS_SESSION)DSA_GetItemPtr(m_hdsaSessions, 0))->dwCookie;
        hr = S_OK;
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

/* Windows 7 reports E_FAIL here even after activating the current session */
STDMETHODIMP
CBrowserProgressAggregator::ActivateSession(
    DWORD dwCookie)
{
    BOOL bCurrent;
    HRESULT hr;

    EnterCriticalSection(&m_Lock);
    hr = IsCurrentSession(dwCookie, &bCurrent);
    if (SUCCEEDED(hr))
    {
        hr = E_FAIL;
        if (bCurrent)
        {
            ClearInforms();
            ((PPROGRESS_SESSION)DSA_GetItemPtr(m_hdsaSessions, 0))->pConnection->Activate();
        }
    }
    LeaveCriticalSection(&m_Lock);
    return hr;
}

STDMETHODIMP
CBrowserProgressAggregator::Initialize(
    HWND hwndTarget,
    UINT uMsgProgress,
    IUnknown *punkSite)
{
    HRESULT hr;

    m_hwndTarget = hwndTarget;
    m_uMsgProgress = uMsgProgress;

    hr = CoCreateFreeThreadedMarshaler(static_cast<IBrowserProgressSessionProvider *>(this), &m_pMarshaler);
    if (FAILED(hr))
        return hr;

    m_hdsaSessions = DSA_Create(sizeof(PROGRESS_SESSION), 2);
    if (!m_hdsaSessions)
        return E_FAIL;

    m_hwndWorker = SHCreateWorkerWindowW(WorkerWndProc, HWND_MESSAGE, 0, 0, NULL, (LONG_PTR)this);
    if (!m_hwndWorker)
        return E_FAIL;

    /* The browser owns this object, so the site is not referenced */
    m_punkSite = punkSite;
    return S_OK;
}

STDMETHODIMP
CBrowserProgressAggregator::Reset()
{
    EnterCriticalSection(&m_Lock);
    ClearProgress();
    ClearInforms();
    LeaveCriticalSection(&m_Lock);
    return S_OK;
}
