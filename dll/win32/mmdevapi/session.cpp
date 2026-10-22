/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Audio sessions, their volumes and meters, and the session manager
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define MMD_MAX_DUCK_NOTIFICATIONS  8

static
HRESULT
MmdDuplicateString(
    _In_ PCWSTR Source,
    _Outptr_ LPWSTR *String)
{
    SIZE_T Bytes = (wcslen(Source) + 1) * sizeof(WCHAR);

    *String = (LPWSTR)CoTaskMemAlloc(Bytes);
    if (!*String)
        return E_OUTOFMEMORY;
    CopyMemory(*String, Source, Bytes);
    return S_OK;
}

/* One session slot of the engine, as a control, its volumes and its meter */
class CSession
    : public CMmdObject
    , public IAudioSessionControl2
    , public ISimpleAudioVolume
    , public IChannelAudioVolume
    , public IAudioMeterInformation
{
public:
    CSession(
        _In_ ULONG Session,
        _In_ ULONG Generation)
        : m_Session(Session)
        , m_Generation(Generation)
    {
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;

        if (IsEqualIID(Riid, IID_IUnknown) || IsEqualIID(Riid, IID_IAudioSessionControl) ||
            IsEqualIID(Riid, IID_IAudioSessionControl2))
        {
            *Object = static_cast<IAudioSessionControl2 *>(this);
        }
        else if (IsEqualIID(Riid, IID_ISimpleAudioVolume))
            *Object = static_cast<ISimpleAudioVolume *>(this);
        else if (IsEqualIID(Riid, IID_IChannelAudioVolume))
            *Object = static_cast<IChannelAudioVolume *>(this);
        else if (IsEqualIID(Riid, IID_IAudioMeterInformation))
            *Object = static_cast<IAudioMeterInformation *>(this);
        else
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    /* IAudioSessionControl */
    STDMETHODIMP
    GetState(
        _Out_ AudioSessionState *State) override
    {
        const AE_SESSION_STATE *Session;

        if (!State)
            return E_POINTER;
        Session = Get();
        *State = Session ? (AudioSessionState)Session->State : AudioSessionStateExpired;
        return S_OK;
    }

    STDMETHODIMP
    GetDisplayName(
        _Outptr_ LPWSTR *Name) override
    {
        const AE_SESSION_STATE *Session;

        if (!Name)
            return E_POINTER;
        *Name = NULL;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        return MmdDuplicateString(Session->DisplayName, Name);
    }

    STDMETHODIMP
    SetDisplayName(
        _In_ LPCWSTR Name,
        _In_opt_ LPCGUID Context) override
    {
        if (!Name)
            return E_POINTER;
        return SetText(AE_SET_DISPLAY_NAME, Name, Context);
    }

    STDMETHODIMP
    GetIconPath(
        _Outptr_ LPWSTR *Path) override
    {
        const AE_SESSION_STATE *Session;

        if (!Path)
            return E_POINTER;
        *Path = NULL;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        return MmdDuplicateString(Session->IconPath, Path);
    }

    STDMETHODIMP
    SetIconPath(
        _In_ LPCWSTR Path,
        _In_opt_ LPCGUID Context) override
    {
        if (!Path)
            return E_POINTER;
        return SetText(AE_SET_ICON_PATH, Path, Context);
    }

    STDMETHODIMP
    GetGroupingParam(
        _Out_ GUID *Grouping) override
    {
        const AE_SESSION_STATE *Session;

        if (!Grouping)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Grouping = Session->GroupingParam;
        return S_OK;
    }

    STDMETHODIMP
    SetGroupingParam(
        _In_ LPCGUID Grouping,
        _In_opt_ LPCGUID Context) override
    {
        AE_REQUEST Request;

        if (!Grouping)
            return E_POINTER;
        Prepare(&Request, AE_SET_GROUPING, Context);
        Request.SetSession.Guid = *Grouping;
        return MmdCall(&Request, AeCommandSetSession, sizeof(Request.SetSession), NULL, 0);
    }

    STDMETHODIMP
    RegisterAudioSessionNotification(
        _In_ IAudioSessionEvents *Events) override
    {
        if (!Events)
            return E_POINTER;
        if (!Get())
            return AUDCLNT_E_DEVICE_INVALIDATED;
        return MmdAddWatch(MmdWatchSessionEvents, m_Session, m_Generation, Events);
    }

    STDMETHODIMP
    UnregisterAudioSessionNotification(
        _In_ IAudioSessionEvents *Events) override
    {
        if (!Events)
            return E_POINTER;
        return MmdRemoveWatch(MmdWatchSessionEvents, m_Session, Events);
    }

    /* IAudioSessionControl2 */
    STDMETHODIMP
    GetSessionIdentifier(
        _Outptr_ LPWSTR *Identifier) override
    {
        WCHAR Text[AE_ID_LENGTH + 2 * MAX_PATH + 96];
        HRESULT Hr;

        if (!Identifier)
            return E_POINTER;
        *Identifier = NULL;
        Hr = FormatIdentifier(Text, ARRAYSIZE(Text), FALSE);
        return SUCCEEDED(Hr) ? MmdDuplicateString(Text, Identifier) : Hr;
    }

    STDMETHODIMP
    GetSessionInstanceIdentifier(
        _Outptr_ LPWSTR *Identifier) override
    {
        WCHAR Text[AE_ID_LENGTH + 2 * MAX_PATH + 128];
        HRESULT Hr;

        if (!Identifier)
            return E_POINTER;
        *Identifier = NULL;
        Hr = FormatIdentifier(Text, ARRAYSIZE(Text), TRUE);
        return SUCCEEDED(Hr) ? MmdDuplicateString(Text, Identifier) : Hr;
    }

    STDMETHODIMP
    GetProcessId(
        _Out_ DWORD *ProcessId) override
    {
        const AE_SESSION_STATE *Session;

        if (!ProcessId)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *ProcessId = Session->ProcessId;
        return Session->CrossProcess ? AUDCLNT_S_NO_SINGLE_PROCESS : S_OK;
    }

    STDMETHODIMP
    IsSystemSoundsSession() override
    {
        const AE_SESSION_STATE *Session = Get();

        return (Session && Session->SystemSounds) ? S_OK : S_FALSE;
    }

    STDMETHODIMP
    SetDuckingPreference(
        _In_ BOOL OptOut) override
    {
        UNREFERENCED_PARAMETER(OptOut);

        /* Nothing ducks other streams yet */
        return Get() ? S_OK : AUDCLNT_E_DEVICE_INVALIDATED;
    }

    /* ISimpleAudioVolume */
    STDMETHODIMP
    SetMasterVolume(
        _In_ float Level,
        _In_opt_ LPCGUID Context) override
    {
        AE_REQUEST Request;

        if (!(Level >= 0.0f && Level <= 1.0f))
            return E_INVALIDARG;
        Prepare(&Request, AE_SET_VOLUME, Context);
        Request.SetSession.Value = Level;
        return MmdCall(&Request, AeCommandSetSession, sizeof(Request.SetSession), NULL, 0);
    }

    STDMETHODIMP
    GetMasterVolume(
        _Out_ float *Level) override
    {
        const AE_SESSION_STATE *Session;

        if (!Level)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Level = Session->Volume;
        return S_OK;
    }

    STDMETHODIMP
    SetMute(
        _In_ const BOOL Mute,
        _In_opt_ LPCGUID Context) override
    {
        AE_REQUEST Request;

        Prepare(&Request, AE_SET_MUTE, Context);
        Request.SetSession.Mute = Mute ? TRUE : FALSE;
        return MmdCall(&Request, AeCommandSetSession, sizeof(Request.SetSession), NULL, 0);
    }

    STDMETHODIMP
    GetMute(
        _Out_ BOOL *Mute) override
    {
        const AE_SESSION_STATE *Session;

        if (!Mute)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Mute = Session->Mute ? TRUE : FALSE;
        return S_OK;
    }

    /* IChannelAudioVolume */
    STDMETHODIMP
    GetChannelCount(
        _Out_ UINT32 *Count) override
    {
        const AE_SESSION_STATE *Session;

        if (!Count)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Count = Session->Channels;
        return S_OK;
    }

    STDMETHODIMP
    SetChannelVolume(
        _In_ UINT32 Index,
        _In_ const float Level,
        _In_opt_ LPCGUID Context) override
    {
        AE_REQUEST Request;

        if (!(Level >= 0.0f && Level <= 1.0f))
            return E_INVALIDARG;
        Prepare(&Request, AE_SET_CHANNEL_SCALAR, Context);
        Request.SetSession.Channel = Index;
        Request.SetSession.Value = Level;
        return MmdCall(&Request, AeCommandSetSession, sizeof(Request.SetSession), NULL, 0);
    }

    STDMETHODIMP
    GetChannelVolume(
        _In_ UINT32 Index,
        _Out_ float *Level) override
    {
        const AE_SESSION_STATE *Session;

        if (!Level)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Index >= Session->Channels)
            return E_INVALIDARG;
        *Level = Session->ChannelVolume[Index];
        return S_OK;
    }

    STDMETHODIMP
    SetAllVolumes(
        _In_ UINT32 Count,
        _In_reads_(Count) const float *Levels,
        _In_opt_ LPCGUID Context) override
    {
        const AE_SESSION_STATE *Session;
        UINT32 Index;
        HRESULT Hr = S_OK;

        if (!Levels)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Count != Session->Channels)
            return E_INVALIDARG;
        for (Index = 0; Index < Count; Index++)
        {
            if (!(Levels[Index] >= 0.0f && Levels[Index] <= 1.0f))
                return E_INVALIDARG;
        }
        for (Index = 0; Index < Count && SUCCEEDED(Hr); Index++)
            Hr = SetChannelVolume(Index, Levels[Index], Context);
        return Hr;
    }

    STDMETHODIMP
    GetAllVolumes(
        _In_ UINT32 Count,
        _Out_writes_(Count) float *Levels) override
    {
        const AE_SESSION_STATE *Session;
        UINT32 Index;

        if (!Levels)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Count != Session->Channels)
            return E_INVALIDARG;
        for (Index = 0; Index < Count; Index++)
            Levels[Index] = Session->ChannelVolume[Index];
        return S_OK;
    }

    /* IAudioMeterInformation */
    STDMETHODIMP
    GetPeakValue(
        _Out_ float *Peak) override
    {
        const AE_SESSION_STATE *Session;

        if (!Peak)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Peak = (Session->State == AudioSessionStateActive) ? Session->Peak : 0.0f;
        return S_OK;
    }

    STDMETHODIMP
    GetMeteringChannelCount(
        _Out_ UINT *Count) override
    {
        return GetChannelCount(Count);
    }

    STDMETHODIMP
    GetChannelsPeakValues(
        _In_ UINT32 Count,
        _Out_writes_(Count) float *Peaks) override
    {
        const AE_SESSION_STATE *Session;
        UINT32 Index;

        if (!Peaks)
            return E_POINTER;
        Session = Get();
        if (!Session)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Count != Session->Channels)
            return E_INVALIDARG;
        for (Index = 0; Index < Count; Index++)
            Peaks[Index] = (Session->State == AudioSessionStateActive) ? Session->PeakChannel[Index] : 0.0f;
        return S_OK;
    }

    STDMETHODIMP
    QueryHardwareSupport(
        _Out_ DWORD *Mask) override
    {
        if (!Mask)
            return E_POINTER;
        *Mask = 0;
        return S_OK;
    }

private:
    /* The published session, or NULL once its slot went to another session */
    const AE_SESSION_STATE *
    Get()
    {
        const AE_STATE *State = MmdGetState();
        const AE_SESSION_STATE *Session;

        if (!State)
            return NULL;
        Session = &State->Sessions[m_Session];
        return (Session->InUse && Session->Generation == m_Generation) ? Session : NULL;
    }

    VOID
    Prepare(
        _Out_ AE_REQUEST *Request,
        _In_ ULONG What,
        _In_opt_ LPCGUID Context)
    {
        ZeroMemory(Request, sizeof(Request->SetSession));
        Request->SetSession.Session = m_Session;
        Request->SetSession.Generation = m_Generation;
        Request->SetSession.What = What;
        Request->SetSession.Context = Context ? *Context : GUID_NULL;
    }

    HRESULT
    SetText(
        _In_ ULONG What,
        _In_ PCWSTR Text,
        _In_opt_ LPCGUID Context)
    {
        AE_REQUEST Request;

        Prepare(&Request, What, Context);
        if (FAILED(StringCchCopyW(Request.SetSession.Text, ARRAYSIZE(Request.SetSession.Text), Text)))
            return E_INVALIDARG;
        return MmdCall(&Request, AeCommandSetSession, sizeof(Request.SetSession), NULL, 0);
    }

    /* Appends Part to Text with % written as %% and | as %b */
    static
    HRESULT
    AppendEscaped(
        _Inout_updates_(Length) PWSTR Text,
        _In_ SIZE_T Length,
        _In_ PCWSTR Part)
    {
        SIZE_T Used = wcslen(Text);

        for (; *Part; Part++)
        {
            if (Used + 3 > Length)
                return STRSAFE_E_INSUFFICIENT_BUFFER;
            if (*Part == L'%' || *Part == L'|')
            {
                Text[Used++] = L'%';
                Text[Used++] = (*Part == L'%') ? L'%' : L'b';
            }
            else
            {
                Text[Used++] = *Part;
            }
        }
        Text[Used] = UNICODE_NULL;
        return S_OK;
    }

    /*
     * "endpoint|app|{session}" with the app part escaped, the app being the image path in NT
     * form or "#" for a cross process session. An instance adds "|tssession|pid", also escaped.
     */
    HRESULT
    FormatIdentifier(
        _Out_writes_(Length) PWSTR Text,
        _In_ SIZE_T Length,
        _In_ BOOL Instance)
    {
        const AE_STATE *State = MmdGetState();
        const AE_SESSION_STATE *Session = Get();
        WCHAR Image[MAX_PATH], Part[MAX_PATH + 48], Guid[40];
        DWORD ImageLength = ARRAYSIZE(Image), TsSession = 0;
        HANDLE Process;
        HRESULT Hr;

        if (!State || !Session || Session->Endpoint >= State->EndpointCount)
            return AUDCLNT_E_DEVICE_INVALIDATED;

        StringCchCopyW(Image, ARRAYSIZE(Image), L"#");
        if (!Session->CrossProcess)
        {
            Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Session->ProcessId);
            if (Process)
            {
                if (!QueryFullProcessImageNameW(Process, PROCESS_NAME_NATIVE, Image, &ImageLength))
                    StringCchCopyW(Image, ARRAYSIZE(Image), L"#");
                CloseHandle(Process);
            }
        }
        StringFromGUID2(Session->SessionGuid, Guid, ARRAYSIZE(Guid));

        Hr = StringCchPrintfW(Text, Length, L"%s|", State->Endpoints[Session->Endpoint].Id);
        if (SUCCEEDED(Hr))
            Hr = StringCchPrintfW(Part, ARRAYSIZE(Part), L"%s|%s", Image, Guid);
        if (SUCCEEDED(Hr))
            Hr = AppendEscaped(Text, Length, Part);
        if (FAILED(Hr) || !Instance)
            return Hr;

        if (!ProcessIdToSessionId(Session->CrossProcess ? GetCurrentProcessId() : Session->ProcessId, &TsSession))
            TsSession = 0;
        if (Session->CrossProcess)
            Hr = StringCchPrintfW(Part, ARRAYSIZE(Part), L"%lu|#", TsSession);
        else
            Hr = StringCchPrintfW(Part, ARRAYSIZE(Part), L"%lu|%lu", TsSession, Session->ProcessId);
        if (SUCCEEDED(Hr))
            Hr = StringCchCatW(Text, Length, L"|");
        if (SUCCEEDED(Hr))
            Hr = AppendEscaped(Text, Length, Part);
        return Hr;
    }

    ULONG m_Session;
    ULONG m_Generation;
};

class CSessionEnumerator
    : public CMmdObject
    , public IAudioSessionEnumerator
{
public:
    CSessionEnumerator() : m_Count(0) {}

    VOID
    Add(
        _In_ ULONG Session,
        _In_ ULONG Generation)
    {
        m_Sessions[m_Count] = Session;
        m_Generations[m_Count] = Generation;
        m_Count++;
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioSessionEnumerator))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IAudioSessionEnumerator *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetCount(
        _Out_ INT *Count) override
    {
        if (!Count)
            return E_POINTER;
        *Count = (INT)m_Count;
        return S_OK;
    }

    STDMETHODIMP
    GetSession(
        _In_ INT Index,
        _Outptr_ IAudioSessionControl **Session) override
    {
        if (!Session)
            return E_POINTER;
        *Session = NULL;
        if (Index < 0 || (ULONG)Index >= m_Count)
            return E_INVALIDARG;
        return MmdCreateSessionObject(m_Sessions[Index], m_Generations[Index], IID_IAudioSessionControl,
                                      (VOID **)Session);
    }

private:
    ULONG m_Sessions[AE_MAX_SESSIONS];
    ULONG m_Generations[AE_MAX_SESSIONS];
    ULONG m_Count;
};

class CSessionManager
    : public CMmdObject
    , public IAudioSessionManager2
{
public:
    CSessionManager(
        _In_ ULONG Endpoint)
        : m_Endpoint(Endpoint)
    {
        ZeroMemory(m_Ducks, sizeof(m_Ducks));
    }

    ~CSessionManager()
    {
        ULONG Index;

        for (Index = 0; Index < MMD_MAX_DUCK_NOTIFICATIONS; Index++)
        {
            if (m_Ducks[Index])
                m_Ducks[Index]->Release();
        }
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioSessionManager) &&
            !IsEqualIID(Riid, IID_IAudioSessionManager2))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IAudioSessionManager2 *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetAudioSessionControl(
        _In_opt_ LPCGUID SessionGuid,
        _In_ DWORD StreamFlags,
        _Outptr_ IAudioSessionControl **Control) override
    {
        if (!Control)
            return E_POINTER;
        *Control = NULL;
        return Open(SessionGuid, StreamFlags, IID_IAudioSessionControl, (VOID **)Control);
    }

    STDMETHODIMP
    GetSimpleAudioVolume(
        _In_opt_ LPCGUID SessionGuid,
        _In_ DWORD StreamFlags,
        _Outptr_ ISimpleAudioVolume **Volume) override
    {
        if (!Volume)
            return E_POINTER;
        *Volume = NULL;
        return Open(SessionGuid, StreamFlags, IID_ISimpleAudioVolume, (VOID **)Volume);
    }

    STDMETHODIMP
    GetSessionEnumerator(
        _Outptr_ IAudioSessionEnumerator **Enumerator) override
    {
        const AE_STATE *State = MmdGetState();
        const AE_SESSION_STATE *Session;
        CSessionEnumerator *Object;
        ULONG Index;

        if (!Enumerator)
            return E_POINTER;
        *Enumerator = NULL;
        if (!State)
            return AUDCLNT_E_SERVICE_NOT_RUNNING;

        Object = new (std::nothrow) CSessionEnumerator();
        if (!Object)
            return E_OUTOFMEMORY;
        for (Index = 0; Index < AE_MAX_SESSIONS; Index++)
        {
            Session = &State->Sessions[Index];
            if (Session->InUse && Session->Endpoint == m_Endpoint && Session->State != AudioSessionStateExpired)
                Object->Add(Index, Session->Generation);
        }
        *Enumerator = Object;
        return S_OK;
    }

    STDMETHODIMP
    RegisterSessionNotification(
        _In_ IAudioSessionNotification *Notification) override
    {
        if (!Notification)
            return E_POINTER;
        return MmdAddWatch(MmdWatchSessionCreated, m_Endpoint, 0, Notification);
    }

    STDMETHODIMP
    UnregisterSessionNotification(
        _In_ IAudioSessionNotification *Notification) override
    {
        if (!Notification)
            return E_POINTER;
        return MmdRemoveWatch(MmdWatchSessionCreated, m_Endpoint, Notification);
    }

    /* Nothing ducks yet, so these notifications are only kept */
    STDMETHODIMP
    RegisterDuckNotification(
        _In_opt_ LPCWSTR SessionId,
        _In_ IAudioVolumeDuckNotification *Notification) override
    {
        ULONG Index;

        UNREFERENCED_PARAMETER(SessionId);

        if (!Notification)
            return E_POINTER;
        for (Index = 0; Index < MMD_MAX_DUCK_NOTIFICATIONS; Index++)
        {
            if (!m_Ducks[Index])
            {
                m_Ducks[Index] = Notification;
                Notification->AddRef();
                return S_OK;
            }
        }
        return E_OUTOFMEMORY;
    }

    STDMETHODIMP
    UnregisterDuckNotification(
        _In_ IAudioVolumeDuckNotification *Notification) override
    {
        ULONG Index;

        if (!Notification)
            return E_POINTER;
        for (Index = 0; Index < MMD_MAX_DUCK_NOTIFICATIONS; Index++)
        {
            if (m_Ducks[Index] == Notification)
            {
                m_Ducks[Index] = NULL;
                Notification->Release();
                return S_OK;
            }
        }
        return E_NOTFOUND;
    }

private:
    HRESULT
    Open(
        _In_opt_ LPCGUID SessionGuid,
        _In_ DWORD StreamFlags,
        _In_ REFIID Riid,
        _Outptr_ VOID **Object)
    {
        AE_REQUEST Request;
        AE_REPLY Reply;
        HRESULT Hr;

        if ((StreamFlags & AUDCLNT_STREAMFLAGS_CROSSPROCESS) && (!SessionGuid || IsEqualGUID(*SessionGuid, GUID_NULL)))
            return E_INVALIDARG;

        ZeroMemory(&Request, sizeof(Request.OpenSession));
        Request.OpenSession.Endpoint = m_Endpoint;
        Request.OpenSession.SessionGuid = SessionGuid ? *SessionGuid : GUID_NULL;
        Request.OpenSession.CrossProcess = (StreamFlags & AUDCLNT_STREAMFLAGS_CROSSPROCESS) ? TRUE : FALSE;
        Hr = MmdCall(&Request, AeCommandOpenSession, sizeof(Request.OpenSession), &Reply, sizeof(Reply.OpenSession));
        if (FAILED(Hr))
            return Hr;
        return MmdCreateSessionObject(Reply.OpenSession.Session, Reply.OpenSession.Generation, Riid, Object);
    }

    ULONG m_Endpoint;
    IAudioVolumeDuckNotification *m_Ducks[MMD_MAX_DUCK_NOTIFICATIONS];
};

HRESULT
MmdCreateSessionObject(
    _In_ ULONG Session,
    _In_ ULONG Generation,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    CSession *Control;
    HRESULT Hr;

    *Object = NULL;
    if (Session >= AE_MAX_SESSIONS)
        return E_INVALIDARG;
    Control = new (std::nothrow) CSession(Session, Generation);
    if (!Control)
        return E_OUTOFMEMORY;
    Hr = Control->QueryInterface(Riid, Object);
    Control->Release();
    return Hr;
}

HRESULT
MmdCreateSessionManager(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    CSessionManager *Manager;
    HRESULT Hr;

    *Object = NULL;
    Manager = new (std::nothrow) CSessionManager(Endpoint);
    if (!Manager)
        return E_OUTOFMEMORY;
    Hr = Manager->QueryInterface(Riid, Object);
    Manager->Release();
    return Hr;
}
