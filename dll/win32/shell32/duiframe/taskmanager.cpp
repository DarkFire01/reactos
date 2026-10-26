/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Frame task manager: background work items of the view frame modules
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Operation name the browser's ITrackEvents sees for every work item */
static const WCHAR c_szDUIWorkItem[] = L"DUIWorkItem";

/* ITrackEvents::OperationEvent events */
#define FRAMEWORK_TRACK_START   0
#define FRAMEWORK_TRACK_END     1

/* IShellTaskScheduler2::AddTask2 placement: queue behind, and drop queued tasks with the same id */
#define FRAMETASK_PLACE_BEHIND      0x2
#define FRAMETASK_PURGE_MATCHING    0x4

static
VOID
FrameTask_Track(
    _In_opt_ IUnknown *punkSite,
    _In_ UINT uEvent)
{
    ITrackEvents *pte;

    if (SUCCEEDED(IUnknown_QueryService(punkSite, IID_ITrackEvents, IID_PPV_ARGS(&pte))))
    {
        pte->OperationEvent(c_szDUIWorkItem, (TRACKOPERATIONTYPE)0, (TRACKOPERATIONEVENT)uEvent);
        pte->Release();
    }
}

/* Same scheduler setup the rest of the shell uses for its default background tasks */
static
HRESULT
FrameTask_CreateScheduler(
    _COM_Outptr_ IShellTaskScheduler2 **ppScheduler)
{
    IShellTaskSchedulerSettings *pSettings;
    HRESULT hr;

    *ppScheduler = NULL;
    hr = CoCreateInstance(CLSID_ShellTaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pSettings));
    if (FAILED(hr))
        return hr;

    hr = pSettings->SetWorkerThreadCountMax(
        SHRegGetUSDWORDW(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellTaskScheduler",
                         L"MaxWorkerThreadsPerScheduler", 5));
    if (SUCCEEDED(hr))
    {
        IShellTaskScheduler *psts;

        hr = pSettings->QueryInterface(IID_PPV_ARGS(&psts));
        if (SUCCEEDED(hr))
        {
            psts->Status(ITSSFLAG_KILL_ON_DESTROY, 60000);
            hr = psts->QueryInterface(IID_IShellTaskScheduler2, (void **)ppScheduler);
            psts->Release();
        }
    }
    pSettings->Release();
    return hr;
}

/* CFrameWorkItem */

CFrameWorkItem::CFrameWorkItem() :
    _cRef(1)
{
    InitializeCriticalSection(&_cs);
}

CFrameWorkItem::~CFrameWorkItem()
{
    DeleteCriticalSection(&_cs);
}

STDMETHODIMP
CFrameWorkItem::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFrameWorkItem, IFrameWorkItem),
        {0},
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CFrameWorkItem::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CFrameWorkItem::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* The first answer sticks: a cancel arriving after the work finished changes nothing */
STDMETHODIMP
CFrameWorkItem::SetDispatchable(
    BOOL fDispatchable)
{
    EnterCriticalSection(&_cs);
    if (!_fStateKnown)
    {
        _fStateKnown = TRUE;
        _fDispatchable = fDispatchable;
    }
    LeaveCriticalSection(&_cs);
    return S_OK;
}

STDMETHODIMP
CFrameWorkItem::GetDispatchable(
    BOOL *pfDispatchable)
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&_cs);
    if (_fStateKnown)
        *pfDispatchable = _fDispatchable;
    else
        hr = E_FAIL;
    LeaveCriticalSection(&_cs);
    return hr;
}

/* Work keeps going until the item has been canceled */
BOOL
CFrameWorkItem::_ShouldContinue()
{
    BOOL fDispatchable;

    return FAILED(GetDispatchable(&fDispatchable)) || fDispatchable;
}

/* CFrameTask */

/*
 * Win7 hands the finished item back through a marshaled IFrameTaskManager. ReactOS registers no
 * proxy for that interface, so the task posts to a window the manager owns on the frame thread.
 */
#define WM_FRAMETASK_DISPATCH   (WM_APP + 1)

static const WCHAR c_szDispatchClass[] = L"DUIFrameTaskDispatch";

CFrameTask::CFrameTask(
    _In_ HWND hwndDispatch,
    _In_ IFrameWorkItem *pfwi) :
    CRunnableTask(RTF_DEFAULT)
{
    _pfwi = pfwi;
    _pfwi->AddRef();
    _hwndDispatch = hwndDispatch;
}

CFrameTask::~CFrameTask()
{
    IUnknown_SafeReleaseAndNullPtr(&_pfwi);
}

/* Runs the item on the worker thread, then asks the frame thread to dispatch what finished */
STDMETHODIMP
CFrameTask::InternalResumeRT()
{
    HRESULT hr;

    hr = _pfwi->SetDispatchable(SUCCEEDED(_pfwi->DoWork()));
    if (FAILED(hr))
        return hr;

    if (!PostMessageW(_hwndDispatch, WM_FRAMETASK_DISPATCH, 0, 0))
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

/* CFrameTaskManager */

CFrameTaskManager::CFrameTaskManager() :
    _cRef(1)
{
}

CFrameTaskManager::~CFrameTaskManager()
{
    if (_hwndDispatch)
    {
        SetWindowLongPtrW(_hwndDispatch, GWLP_USERDATA, 0);
        if (GetWindowThreadProcessId(_hwndDispatch, NULL) == GetCurrentThreadId())
            DestroyWindow(_hwndDispatch);
        else
            PostMessageW(_hwndDispatch, WM_CLOSE, 0, 0);
    }
    CancelAllWorkItems();
    IUnknown_SafeReleaseAndNullPtr(&_pScheduler);
    if (_hdsaItems)
    {
        DSA_DestroyCallback(_hdsaItems, s_ReleaseEntryCB, NULL);
        _hdsaItems = NULL;
    }
}

HRESULT
WINAPI
CFrameTaskManager::CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CFrameTaskManager *pftm;
    HRESULT hr;

    *ppv = NULL;
    pftm = new CFrameTaskManager();
    if (!pftm)
        return E_OUTOFMEMORY;

    hr = pftm->QueryInterface(riid, ppv);
    pftm->Release();
    return hr;
}

STDMETHODIMP
CFrameTaskManager::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFrameTaskManager, IFrameTaskManager),
        QITABENT(CFrameTaskManager, IObjectWithSite),
        {0},
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CFrameTaskManager::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CFrameTaskManager::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CFrameTaskManager::SetSite(
    IUnknown *punkSite)
{
    IUnknown_Set(&_punkSite, punkSite);
    return S_OK;
}

int
CALLBACK
CFrameTaskManager::s_ReleaseEntryCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    FRAMEWORKITEMENTRY *pEntry = (FRAMEWORKITEMENTRY *)p;

    UNREFERENCED_PARAMETER(pData);
    pEntry->pfwi->Release();
    return TRUE;
}

int
CALLBACK
CFrameTaskManager::s_DispatchWorkItem(
    _In_ void *p,
    _In_opt_ void *pData)
{
    CFrameTaskManager *pThis = (CFrameTaskManager *)pData;

    ((IFrameWorkItem *)p)->Dispatch();
    FrameTask_Track(pThis->_punkSite, FRAMEWORK_TRACK_END);
    return TRUE;
}

int
CALLBACK
CFrameTaskManager::s_ReleaseItemCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    UNREFERENCED_PARAMETER(pData);
    ((IFrameWorkItem *)p)->Release();
    return TRUE;
}

HRESULT
CFrameTaskManager::_EnsureTaskScheduler()
{
    HRESULT hr;

    if (_pScheduler)
        return S_OK;

    hr = IUnknown_QueryService(_punkSite, IID_IShellTaskScheduler, IID_IShellTaskScheduler2, (void **)&_pScheduler);
    if (FAILED(hr))
        hr = FrameTask_CreateScheduler(&_pScheduler);
    return hr;
}

LRESULT
CALLBACK
CFrameTaskManager::s_DispatchWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    CFrameTaskManager *pThis = (CFrameTaskManager *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (uMsg == WM_FRAMETASK_DISPATCH)
    {
        if (pThis)
        {
            pThis->AddRef();
            pThis->DispatchCompletedWorkItems();
            pThis->Release();
        }
        return 0;
    }
    if (uMsg == WM_CLOSE)
    {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* The message window lives on the thread that starts work, which is the frame thread */
HRESULT
CFrameTaskManager::_EnsureDispatchWindow()
{
    WNDCLASSW wc = { 0 };

    if (_hwndDispatch)
        return S_OK;

    wc.lpfnWndProc = s_DispatchWndProc;
    wc.hInstance = DUIFRAME_HINST;
    wc.lpszClassName = c_szDispatchClass;
    RegisterClassW(&wc);

    _hwndDispatch = CreateWindowExW(0, c_szDispatchClass, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                    DUIFRAME_HINST, NULL);
    if (!_hwndDispatch)
        return HRESULT_FROM_WIN32(GetLastError());

    SetWindowLongPtrW(_hwndDispatch, GWLP_USERDATA, (LONG_PTR)this);
    return S_OK;
}

HRESULT
CFrameTaskManager::_EnsureWorkItemList()
{
    if (_hdsaItems)
        return S_OK;

    _hdsaItems = DSA_Create(sizeof(FRAMEWORKITEMENTRY), 1);
    return _hdsaItems ? S_OK : E_OUTOFMEMORY;
}

/* With FTMF_REPLACE, items still pending under the same task id are canceled first */
STDMETHODIMP
CFrameTaskManager::StartWorkItem(
    IFrameWorkItem *pfwi,
    REFCLSID rclsidTaskID,
    int dwFlags)
{
    CFrameTask *pTask;
    DWORD dwPlacement = FRAMETASK_PLACE_BEHIND;
    HRESULT hr;

    hr = _EnsureTaskScheduler();
    if (SUCCEEDED(hr))
        hr = _EnsureWorkItemList();
    if (SUCCEEDED(hr))
        hr = _EnsureDispatchWindow();
    if (FAILED(hr))
        return hr;

    pTask = new CFrameTask(_hwndDispatch, pfwi);
    if (!pTask)
        return E_OUTOFMEMORY;

    if (dwFlags & FTMF_REPLACE)
    {
        dwPlacement |= FRAMETASK_PURGE_MATCHING;
        for (int i = DSA_GetItemCount(_hdsaItems) - 1; i >= 0; i -= 1)
        {
            FRAMEWORKITEMENTRY *pEntry = (FRAMEWORKITEMENTRY *)DSA_GetItemPtr(_hdsaItems, i);

            if (IsEqualGUID(pEntry->guidTask, rclsidTaskID))
            {
                pEntry->pfwi->SetDispatchable(FALSE);
                pEntry->pfwi->Release();
                DSA_DeleteItem(_hdsaItems, i);
                FrameTask_Track(_punkSite, FRAMEWORK_TRACK_END);
            }
        }
    }

    hr = _pScheduler->AddTask2(pTask, rclsidTaskID, 0, ITSAT_DEFAULT_PRIORITY, dwPlacement);
    if (SUCCEEDED(hr))
    {
        FRAMEWORKITEMENTRY entry;

        entry.pfwi = pfwi;
        entry.guidTask = rclsidTaskID;
        if (DSA_AppendItem(_hdsaItems, &entry) != DSA_ERR)
        {
            pfwi->AddRef();
            FrameTask_Track(_punkSite, FRAMEWORK_TRACK_START);
        }
        else
        {
            hr = E_OUTOFMEMORY;
        }
    }

    pTask->Release();
    return hr;
}

/* Finished items leave the list before any is dispatched, since a dispatch may start new work */
STDMETHODIMP
CFrameTaskManager::DispatchCompletedWorkItems()
{
    HDPA hdpaReady;
    HRESULT hr = S_OK;
    int cItems = _hdsaItems ? DSA_GetItemCount(_hdsaItems) : 0;

    hdpaReady = DPA_Create(cItems);
    if (!hdpaReady)
        return E_FAIL;

    for (int i = cItems - 1; i >= 0 && SUCCEEDED(hr); i -= 1)
    {
        FRAMEWORKITEMENTRY *pEntry = (FRAMEWORKITEMENTRY *)DSA_GetItemPtr(_hdsaItems, i);
        BOOL fDispatchable = FALSE;

        if (FAILED(pEntry->pfwi->GetDispatchable(&fDispatchable)))
            continue;

        if (fDispatchable)
        {
            if (DPA_AppendPtr(hdpaReady, pEntry->pfwi) == -1)
                hr = E_OUTOFMEMORY;
            else
                pEntry->pfwi->AddRef();
        }
        else
        {
            FrameTask_Track(_punkSite, FRAMEWORK_TRACK_END);
        }

        if (SUCCEEDED(hr))
        {
            pEntry->pfwi->Release();
            DSA_DeleteItem(_hdsaItems, i);
        }
    }

    DPA_EnumCallback(hdpaReady, s_DispatchWorkItem, this);
    DPA_DestroyCallback(hdpaReady, s_ReleaseItemCB, NULL);
    return hr;
}

STDMETHODIMP
CFrameTaskManager::CancelAllWorkItems()
{
    int cItems = _hdsaItems ? DSA_GetItemCount(_hdsaItems) : 0;

    for (int i = cItems - 1; i >= 0; i -= 1)
    {
        FRAMEWORKITEMENTRY *pEntry = (FRAMEWORKITEMENTRY *)DSA_GetItemPtr(_hdsaItems, i);

        if (SUCCEEDED(_pScheduler->RemoveTasks(pEntry->guidTask, 0, FALSE)))
        {
            pEntry->pfwi->Release();
            DSA_DeleteItem(_hdsaItems, i);
            FrameTask_Track(_punkSite, FRAMEWORK_TRACK_END);
        }
    }
    return S_OK;
}
