/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions of the audio engine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <sddl.h>
#include <strsafe.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <ndk/psfuncs.h>
#include <audioengine.h>

/* Engine period; all endpoints mix this much at a time */
#define AE_PERIOD_100NS         100000
#define AE_DEVICE_BUFFERS       4
#define AE_IDLE_CLOSE_MS        2000
#define AE_OPEN_ATTEMPTS        10
#define AE_OPEN_RETRY_MS        50

#ifndef DEVICE_STATE_ACTIVE
#define DEVICE_STATE_ACTIVE     0x1
#define DEVICE_STATE_DISABLED   0x2
#define DEVICE_STATE_NOTPRESENT 0x4
#define DEVICE_STATE_UNPLUGGED  0x8
#endif

/* Default and minimum device periods reported to clients */
#define AE_DEFAULT_PERIOD       100000
#define AE_MINIMUM_PERIOD       30000

#define AE_MAX_LOOPBACKS        16

/* Rates the engine converts between */
#define AE_MIN_RATE             1000
#define AE_MAX_RATE             384000

/* A decoded WAVEFORMATEX */
typedef enum _AE_SAMPLE_TYPE
{
    AeSamplePcm,
    AeSampleFloat,
} AE_SAMPLE_TYPE;

typedef struct _AE_FORMAT
{
    AE_SAMPLE_TYPE SampleType;
    ULONG ContainerBits;
    ULONG ValidBits;
    ULONG Channels;
    ULONG ChannelMask;
    ULONG SamplesPerSec;
    ULONG BlockAlign;
} AE_FORMAT, *PAE_FORMAT;

/* format.cpp */
BOOL
AeDecodeFormat(
    _In_ const WAVEFORMATEX *Format,
    _Out_ PAE_FORMAT Decoded);

ULONG
AeDefaultChannelMask(
    _In_ ULONG Channels);

VOID
AeToFloat(
    _In_ const AE_FORMAT *Format,
    _In_reads_bytes_(Frames * Format->BlockAlign) const BYTE *Source,
    _Out_writes_(Frames * Format->Channels) float *Destination,
    _In_ ULONG Frames);

VOID
AeFromFloat(
    _In_ const AE_FORMAT *Format,
    _In_reads_(Frames * Format->Channels) const float *Source,
    _Out_writes_bytes_(Frames * Format->BlockAlign) BYTE *Destination,
    _In_ ULONG Frames);

VOID
AeMakeFloatFormat(
    _In_ ULONG SamplesPerSec,
    _In_ ULONG Channels,
    _In_ ULONG ChannelMask,
    _Out_ WAVEFORMATEXTENSIBLE *Format);

/* Mixes InChannels of each frame into OutChannels by speaker position */
class CAeChannelMap
{
public:
    VOID
    Initialize(
        _In_ ULONG InChannels,
        _In_ ULONG InMask,
        _In_ ULONG OutChannels,
        _In_ ULONG OutMask);

    VOID
    Apply(
        _In_reads_(Frames * m_InChannels) const float *Source,
        _Out_writes_(Frames * m_OutChannels) float *Destination,
        _In_ ULONG Frames) const;

    BOOL IsIdentity() const { return m_Identity; }
    ULONG InChannels() const { return m_InChannels; }
    ULONG OutChannels() const { return m_OutChannels; }

private:
    float m_Matrix[AE_MAX_CHANNELS][AE_MAX_CHANNELS];
    ULONG m_InChannels;
    ULONG m_OutChannels;
    BOOL m_Identity;
};

/* Converts a rate with four point cubic interpolation; frames go in with Push and come out with Pull */
class CAeResampler
{
public:
    CAeResampler();
    ~CAeResampler();

    BOOL
    Initialize(
        _In_ ULONG Channels,
        _In_ ULONG InRate,
        _In_ ULONG OutRate);

    VOID Reset();

    /* Frames Push can still take */
    ULONG Space() const;

    VOID
    Push(
        _In_reads_(Frames * m_Channels) const float *Source,
        _In_ ULONG Frames);

    ULONG
    Pull(
        _Out_writes_(MaxFrames * m_Channels) float *Destination,
        _In_ ULONG MaxFrames);

    /* Input frames needed before Pull can give Frames more */
    ULONG
    InputNeeded(
        _In_ ULONG Frames) const;

    BOOL IsPassThrough() const { return m_InRate == m_OutRate; }

private:
    float *m_Fifo;
    ULONG m_Capacity;
    ULONG m_Count;
    double m_Position;
    double m_Step;
    ULONG m_Channels;
    ULONG m_InRate;
    ULONG m_OutRate;
};

/* endpoints.cpp */
typedef struct _AE_ENDPOINT
{
    ULONG Index;
    ULONG Flow;
    UINT WaveDevice;
    GUID Guid;
    WCHAR Id[AE_ID_LENGTH];
    WCHAR WaveName[MAXPNAMELEN];
    ULONG WaveOrdinal;
    BOOL Present;

    /* What the device is opened with, and what clients mix in */
    AE_FORMAT DeviceFormat;
    WAVEFORMATEXTENSIBLE DeviceFormatEx;
    AE_FORMAT MixFormat;
    ULONG PeriodFrames;

    /* Gain applied after mixing, from the endpoint volume */
    float ChannelGain[AE_MAX_CHANNELS];

    /* devices.cpp */
    HANDLE Thread;
    HANDLE WakeEvent;
    volatile LONG StopRequested;
    ULONG StreamCount;
    DWORD IdleSince;
} AE_ENDPOINT, *PAE_ENDPOINT;

extern AE_ENDPOINT AeEndpoints[AE_MAX_ENDPOINTS];
extern ULONG AeEndpointCount;

/* Set when devices changed while an endpoint was open, so the wave list waits until none is */
extern BOOL AeWaveRefreshPending;

VOID
AeBuildEndpoints(VOID);

VOID
AeLoadDefaults(VOID);

VOID
AeSaveDefault(
    _In_ ULONG Endpoint,
    _In_ ULONG Role);

VOID
AeSaveEndpointVolume(
    _In_ ULONG Endpoint);

PAE_ENDPOINT
AeFindEndpoint(
    _In_ PCWSTR Id);

/* Range and taper of endpoints without hardware volume */
#define AE_VOLUME_MIN_DB        -96.0f
#define AE_VOLUME_MAX_DB        0.0f
#define AE_VOLUME_STEP_DB       1.5f
#define AE_VOLUME_TAPER         1.75f

float
AeScalarToDb(
    _In_ float Scalar);

float
AeDbToScalar(
    _In_ float Db);

VOID
AeUpdateEndpointGain(
    _In_ ULONG Endpoint);

/* server.cpp */
struct AE_CONNECTION;

typedef struct _AE_STREAM
{
    LIST_ENTRY Link;
    ULONG Id;
    ULONG Kind;
    ULONG Endpoint;
    ULONG Session;
    AE_CONNECTION *Owner;
    ULONG ProcessId;
    BOOL EventDriven;

    /* The ring's geometry; the copy in the shared header is the client's to overwrite */
    HANDLE Section;
    PAE_STREAM_SHARED Shared;
    BYTE *Ring;
    ULONG BufferFrames;
    ULONG FrameBytes;
    BOOL Running;
    HANDLE Event;
    HANDLE ClientEvent;

    /* The client's format, and the conversion between it and the endpoint's mix format */
    AE_FORMAT Format;
    CAeChannelMap ChannelMap;
    CAeResampler Resampler;
    float *Scratch;
    float *Converted;
    ULONG ScratchFrames;
    LONGLONG LatencyFrames;

    /* Process loopback: whose audio, and which render streams feed it */
    ULONG TargetProcessId;
    BOOL IncludeTree;
    ULONG LoopbackSlot;
    float *Accumulator;

    /* Render streams: which process loopbacks they feed, and when that was worked out */
    ULONG LoopbackStamp;
    ULONG LoopbackMask;
} AE_STREAM, *PAE_STREAM;

extern CRITICAL_SECTION AeLock;
extern LIST_ENTRY AeStreams;
extern PAE_STATE AeState;
extern PAE_STREAM AeLoopbacks[AE_MAX_LOOPBACKS];
extern volatile LONG AeLoopbackStamp;

VOID
AeSignalChange(VOID);

VOID
AeSignalStream(
    _In_ PAE_STREAM Stream);

BOOL
AeStreamMatchesLoopback(
    _In_ PAE_STREAM Stream,
    _In_ PAE_STREAM Loopback);

/* devices.cpp */
VOID
AeEndpointStreamAdded(
    _In_ ULONG Endpoint);

VOID
AeEndpointStreamRemoved(
    _In_ ULONG Endpoint);

VOID
AeStopAllDevices(VOID);

ULONG
AeRenderPull(
    _In_ PAE_STREAM Stream,
    _Out_writes_(Frames * AeEndpoints[Stream->Endpoint].MixFormat.Channels) float *Destination,
    _In_ ULONG Frames);

VOID
AeCapturePush(
    _In_ PAE_STREAM Stream,
    _In_reads_(Frames * Channels) const float *Source,
    _In_ ULONG Channels,
    _In_ ULONG Frames,
    _In_ BOOL Silent);

/* wavedrv.cpp */
typedef struct _AE_WAVE
{
    ULONG Flow;
    UINT Device;
    DWORD_PTR Instance;
} AE_WAVE, *PAE_WAVE;

BOOL
AeWaveRefresh(VOID);

UINT
AeWaveGetCount(
    _In_ ULONG Flow);

MMRESULT
AeWaveGetName(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _Out_writes_(MAXPNAMELEN) PWSTR Name);

MMRESULT
AeWaveQuery(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _In_ const WAVEFORMATEX *Format);

MMRESULT
AeWaveOpen(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _In_ const WAVEFORMATEX *Format,
    _In_ HANDLE Event,
    _Out_ PAE_WAVE Wave);

MMRESULT
AeWavePrepare(
    _In_ PAE_WAVE Wave,
    _Inout_ WAVEHDR *Header);

MMRESULT
AeWaveUnprepare(
    _In_ PAE_WAVE Wave,
    _Inout_ WAVEHDR *Header);

MMRESULT
AeWaveSubmit(
    _In_ PAE_WAVE Wave,
    _Inout_ WAVEHDR *Header);

MMRESULT
AeWaveStart(
    _In_ PAE_WAVE Wave);

MMRESULT
AeWaveReset(
    _In_ PAE_WAVE Wave);

MMRESULT
AeWaveClose(
    _In_ PAE_WAVE Wave);

/* Entry points for audiosrv */
EXTERN_C
BOOL
AeStartEngine(VOID);

EXTERN_C
VOID
AeStopEngine(VOID);

EXTERN_C
VOID
AeDevicesChanged(VOID);
