/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Connection to the audio engine and the thread that reports its changes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define MMD_PIPE_WAIT_MS    2000
#define MMD_WATCH_POLL_MS   1000

/* What the watch thread saw last, to tell what changed since */
typedef struct _MMD_SESSION_SEEN
{
    ULONG InUse;
    ULONG Generation;
    ULONG State;
    LONG VolumeSequence;
    LONG PropertySequence;
    float Volume;
    ULONG Mute;
    float ChannelVolume[AE_MAX_CHANNELS];
    WCHAR DisplayName[AE_PATH_LENGTH];
    WCHAR IconPath[AE_PATH_LENGTH];
    GUID GroupingParam;
} MMD_SESSION_SEEN;

typedef struct _MMD_WATCH
{
    LIST_ENTRY Link;
    MMD_WATCH_KIND Kind;
    ULONG Index;
    ULONG Generation;
    IUnknown *Callback;
    BOOL Disconnected;
    BOOL Busy;

    /* What this watch last reported, so it only hears of what changed after it registered */
    LONG VolumeSequence;
    MMD_SESSION_SEEN Session;
} MMD_WATCH, *PMMD_WATCH;

typedef struct _MMD_SEEN
{
    BOOL Valid;
    ULONG EndpointCount;
    WCHAR Ids[AE_MAX_ENDPOINTS][AE_ID_LENGTH];
    ULONG DeviceState[AE_MAX_ENDPOINTS];
    LONG VolumeSequence[AE_MAX_ENDPOINTS];
    ULONG Defaults[2][AE_ROLE_COUNT];
    MMD_SESSION_SEEN Sessions[AE_MAX_SESSIONS];
} MMD_SEEN;

static CRITICAL_SECTION MmdLock;
static HANDLE volatile MmdPipe = INVALID_HANDLE_VALUE;
static const AE_STATE * volatile MmdState;
static volatile ULONG MmdConnection;

static CRITICAL_SECTION MmdWatchLock;
static LIST_ENTRY MmdWatches;
static HANDLE MmdWatchEvent;
static HANDLE MmdWatchThread;
static MMD_SEEN MmdSeen;

static
BOOL
MmdTransact(
    _In_ HANDLE Pipe,
    _In_reads_bytes_(Size) const AE_REQUEST *Request,
    _In_ ULONG Size,
    _Out_ AE_REPLY *Reply,
    _Out_ DWORD *Read)
{
    DWORD Written;

    if (!WriteFile(Pipe, Request, Size, &Written, NULL) || Written != Size)
        return FALSE;
    return ReadFile(Pipe, Reply, sizeof(*Reply), Read, NULL) && *Read >= sizeof(AE_REPLY_HEADER);
}

static
VOID
MmdDisconnect(VOID)
{
    if (MmdPipe != INVALID_HANDLE_VALUE)
        CloseHandle(MmdPipe);
    MmdPipe = INVALID_HANDLE_VALUE;
}

static
VOID
MmdSendWatch(VOID)
{
    AE_REQUEST Request;
    AE_REPLY Reply;
    DWORD Read;

    ZeroMemory(&Request, sizeof(Request.Stream));
    Request.Header.Command = AeCommandWatch;
    Request.Header.Size = sizeof(Request.Stream);
    Request.Stream.Handle = (ULONG64)(ULONG_PTR)MmdWatchEvent;
    if (!MmdTransact(MmdPipe, &Request, sizeof(Request.Stream), &Reply, &Read))
        MmdDisconnect();
}

/* Opens the pipe and maps the state; the caller holds MmdLock */
static
HRESULT
MmdConnect(VOID)
{
    AE_REQUEST Request;
    AE_REPLY Reply;
    DWORD Mode = PIPE_READMODE_MESSAGE, Read;
    HANDLE Pipe, Section;
    const AE_STATE *State;

    if (MmdPipe != INVALID_HANDLE_VALUE)
        return S_OK;

    /* Lock free readers see the pipe only once the state and connection number go with it */
    Pipe = CreateFileW(AE_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(AE_PIPE_NAME, MMD_PIPE_WAIT_MS))
        Pipe = CreateFileW(AE_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Pipe == INVALID_HANDLE_VALUE)
        return AUDCLNT_E_SERVICE_NOT_RUNNING;

    ZeroMemory(&Request, sizeof(Request.Hello));
    Request.Header.Command = AeCommandHello;
    Request.Header.Size = sizeof(Request.Hello);
    Request.Hello.Version = AE_PROTOCOL_VERSION;
    Request.Hello.ProcessId = GetCurrentProcessId();
    if (!SetNamedPipeHandleState(Pipe, &Mode, NULL, NULL) ||
        !MmdTransact(Pipe, &Request, sizeof(Request.Hello), &Reply, &Read) || FAILED(Reply.Header.Status))
    {
        CloseHandle(Pipe);
        return AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    /* A view of an earlier engine stays mapped; another thread may still be reading it */
    Section = OpenFileMappingW(FILE_MAP_READ, FALSE, AE_STATE_SECTION_NAME);
    State = Section ? (const AE_STATE *)MapViewOfFile(Section, FILE_MAP_READ, 0, 0, sizeof(AE_STATE)) : NULL;
    if (Section)
        CloseHandle(Section);
    if (!State || State->Magic != AE_STATE_MAGIC || State->Version != AE_PROTOCOL_VERSION)
    {
        if (State)
            UnmapViewOfFile(State);
        CloseHandle(Pipe);
        return AUDCLNT_E_SERVICE_NOT_RUNNING;
    }
    MmdState = State;
    MmdConnection++;
    MemoryBarrier();
    MmdPipe = Pipe;

    if (MmdWatchEvent)
        MmdSendWatch();
    return S_OK;
}

/* Notices an engine that went away, so the next call connects to the new one */
static
VOID
MmdCheckConnection(VOID)
{
    DWORD Available;

    EnterCriticalSection(&MmdLock);
    if (MmdPipe != INVALID_HANDLE_VALUE && !PeekNamedPipe(MmdPipe, NULL, 0, NULL, &Available, NULL))
        MmdDisconnect();
    MmdConnect();
    LeaveCriticalSection(&MmdLock);
}

const AE_STATE *
MmdGetState(VOID)
{
    const AE_STATE *State;

    /* Realtime threads of clients come through here; only a missing engine takes the lock */
    if (MmdPipe != INVALID_HANDLE_VALUE)
        return MmdState;

    EnterCriticalSection(&MmdLock);
    if (MmdPipe == INVALID_HANDLE_VALUE)
        MmdConnect();
    State = (MmdPipe != INVALID_HANDLE_VALUE) ? MmdState : NULL;
    LeaveCriticalSection(&MmdLock);
    return State;
}

ULONG
MmdGetConnection(VOID)
{
    return (MmdPipe != INVALID_HANDLE_VALUE) ? MmdConnection : 0;
}

HRESULT
MmdCall(
    _Inout_updates_bytes_(Size) AE_REQUEST *Request,
    _In_ ULONG Command,
    _In_ ULONG Size,
    _Out_writes_bytes_opt_(ReplySize) AE_REPLY *Reply,
    _In_ ULONG ReplySize)
{
    AE_REPLY Buffer;
    DWORD Read;
    HRESULT Hr;

    Request->Header.Command = Command;
    Request->Header.Size = Size;

    EnterCriticalSection(&MmdLock);
    Hr = MmdConnect();
    if (SUCCEEDED(Hr))
    {
        if (MmdTransact(MmdPipe, Request, Size, &Buffer, &Read))
        {
            Hr = Buffer.Header.Status;
            if (Reply)
            {
                ZeroMemory(Reply, ReplySize);
                CopyMemory(Reply, &Buffer, min(ReplySize, Read));
            }
        }
        else
        {
            MmdDisconnect();
            Hr = AUDCLNT_E_SERVICE_NOT_RUNNING;
        }
    }
    LeaveCriticalSection(&MmdLock);
    return Hr;
}

ULONG
MmdFindEndpoint(
    _In_ PCWSTR Id)
{
    const AE_STATE *State = MmdGetState();
    ULONG Index;

    if (!State)
        return AE_NO_ENDPOINT;
    for (Index = 0; Index < State->EndpointCount && Index < AE_MAX_ENDPOINTS; Index++)
    {
        if (State->Endpoints[Index].Id[0] && !_wcsicmp(State->Endpoints[Index].Id, Id))
            return Index;
    }
    return AE_NO_ENDPOINT;
}

BOOL
MmdIsEndpointActive(
    _In_ ULONG Endpoint)
{
    const AE_STATE *State = MmdGetState();

    return State && Endpoint < State->EndpointCount && State->Endpoints[Endpoint].DeviceState == DEVICE_STATE_ACTIVE;
}

static
VOID
MmdCopySession(
    _In_ const AE_SESSION_STATE *Session,
    _Out_ MMD_SESSION_SEEN *Copy)
{
    Copy->InUse = Session->InUse;
    Copy->Generation = Session->Generation;
    Copy->State = Session->State;
    Copy->VolumeSequence = Session->VolumeSequence;
    Copy->PropertySequence = Session->PropertySequence;
    Copy->Volume = Session->Volume;
    Copy->Mute = Session->Mute;
    CopyMemory(Copy->ChannelVolume, Session->ChannelVolume, sizeof(Copy->ChannelVolume));
    StringCchCopyW(Copy->DisplayName, AE_PATH_LENGTH, Session->DisplayName);
    StringCchCopyW(Copy->IconPath, AE_PATH_LENGTH, Session->IconPath);
    Copy->GroupingParam = Session->GroupingParam;
}

/* Copies what the watch thread compares against */
static
VOID
MmdTakeSnapshot(
    _In_ const AE_STATE *State,
    _Out_ MMD_SEEN *Seen)
{
    ULONG Index;

    Seen->Valid = TRUE;
    Seen->EndpointCount = min(State->EndpointCount, (ULONG)AE_MAX_ENDPOINTS);
    for (Index = 0; Index < Seen->EndpointCount; Index++)
    {
        StringCchCopyW(Seen->Ids[Index], AE_ID_LENGTH, State->Endpoints[Index].Id);
        Seen->DeviceState[Index] = State->Endpoints[Index].DeviceState;
        Seen->VolumeSequence[Index] = State->Endpoints[Index].VolumeSequence;
    }
    CopyMemory(Seen->Defaults, State->DefaultEndpoint, sizeof(Seen->Defaults));

    for (Index = 0; Index < AE_MAX_SESSIONS; Index++)
        MmdCopySession(&State->Sessions[Index], &Seen->Sessions[Index]);
}

static
VOID
MmdNotifyEndpoints(
    _In_ IMMNotificationClient *Client,
    _In_ const MMD_SEEN *Old,
    _In_ const MMD_SEEN *New)
{
    ULONG Index, Flow, Role, Endpoint;
    BOOL Known;

    for (Index = 0; Index < New->EndpointCount; Index++)
    {
        Known = Index < Old->EndpointCount && !_wcsicmp(Old->Ids[Index], New->Ids[Index]);
        if (!Known)
        {
            Client->OnDeviceAdded(New->Ids[Index]);
            Client->OnDeviceStateChanged(New->Ids[Index], New->DeviceState[Index]);
        }
        else if (Old->DeviceState[Index] != New->DeviceState[Index])
        {
            Client->OnDeviceStateChanged(New->Ids[Index], New->DeviceState[Index]);
        }
    }

    for (Flow = 0; Flow < 2; Flow++)
    {
        for (Role = 0; Role < AE_ROLE_COUNT; Role++)
        {
            Endpoint = New->Defaults[Flow][Role];
            if (Old->Defaults[Flow][Role] == Endpoint &&
                (Endpoint >= New->EndpointCount || !_wcsicmp(Old->Ids[Endpoint], New->Ids[Endpoint])))
            {
                continue;
            }
            Client->OnDefaultDeviceChanged((EDataFlow)Flow, (ERole)Role,
                                           Endpoint < New->EndpointCount ? New->Ids[Endpoint] : NULL);
        }
    }
}

static
VOID
MmdNotifyEndpointVolume(
    _In_ IAudioEndpointVolumeCallback *Callback,
    _In_ const AE_ENDPOINT_STATE *Endpoint,
    _Inout_ PLONG Seen)
{
    BYTE Buffer[sizeof(AUDIO_VOLUME_NOTIFICATION_DATA) + AE_MAX_CHANNELS * sizeof(float)];
    PAUDIO_VOLUME_NOTIFICATION_DATA Data = (PAUDIO_VOLUME_NOTIFICATION_DATA)Buffer;
    AE_VOLUME_RECORD Record;
    LONG Sequence, Last = Endpoint->VolumeSequence;
    ULONG Channel;

    /* Each update is reported on its own; ones that fell out of the history become the latest */
    if (Last - *Seen > AE_VOLUME_HISTORY)
        *Seen = Last - 1;

    for (Sequence = *Seen + 1; Sequence - Last <= 0; Sequence++)
    {
        Record = Endpoint->VolumeHistory[(ULONG)Sequence % AE_VOLUME_HISTORY];
        if (Record.Sequence != Sequence)
        {
            Record.Context = Endpoint->VolumeContext;
            Record.MasterScalar = Endpoint->MasterScalar;
            Record.Mute = Endpoint->Mute;
            CopyMemory(Record.ChannelScalar, Endpoint->ChannelScalar, sizeof(Record.ChannelScalar));
            Sequence = Last;
        }

        Data->guidEventContext = Record.Context;
        Data->bMuted = Record.Mute != 0;
        Data->fMasterVolume = Record.MasterScalar;
        Data->nChannels = min(Endpoint->Channels, (ULONG)AE_MAX_CHANNELS);
        for (Channel = 0; Channel < Data->nChannels; Channel++)
            Data->afChannelVolumes[Channel] = Record.ChannelScalar[Channel];
        Callback->OnNotify(Data);
    }
    *Seen = Last;
}

static
VOID
MmdNotifySession(
    _In_ PMMD_WATCH Watch,
    _In_ IAudioSessionEvents *Events,
    _In_ const AE_STATE *State,
    _In_ const MMD_SESSION_SEEN *Old,
    _In_ const MMD_SESSION_SEEN *New)
{
    const AE_SESSION_STATE *Session = &State->Sessions[Watch->Index];
    float Volumes[AE_MAX_CHANNELS];
    ULONG Channel, Changed, Count;

    if (Watch->Disconnected)
        return;

    if (!New->InUse || New->Generation != Watch->Generation)
    {
        Watch->Disconnected = TRUE;
        Events->OnSessionDisconnected(DisconnectReasonSessionDisconnected);
        return;
    }
    if (!MmdIsEndpointActive(Session->Endpoint))
    {
        Watch->Disconnected = TRUE;
        Events->OnSessionDisconnected(DisconnectReasonDeviceRemoval);
        return;
    }

    /* A slot that was reused since the last look has no old values worth comparing */
    if (Old->Generation != New->Generation)
        return;

    if (Old->PropertySequence != New->PropertySequence)
    {
        if (wcscmp(Old->DisplayName, New->DisplayName))
            Events->OnDisplayNameChanged(New->DisplayName, &Session->PropertyContext);
        if (wcscmp(Old->IconPath, New->IconPath))
            Events->OnIconPathChanged(New->IconPath, &Session->PropertyContext);
        if (!IsEqualGUID(Old->GroupingParam, New->GroupingParam))
            Events->OnGroupingParamChanged(&New->GroupingParam, &Session->PropertyContext);
    }

    if (Old->VolumeSequence != New->VolumeSequence)
    {
        if (Old->Volume != New->Volume || Old->Mute != New->Mute)
            Events->OnSimpleVolumeChanged(New->Volume, New->Mute, &Session->VolumeContext);

        Count = min(Session->Channels, (ULONG)AE_MAX_CHANNELS);
        Changed = 0;
        for (Channel = 0; Channel < Count; Channel++)
        {
            Volumes[Channel] = New->ChannelVolume[Channel];
            if (Old->ChannelVolume[Channel] != New->ChannelVolume[Channel])
                Changed = Changed ? (DWORD)-1 : Channel + 1;
        }
        if (Changed)
        {
            Events->OnChannelVolumeChanged(Count, Volumes, (Changed == (DWORD)-1) ? (DWORD)-1 : Changed - 1,
                                           &Session->VolumeContext);
        }
    }

    if (Old->State != New->State)
        Events->OnStateChanged((AudioSessionState)New->State);
}

static
VOID
MmdNotifySessionCreated(
    _In_ PMMD_WATCH Watch,
    _In_ IAudioSessionNotification *Notification,
    _In_ const AE_STATE *State,
    _In_ const MMD_SEEN *Old,
    _In_ const MMD_SEEN *New)
{
    IAudioSessionControl *Control;
    ULONG Index;

    for (Index = 0; Index < AE_MAX_SESSIONS; Index++)
    {
        if (!New->Sessions[Index].InUse || State->Sessions[Index].Endpoint != Watch->Index)
            continue;
        if (Old->Sessions[Index].InUse && Old->Sessions[Index].Generation == New->Sessions[Index].Generation)
            continue;
        if (SUCCEEDED(MmdCreateSessionObject(Index, New->Sessions[Index].Generation, IID_IAudioSessionControl,
                                             (VOID **)&Control)))
        {
            Notification->OnSessionCreated(Control);
            Control->Release();
        }
    }
}

static
VOID
MmdDispatchChanges(VOID)
{
    static MMD_SEEN New;
    const AE_STATE *State = MmdGetState();
    PMMD_WATCH *Watches = NULL;
    PLIST_ENTRY Entry;
    ULONG Count = 0, Index;
    PMMD_WATCH Watch;

    if (!State)
        return;

    MmdTakeSnapshot(State, &New);
    if (!MmdSeen.Valid)
    {
        MmdSeen = New;
        return;
    }

    /* Callbacks run without the lock so they may register and unregister themselves */
    EnterCriticalSection(&MmdWatchLock);
    for (Entry = MmdWatches.Flink; Entry != &MmdWatches; Entry = Entry->Flink)
        Count++;
    if (Count)
        Watches = (PMMD_WATCH *)HeapAlloc(GetProcessHeap(), 0, Count * sizeof(*Watches));
    Count = 0;
    if (Watches)
    {
        for (Entry = MmdWatches.Flink; Entry != &MmdWatches; Entry = Entry->Flink)
        {
            Watch = CONTAINING_RECORD(Entry, MMD_WATCH, Link);
            Watch->Busy = TRUE;
            Watches[Count++] = Watch;
        }
    }
    LeaveCriticalSection(&MmdWatchLock);

    for (Index = 0; Index < Count; Index++)
    {
        Watch = Watches[Index];
        if (!Watch->Link.Flink)
            continue;

        switch (Watch->Kind)
        {
            case MmdWatchEndpoints:
                MmdNotifyEndpoints((IMMNotificationClient *)Watch->Callback, &MmdSeen, &New);
                break;

            case MmdWatchEndpointVolume:
                if (Watch->Index < New.EndpointCount && Watch->VolumeSequence != New.VolumeSequence[Watch->Index])
                {
                    MmdNotifyEndpointVolume((IAudioEndpointVolumeCallback *)Watch->Callback,
                                            &State->Endpoints[Watch->Index],
                                            &Watch->VolumeSequence);
                }
                break;

            case MmdWatchSessionEvents:
                MmdNotifySession(Watch, (IAudioSessionEvents *)Watch->Callback, State,
                                 &Watch->Session, &New.Sessions[Watch->Index]);
                Watch->Session = New.Sessions[Watch->Index];
                break;

            case MmdWatchSessionCreated:
                MmdNotifySessionCreated(Watch, (IAudioSessionNotification *)Watch->Callback, State, &MmdSeen, &New);
                break;
        }
    }

    /* A watch removed while its callback ran is freed here, after the last use of it */
    EnterCriticalSection(&MmdWatchLock);
    for (Index = 0; Index < Count; Index++)
    {
        Watch = Watches[Index];
        Watch->Busy = FALSE;
        if (!Watch->Link.Flink)
        {
            Watch->Callback->Release();
            HeapFree(GetProcessHeap(), 0, Watch);
        }
    }
    LeaveCriticalSection(&MmdWatchLock);

    if (Watches)
        HeapFree(GetProcessHeap(), 0, Watches);
    MmdSeen = New;
}

static
DWORD
WINAPI
MmdWatchThreadProc(
    _In_ LPVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;)
    {
        if (WaitForSingleObject(MmdWatchEvent, MMD_WATCH_POLL_MS) == WAIT_TIMEOUT)
            MmdCheckConnection();
        MmdDispatchChanges();
    }
}

static
HRESULT
MmdStartWatching(VOID)
{
    HMODULE Self;

    if (MmdWatchThread)
        return S_OK;

    MmdWatchEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!MmdWatchEvent)
        return HRESULT_FROM_WIN32(GetLastError());

    /* The thread never ends, so the module must stay for as long as the process */
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)MmdStartWatching, &Self);

    EnterCriticalSection(&MmdLock);
    if (MmdPipe != INVALID_HANDLE_VALUE)
        MmdSendWatch();
    else
        MmdConnect();
    if (MmdPipe != INVALID_HANDLE_VALUE)
        MmdTakeSnapshot(MmdState, &MmdSeen);
    LeaveCriticalSection(&MmdLock);

    MmdWatchThread = CreateThread(NULL, 0, MmdWatchThreadProc, NULL, 0, NULL);
    if (!MmdWatchThread)
    {
        CloseHandle(MmdWatchEvent);
        MmdWatchEvent = NULL;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

HRESULT
MmdAddWatch(
    _In_ MMD_WATCH_KIND Kind,
    _In_ ULONG Index,
    _In_ ULONG Generation,
    _In_ IUnknown *Callback)
{
    const AE_STATE *State;
    PMMD_WATCH Watch;
    HRESULT Hr;

    Watch = (PMMD_WATCH)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Watch));
    if (!Watch)
        return E_OUTOFMEMORY;
    Watch->Kind = Kind;
    Watch->Index = Index;
    Watch->Generation = Generation;
    Watch->Callback = Callback;
    Callback->AddRef();

    EnterCriticalSection(&MmdWatchLock);
    Hr = MmdStartWatching();
    if (SUCCEEDED(Hr))
    {
        /* The watch starts from how things are now, not from the last look of the thread */
        State = MmdGetState();
        if (State && Kind == MmdWatchEndpointVolume && Index < AE_MAX_ENDPOINTS)
            Watch->VolumeSequence = State->Endpoints[Index].VolumeSequence;
        else if (State && Kind == MmdWatchSessionEvents && Index < AE_MAX_SESSIONS)
            MmdCopySession(&State->Sessions[Index], &Watch->Session);
        InsertTailList(&MmdWatches, &Watch->Link);
    }
    LeaveCriticalSection(&MmdWatchLock);

    if (FAILED(Hr))
    {
        Callback->Release();
        HeapFree(GetProcessHeap(), 0, Watch);
    }
    return Hr;
}

HRESULT
MmdRemoveWatch(
    _In_ MMD_WATCH_KIND Kind,
    _In_ ULONG Index,
    _In_ IUnknown *Callback)
{
    PLIST_ENTRY Entry;
    PMMD_WATCH Watch;

    EnterCriticalSection(&MmdWatchLock);
    for (Entry = MmdWatches.Flink; Entry != &MmdWatches; Entry = Entry->Flink)
    {
        Watch = CONTAINING_RECORD(Entry, MMD_WATCH, Link);
        if (Watch->Kind != Kind || Watch->Callback != Callback || (Kind != MmdWatchEndpoints && Watch->Index != Index))
            continue;

        RemoveEntryList(&Watch->Link);
        Watch->Link.Flink = NULL;

        /* The watch thread frees it once the callbacks it is running return */
        if (!Watch->Busy)
        {
            Callback->Release();
            HeapFree(GetProcessHeap(), 0, Watch);
        }
        LeaveCriticalSection(&MmdWatchLock);
        return S_OK;
    }
    LeaveCriticalSection(&MmdWatchLock);
    return E_NOTFOUND;
}

VOID
MmdInitialize(VOID)
{
    InitializeCriticalSection(&MmdLock);
    InitializeCriticalSection(&MmdWatchLock);
    InitializeListHead(&MmdWatches);
}

VOID
MmdUninitialize(VOID)
{
    MmdDisconnect();
    DeleteCriticalSection(&MmdWatchLock);
    DeleteCriticalSection(&MmdLock);
}
