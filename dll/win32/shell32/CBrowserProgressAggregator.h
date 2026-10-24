/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Browser progress aggregator and its session connections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

struct DECLSPEC_NOVTABLE IInfoBarMessage : public IUnknown
{
    STDMETHOD(GetMessageID)(
        _Out_ GUID *pguidMessage,
        _Out_ INT *pnInstance) PURE;
};

struct DECLSPEC_UUID("E38FE0F3-3DB0-47EE-A314-25CF7F4BF521") DECLSPEC_NOVTABLE
IInfoBarHost : public IUnknown
{
    STDMETHOD(Inform)(
        _In_ IInfoBarMessage *pMessage) PURE;
    STDMETHOD(CancelInform)(
        _In_ REFGUID guidMessage) PURE;
};

struct DECLSPEC_UUID("20174539-B2C7-4EC7-970B-04201F9CDBAD") DECLSPEC_NOVTABLE
IBrowserProgressConnection : public IUnknown
{
    STDMETHOD(Register)(
        _In_ REFGUID guidSource,
        _Out_opt_ UINT *puInstance) PURE;
    STDMETHOD(Unregister)(
        _In_ REFGUID guidSource,
        _In_ UINT uInstance) PURE;
    STDMETHOD(UpdateProgress)(
        _In_ REFGUID guidSource,
        _In_ UINT uInstance,
        _In_ UINT uCompleted,
        _In_ UINT uTotal) PURE;
    STDMETHOD(UpdateState)(
        _In_ REFGUID guidSource,
        _In_ UINT uInstance,
        _In_ INT nState) PURE;
    STDMETHOD(CancelProgressUI)() PURE;
};

struct DECLSPEC_UUID("18140CBD-AA23-4384-A38D-6A8D3E2BE505") DECLSPEC_NOVTABLE
IBrowserProgressSessionProvider : public IUnknown
{
    STDMETHOD(BeginSession)(
        _Out_ DWORD *pdwCookie) PURE;
    STDMETHOD(EndSession)(
        _In_ DWORD dwCookie) PURE;
    STDMETHOD(GetCurrentSession)(
        _Out_ DWORD *pdwCookie) PURE;
    STDMETHOD(ActivateSession)(
        _In_ DWORD dwCookie) PURE;
};

struct DECLSPEC_UUID("5EA8EEC4-C34B-4DE0-9B56-0E15FD8C8F80") DECLSPEC_NOVTABLE
IBrowserProgressAggregator : public IUnknown
{
    STDMETHOD(Initialize)(
        _In_ HWND hwndTarget,
        _In_ UINT uMsgProgress,
        _In_opt_ IUnknown *punkSite) PURE;
    STDMETHOD(Reset)() PURE;
};

class CProgressConnection;

class CBrowserProgressAggregator :
    public CComCoClass<CBrowserProgressAggregator, &CLSID_BrowserProgressAggregator>,
    public IBrowserProgressSessionProvider,
    public IBrowserProgressAggregator
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_opt_ PVOID pUnkOuter,
        _In_ REFIID riid,
        _Outptr_ PVOID *ppv);

    typedef CBrowserProgressAggregator _CreatorClass;

    static void WINAPI ObjectMain(bool) {}
    static const struct _ATL_CATMAP_ENTRY *GetCategoryMap() { return NULL; }

    DECLARE_REGISTRY_RESOURCEID(IDR_BROWSERPROGRESSAGGREGATOR)

    // *** IUnknown methods ***
    STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // *** IBrowserProgressSessionProvider methods ***
    STDMETHODIMP BeginSession(DWORD *pdwCookie) override;
    STDMETHODIMP EndSession(DWORD dwCookie) override;
    STDMETHODIMP GetCurrentSession(DWORD *pdwCookie) override;
    STDMETHODIMP ActivateSession(DWORD dwCookie) override;

    // *** IBrowserProgressAggregator methods ***
    STDMETHODIMP Initialize(HWND hwndTarget, UINT uMsgProgress, IUnknown *punkSite) override;
    STDMETHODIMP Reset() override;

private:
    friend class CProgressConnection;

    CBrowserProgressAggregator();
    ~CBrowserProgressAggregator();

    HRESULT IsCurrentSession(_In_ DWORD dwCookie, _Out_ BOOL *pbCurrent);
    INT FindSource(_In_ REFGUID guidSource, _In_ UINT uInstance);
    HRESULT RegisterSource(_In_ REFGUID guidSource, _Out_opt_ UINT *puInstance);
    HRESULT UnregisterSource(_In_ REFGUID guidSource, _In_ UINT uInstance);
    void UpdateSourceProgress(_In_ REFGUID guidSource, _In_ UINT uInstance, _In_ UINT uCompleted, _In_ UINT uTotal);
    HRESULT UpdateSourceState(_In_ REFGUID guidSource, _In_ UINT uInstance, _In_ INT nState);
    void CancelProgress();
    HRESULT QueueInform(_In_ IInfoBarMessage *pMessage);
    HRESULT QueueCancelInform(_In_ REFGUID guidMessage);

    void UpdateBusyTimer();
    void ClearProgress();
    void ClearInforms();
    void DropSourcesForNewSession();
    void Recompute(_Out_ HDSA *phdsaCancels, _Out_ HDPA *phdpaInforms);
    void DeliverInfoBars(_In_opt_ HDSA hdsaCancels, _In_opt_ HDPA hdpaInforms);
    void PostToWorker(_In_ UINT uMsg);

    static LRESULT CALLBACK WorkerWndProc(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam);

    LONG m_cRef;
    CRITICAL_SECTION m_Lock;
    IUnknown *m_pMarshaler;
    HWND m_hwndWorker;
    BOOL m_bBusyTimer;
    INT m_cBusy;
    BOOL m_bStartupDelay;
    BOOL m_bStarted;
    BOOL m_bCancelled;
    HWND m_hwndTarget;
    UINT m_uMsgProgress;
    HDSA m_hdsaSources;
    FLOAT m_flProgress;
    IUnknown *m_punkSite;
    HDPA m_hdpaInforms;
    HDSA m_hdsaCancels;
    HDSA m_hdsaSessions;
};
