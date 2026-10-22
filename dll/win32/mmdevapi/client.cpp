/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared mode audio clients and their render, capture, clock and volume services
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Stream flags only Windows itself may pass */
#define MMD_RESERVED_STREAM_FLAGS   0x00C00003

/* All a process loopback stream may ask for */
#define MMD_PROCESS_LOOPBACK_FLAGS  (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK)

/* Size of AudioClientProperties before Options was added */
#define MMD_CLIENT_PROPERTIES_V1    12

static
LONGLONG
MmdNow100ns(VOID)
{
    static LARGE_INTEGER Frequency;
    LARGE_INTEGER Counter;

    if (!Frequency.QuadPart)
        QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Counter);
    return (LONGLONG)((double)Counter.QuadPart * 10000000.0 / (double)Frequency.QuadPart);
}

static
LONGLONG
MmdReadPosition(
    _In_ volatile LONGLONG *Position)
{
    return InterlockedCompareExchange64(Position, 0, 0);
}

static
ULONG
MmdFramesFromDuration(
    _In_ LONGLONG Duration,
    _In_ ULONG SamplesPerSec)
{
    return (ULONG)((Duration * SamplesPerSec + 5000000) / 10000000);
}

typedef enum _MMD_FORMAT_CHECK
{
    MmdFormatValid,
    MmdFormatMalformed,
    MmdFormatShortExtensible,
    MmdFormatPlainMultichannel,
    MmdFormatNotPcm,
} MMD_FORMAT_CHECK;

/* Sorts a format the way the audio service validates it, so each caller can pick its error */
static
MMD_FORMAT_CHECK
MmdCheckFormat(
    _In_ const WAVEFORMATEX *Format)
{
    const WAVEFORMATEXTENSIBLE *Extensible = (const WAVEFORMATEXTENSIBLE *)Format;
    WORD Bits = Format->wBitsPerSample;
    WORD ValidBits = Bits;
    BOOL Float;

    switch (Format->wFormatTag)
    {
        case WAVE_FORMAT_PCM:
        case WAVE_FORMAT_IEEE_FLOAT:
            Float = (Format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
            if (Format->nChannels > 2)
                return MmdFormatPlainMultichannel;
            if (Format->cbSize)
                return MmdFormatMalformed;
            break;

        case WAVE_FORMAT_EXTENSIBLE:
            if (Format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
                return MmdFormatShortExtensible;
            if (IsEqualGUID(Extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
                Float = FALSE;
            else if (IsEqualGUID(Extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                Float = TRUE;
            else
                return MmdFormatNotPcm;
            ValidBits = Extensible->Samples.wValidBitsPerSample;
            break;

        default:
            return MmdFormatNotPcm;
    }

    if (!Format->nChannels || Format->nChannels > AE_MAX_CHANNELS || !Format->nSamplesPerSec)
        return MmdFormatMalformed;
    if (Float ? (Bits != 32 && Bits != 64) : (Bits != 8 && Bits != 16 && Bits != 24 && Bits != 32))
        return MmdFormatMalformed;
    if (!ValidBits || ValidBits > Bits)
        return MmdFormatMalformed;
    if (Format->nBlockAlign != Format->nChannels * Bits / 8 ||
        Format->nAvgBytesPerSec != Format->nSamplesPerSec * Format->nBlockAlign)
    {
        return MmdFormatMalformed;
    }
    return MmdFormatValid;
}

/* Only the extensible part of cbSize means anything to the engine */
static
SIZE_T
MmdFormatSize(
    _In_ const WAVEFORMATEX *Format)
{
    return (Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
}

VOID
MmdMakeMixFormat(
    _In_ ULONG Endpoint,
    _Out_ WAVEFORMATEXTENSIBLE *Format)
{
    const AE_STATE *State = MmdGetState();

    ZeroMemory(Format, sizeof(*Format));
    if (State && Endpoint < State->EndpointCount)
        CopyMemory(Format, State->Endpoints[Endpoint].MixFormat, sizeof(*Format));
}

static
HRESULT
MmdCloneFormat(
    _In_ const WAVEFORMATEX *Format,
    _Outptr_ WAVEFORMATEX **Clone)
{
    SIZE_T Size = MmdFormatSize(Format);

    *Clone = (WAVEFORMATEX *)CoTaskMemAlloc(Size);
    if (!*Clone)
        return E_OUTOFMEMORY;
    CopyMemory(*Clone, Format, Size);
    (*Clone)->cbSize = (WORD)(Size - sizeof(WAVEFORMATEX));
    return S_OK;
}

class CAudioClient;

class CRenderClient
    : public CMmdObject
    , public IAudioRenderClient
{
public:
    CRenderClient(_In_ CAudioClient *Client);
    ~CRenderClient();

    STDMETHODIMP QueryInterface(_In_ REFIID Riid, _Outptr_ VOID **Object) override;
    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetBuffer(
        _In_ UINT32 Frames,
        _Outptr_result_maybenull_ BYTE **Data) override;

    STDMETHODIMP
    ReleaseBuffer(
        _In_ UINT32 Frames,
        _In_ DWORD Flags) override;

private:
    CAudioClient *m_Client;
};

class CCaptureClient
    : public CMmdObject
    , public IAudioCaptureClient
{
public:
    CCaptureClient(_In_ CAudioClient *Client);
    ~CCaptureClient();

    STDMETHODIMP QueryInterface(_In_ REFIID Riid, _Outptr_ VOID **Object) override;
    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetBuffer(
        _Outptr_result_maybenull_ BYTE **Data,
        _Out_ UINT32 *Frames,
        _Out_ DWORD *Flags,
        _Out_opt_ UINT64 *DevicePosition,
        _Out_opt_ UINT64 *QpcPosition) override;

    STDMETHODIMP
    ReleaseBuffer(
        _In_ UINT32 Frames) override;

    STDMETHODIMP
    GetNextPacketSize(
        _Out_ UINT32 *Frames) override;

private:
    CAudioClient *m_Client;
};

class CAudioClock
    : public CMmdObject
    , public IAudioClock
    , public IAudioClock2
{
public:
    CAudioClock(_In_ CAudioClient *Client);
    ~CAudioClock();

    STDMETHODIMP QueryInterface(_In_ REFIID Riid, _Outptr_ VOID **Object) override;
    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetFrequency(
        _Out_ UINT64 *Frequency) override;

    STDMETHODIMP
    GetPosition(
        _Out_ UINT64 *Position,
        _Out_opt_ UINT64 *QpcPosition) override;

    STDMETHODIMP
    GetCharacteristics(
        _Out_ DWORD *Characteristics) override;

    STDMETHODIMP
    GetDevicePosition(
        _Out_ UINT64 *Position,
        _Out_opt_ UINT64 *QpcPosition) override;

private:
    CAudioClient *m_Client;
};

class CStreamVolume
    : public CMmdObject
    , public IAudioStreamVolume
{
public:
    CStreamVolume(_In_ CAudioClient *Client);
    ~CStreamVolume();

    STDMETHODIMP QueryInterface(_In_ REFIID Riid, _Outptr_ VOID **Object) override;
    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetChannelCount(
        _Out_ UINT32 *Count) override;

    STDMETHODIMP
    SetChannelVolume(
        _In_ UINT32 Index,
        _In_ const float Level) override;

    STDMETHODIMP
    GetChannelVolume(
        _In_ UINT32 Index,
        _Out_ float *Level) override;

    STDMETHODIMP
    SetAllVolumes(
        _In_ UINT32 Count,
        _In_reads_(Count) const float *Levels) override;

    STDMETHODIMP
    GetAllVolumes(
        _In_ UINT32 Count,
        _Out_writes_(Count) float *Levels) override;

private:
    CAudioClient *m_Client;
};

template <class T>
static
HRESULT
MmdCreateService(
    _In_ CAudioClient *Client,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    T *Service = new (std::nothrow) T(Client);
    HRESULT Hr;

    if (!Service)
        return E_OUTOFMEMORY;
    Hr = Service->QueryInterface(Riid, Object);
    Service->Release();
    return Hr;
}

class CAudioClient
    : public CMmdObject
    , public IAudioClient3
{
    friend class CRenderClient;
    friend class CCaptureClient;
    friend class CAudioClock;
    friend class CStreamVolume;

public:
    CAudioClient(
        _In_ ULONG Endpoint,
        _In_ ULONG Kind,
        _In_ ULONG TargetProcessId,
        _In_ BOOL IncludeTree)
        : m_Endpoint(Endpoint)
        , m_Kind(Kind)
        , m_TargetProcessId(TargetProcessId)
        , m_IncludeTree(IncludeTree)
        , m_Initialized(FALSE)
        , m_Flags(0)
        , m_StreamId(0)
        , m_Connection(0)
        , m_Section(NULL)
        , m_Shared(NULL)
        , m_Ring(NULL)
        , m_Staging(NULL)
        , m_EngineEvent(NULL)
        , m_EventSet(FALSE)
        , m_BufferFrames(0)
        , m_PeriodFrames(0)
        , m_FrameBytes(0)
        , m_Latency(0)
        , m_Session(0)
        , m_SessionGeneration(0)
        , m_Pending(0)
        , m_PendingStaging(FALSE)
        , m_LastPosition(0)
    {
        InitializeCriticalSection(&m_Lock);
        ZeroMemory(&m_Format, sizeof(m_Format));
        ZeroMemory(&m_Properties, sizeof(m_Properties));
    }

    ~CAudioClient()
    {
        AE_REQUEST Request;

        if (m_Initialized && m_Connection == MmdGetConnection())
        {
            ZeroMemory(&Request, sizeof(Request.Stream));
            Request.Stream.StreamId = m_StreamId;
            MmdCall(&Request, AeCommandCloseStream, sizeof(Request.Stream), NULL, 0);
        }
        if (m_Shared)
            UnmapViewOfFile(m_Shared);
        if (m_Section)
            CloseHandle(m_Section);
        if (m_EngineEvent)
            CloseHandle(m_EngineEvent);
        DeleteCriticalSection(&m_Lock);
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (IsEqualIID(Riid, IID_IUnknown) || IsEqualIID(Riid, IID_IAudioClient) ||
            IsEqualIID(Riid, IID_IAudioClient2) || IsEqualIID(Riid, IID_IAudioClient3))
        {
            *Object = static_cast<IAudioClient3 *>(this);
            AddRef();
            return S_OK;
        }
        *Object = NULL;
        return E_NOINTERFACE;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    Initialize(
        _In_ AUDCLNT_SHAREMODE ShareMode,
        _In_ DWORD StreamFlags,
        _In_ REFERENCE_TIME BufferDuration,
        _In_ REFERENCE_TIME Periodicity,
        _In_ const WAVEFORMATEX *Format,
        _In_opt_ LPCGUID SessionGuid) override
    {
        AE_REQUEST Request;
        AE_REPLY Reply;
        ULONG Kind = m_Kind;
        SIZE_T FormatSize;
        HRESULT Hr;

        /* Shared mode streams run at the engine period whatever the client asks for */
        UNREFERENCED_PARAMETER(Periodicity);

        EnterCriticalSection(&m_Lock);
        if (m_Initialized)
        {
            Hr = AUDCLNT_E_ALREADY_INITIALIZED;
            goto Exit;
        }

        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK)
        {
            Hr = CheckProcessLoopback(ShareMode, StreamFlags, Format);
            BufferDuration = 0;
        }
        else
        {
            Hr = CheckShared(ShareMode, StreamFlags, BufferDuration, Format, SessionGuid, &Kind);
        }
        if (FAILED(Hr))
            goto Exit;

        FormatSize = MmdFormatSize(Format);
        ZeroMemory(&Request, sizeof(Request.OpenStream));
        if (Kind != AE_STREAM_PROCESS_LOOPBACK && !CopyEndpointId(Request.OpenStream.EndpointId))
        {
            Hr = AUDCLNT_E_DEVICE_INVALIDATED;
            goto Exit;
        }
        Request.OpenStream.Kind = Kind;
        Request.OpenStream.StreamFlags = StreamFlags;
        Request.OpenStream.BufferDuration = BufferDuration;
        Request.OpenStream.SessionGuid = SessionGuid ? *SessionGuid : GUID_NULL;
        Request.OpenStream.TargetProcessId = m_TargetProcessId;
        Request.OpenStream.IncludeProcessTree = m_IncludeTree;
        CopyMemory(Request.OpenStream.Format, Format, FormatSize);
        ((WAVEFORMATEX *)Request.OpenStream.Format)->cbSize = (WORD)(FormatSize - sizeof(WAVEFORMATEX));

        Hr = MmdCall(&Request, AeCommandOpenStream, sizeof(Request.OpenStream), &Reply, sizeof(Reply.OpenStream));
        if (FAILED(Hr))
            goto Exit;

        m_Section = (HANDLE)(ULONG_PTR)Reply.OpenStream.Section;
        m_EngineEvent = (HANDLE)(ULONG_PTR)Reply.OpenStream.Event;
        m_Shared = (PAE_STREAM_SHARED)MapViewOfFile(m_Section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        m_StreamId = Reply.OpenStream.StreamId;
        m_Connection = MmdGetConnection();
        if (!m_Shared || m_Shared->Magic != AE_STREAM_MAGIC)
        {
            ZeroMemory(&Request, sizeof(Request.Stream));
            Request.Stream.StreamId = m_StreamId;
            MmdCall(&Request, AeCommandCloseStream, sizeof(Request.Stream), NULL, 0);
            Hr = E_OUTOFMEMORY;
            goto Exit;
        }

        m_Ring = (BYTE *)m_Shared + m_Shared->RingOffset;
        m_Staging = (BYTE *)m_Shared + m_Shared->StagingOffset;
        m_BufferFrames = Reply.OpenStream.BufferFrames;
        m_PeriodFrames = Reply.OpenStream.PeriodFrames;
        m_FrameBytes = Format->nBlockAlign;
        m_Latency = Reply.OpenStream.Latency;
        m_Session = Reply.OpenStream.Session;
        m_SessionGeneration = Reply.OpenStream.SessionGeneration;
        CopyMemory(&m_Format, Request.OpenStream.Format, FormatSize);
        m_Kind = Kind;
        m_Flags = StreamFlags;
        m_Initialized = TRUE;
        Hr = S_OK;

    Exit:
        LeaveCriticalSection(&m_Lock);
        return Hr;
    }

    STDMETHODIMP
    GetBufferSize(
        _Out_ UINT32 *Frames) override
    {
        if (!Frames)
            return E_POINTER;
        if (!m_Initialized)
            return AUDCLNT_E_NOT_INITIALIZED;

        /* A process loopback client reports one engine period */
        *Frames = (m_Kind == AE_STREAM_PROCESS_LOOPBACK) ? m_PeriodFrames : m_BufferFrames;
        return S_OK;
    }

    STDMETHODIMP
    GetStreamLatency(
        _Out_ REFERENCE_TIME *Latency) override
    {
        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK)
            return E_NOTIMPL;
        if (!Latency)
            return E_POINTER;
        if (!m_Initialized)
            return AUDCLNT_E_NOT_INITIALIZED;
        *Latency = m_Latency;
        return S_OK;
    }

    STDMETHODIMP
    GetCurrentPadding(
        _Out_ UINT32 *Frames) override
    {
        HRESULT Hr;

        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK)
            return E_NOTIMPL;
        if (!Frames)
            return E_POINTER;
        *Frames = 0;
        Hr = CheckUsable();
        if (FAILED(Hr))
            return Hr;
        *Frames = Queued();
        return S_OK;
    }

    STDMETHODIMP
    IsFormatSupported(
        _In_ AUDCLNT_SHAREMODE ShareMode,
        _In_ const WAVEFORMATEX *Format,
        _Outptr_opt_result_maybenull_ WAVEFORMATEX **Closest) override
    {
        const WAVEFORMATEXTENSIBLE *Extensible = (const WAVEFORMATEXTENSIBLE *)Format;
        WAVEFORMATEXTENSIBLE Mix;
        MMD_FORMAT_CHECK Check;

        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK)
            return E_NOTIMPL;
        if (Closest)
            *Closest = NULL;
        if (!Format || (ShareMode == AUDCLNT_SHAREMODE_SHARED && !Closest))
            return E_POINTER;
        if (ShareMode != AUDCLNT_SHAREMODE_SHARED && ShareMode != AUDCLNT_SHAREMODE_EXCLUSIVE)
            return E_INVALIDARG;

        Check = MmdCheckFormat(Format);
        if (Check == MmdFormatShortExtensible)
            return E_INVALIDARG;
        if (Check == MmdFormatPlainMultichannel || Check == MmdFormatNotPcm)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (ShareMode == AUDCLNT_SHAREMODE_EXCLUSIVE)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;

        /* Sample types convert freely; rate and channels have to match the mix */
        if (Check == MmdFormatValid && MatchesMix(Format) &&
            (Format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || !(Extensible->Samples.wValidBitsPerSample % 8)))
        {
            return S_OK;
        }

        MmdMakeMixFormat(m_Endpoint, &Mix);
        if (FAILED(MmdCloneFormat(&Mix.Format, Closest)))
            return E_OUTOFMEMORY;
        return S_FALSE;
    }

    STDMETHODIMP
    GetMixFormat(
        _Outptr_ WAVEFORMATEX **Format) override
    {
        WAVEFORMATEXTENSIBLE Mix;

        if (!Format)
            return E_POINTER;
        *Format = NULL;
        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK)
            return E_NOTIMPL;
        if (!MmdIsEndpointActive(m_Endpoint))
            return AUDCLNT_E_DEVICE_INVALIDATED;
        MmdMakeMixFormat(m_Endpoint, &Mix);
        return MmdCloneFormat(&Mix.Format, Format);
    }

    STDMETHODIMP
    GetDevicePeriod(
        _Out_opt_ REFERENCE_TIME *DefaultPeriod,
        _Out_opt_ REFERENCE_TIME *MinimumPeriod) override
    {
        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK)
            return E_NOTIMPL;
        if (!DefaultPeriod && !MinimumPeriod)
            return E_POINTER;
        if (DefaultPeriod)
            *DefaultPeriod = MMD_DEFAULT_PERIOD;
        if (MinimumPeriod)
            *MinimumPeriod = MMD_MINIMUM_PERIOD;
        return S_OK;
    }

    STDMETHODIMP
    Start() override
    {
        HRESULT Hr;

        EnterCriticalSection(&m_Lock);
        Hr = CheckUsable();
        if (SUCCEEDED(Hr) && m_Shared->Running)
            Hr = AUDCLNT_E_NOT_STOPPED;
        else if (SUCCEEDED(Hr) && (m_Flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !m_EventSet &&
                 m_Kind != AE_STREAM_PROCESS_LOOPBACK)
            Hr = AUDCLNT_E_EVENTHANDLE_NOT_SET;
        else if (SUCCEEDED(Hr))
            Hr = StreamCommand(AeCommandStart, NULL);
        LeaveCriticalSection(&m_Lock);
        return Hr;
    }

    STDMETHODIMP
    Stop() override
    {
        HRESULT Hr;

        EnterCriticalSection(&m_Lock);
        Hr = CheckUsable();
        if (SUCCEEDED(Hr) && !m_Shared->Running)
            Hr = S_FALSE;
        else if (SUCCEEDED(Hr))
            Hr = StreamCommand(AeCommandStop, NULL);
        LeaveCriticalSection(&m_Lock);
        return Hr;
    }

    STDMETHODIMP
    Reset() override
    {
        HRESULT Hr;

        EnterCriticalSection(&m_Lock);
        Hr = CheckUsable();
        if (SUCCEEDED(Hr) && m_Shared->Running)
            Hr = AUDCLNT_E_NOT_STOPPED;
        else if (SUCCEEDED(Hr) && m_Pending)
            Hr = AUDCLNT_E_BUFFER_OPERATION_PENDING;
        else if (SUCCEEDED(Hr))
        {
            Hr = StreamCommand(AeCommandReset, NULL);
            m_LastPosition = 0;
        }
        LeaveCriticalSection(&m_Lock);
        return Hr;
    }

    STDMETHODIMP
    SetEventHandle(
        _In_ HANDLE Event) override
    {
        HRESULT Hr;

        if (!Event || Event == INVALID_HANDLE_VALUE)
            return E_INVALIDARG;

        EnterCriticalSection(&m_Lock);
        Hr = CheckUsable();
        if (SUCCEEDED(Hr) && !(m_Flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
            Hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
        else if (SUCCEEDED(Hr))
        {
            Hr = StreamCommand(AeCommandSetEvent, Event);
            if (SUCCEEDED(Hr))
                m_EventSet = TRUE;
        }
        LeaveCriticalSection(&m_Lock);
        return Hr;
    }

    STDMETHODIMP
    GetService(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        BOOL Rendering = (m_Kind == AE_STREAM_RENDER);

        if (!Object)
            return E_POINTER;
        *Object = NULL;
        if (!m_Initialized)
            return AUDCLNT_E_NOT_INITIALIZED;

        if (m_Kind == AE_STREAM_PROCESS_LOOPBACK && !IsEqualIID(Riid, IID_IAudioCaptureClient))
            return E_NOINTERFACE;

        if (IsEqualIID(Riid, IID_IAudioRenderClient))
        {
            if (!Rendering)
                return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
            return MmdCreateService<CRenderClient>(this, Riid, Object);
        }
        else if (IsEqualIID(Riid, IID_IAudioCaptureClient))
        {
            if (Rendering)
                return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
            return MmdCreateService<CCaptureClient>(this, Riid, Object);
        }
        else if (IsEqualIID(Riid, IID_IAudioClock) || IsEqualIID(Riid, IID_IAudioClock2))
        {
            return MmdCreateService<CAudioClock>(this, Riid, Object);
        }
        else if (IsEqualIID(Riid, IID_IAudioStreamVolume))
        {
            return MmdCreateService<CStreamVolume>(this, Riid, Object);
        }
        else if (IsEqualIID(Riid, IID_ISimpleAudioVolume) || IsEqualIID(Riid, IID_IChannelAudioVolume) ||
                 IsEqualIID(Riid, IID_IAudioSessionControl) || IsEqualIID(Riid, IID_IAudioSessionControl2))
        {
            return MmdCreateSessionObject(m_Session, m_SessionGeneration, Riid, Object);
        }
        return E_NOINTERFACE;
    }

    /* IAudioClient2 */
    STDMETHODIMP
    IsOffloadCapable(
        _In_ AUDIO_STREAM_CATEGORY Category,
        _Out_ BOOL *OffloadCapable) override
    {
        UNREFERENCED_PARAMETER(Category);

        if (!OffloadCapable)
            return E_POINTER;
        *OffloadCapable = FALSE;
        return S_OK;
    }

    STDMETHODIMP
    SetClientProperties(
        _In_ const AudioClientProperties *Properties) override
    {
        if (!Properties)
            return E_POINTER;
        if (Properties->cbSize != MMD_CLIENT_PROPERTIES_V1 && Properties->cbSize != sizeof(AudioClientProperties))
            return E_INVALIDARG;
        if (m_Initialized)
            return AUDCLNT_E_ALREADY_INITIALIZED;
        if (Properties->bIsOffload)
            return AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE;

        ZeroMemory(&m_Properties, sizeof(m_Properties));
        CopyMemory(&m_Properties, Properties, Properties->cbSize);
        return S_OK;
    }

    STDMETHODIMP
    GetBufferSizeLimits(
        _In_ const WAVEFORMATEX *Format,
        _In_ BOOL EventDriven,
        _Out_ REFERENCE_TIME *MinimumDuration,
        _Out_ REFERENCE_TIME *MaximumDuration) override
    {
        UNREFERENCED_PARAMETER(Format);
        UNREFERENCED_PARAMETER(EventDriven);

        if (!Format || !MinimumDuration || !MaximumDuration)
            return E_POINTER;
        return AUDCLNT_E_OFFLOAD_MODE_ONLY;
    }

    /* IAudioClient3 */
    STDMETHODIMP
    GetSharedModeEnginePeriod(
        _In_ const WAVEFORMATEX *Format,
        _Out_ UINT32 *DefaultPeriod,
        _Out_ UINT32 *FundamentalPeriod,
        _Out_ UINT32 *MinimumPeriod,
        _Out_ UINT32 *MaximumPeriod) override
    {
        ULONG Frames;

        if (!Format || !DefaultPeriod || !FundamentalPeriod || !MaximumPeriod)
            return E_INVALIDARG;
        if (MmdCheckFormat(Format) != MmdFormatValid)
            return E_INVALIDARG;

        /* The engine runs at one period only */
        Frames = MmdFramesFromDuration(MMD_DEFAULT_PERIOD, Format->nSamplesPerSec);
        *DefaultPeriod = Frames;
        *FundamentalPeriod = Frames;
        if (MinimumPeriod)
            *MinimumPeriod = Frames;
        *MaximumPeriod = Frames;
        return S_OK;
    }

    STDMETHODIMP
    GetCurrentSharedModeEnginePeriod(
        _Outptr_ WAVEFORMATEX **Format,
        _Out_ UINT32 *Period) override
    {
        WAVEFORMATEXTENSIBLE Mix;
        HRESULT Hr;

        if (!Format || !Period)
            return E_POINTER;
        *Format = NULL;
        if (!MmdIsEndpointActive(m_Endpoint))
            return AUDCLNT_E_DEVICE_INVALIDATED;
        MmdMakeMixFormat(m_Endpoint, &Mix);
        Hr = MmdCloneFormat(&Mix.Format, Format);
        if (SUCCEEDED(Hr))
            *Period = MmdFramesFromDuration(MMD_DEFAULT_PERIOD, Mix.Format.nSamplesPerSec);
        return Hr;
    }

    STDMETHODIMP
    InitializeSharedAudioStream(
        _In_ DWORD StreamFlags,
        _In_ UINT32 PeriodInFrames,
        _In_ const WAVEFORMATEX *Format,
        _In_opt_ LPCGUID SessionGuid) override
    {
        if (!Format)
            return E_INVALIDARG;
        if (StreamFlags & ~(AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED |
                            AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED))
        {
            return AUDCLNT_E_INVALID_STREAM_FLAG;
        }
        if (PeriodInFrames != MmdFramesFromDuration(MMD_DEFAULT_PERIOD, Format->nSamplesPerSec))
            return AUDCLNT_E_INVALID_DEVICE_PERIOD;
        return Initialize(AUDCLNT_SHAREMODE_SHARED, StreamFlags, 0, 0, Format, SessionGuid);
    }

private:
    /* The order the audio service checks a shared mode Initialize in */
    HRESULT
    CheckShared(
        _In_ AUDCLNT_SHAREMODE ShareMode,
        _In_ DWORD StreamFlags,
        _In_ REFERENCE_TIME BufferDuration,
        _In_opt_ const WAVEFORMATEX *Format,
        _In_opt_ LPCGUID SessionGuid,
        _Inout_ ULONG *Kind)
    {
        MMD_FORMAT_CHECK Check;

        if (BufferDuration < 0)
            return E_INVALIDARG;
        if ((StreamFlags & AUDCLNT_STREAMFLAGS_CROSSPROCESS) && (!SessionGuid || IsEqualGUID(*SessionGuid, GUID_NULL)))
            return E_INVALIDARG;
        if (StreamFlags & MMD_RESERVED_STREAM_FLAGS)
            return AUDCLNT_E_INVALID_STREAM_FLAG;
        if (ShareMode != AUDCLNT_SHAREMODE_SHARED && ShareMode != AUDCLNT_SHAREMODE_EXCLUSIVE)
            return E_INVALIDARG;
        if (!Format)
            return E_POINTER;

        Check = MmdCheckFormat(Format);
        if (Check == MmdFormatMalformed || Check == MmdFormatShortExtensible || Check == MmdFormatPlainMultichannel)
            return E_INVALIDARG;

        /* The engine only mixes, so there is no exclusive mode */
        if (ShareMode == AUDCLNT_SHAREMODE_EXCLUSIVE)
            return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;

        if (StreamFlags & AUDCLNT_STREAMFLAGS_LOOPBACK)
        {
            if (*Kind != AE_STREAM_RENDER)
                return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
            *Kind = AE_STREAM_LOOPBACK;
        }

        if (Check == MmdFormatNotPcm)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (!(StreamFlags & AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) && !MatchesMix(Format))
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        return S_OK;
    }

    /* A process loopback stream takes any PCM or float format at any rate */
    HRESULT
    CheckProcessLoopback(
        _In_ AUDCLNT_SHAREMODE ShareMode,
        _In_ DWORD StreamFlags,
        _In_opt_ const WAVEFORMATEX *Format)
    {
        MMD_FORMAT_CHECK Check;

        if (!Format)
            return E_INVALIDARG;
        if ((StreamFlags & ~MMD_PROCESS_LOOPBACK_FLAGS) || !(StreamFlags & AUDCLNT_STREAMFLAGS_LOOPBACK))
            return AUDCLNT_E_INVALID_STREAM_FLAG;
        if (ShareMode != AUDCLNT_SHAREMODE_SHARED)
            return E_INVALIDARG;

        Check = MmdCheckFormat(Format);
        return (Check == MmdFormatValid || Check == MmdFormatPlainMultichannel) ? S_OK : E_INVALIDARG;
    }

    BOOL
    MatchesMix(
        _In_ const WAVEFORMATEX *Format)
    {
        WAVEFORMATEXTENSIBLE Mix;

        MmdMakeMixFormat(m_Endpoint, &Mix);
        return Format->nSamplesPerSec == Mix.Format.nSamplesPerSec && Format->nChannels == Mix.Format.nChannels;
    }

    BOOL
    CopyEndpointId(
        _Out_writes_(AE_ID_LENGTH) PWSTR Id)
    {
        const AE_STATE *State = MmdGetState();

        if (!State || m_Endpoint >= State->EndpointCount)
            return FALSE;
        return SUCCEEDED(StringCchCopyW(Id, AE_ID_LENGTH, State->Endpoints[m_Endpoint].Id));
    }

    /* Streams die with their engine and with their endpoint */
    HRESULT
    CheckUsable()
    {
        const AE_STATE *State;

        if (!m_Initialized)
            return AUDCLNT_E_NOT_INITIALIZED;
        if (m_Connection != MmdGetConnection())
            return AUDCLNT_E_DEVICE_INVALIDATED;
        State = MmdGetState();
        if (m_Kind != AE_STREAM_PROCESS_LOOPBACK &&
            (!State || State->Endpoints[m_Endpoint].DeviceState != DEVICE_STATE_ACTIVE))
        {
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        return S_OK;
    }

    HRESULT
    StreamCommand(
        _In_ ULONG Command,
        _In_opt_ HANDLE Handle)
    {
        AE_REQUEST Request;

        ZeroMemory(&Request, sizeof(Request.Stream));
        Request.Stream.StreamId = m_StreamId;
        Request.Stream.Handle = (ULONG64)(ULONG_PTR)Handle;
        return MmdCall(&Request, Command, sizeof(Request.Stream), NULL, 0);
    }

    /* Frames written and not yet taken, by whichever side produces */
    ULONG
    Queued()
    {
        LONGLONG Frames = MmdReadPosition(&m_Shared->WritePosition) - MmdReadPosition(&m_Shared->ReadPosition);

        return (ULONG)max(min(Frames, (LONGLONG)m_BufferFrames), 0);
    }

    /* Copies Frames frames between the ring at Position and a flat buffer */
    VOID
    CopyRing(
        _In_ LONGLONG Position,
        _In_ ULONG Frames,
        _Inout_ BYTE *Flat,
        _In_ BOOL ToRing)
    {
        ULONG Start = (ULONG)(Position % m_BufferFrames);
        ULONG First = min(Frames, m_BufferFrames - Start);

        if (ToRing)
        {
            CopyMemory(m_Ring + (SIZE_T)Start * m_FrameBytes, Flat, (SIZE_T)First * m_FrameBytes);
            CopyMemory(m_Ring, Flat + (SIZE_T)First * m_FrameBytes, (SIZE_T)(Frames - First) * m_FrameBytes);
        }
        else
        {
            CopyMemory(Flat, m_Ring + (SIZE_T)Start * m_FrameBytes, (SIZE_T)First * m_FrameBytes);
            CopyMemory(Flat + (SIZE_T)First * m_FrameBytes, m_Ring, (SIZE_T)(Frames - First) * m_FrameBytes);
        }
    }

    BOOL
    IsWrapped(
        _In_ LONGLONG Position,
        _In_ ULONG Frames)
    {
        return (ULONG)(Position % m_BufferFrames) + Frames > m_BufferFrames;
    }

    BYTE *
    RingAt(
        _In_ LONGLONG Position)
    {
        return m_Ring + (SIZE_T)(Position % m_BufferFrames) * m_FrameBytes;
    }

    /* Frames that reached the device, carried forward to now while the stream runs */
    UINT64
    PlayedFrames(
        _Out_opt_ UINT64 *Qpc)
    {
        LONGLONG Now = MmdNow100ns();
        LONGLONG Time = MmdReadPosition(&m_Shared->DevicePositionTime);
        LONGLONG Position = MmdReadPosition(&m_Shared->DevicePosition);
        LONGLONG Limit;

        if (m_Kind == AE_STREAM_RENDER && m_Shared->Running && Time && Now > Time)
        {
            Limit = MmdReadPosition(&m_Shared->ReadPosition);
            Position += (Now - Time) * (LONGLONG)m_Format.Format.nSamplesPerSec / 10000000;
            Position = min(Position, Limit);
        }

        /* The clock never runs backwards between resets */
        EnterCriticalSection(&m_Lock);
        Position = max(Position, m_LastPosition);
        m_LastPosition = Position;
        LeaveCriticalSection(&m_Lock);

        if (Qpc)
            *Qpc = Now;
        return (UINT64)Position;
    }

    CRITICAL_SECTION m_Lock;
    ULONG m_Endpoint;
    ULONG m_Kind;
    ULONG m_TargetProcessId;
    BOOL m_IncludeTree;
    BOOL m_Initialized;
    DWORD m_Flags;
    ULONG m_StreamId;
    ULONG m_Connection;
    HANDLE m_Section;
    PAE_STREAM_SHARED m_Shared;
    BYTE *m_Ring;
    BYTE *m_Staging;
    HANDLE m_EngineEvent;
    BOOL m_EventSet;
    WAVEFORMATEXTENSIBLE m_Format;
    ULONG m_BufferFrames;
    ULONG m_PeriodFrames;
    ULONG m_FrameBytes;
    LONGLONG m_Latency;
    ULONG m_Session;
    ULONG m_SessionGeneration;
    AudioClientProperties m_Properties;

    /* GetBuffer handed out this many frames, from the ring or from the staging area */
    ULONG m_Pending;
    BOOL m_PendingStaging;
    LONGLONG m_LastPosition;
};

CRenderClient::CRenderClient(
    _In_ CAudioClient *Client)
    : m_Client(Client)
{
    Client->AddRef();
}

CRenderClient::~CRenderClient()
{
    m_Client->Release();
}

STDMETHODIMP
CRenderClient::QueryInterface(
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    if (!Object)
        return E_POINTER;
    if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioRenderClient))
    {
        *Object = NULL;
        return E_NOINTERFACE;
    }
    *Object = static_cast<IAudioRenderClient *>(this);
    AddRef();
    return S_OK;
}

STDMETHODIMP
CRenderClient::GetBuffer(
    _In_ UINT32 Frames,
    _Outptr_result_maybenull_ BYTE **Data)
{
    CAudioClient *Client = m_Client;
    LONGLONG Position;
    HRESULT Hr;

    if (!Data)
        return E_POINTER;
    *Data = NULL;

    EnterCriticalSection(&Client->m_Lock);
    Hr = Client->CheckUsable();
    if (FAILED(Hr))
        goto Exit;
    if (Client->m_Pending)
    {
        Hr = AUDCLNT_E_OUT_OF_ORDER;
        goto Exit;
    }
    if (Frames > Client->m_BufferFrames - Client->Queued())
    {
        Hr = AUDCLNT_E_BUFFER_TOO_LARGE;
        goto Exit;
    }
    if (!Frames)
        goto Exit;

    /* A request that runs past the end of the ring is written to the staging area first */
    Position = MmdReadPosition(&Client->m_Shared->WritePosition);
    Client->m_PendingStaging = Client->IsWrapped(Position, Frames);
    *Data = Client->m_PendingStaging ? Client->m_Staging : Client->RingAt(Position);
    Client->m_Pending = Frames;

Exit:
    LeaveCriticalSection(&Client->m_Lock);
    return Hr;
}

STDMETHODIMP
CRenderClient::ReleaseBuffer(
    _In_ UINT32 Frames,
    _In_ DWORD Flags)
{
    CAudioClient *Client = m_Client;
    LONGLONG Position;
    BYTE *Data;
    HRESULT Hr = S_OK;

    if (Flags & ~AUDCLNT_BUFFERFLAGS_SILENT)
        return E_INVALIDARG;

    EnterCriticalSection(&Client->m_Lock);
    if (!Client->m_Pending)
    {
        Hr = Frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
        goto Exit;
    }
    if (Frames > Client->m_Pending)
    {
        Hr = AUDCLNT_E_INVALID_SIZE;
        goto Exit;
    }

    Position = MmdReadPosition(&Client->m_Shared->WritePosition);
    Data = Client->m_PendingStaging ? Client->m_Staging : Client->RingAt(Position);
    if (Flags & AUDCLNT_BUFFERFLAGS_SILENT)
    {
        FillMemory(Data, (SIZE_T)Frames * Client->m_FrameBytes,
                   (Client->m_Format.Format.wBitsPerSample == 8) ? 0x80 : 0);
    }
    if (Client->m_PendingStaging)
        Client->CopyRing(Position, Frames, Client->m_Staging, TRUE);

    /* The data must be in the ring before the engine can see the new write position */
    MemoryBarrier();
    InterlockedExchange64(&Client->m_Shared->WritePosition, Position + Frames);
    Client->m_Pending = 0;

Exit:
    LeaveCriticalSection(&Client->m_Lock);
    return Hr;
}

CCaptureClient::CCaptureClient(
    _In_ CAudioClient *Client)
    : m_Client(Client)
{
    Client->AddRef();
}

CCaptureClient::~CCaptureClient()
{
    m_Client->Release();
}

STDMETHODIMP
CCaptureClient::QueryInterface(
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    if (!Object)
        return E_POINTER;
    if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioCaptureClient))
    {
        *Object = NULL;
        return E_NOINTERFACE;
    }
    *Object = static_cast<IAudioCaptureClient *>(this);
    AddRef();
    return S_OK;
}

STDMETHODIMP
CCaptureClient::GetBuffer(
    _Outptr_result_maybenull_ BYTE **Data,
    _Out_ UINT32 *Frames,
    _Out_ DWORD *Flags,
    _Out_opt_ UINT64 *DevicePosition,
    _Out_opt_ UINT64 *QpcPosition)
{
    CAudioClient *Client = m_Client;
    LONGLONG Position, Write, Time;
    ULONG Packet;
    HRESULT Hr;

    if (!Data || !Frames || !Flags)
        return E_POINTER;
    *Data = NULL;
    *Frames = 0;
    *Flags = 0;

    EnterCriticalSection(&Client->m_Lock);
    Hr = Client->CheckUsable();
    if (FAILED(Hr))
        goto Exit;
    if (Client->m_Pending)
    {
        Hr = AUDCLNT_E_OUT_OF_ORDER;
        goto Exit;
    }

    Packet = min(Client->Queued(), Client->m_PeriodFrames);
    if (!Packet)
    {
        Hr = AUDCLNT_S_BUFFER_EMPTY;
        goto Exit;
    }

    Position = MmdReadPosition(&Client->m_Shared->ReadPosition);
    Client->m_PendingStaging = Client->IsWrapped(Position, Packet);
    if (Client->m_PendingStaging)
        Client->CopyRing(Position, Packet, Client->m_Staging, FALSE);
    *Data = Client->m_PendingStaging ? Client->m_Staging : Client->RingAt(Position);
    *Frames = Packet;
    Client->m_Pending = Packet;

    if (InterlockedExchange(&Client->m_Shared->Discontinuity, 0))
        *Flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;

    if (DevicePosition)
        *DevicePosition = (UINT64)Position;
    if (QpcPosition)
    {
        /* The engine stamped the newest frame; this packet's first one came that many frames earlier */
        Write = MmdReadPosition(&Client->m_Shared->WritePosition);
        Time = MmdReadPosition(&Client->m_Shared->DevicePositionTime);
        *QpcPosition = (UINT64)(Time - (Write - Position) * 10000000 / (LONGLONG)Client->m_Format.Format.nSamplesPerSec);
    }

Exit:
    LeaveCriticalSection(&Client->m_Lock);
    return Hr;
}

STDMETHODIMP
CCaptureClient::ReleaseBuffer(
    _In_ UINT32 Frames)
{
    CAudioClient *Client = m_Client;
    HRESULT Hr = S_OK;

    EnterCriticalSection(&Client->m_Lock);
    if (!Client->m_Pending)
        Hr = Frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    else if (Frames && Frames != Client->m_Pending)
        Hr = AUDCLNT_E_INVALID_SIZE;
    else
    {
        if (Frames)
        {
            MemoryBarrier();
            InterlockedExchange64(&Client->m_Shared->ReadPosition,
                                  MmdReadPosition(&Client->m_Shared->ReadPosition) + Frames);
        }
        Client->m_Pending = 0;
    }
    LeaveCriticalSection(&Client->m_Lock);
    return Hr;
}

STDMETHODIMP
CCaptureClient::GetNextPacketSize(
    _Out_ UINT32 *Frames)
{
    CAudioClient *Client = m_Client;
    HRESULT Hr;

    if (!Frames)
        return E_POINTER;
    *Frames = 0;
    Hr = Client->CheckUsable();
    if (FAILED(Hr))
        return Hr;
    *Frames = min(Client->Queued(), Client->m_PeriodFrames);
    return S_OK;
}

CAudioClock::CAudioClock(
    _In_ CAudioClient *Client)
    : m_Client(Client)
{
    Client->AddRef();
}

CAudioClock::~CAudioClock()
{
    m_Client->Release();
}

STDMETHODIMP
CAudioClock::QueryInterface(
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    if (!Object)
        return E_POINTER;
    if (IsEqualIID(Riid, IID_IUnknown) || IsEqualIID(Riid, IID_IAudioClock))
        *Object = static_cast<IAudioClock *>(this);
    else if (IsEqualIID(Riid, IID_IAudioClock2))
        *Object = static_cast<IAudioClock2 *>(this);
    else
    {
        *Object = NULL;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

/* Shared mode clocks count bytes */
STDMETHODIMP
CAudioClock::GetFrequency(
    _Out_ UINT64 *Frequency)
{
    if (!Frequency)
        return E_POINTER;
    *Frequency = (UINT64)m_Client->m_Format.Format.nSamplesPerSec * m_Client->m_FrameBytes;
    return S_OK;
}

STDMETHODIMP
CAudioClock::GetPosition(
    _Out_ UINT64 *Position,
    _Out_opt_ UINT64 *QpcPosition)
{
    HRESULT Hr;

    if (!Position)
        return E_POINTER;
    Hr = m_Client->CheckUsable();
    if (FAILED(Hr))
        return Hr;
    *Position = m_Client->PlayedFrames(QpcPosition) * m_Client->m_FrameBytes;
    return S_OK;
}

STDMETHODIMP
CAudioClock::GetCharacteristics(
    _Out_ DWORD *Characteristics)
{
    if (!Characteristics)
        return E_POINTER;
    *Characteristics = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;
    return S_OK;
}

STDMETHODIMP
CAudioClock::GetDevicePosition(
    _Out_ UINT64 *Position,
    _Out_opt_ UINT64 *QpcPosition)
{
    HRESULT Hr;

    if (!Position)
        return E_POINTER;
    Hr = m_Client->CheckUsable();
    if (FAILED(Hr))
        return Hr;
    *Position = m_Client->PlayedFrames(QpcPosition);
    return S_OK;
}

CStreamVolume::CStreamVolume(
    _In_ CAudioClient *Client)
    : m_Client(Client)
{
    Client->AddRef();
}

CStreamVolume::~CStreamVolume()
{
    m_Client->Release();
}

STDMETHODIMP
CStreamVolume::QueryInterface(
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    if (!Object)
        return E_POINTER;
    if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioStreamVolume))
    {
        *Object = NULL;
        return E_NOINTERFACE;
    }
    *Object = static_cast<IAudioStreamVolume *>(this);
    AddRef();
    return S_OK;
}

STDMETHODIMP
CStreamVolume::GetChannelCount(
    _Out_ UINT32 *Count)
{
    if (!Count)
        return E_POINTER;
    *Count = m_Client->m_Format.Format.nChannels;
    return S_OK;
}

STDMETHODIMP
CStreamVolume::SetChannelVolume(
    _In_ UINT32 Index,
    _In_ const float Level)
{
    if (Index >= m_Client->m_Format.Format.nChannels || !(Level >= 0.0f && Level <= 1.0f))
        return E_INVALIDARG;
    m_Client->m_Shared->Volume[Index] = Level;
    return S_OK;
}

STDMETHODIMP
CStreamVolume::GetChannelVolume(
    _In_ UINT32 Index,
    _Out_ float *Level)
{
    if (!Level)
        return E_POINTER;
    if (Index >= m_Client->m_Format.Format.nChannels)
        return E_INVALIDARG;
    *Level = m_Client->m_Shared->Volume[Index];
    return S_OK;
}

STDMETHODIMP
CStreamVolume::SetAllVolumes(
    _In_ UINT32 Count,
    _In_reads_(Count) const float *Levels)
{
    UINT32 Index;

    if (!Levels)
        return E_POINTER;
    if (Count != m_Client->m_Format.Format.nChannels)
        return E_INVALIDARG;
    for (Index = 0; Index < Count; Index++)
    {
        if (!(Levels[Index] >= 0.0f && Levels[Index] <= 1.0f))
            return E_INVALIDARG;
    }
    for (Index = 0; Index < Count; Index++)
        m_Client->m_Shared->Volume[Index] = Levels[Index];
    return S_OK;
}

STDMETHODIMP
CStreamVolume::GetAllVolumes(
    _In_ UINT32 Count,
    _Out_writes_(Count) float *Levels)
{
    UINT32 Index;

    if (!Levels)
        return E_POINTER;
    if (Count != m_Client->m_Format.Format.nChannels)
        return E_INVALIDARG;
    for (Index = 0; Index < Count; Index++)
        Levels[Index] = m_Client->m_Shared->Volume[Index];
    return S_OK;
}

HRESULT
MmdCreateAudioClient(
    _In_ ULONG Endpoint,
    _In_ ULONG Kind,
    _In_ ULONG TargetProcessId,
    _In_ BOOL IncludeTree,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    CAudioClient *Client;
    HRESULT Hr;

    *Object = NULL;
    Client = new (std::nothrow) CAudioClient(Endpoint, Kind, TargetProcessId, IncludeTree);
    if (!Client)
        return E_OUTOFMEMORY;
    Hr = Client->QueryInterface(Riid, Object);
    Client->Release();
    return Hr;
}
