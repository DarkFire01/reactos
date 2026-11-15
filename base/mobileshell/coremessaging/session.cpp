/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     In process message session with named endpoints and a priority queue
 */

#define COREMSG_EXPORTS
#include <coremsg.h>
#include <string>
#include <vector>
#include <deque>

typedef struct _MSG_ENDPOINT_ENTRY
{
    PMSG_CALLBACK Callback;
    PVOID Context;
    BOOL Open;
} MSG_ENDPOINT_ENTRY;

typedef struct _MSG_NAME_ENTRY
{
    std::wstring Name;
    MSG_ENDPOINT Endpoint;
} MSG_NAME_ENTRY;

typedef struct _MSG_QUEUED
{
    MSG_ENDPOINT Target;
    std::vector<BYTE> Payload;
} MSG_QUEUED;

class CMessageSession : public IMessageSession
{
public:
    CMessageSession() : m_Refs(1), m_Wake(NULL), m_WakeContext(NULL)
    {
        InitializeCriticalSection(&m_Lock);
    }

    ~CMessageSession()
    {
        DeleteCriticalSection(&m_Lock);
    }

    ULONG STDMETHODCALLTYPE AddRef(VOID) override
    {
        return InterlockedIncrement(&m_Refs);
    }

    ULONG STDMETHODCALLTYPE Release(VOID) override
    {
        ULONG Refs = InterlockedDecrement(&m_Refs);

        if (Refs == 0)
            delete this;
        return Refs;
    }

    HRESULT STDMETHODCALLTYPE CreateEndpoint(
        _In_ PMSG_CALLBACK Callback,
        _In_opt_ PVOID Context,
        _Out_ PMSG_ENDPOINT Endpoint) override;

    HRESULT STDMETHODCALLTYPE CloseEndpoint(
        _In_ MSG_ENDPOINT Endpoint) override;

    HRESULT STDMETHODCALLTYPE PublishEndpoint(
        _In_z_ PCWSTR Name,
        _In_ MSG_ENDPOINT Endpoint) override;

    HRESULT STDMETHODCALLTYPE OpenEndpoint(
        _In_z_ PCWSTR Name,
        _Out_ PMSG_ENDPOINT Endpoint) override;

    HRESULT STDMETHODCALLTYPE Send(
        _In_ MSG_ENDPOINT Target,
        _In_ MSG_PRIORITY Priority,
        _In_reads_bytes_(Size) const VOID *Payload,
        _In_ ULONG Size) override;

    HRESULT STDMETHODCALLTYPE SendSync(
        _In_ MSG_ENDPOINT Target,
        _In_reads_bytes_(Size) const VOID *Payload,
        _In_ ULONG Size) override;

    VOID STDMETHODCALLTYPE SetWake(
        _In_opt_ PMSG_WAKE Wake,
        _In_opt_ PVOID Context) override;

    ULONG STDMETHODCALLTYPE Dispatch(VOID) override;

private:
    BOOL
    LookupLocked(
        _In_ MSG_ENDPOINT Endpoint,
        _Out_ MSG_ENDPOINT_ENTRY *Entry);

    LONG m_Refs;
    CRITICAL_SECTION m_Lock;
    std::vector<MSG_ENDPOINT_ENTRY> m_Endpoints;
    std::vector<MSG_NAME_ENTRY> m_Names;
    std::deque<MSG_QUEUED> m_Queue;
    PMSG_WAKE m_Wake;
    PVOID m_WakeContext;
};

HRESULT
STDMETHODCALLTYPE
CMessageSession::CreateEndpoint(
    _In_ PMSG_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Out_ PMSG_ENDPOINT Endpoint)
{
    MSG_ENDPOINT_ENTRY Entry;

    *Endpoint = MSG_ENDPOINT_NONE;
    if (!Callback)
        return E_INVALIDARG;

    Entry.Callback = Callback;
    Entry.Context = Context;
    Entry.Open = TRUE;

    EnterCriticalSection(&m_Lock);
    m_Endpoints.push_back(Entry);
    *Endpoint = static_cast<MSG_ENDPOINT>(m_Endpoints.size());
    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CMessageSession::CloseEndpoint(
    _In_ MSG_ENDPOINT Endpoint)
{
    HRESULT Status = E_HANDLE;
    SIZE_T Index;

    EnterCriticalSection(&m_Lock);
    if (Endpoint != MSG_ENDPOINT_NONE && Endpoint <= m_Endpoints.size())
    {
        m_Endpoints[Endpoint - 1].Open = FALSE;
        for (Index = m_Names.size(); Index > 0; Index -= 1)
        {
            if (m_Names[Index - 1].Endpoint == Endpoint)
                m_Names.erase(m_Names.begin() + (Index - 1));
        }
        Status = S_OK;
    }
    LeaveCriticalSection(&m_Lock);
    return Status;
}

HRESULT
STDMETHODCALLTYPE
CMessageSession::PublishEndpoint(
    _In_z_ PCWSTR Name,
    _In_ MSG_ENDPOINT Endpoint)
{
    MSG_NAME_ENTRY Entry;
    SIZE_T Index;

    if (!Name || Endpoint == MSG_ENDPOINT_NONE)
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);
    for (Index = 0; Index < m_Names.size(); Index++)
    {
        if (_wcsicmp(m_Names[Index].Name.c_str(), Name) == 0)
        {
            LeaveCriticalSection(&m_Lock);
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    }

    Entry.Name = Name;
    Entry.Endpoint = Endpoint;
    m_Names.push_back(Entry);
    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CMessageSession::OpenEndpoint(
    _In_z_ PCWSTR Name,
    _Out_ PMSG_ENDPOINT Endpoint)
{
    SIZE_T Index;

    *Endpoint = MSG_ENDPOINT_NONE;
    EnterCriticalSection(&m_Lock);
    for (Index = 0; Index < m_Names.size(); Index++)
    {
        if (_wcsicmp(m_Names[Index].Name.c_str(), Name) == 0)
        {
            *Endpoint = m_Names[Index].Endpoint;
            break;
        }
    }
    LeaveCriticalSection(&m_Lock);
    return (*Endpoint != MSG_ENDPOINT_NONE) ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

BOOL
CMessageSession::LookupLocked(
    _In_ MSG_ENDPOINT Endpoint,
    _Out_ MSG_ENDPOINT_ENTRY *Entry)
{
    if (Endpoint == MSG_ENDPOINT_NONE || Endpoint > m_Endpoints.size())
        return FALSE;

    *Entry = m_Endpoints[Endpoint - 1];
    return Entry->Open;
}

HRESULT
STDMETHODCALLTYPE
CMessageSession::Send(
    _In_ MSG_ENDPOINT Target,
    _In_ MSG_PRIORITY Priority,
    _In_reads_bytes_(Size) const VOID *Payload,
    _In_ ULONG Size)
{
    MSG_ENDPOINT_ENTRY Entry;
    MSG_QUEUED Queued;
    PMSG_WAKE Wake = NULL;
    PVOID WakeContext = NULL;

    Queued.Target = Target;
    Queued.Payload.assign(static_cast<const BYTE *>(Payload), static_cast<const BYTE *>(Payload) + Size);

    EnterCriticalSection(&m_Lock);
    if (!LookupLocked(Target, &Entry))
    {
        LeaveCriticalSection(&m_Lock);
        return E_HANDLE;
    }

    if (m_Queue.empty())
    {
        Wake = m_Wake;
        WakeContext = m_WakeContext;
    }

    if (Priority == MsgPriorityHigh)
        m_Queue.push_front(std::move(Queued));
    else
        m_Queue.push_back(std::move(Queued));
    LeaveCriticalSection(&m_Lock);

    if (Wake)
        Wake(WakeContext);
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CMessageSession::SendSync(
    _In_ MSG_ENDPOINT Target,
    _In_reads_bytes_(Size) const VOID *Payload,
    _In_ ULONG Size)
{
    MSG_ENDPOINT_ENTRY Entry;
    BOOL Found;

    EnterCriticalSection(&m_Lock);
    Found = LookupLocked(Target, &Entry);
    LeaveCriticalSection(&m_Lock);

    if (!Found)
        return E_HANDLE;
    return Entry.Callback(Entry.Context, Payload, Size);
}

VOID
STDMETHODCALLTYPE
CMessageSession::SetWake(
    _In_opt_ PMSG_WAKE Wake,
    _In_opt_ PVOID Context)
{
    EnterCriticalSection(&m_Lock);
    m_Wake = Wake;
    m_WakeContext = Context;
    LeaveCriticalSection(&m_Lock);
}

ULONG
STDMETHODCALLTYPE
CMessageSession::Dispatch(VOID)
{
    MSG_ENDPOINT_ENTRY Entry;
    MSG_QUEUED Queued;
    ULONG Count = 0;
    BOOL Found;

    for (;;)
    {
        EnterCriticalSection(&m_Lock);
        if (m_Queue.empty())
        {
            LeaveCriticalSection(&m_Lock);
            break;
        }
        Queued = std::move(m_Queue.front());
        m_Queue.pop_front();
        Found = LookupLocked(Queued.Target, &Entry);
        LeaveCriticalSection(&m_Lock);

        if (Found)
        {
            Entry.Callback(Entry.Context, Queued.Payload.data(), static_cast<ULONG>(Queued.Payload.size()));
            Count++;
        }
    }
    return Count;
}

EXTERN_C
COREMSG_API
HRESULT
WINAPI
CoreMsgCreateSession(
    _Outptr_ IMessageSession **Session)
{
    *Session = new CMessageSession();
    return *Session ? S_OK : E_OUTOFMEMORY;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);
    return TRUE;
}
