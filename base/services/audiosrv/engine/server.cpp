/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client connections, streams, sessions and the published engine state
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "engine.h"
#include <new>

/* Who may do what: clients use the pipe and read the state, only the engine writes it */
#define AE_PIPE_SDDL        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)(A;;GRGW;;;RC)"
#define AE_SECTION_SDDL     L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)(A;;GR;;;RC)"

#define AE_HOUSEKEEPING_MS  100

/* Sessions one process may hold, so no one process fills the table */
#define AE_CONNECTION_SESSIONS  16

/* Rescans a client may ask for, at most one each this often */
#define AE_RESCAN_INTERVAL_MS   1000

/* How far up its parents a process is followed for process loopback */
#define AE_PROCESS_TREE_DEPTH   10
#define AE_RESCAN_MS        5000

/* After a device arrives or leaves, the kernel side lists it on its own timer, so the engine
   looks again for a while */
#define AE_RESCAN_WINDOW_MS 20000
#define AE_EXPIRED_KEEP_MS  3000

/* Longest shared buffer a stream gets; 384 kHz with eight 64 bit channels stays near 250 MB */
#define AE_MAX_BUFFER_100NS     (10 * 10000000LL)

/* Shared buffers hold at least two pipe periods, a pipe period being a tenth over the engine's */
#define AE_MIN_BUFFER_PERIODS   2
#define AE_PIPE_PERIOD_SCALE    1.1

/* The session system sounds play in, which every render endpoint has from the start */
static const GUID AeSystemSoundsSession =
    { 0xa9ef3fd9, 0x4240, 0x455e, { 0xa4, 0xd5, 0xf2, 0xb3, 0x30, 0x18, 0x87, 0xb2 } };
#define AE_SYSTEM_SOUNDS_NAME   L"@%SystemRoot%\\System32\\audiosrv.exe,-202"

struct AE_CONNECTION
{
    LIST_ENTRY Link;
    HANDLE Pipe;
    HANDLE Process;
    ULONG ProcessId;
    HANDLE ChangeEvent;
    ULONG SessionRefs[AE_MAX_SESSIONS];
    ULONG SessionCount;
};

/* Engine side bookkeeping of a session, beside what is published */
typedef struct _AE_SESSION_RUNTIME
{
    ULONG Streams;
    ULONG RunningStreams;
    ULONG References;
    DWORD ExpiredSince;
} AE_SESSION_RUNTIME;

CRITICAL_SECTION AeLock;
LIST_ENTRY AeStreams;
PAE_STATE AeState;
PAE_STREAM AeLoopbacks[AE_MAX_LOOPBACKS];
volatile LONG AeLoopbackStamp;

static LIST_ENTRY AeConnections;
static AE_SESSION_RUNTIME AeSessionRuntime[AE_MAX_SESSIONS];
static HANDLE AeStateSection;
static HANDLE AeListener;
static HANDLE AeHousekeepingThread;
static volatile LONG AeStopping;
static ULONG AeNextStreamId = 1;
static volatile DWORD AeRescanUntil;

static
VOID
AeRescan(VOID);
static PSECURITY_DESCRIPTOR AePipeSecurity;

VOID
AeSignalChange(VOID)
{
    PLIST_ENTRY Entry;
    AE_CONNECTION *Connection;

    EnterCriticalSection(&AeLock);
    for (Entry = AeConnections.Flink; Entry != &AeConnections; Entry = Entry->Flink)
    {
        Connection = CONTAINING_RECORD(Entry, AE_CONNECTION, Link);
        if (Connection->ChangeEvent)
            SetEvent(Connection->ChangeEvent);
    }
    LeaveCriticalSection(&AeLock);
}

VOID
AeSignalStream(
    _In_ PAE_STREAM Stream)
{
    SetEvent(Stream->ClientEvent ? Stream->ClientEvent : Stream->Event);
}

static
ULONG
AeParentProcessId(
    _In_ ULONG ProcessId)
{
    PROCESS_BASIC_INFORMATION Information;
    HANDLE Process;
    NTSTATUS Status;

    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);
    if (!Process)
        return 0;
    Status = NtQueryInformationProcess(Process, ProcessBasicInformation, &Information, sizeof(Information), NULL);
    CloseHandle(Process);
    return NT_SUCCESS(Status) ? (ULONG)Information.InheritedFromUniqueProcessId : 0;
}

static
BOOL
AeIsInProcessTree(
    _In_ ULONG ProcessId,
    _In_ ULONG Root)
{
    ULONG Depth, Parent;

    for (Depth = 0; Depth < AE_PROCESS_TREE_DEPTH && ProcessId; Depth++)
    {
        if (ProcessId == Root)
            return TRUE;
        Parent = AeParentProcessId(ProcessId);
        if (Parent == ProcessId)
            break;
        ProcessId = Parent;
    }
    return FALSE;
}

/* Whether a render stream's audio belongs in a process loopback */
BOOL
AeStreamMatchesLoopback(
    _In_ PAE_STREAM Stream,
    _In_ PAE_STREAM Loopback)
{
    BOOL InTree;

    /* A capturing process never hears itself through its own loopback in exclude mode */
    if (!Loopback->IncludeTree && Stream->ProcessId == Loopback->ProcessId)
        return FALSE;

    InTree = AeIsInProcessTree(Stream->ProcessId, Loopback->TargetProcessId);
    return Loopback->IncludeTree ? InTree : !InTree;
}

/* Publishes a session's activity after its running streams changed */
static
VOID
AeUpdateSessionState(
    _In_ ULONG Session)
{
    PAE_SESSION_STATE State = &AeState->Sessions[Session];
    ULONG NewState = AeSessionRuntime[Session].RunningStreams ? AudioSessionStateActive : AudioSessionStateInactive;

    if (State->State == NewState)
        return;
    State->State = NewState;
    InterlockedIncrement(&State->StateSequence);
    AeSignalChange();
}

/* A session nothing refers to any more expires, and its slot is freed a little later */
static
VOID
AeReleaseSessionIfUnused(
    _In_ ULONG Session)
{
    AE_SESSION_RUNTIME *Runtime = &AeSessionRuntime[Session];
    PAE_SESSION_STATE State = &AeState->Sessions[Session];

    if (Runtime->Streams || Runtime->References || State->State == AudioSessionStateExpired)
        return;
    State->State = AudioSessionStateExpired;
    InterlockedIncrement(&State->StateSequence);
    Runtime->ExpiredSince = GetTickCount();
    AeSignalChange();
}

/* The session of a process, or a shared one, on an endpoint; made if there is none */
static
ULONG
AeFindOrCreateSession(
    _In_ ULONG Endpoint,
    _In_ const GUID *SessionGuid,
    _In_ BOOL CrossProcess,
    _In_ ULONG ProcessId)
{
    PAE_SESSION_STATE State;
    ULONG Index, Free = AE_MAX_SESSIONS, Channel;

    for (Index = 0; Index < AE_MAX_SESSIONS; Index++)
    {
        State = &AeState->Sessions[Index];
        if (!State->InUse)
        {
            if (Free == AE_MAX_SESSIONS)
                Free = Index;
            continue;
        }
        if (State->Endpoint == Endpoint && IsEqualGUID(State->SessionGuid, *SessionGuid) &&
            State->CrossProcess == (ULONG)CrossProcess && (CrossProcess || State->ProcessId == ProcessId))
        {
            if (State->State == AudioSessionStateExpired)
            {
                State->State = AudioSessionStateInactive;
                InterlockedIncrement(&State->StateSequence);
                AeSignalChange();
            }
            return Index;
        }
    }

    if (Free == AE_MAX_SESSIONS)
        return AE_MAX_SESSIONS;

    State = &AeState->Sessions[Free];
    State->Generation++;
    State->Endpoint = Endpoint;
    State->SessionGuid = *SessionGuid;
    State->ProcessId = CrossProcess ? 0 : ProcessId;
    State->CrossProcess = CrossProcess;
    State->SystemSounds = CrossProcess && IsEqualGUID(*SessionGuid, AeSystemSoundsSession);
    if (State->SystemSounds)
    {
        State->ProcessId = GetCurrentProcessId();
        StringCchCopyW(State->DisplayName, ARRAYSIZE(State->DisplayName), AE_SYSTEM_SOUNDS_NAME);
    }
    State->State = AudioSessionStateInactive;
    State->Volume = 1.0f;
    State->Mute = FALSE;
    State->Channels = AeEndpoints[Endpoint].MixFormat.Channels;
    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
        State->ChannelVolume[Channel] = 1.0f;
    if (!State->SystemSounds)
        State->DisplayName[0] = UNICODE_NULL;
    State->IconPath[0] = UNICODE_NULL;
    State->GroupingParam = GUID_NULL;
    State->Peak = 0.0f;
    State->InUse = TRUE;
    ZeroMemory(&AeSessionRuntime[Free], sizeof(AeSessionRuntime[Free]));

    InterlockedIncrement(&AeState->SessionSequence);
    AeSignalChange();
    return Free;
}

/* Counts a connection's hold on a session, refusing a process more sessions than its share */
static
BOOL
AeReferenceSession(
    _In_ AE_CONNECTION *Connection,
    _In_ ULONG Session)
{
    if (!Connection->SessionRefs[Session])
    {
        if (Connection->SessionCount >= AE_CONNECTION_SESSIONS)
            return FALSE;
        Connection->SessionCount++;
        AeSessionRuntime[Session].References++;
    }
    Connection->SessionRefs[Session]++;
    return TRUE;
}

static
VOID
AeFreeStream(
    _In_ PAE_STREAM Stream)
{
    if (Stream->Shared)
        UnmapViewOfFile(Stream->Shared);
    if (Stream->Section)
        CloseHandle(Stream->Section);
    if (Stream->Event)
        CloseHandle(Stream->Event);
    if (Stream->ClientEvent)
        CloseHandle(Stream->ClientEvent);
    if (Stream->Scratch)
        HeapFree(GetProcessHeap(), 0, Stream->Scratch);
    if (Stream->Converted)
        HeapFree(GetProcessHeap(), 0, Stream->Converted);
    if (Stream->Accumulator)
        HeapFree(GetProcessHeap(), 0, Stream->Accumulator);
    delete Stream;
}

/* Unlinks a stream from everything that counts it; the engine lock must be held */
static
VOID
AeCloseStream(
    _In_ PAE_STREAM Stream)
{
    AE_SESSION_RUNTIME *Runtime = &AeSessionRuntime[Stream->Session];

    RemoveEntryList(&Stream->Link);
    if (Stream->Kind == AE_STREAM_PROCESS_LOOPBACK && AeLoopbacks[Stream->LoopbackSlot] == Stream)
    {
        AeLoopbacks[Stream->LoopbackSlot] = NULL;
        InterlockedIncrement(&AeLoopbackStamp);
    }

    if (Stream->Running && Runtime->RunningStreams)
        Runtime->RunningStreams--;
    if (Runtime->Streams)
        Runtime->Streams--;
    AeUpdateSessionState(Stream->Session);
    AeReleaseSessionIfUnused(Stream->Session);

    AeEndpointStreamRemoved(Stream->Endpoint);
    AeFreeStream(Stream);
}

static
PAE_STREAM
AeFindStream(
    _In_ AE_CONNECTION *Connection,
    _In_ ULONG Id)
{
    PLIST_ENTRY Entry;
    PAE_STREAM Stream;

    for (Entry = AeStreams.Flink; Entry != &AeStreams; Entry = Entry->Flink)
    {
        Stream = CONTAINING_RECORD(Entry, AE_STREAM, Link);
        if (Stream->Id == Id && Stream->Owner == Connection)
            return Stream;
    }
    return NULL;
}

static
BOOL
AeDuplicateToClient(
    _In_ AE_CONNECTION *Connection,
    _In_ HANDLE Handle,
    _In_ DWORD Access,
    _Out_ ULONG64 *Remote)
{
    HANDLE Duplicate = NULL;

    if (!DuplicateHandle(GetCurrentProcess(), Handle, Connection->Process, &Duplicate, Access, FALSE, 0))
        return FALSE;
    *Remote = (ULONG64)(ULONG_PTR)Duplicate;
    return TRUE;
}

/* The ring of a shared mode stream: what it asked for, but at least two pipe periods */
static
ULONG
AeBufferFrames(
    _In_ const AE_FORMAT *Format,
    _In_ LONGLONG Duration)
{
    ULONG Frames = (ULONG)((double)max(Duration, 0) * Format->SamplesPerSec / 10000000.0 + 0.5);
    ULONG Pipe = (ULONG)((double)(ULONG)((double)AE_PERIOD_100NS * Format->SamplesPerSec / 10000000.0 + 0.5) *
                         AE_PIPE_PERIOD_SCALE);

    return max(Frames, Pipe * AE_MIN_BUFFER_PERIODS);
}

static
HRESULT
AeOpenStream(
    _In_ AE_CONNECTION *Connection,
    _In_ const AE_OPEN_STREAM_REQUEST *Request,
    _Out_ AE_OPEN_STREAM_REPLY *Reply)
{
    PAE_ENDPOINT Device = NULL;
    PAE_STREAM Stream;
    ULONG Kind = Request->Kind, Endpoint, Session, Slot = 0, Channel;
    ULONG ClientPeriodFrames, SharedBytes, RingBytes, ScratchFrames;
    AE_FORMAT Format;
    WCHAR EndpointId[AE_ID_LENGTH];
    BOOL Resampled;
    BOOL CrossProcess = (Request->StreamFlags & AUDCLNT_STREAMFLAGS_CROSSPROCESS) != 0;

    if (Kind != AE_STREAM_RENDER && Kind != AE_STREAM_CAPTURE &&
        Kind != AE_STREAM_LOOPBACK && Kind != AE_STREAM_PROCESS_LOOPBACK)
    {
        return E_INVALIDARG;
    }
    if (!AeDecodeFormat((const WAVEFORMATEX *)Request->Format, &Format))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    /* Process loopback listens to the default render endpoint */
    if (Kind == AE_STREAM_PROCESS_LOOPBACK)
    {
        Endpoint = AeState->DefaultEndpoint[AE_FLOW_RENDER][eConsole];
        if (Endpoint == AE_NO_ENDPOINT)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        Device = &AeEndpoints[Endpoint];
        for (Slot = 0; Slot < AE_MAX_LOOPBACKS && AeLoopbacks[Slot]; Slot++);
        if (Slot == AE_MAX_LOOPBACKS)
            return E_OUTOFMEMORY;
    }
    else
    {
        if (FAILED(StringCchCopyNW(EndpointId, ARRAYSIZE(EndpointId), Request->EndpointId, ARRAYSIZE(Request->EndpointId))))
            return AUDCLNT_E_DEVICE_INVALIDATED;
        Device = AeFindEndpoint(EndpointId);
        if (!Device || !Device->Present)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if ((Kind == AE_STREAM_CAPTURE) != (Device->Flow == AE_FLOW_CAPTURE))
            return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        Endpoint = Device->Index;
    }

    Session = AeFindOrCreateSession(Endpoint, &Request->SessionGuid, CrossProcess, Connection->ProcessId);
    if (Session == AE_MAX_SESSIONS)
        return E_OUTOFMEMORY;
    if (!AeReferenceSession(Connection, Session))
    {
        AeReleaseSessionIfUnused(Session);
        return E_OUTOFMEMORY;
    }

    Stream = new (std::nothrow) AE_STREAM();
    if (!Stream)
    {
        AeReleaseSessionIfUnused(Session);
        return E_OUTOFMEMORY;
    }

    Stream->Id = AeNextStreamId++;
    Stream->Kind = Kind;
    Stream->Endpoint = Endpoint;
    Stream->Session = Session;
    Stream->Owner = Connection;
    Stream->ProcessId = Connection->ProcessId;
    Stream->EventDriven = (Request->StreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
    Stream->Format = Format;
    Stream->TargetProcessId = Request->TargetProcessId;
    Stream->IncludeTree = Request->IncludeProcessTree;

    ClientPeriodFrames = (ULONG)((ULONGLONG)Format.SamplesPerSec * AE_PERIOD_100NS / 10000000);

    /* Rendering converts the client's format into the mix; everything else the other way round */
    if (Kind == AE_STREAM_RENDER)
    {
        Stream->ChannelMap.Initialize(Format.Channels, Format.ChannelMask,
                                      Device->MixFormat.Channels, Device->MixFormat.ChannelMask);
        Resampled = Stream->Resampler.Initialize(Device->MixFormat.Channels, Format.SamplesPerSec,
                                                 Device->MixFormat.SamplesPerSec);
    }
    else
    {
        Stream->ChannelMap.Initialize(Device->MixFormat.Channels, Device->MixFormat.ChannelMask,
                                      Format.Channels, Format.ChannelMask);
        Resampled = Stream->Resampler.Initialize(Format.Channels, Device->MixFormat.SamplesPerSec, Format.SamplesPerSec);
    }

    ScratchFrames = (ULONG)((ULONGLONG)max(Format.SamplesPerSec, Device->MixFormat.SamplesPerSec) * 2 * AE_PERIOD_100NS /
                            10000000) + 64;
    Stream->ScratchFrames = ScratchFrames;
    Stream->Scratch = (float *)HeapAlloc(GetProcessHeap(), 0, ScratchFrames * AE_MAX_CHANNELS * sizeof(float));
    Stream->Converted = (float *)HeapAlloc(GetProcessHeap(), 0, ScratchFrames * AE_MAX_CHANNELS * sizeof(float));
    if (Kind == AE_STREAM_PROCESS_LOOPBACK)
    {
        Stream->Accumulator = (float *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 Device->PeriodFrames * Device->MixFormat.Channels * sizeof(float));
    }

    Reply->BufferFrames = AeBufferFrames(&Format, min(Request->BufferDuration, (LONGLONG)AE_MAX_BUFFER_100NS));
    RingBytes = Reply->BufferFrames * Format.BlockAlign;
    SharedBytes = (sizeof(AE_STREAM_SHARED) + 63) & ~63UL;

    Stream->Section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, SharedBytes + 2 * RingBytes, NULL);
    Stream->Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (Stream->Section)
        Stream->Shared = (PAE_STREAM_SHARED)MapViewOfFile(Stream->Section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!Stream->Scratch || !Stream->Converted || !Stream->Shared || !Stream->Event || !Resampled ||
        (Kind == AE_STREAM_PROCESS_LOOPBACK && !Stream->Accumulator) ||
        !AeDuplicateToClient(Connection, Stream->Section, FILE_MAP_READ | FILE_MAP_WRITE, &Reply->Section))
    {
        AeFreeStream(Stream);
        return E_OUTOFMEMORY;
    }
    if (!AeDuplicateToClient(Connection, Stream->Event, SYNCHRONIZE, &Reply->Event))
    {
        /* Takes back the section handle the client already holds */
        DuplicateHandle(Connection->Process, (HANDLE)(ULONG_PTR)Reply->Section, NULL, NULL, 0, FALSE,
                        DUPLICATE_CLOSE_SOURCE);
        AeFreeStream(Stream);
        return E_OUTOFMEMORY;
    }

    Stream->Ring = (BYTE *)Stream->Shared + SharedBytes;
    Stream->BufferFrames = Reply->BufferFrames;
    Stream->FrameBytes = Format.BlockAlign;
    Stream->Shared->Magic = AE_STREAM_MAGIC;
    Stream->Shared->BufferFrames = Reply->BufferFrames;
    Stream->Shared->FrameBytes = Format.BlockAlign;
    Stream->Shared->PeriodFrames = ClientPeriodFrames;
    Stream->Shared->RingOffset = SharedBytes;
    Stream->Shared->StagingOffset = SharedBytes + RingBytes;
    Stream->Shared->Channels = Format.Channels;
    Stream->Shared->SamplesPerSec = Format.SamplesPerSec;
    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
        Stream->Shared->Volume[Channel] = 1.0f;

    /* What is queued in the device on top of the client's buffer */
    Stream->LatencyFrames = (LONGLONG)AE_DEVICE_BUFFERS * ClientPeriodFrames;

    if (Kind == AE_STREAM_PROCESS_LOOPBACK)
    {
        Stream->LoopbackSlot = Slot;
        AeLoopbacks[Slot] = Stream;
        InterlockedIncrement(&AeLoopbackStamp);
    }

    /* A session outlives its streams for as long as the process that opened them is connected */
    InsertTailList(&AeStreams, &Stream->Link);
    AeSessionRuntime[Session].Streams++;
    AeEndpointStreamAdded(Endpoint);

    Reply->StreamId = Stream->Id;
    Reply->PeriodFrames = ClientPeriodFrames;
    Reply->Session = Session;
    Reply->SessionGeneration = AeState->Sessions[Session].Generation;
    Reply->Latency = (LONGLONG)AE_DEVICE_BUFFERS * AE_PERIOD_100NS;
    return S_OK;
}

static
HRESULT
AeStartStream(
    _In_ PAE_STREAM Stream,
    _In_ BOOL Start)
{
    AE_SESSION_RUNTIME *Runtime = &AeSessionRuntime[Stream->Session];

    if (!!Stream->Running == !!Start)
        return S_OK;

    /* The first packet a capture stream gets after starting is marked as not following on */
    if (Start && Stream->Kind != AE_STREAM_RENDER && Stream->Kind != AE_STREAM_PROCESS_LOOPBACK)
        Stream->Shared->Discontinuity = TRUE;

    Stream->Running = Start;
    Stream->Shared->Running = Start;
    if (Start)
        Runtime->RunningStreams++;
    else if (Runtime->RunningStreams)
        Runtime->RunningStreams--;
    AeUpdateSessionState(Stream->Session);
    return S_OK;
}

static
HRESULT
AeResetStream(
    _In_ PAE_STREAM Stream)
{
    if (Stream->Running)
        return AUDCLNT_E_NOT_STOPPED;

    Stream->Shared->WritePosition = 0;
    Stream->Shared->ReadPosition = 0;
    Stream->Shared->DevicePosition = 0;
    Stream->Shared->Discontinuity = FALSE;
    Stream->Resampler.Reset();
    return S_OK;
}

/*
 * Master changes move every channel by the same number of dB, channel changes move one. An
 * unchanged master scalar or mute is not a change and tells no one.
 */
static
HRESULT
AeSetEndpointVolume(
    _In_ const AE_SET_ENDPOINT_VOLUME_REQUEST *Request)
{
    PAE_VOLUME_RECORD Record;
    LONG Sequence;
    PAE_ENDPOINT_STATE State;
    ULONG Channel;
    float Db, Delta;

    if (Request->Endpoint >= AeEndpointCount)
        return E_INVALIDARG;
    State = &AeState->Endpoints[Request->Endpoint];
    if ((Request->What == AE_SET_CHANNEL_DB || Request->What == AE_SET_CHANNEL_SCALAR) &&
        Request->Channel >= State->Channels)
    {
        return E_INVALIDARG;
    }
    if ((Request->What == AE_SET_MASTER_SCALAR || Request->What == AE_SET_CHANNEL_SCALAR) &&
        !(Request->Value >= 0.0f && Request->Value <= 1.0f))
    {
        return E_INVALIDARG;
    }
    if ((Request->What == AE_SET_MASTER_DB || Request->What == AE_SET_CHANNEL_DB) &&
        !(Request->Value >= AE_VOLUME_MIN_DB && Request->Value <= AE_VOLUME_MAX_DB))
    {
        return E_INVALIDARG;
    }

    switch (Request->What)
    {
        case AE_SET_MASTER_DB:
        case AE_SET_MASTER_SCALAR:
            if (Request->What == AE_SET_MASTER_SCALAR && Request->Value == State->MasterScalar)
                return S_OK;
            Db = (Request->What == AE_SET_MASTER_DB) ? Request->Value : AeScalarToDb(Request->Value);
            Delta = Db - State->MasterLevelDb;
            for (Channel = 0; Channel < State->Channels && Channel < AE_MAX_CHANNELS; Channel++)
            {
                State->ChannelLevelDb[Channel] = min(max(State->ChannelLevelDb[Channel] + Delta, AE_VOLUME_MIN_DB),
                                                     AE_VOLUME_MAX_DB);
            }
            break;

        case AE_SET_CHANNEL_DB:
        case AE_SET_CHANNEL_SCALAR:
            Db = (Request->What == AE_SET_CHANNEL_DB) ? Request->Value : AeScalarToDb(Request->Value);
            State->ChannelLevelDb[Request->Channel] = Db;
            break;

        case AE_SET_MUTE:
            if (!State->Mute == !Request->Mute)
                return S_FALSE;
            State->Mute = Request->Mute != 0;
            break;

        default:
            return E_INVALIDARG;
    }

    AeUpdateEndpointGain(Request->Endpoint);
    AeSaveEndpointVolume(Request->Endpoint);
    State->VolumeContext = Request->Context;

    /* Every update is kept, so a client that looks late still reports each one */
    Sequence = State->VolumeSequence + 1;
    Record = &State->VolumeHistory[(ULONG)Sequence % AE_VOLUME_HISTORY];
    Record->Context = Request->Context;
    Record->MasterScalar = State->MasterScalar;
    Record->Mute = State->Mute;
    CopyMemory(Record->ChannelScalar, State->ChannelScalar, sizeof(Record->ChannelScalar));
    InterlockedExchange(&Record->Sequence, Sequence);
    InterlockedExchange(&State->VolumeSequence, Sequence);
    AeSignalChange();
    return S_OK;
}

static
HRESULT
AeSetSession(
    _In_ const AE_SET_SESSION_REQUEST *Request)
{
    PAE_SESSION_STATE State;

    if (Request->Session >= AE_MAX_SESSIONS)
        return E_INVALIDARG;
    State = &AeState->Sessions[Request->Session];
    if (!State->InUse || State->Generation != Request->Generation)
        return AUDCLNT_E_DEVICE_INVALIDATED;

    switch (Request->What)
    {
        case AE_SET_VOLUME:
            if (Request->Value < 0.0f || Request->Value > 1.0f)
                return E_INVALIDARG;
            State->Volume = Request->Value;
            break;

        case AE_SET_MUTE:
            State->Mute = Request->Mute != 0;
            break;

        case AE_SET_CHANNEL_SCALAR:
            if (Request->Channel >= State->Channels || Request->Value < 0.0f || Request->Value > 1.0f)
                return E_INVALIDARG;
            State->ChannelVolume[Request->Channel] = Request->Value;
            break;

        case AE_SET_DISPLAY_NAME:
            StringCchCopyW(State->DisplayName, ARRAYSIZE(State->DisplayName), Request->Text);
            State->PropertyContext = Request->Context;
            InterlockedIncrement(&State->PropertySequence);
            AeSignalChange();
            return S_OK;

        case AE_SET_ICON_PATH:
            StringCchCopyW(State->IconPath, ARRAYSIZE(State->IconPath), Request->Text);
            State->PropertyContext = Request->Context;
            InterlockedIncrement(&State->PropertySequence);
            AeSignalChange();
            return S_OK;

        case AE_SET_GROUPING:
            State->GroupingParam = Request->Guid;
            State->PropertyContext = Request->Context;
            InterlockedIncrement(&State->PropertySequence);
            AeSignalChange();
            return S_OK;

        default:
            return E_INVALIDARG;
    }

    State->VolumeContext = Request->Context;
    InterlockedIncrement(&State->VolumeSequence);
    AeSignalChange();
    return S_OK;
}

static
HRESULT
AeDispatch(
    _In_ AE_CONNECTION *Connection,
    _In_ const AE_REQUEST *Request,
    _In_ DWORD Size,
    _Out_ AE_REPLY *Reply,
    _Out_ DWORD *ReplySize)
{
    PAE_STREAM Stream;
    HANDLE Event;
    HRESULT Hr;
    ULONG Session;

    *ReplySize = sizeof(AE_REPLY_HEADER);
    if (Size < sizeof(AE_REQUEST_HEADER))
        return E_INVALIDARG;

    switch (Request->Header.Command)
    {
        case AeCommandHello:
            if (Size < sizeof(Request->Hello))
                return E_INVALIDARG;
            if (Request->Hello.Version != AE_PROTOCOL_VERSION)
                return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            if (!Connection->Process)
            {
                /* Only when the pipe did not say who connected */
                Connection->ProcessId = Request->Hello.ProcessId;
                Connection->Process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                                  FALSE, Connection->ProcessId);
            }
            return Connection->Process ? S_OK : E_ACCESSDENIED;

    }

    if (!Connection->Process)
        return E_ACCESSDENIED;

    if (Request->Header.Command == AeCommandRescan)
    {
        static DWORD LastRescan;
        DWORD Now = GetTickCount();

        if (Now - LastRescan < AE_RESCAN_INTERVAL_MS)
            return S_FALSE;
        LastRescan = Now;
        AeDevicesChanged();
        return S_OK;
    }

    EnterCriticalSection(&AeLock);
    switch (Request->Header.Command)
    {
        case AeCommandOpenStream:
            if (Size < sizeof(Request->OpenStream))
            {
                Hr = E_INVALIDARG;
                break;
            }
            ZeroMemory(&Reply->OpenStream, sizeof(Reply->OpenStream));
            Hr = AeOpenStream(Connection, &Request->OpenStream, &Reply->OpenStream);
            *ReplySize = sizeof(Reply->OpenStream);
            break;

        case AeCommandCloseStream:
        case AeCommandStart:
        case AeCommandStop:
        case AeCommandReset:
        case AeCommandSetEvent:
            Stream = (Size >= sizeof(Request->Stream)) ? AeFindStream(Connection, Request->Stream.StreamId) : NULL;
            if (!Stream)
            {
                Hr = E_INVALIDARG;
                break;
            }
            switch (Request->Header.Command)
            {
                case AeCommandCloseStream:
                    AeCloseStream(Stream);
                    Hr = S_OK;
                    break;
                case AeCommandStart:
                    Hr = AeStartStream(Stream, TRUE);
                    break;
                case AeCommandStop:
                    Hr = AeStartStream(Stream, FALSE);
                    break;
                case AeCommandReset:
                    Hr = AeResetStream(Stream);
                    break;
                default:
                    Event = NULL;
                    if (!DuplicateHandle(Connection->Process, (HANDLE)(ULONG_PTR)Request->Stream.Handle,
                                         GetCurrentProcess(), &Event, EVENT_MODIFY_STATE, FALSE, 0))
                    {
                        Hr = E_HANDLE;
                        break;
                    }
                    if (Stream->ClientEvent)
                        CloseHandle(Stream->ClientEvent);
                    Stream->ClientEvent = Event;
                    Hr = S_OK;
                    break;
            }
            break;

        case AeCommandSetEndpointVolume:
            Hr = (Size >= sizeof(Request->SetEndpointVolume)) ? AeSetEndpointVolume(&Request->SetEndpointVolume) : E_INVALIDARG;
            break;

        case AeCommandOpenSession:
            if (Size < sizeof(Request->OpenSession) || Request->OpenSession.Endpoint >= AeEndpointCount)
            {
                Hr = E_INVALIDARG;
                break;
            }
            Session = AeFindOrCreateSession(Request->OpenSession.Endpoint, &Request->OpenSession.SessionGuid,
                                            Request->OpenSession.CrossProcess, Connection->ProcessId);
            if (Session == AE_MAX_SESSIONS)
            {
                Hr = E_OUTOFMEMORY;
                break;
            }
            if (!AeReferenceSession(Connection, Session))
            {
                AeReleaseSessionIfUnused(Session);
                Hr = E_OUTOFMEMORY;
                break;
            }
            Reply->OpenSession.Session = Session;
            Reply->OpenSession.Generation = AeState->Sessions[Session].Generation;
            *ReplySize = sizeof(Reply->OpenSession);
            Hr = S_OK;
            break;

        case AeCommandSetSession:
            Hr = (Size >= sizeof(Request->SetSession)) ? AeSetSession(&Request->SetSession) : E_INVALIDARG;
            break;

        case AeCommandWatch:
            Event = NULL;
            if (Size < sizeof(Request->Stream) ||
                !DuplicateHandle(Connection->Process, (HANDLE)(ULONG_PTR)Request->Stream.Handle,
                                 GetCurrentProcess(), &Event, EVENT_MODIFY_STATE, FALSE, 0))
            {
                Hr = E_HANDLE;
                break;
            }
            if (Connection->ChangeEvent)
                CloseHandle(Connection->ChangeEvent);
            Connection->ChangeEvent = Event;
            Hr = S_OK;
            break;

        case AeCommandSetDefault:
            if (Size < sizeof(Request->SetDefault) || Request->SetDefault.Endpoint >= AeEndpointCount ||
                Request->SetDefault.Role >= AE_ROLE_COUNT || !AeEndpoints[Request->SetDefault.Endpoint].Present)
            {
                Hr = E_INVALIDARG;
                break;
            }
            AeSaveDefault(Request->SetDefault.Endpoint, Request->SetDefault.Role);
            AeLoadDefaults();
            InterlockedIncrement(&AeState->DefaultSequence);
            AeSignalChange();
            Hr = S_OK;
            break;

        default:
            Hr = E_NOTIMPL;
            break;
    }
    LeaveCriticalSection(&AeLock);
    return Hr;
}

/* Everything a client left open goes with its connection */
static
VOID
AeCloseConnection(
    _In_ AE_CONNECTION *Connection)
{
    PLIST_ENTRY Entry, Next;
    PAE_STREAM Stream;
    ULONG Session;

    EnterCriticalSection(&AeLock);
    for (Entry = AeStreams.Flink; Entry != &AeStreams; Entry = Next)
    {
        Next = Entry->Flink;
        Stream = CONTAINING_RECORD(Entry, AE_STREAM, Link);
        if (Stream->Owner == Connection)
            AeCloseStream(Stream);
    }
    for (Session = 0; Session < AE_MAX_SESSIONS; Session++)
    {
        if (!Connection->SessionRefs[Session])
            continue;
        if (AeSessionRuntime[Session].References)
            AeSessionRuntime[Session].References--;
        AeReleaseSessionIfUnused(Session);
    }
    RemoveEntryList(&Connection->Link);
    LeaveCriticalSection(&AeLock);

    DisconnectNamedPipe(Connection->Pipe);
    CloseHandle(Connection->Pipe);
    if (Connection->Process)
        CloseHandle(Connection->Process);
    if (Connection->ChangeEvent)
        CloseHandle(Connection->ChangeEvent);
    HeapFree(GetProcessHeap(), 0, Connection);
}

static
DWORD
WINAPI
AeConnectionThread(
    _In_ LPVOID Context)
{
    AE_CONNECTION *Connection = (AE_CONNECTION *)Context;
    AE_REQUEST Request;
    AE_REPLY Reply;
    DWORD Read, Written, ReplySize;

    for (;;)
    {
        if (!ReadFile(Connection->Pipe, &Request, sizeof(Request), &Read, NULL))
            break;

        ZeroMemory(&Reply, sizeof(Reply.Header));
        Reply.Header.Status = AeDispatch(Connection, &Request, Read, &Reply, &ReplySize);
        Reply.Header.Size = ReplySize;
        if (!WriteFile(Connection->Pipe, &Reply, ReplySize, &Written, NULL))
            break;
    }

    AeCloseConnection(Connection);
    return 0;
}

static
DWORD
WINAPI
AeListenThread(
    _In_ LPVOID Context)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), AePipeSecurity, FALSE };
    AE_CONNECTION *Connection;
    HANDLE Pipe, Thread;
    BOOL Connected;

    UNREFERENCED_PARAMETER(Context);

    while (!AeStopping)
    {
        Pipe = CreateNamedPipeW(AE_PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES, sizeof(AE_REPLY), sizeof(AE_REQUEST), 0,
                                AePipeSecurity ? &Attributes : NULL);
        if (Pipe == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        Connected = ConnectNamedPipe(Pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!Connected || AeStopping)
        {
            CloseHandle(Pipe);
            continue;
        }

        Connection = (AE_CONNECTION *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Connection));
        if (!Connection)
        {
            CloseHandle(Pipe);
            continue;
        }
        Connection->Pipe = Pipe;

        /* The pipe says who connected; the hello message is only trusted when it cannot */
        if (GetNamedPipeClientProcessId(Pipe, &Connection->ProcessId) && Connection->ProcessId)
        {
            Connection->Process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                              FALSE, Connection->ProcessId);
        }

        EnterCriticalSection(&AeLock);
        InsertTailList(&AeConnections, &Connection->Link);
        LeaveCriticalSection(&AeLock);

        Thread = CreateThread(NULL, 0, AeConnectionThread, Connection, 0, NULL);
        if (Thread)
            CloseHandle(Thread);
        else
            AeCloseConnection(Connection);
    }
    return 0;
}

static
DWORD
WINAPI
AeHousekeeping(
    _In_ LPVOID Context)
{
    DWORD LastRescan = GetTickCount();
    ULONG Session;

    UNREFERENCED_PARAMETER(Context);

    while (!AeStopping)
    {
        Sleep(AE_HOUSEKEEPING_MS);

        EnterCriticalSection(&AeLock);
        for (Session = 0; Session < AE_MAX_SESSIONS; Session++)
        {
            if (AeState->Sessions[Session].InUse && AeState->Sessions[Session].State == AudioSessionStateExpired &&
                !AeSessionRuntime[Session].Streams && !AeSessionRuntime[Session].References &&
                GetTickCount() - AeSessionRuntime[Session].ExpiredSince >= AE_EXPIRED_KEEP_MS)
            {
                AeState->Sessions[Session].InUse = FALSE;
            }
        }
        LeaveCriticalSection(&AeLock);

        /* Wave devices show up some time after the service starts, and after each device event */
        if ((!AeEndpointCount || AeWaveRefreshPending || (LONG)(AeRescanUntil - GetTickCount()) > 0) &&
            GetTickCount() - LastRescan >= AE_RESCAN_MS)
        {
            LastRescan = GetTickCount();
            AeRescan();
        }
    }
    return 0;
}

static
HANDLE
AeCreateSection(
    _In_ PCWSTR Name,
    _In_ DWORD Size)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, FALSE };
    PSECURITY_DESCRIPTOR Descriptor = NULL;
    HANDLE Handle;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(AE_SECTION_SDDL, SDDL_REVISION_1, &Descriptor, NULL))
        Attributes.lpSecurityDescriptor = Descriptor;

    Handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &Attributes, PAGE_READWRITE, 0, Size, Name);

    if (Descriptor)
        LocalFree(Descriptor);
    return Handle;
}

/* Gives each render endpoint its system sounds session, held by the engine itself */
static
VOID
AeAddSystemSessions(VOID)
{
    static BOOL Held[AE_MAX_SESSIONS];
    ULONG Index, Session;

    for (Index = 0; Index < AeEndpointCount; Index++)
    {
        if (AeEndpoints[Index].Flow != AE_FLOW_RENDER || !AeEndpoints[Index].Present)
            continue;
        Session = AeFindOrCreateSession(Index, &AeSystemSoundsSession, TRUE, GetCurrentProcessId());
        if (Session != AE_MAX_SESSIONS && !Held[Session])
        {
            Held[Session] = TRUE;
            AeSessionRuntime[Session].References++;
        }
    }
}

/* Rebuilds the endpoints and tells clients */
static
VOID
AeRescan(VOID)
{
    if (!AeState)
        return;

    EnterCriticalSection(&AeLock);
    AeBuildEndpoints();
    AeAddSystemSessions();
    InterlockedIncrement(&AeState->DeviceSequence);
    InterlockedIncrement(&AeState->DefaultSequence);
    AeSignalChange();
    LeaveCriticalSection(&AeLock);
}

/**
 * @brief
 * Rebuilds the endpoints after devices came or went and tells clients, then keeps looking
 * for a while as the drivers below catch up.
 */
EXTERN_C
VOID
AeDevicesChanged(VOID)
{
    AeRescanUntil = GetTickCount() + AE_RESCAN_WINDOW_MS;
    AeRescan();
}

/**
 * @brief
 * Publishes the engine state, builds the endpoints and starts taking clients.
 */
EXTERN_C
BOOL
AeStartEngine(VOID)
{
    InitializeCriticalSection(&AeLock);
    InitializeListHead(&AeStreams);
    InitializeListHead(&AeConnections);

    AeStateSection = AeCreateSection(AE_STATE_SECTION_NAME, sizeof(AE_STATE));
    if (!AeStateSection)
        return FALSE;

    AeState = (PAE_STATE)MapViewOfFile(AeStateSection, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AE_STATE));
    if (!AeState)
        return FALSE;
    ZeroMemory(AeState, sizeof(*AeState));
    AeState->Version = AE_PROTOCOL_VERSION;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(AE_PIPE_SDDL, SDDL_REVISION_1, &AePipeSecurity, NULL))
        AePipeSecurity = NULL;

    EnterCriticalSection(&AeLock);
    AeBuildEndpoints();
    AeAddSystemSessions();
    LeaveCriticalSection(&AeLock);

    /* Clients check the magic before they trust anything else in the section */
    AeState->Magic = AE_STATE_MAGIC;

    AeListener = CreateThread(NULL, 0, AeListenThread, NULL, 0, NULL);
    AeHousekeepingThread = CreateThread(NULL, 0, AeHousekeeping, NULL, 0, NULL);
    return AeListener && AeHousekeepingThread;
}

EXTERN_C
VOID
AeStopEngine(VOID)
{
    InterlockedExchange(&AeStopping, TRUE);
    AeStopAllDevices();
}
