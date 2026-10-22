/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Protocol between the audio engine in audiosrv and its clients in mmdevapi
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <mmreg.h>

/*
 * Clients talk to the engine over one message mode pipe per process. Audio itself moves through
 * a section per stream, a ring the producer advances WritePosition in and the consumer advances
 * ReadPosition in, with an event the engine sets once per engine period. Endpoints, defaults,
 * sessions, volumes and meters are published in one global section clients map read only; each
 * item carries a sequence number, and the engine sets the event a client gave it with
 * AeCommandWatch whenever any of it changes.
 */

#define AE_PIPE_NAME            L"\\\\.\\pipe\\ReactOS.AudioEngine"
#define AE_STATE_SECTION_NAME   L"Global\\ReactOS.AudioEngine.State"

#define AE_STATE_MAGIC          0x53454152 /* 'RAES' */
#define AE_STREAM_MAGIC         0x54534552 /* 'REST' */
#define AE_PROTOCOL_VERSION     2

#define AE_MAX_ENDPOINTS        16
#define AE_MAX_SESSIONS         64
#define AE_MAX_CHANNELS         8
#define AE_ID_LENGTH            64
#define AE_NAME_LENGTH          128
#define AE_PATH_LENGTH          260
#define AE_FORMAT_BYTES         64

#define AE_NO_ENDPOINT          0xFFFFFFFF

/* Data flows, the same values as EDataFlow */
#define AE_FLOW_RENDER          0
#define AE_FLOW_CAPTURE         1

#define AE_ROLE_COUNT           3

/* Endpoint volume updates kept for clients that look after several came in */
#define AE_VOLUME_HISTORY       16

typedef struct _AE_VOLUME_RECORD
{
    LONG Sequence;
    GUID Context;
    float MasterScalar;
    ULONG Mute;
    float ChannelScalar[AE_MAX_CHANNELS];
} AE_VOLUME_RECORD, *PAE_VOLUME_RECORD;

typedef struct _AE_ENDPOINT_STATE
{
    WCHAR Id[AE_ID_LENGTH];
    WCHAR FriendlyName[AE_NAME_LENGTH];
    ULONG Flow;
    ULONG DeviceState;
    ULONG Channels;
    ULONG ChannelMask;
    BYTE MixFormat[AE_FORMAT_BYTES];

    /* Volume as IAudioEndpointVolume reports it; Sequence changes with every update */
    volatile LONG VolumeSequence;
    GUID VolumeContext;
    float MasterLevelDb;
    float MasterScalar;
    float ChannelLevelDb[AE_MAX_CHANNELS];
    float ChannelScalar[AE_MAX_CHANNELS];
    ULONG Mute;

    /* Update n is in VolumeHistory[n % AE_VOLUME_HISTORY], written before VolumeSequence reaches n */
    AE_VOLUME_RECORD VolumeHistory[AE_VOLUME_HISTORY];

    /* Peak of the last engine period, after endpoint volume */
    float PeakMaster;
    float PeakChannel[AE_MAX_CHANNELS];
} AE_ENDPOINT_STATE, *PAE_ENDPOINT_STATE;

typedef struct _AE_SESSION_STATE
{
    /* A slot is reused; Generation tells a client its session is gone */
    ULONG InUse;
    ULONG Generation;
    ULONG Endpoint;
    GUID SessionGuid;
    ULONG ProcessId;
    ULONG CrossProcess;
    ULONG SystemSounds;
    ULONG State;
    volatile LONG StateSequence;

    volatile LONG VolumeSequence;
    GUID VolumeContext;
    float Volume;
    ULONG Mute;
    ULONG Channels;
    float ChannelVolume[AE_MAX_CHANNELS];

    volatile LONG PropertySequence;
    GUID PropertyContext;
    WCHAR DisplayName[AE_PATH_LENGTH];
    WCHAR IconPath[AE_PATH_LENGTH];
    GUID GroupingParam;

    float Peak;
    float PeakChannel[AE_MAX_CHANNELS];
} AE_SESSION_STATE, *PAE_SESSION_STATE;

typedef struct _AE_STATE
{
    ULONG Magic;
    ULONG Version;

    /* Endpoints appearing, going or changing state, and default changes */
    volatile LONG DeviceSequence;
    GUID LastStateChangeId;
    volatile LONG DefaultSequence;

    /* A session was created */
    volatile LONG SessionSequence;

    ULONG EndpointCount;
    AE_ENDPOINT_STATE Endpoints[AE_MAX_ENDPOINTS];
    ULONG DefaultEndpoint[2][AE_ROLE_COUNT];
    AE_SESSION_STATE Sessions[AE_MAX_SESSIONS];
} AE_STATE, *PAE_STATE;

/* Per stream section, shared by the client and the engine */
typedef struct _AE_STREAM_SHARED
{
    ULONG Magic;
    ULONG BufferFrames;
    ULONG FrameBytes;
    ULONG PeriodFrames;
    ULONG RingOffset;
    ULONG StagingOffset;
    ULONG Channels;
    ULONG SamplesPerSec;

    /* Frames produced and consumed since the last reset */
    volatile LONGLONG WritePosition;
    volatile LONGLONG ReadPosition;

    /* Frames that reached the device, and when, in 100 ns QPC units */
    volatile LONGLONG DevicePosition;
    volatile LONGLONG DevicePositionTime;

    volatile LONG Running;
    volatile LONG Discontinuity;
    volatile LONG Silent;

    /* IAudioStreamVolume, written by the client */
    float Volume[AE_MAX_CHANNELS];
} AE_STREAM_SHARED, *PAE_STREAM_SHARED;

/* Stream kinds and options for AE_OPEN_STREAM */
#define AE_STREAM_RENDER            0x00000001
#define AE_STREAM_CAPTURE           0x00000002
#define AE_STREAM_LOOPBACK          0x00000004
#define AE_STREAM_PROCESS_LOOPBACK  0x00000008

typedef enum _AE_COMMAND
{
    AeCommandHello = 1,
    AeCommandOpenStream,
    AeCommandCloseStream,
    AeCommandStart,
    AeCommandStop,
    AeCommandReset,
    AeCommandSetEvent,
    AeCommandSetEndpointVolume,
    AeCommandOpenSession,
    AeCommandSetSession,
    AeCommandSetDefault,
    AeCommandRescan,
    AeCommandWatch,
} AE_COMMAND;

typedef struct _AE_REQUEST_HEADER
{
    ULONG Command;
    ULONG Size;
} AE_REQUEST_HEADER;

typedef struct _AE_REPLY_HEADER
{
    HRESULT Status;
    ULONG Size;
} AE_REPLY_HEADER;

typedef struct _AE_HELLO_REQUEST
{
    AE_REQUEST_HEADER Header;
    ULONG Version;
    ULONG ProcessId;
} AE_HELLO_REQUEST;

typedef struct _AE_OPEN_STREAM_REQUEST
{
    AE_REQUEST_HEADER Header;
    WCHAR EndpointId[AE_ID_LENGTH];
    ULONG Kind;
    ULONG StreamFlags;
    LONGLONG BufferDuration;
    LONGLONG Periodicity;
    GUID SessionGuid;
    ULONG TargetProcessId;
    ULONG IncludeProcessTree;
    BYTE Format[AE_FORMAT_BYTES];
} AE_OPEN_STREAM_REQUEST;

typedef struct _AE_OPEN_STREAM_REPLY
{
    AE_REPLY_HEADER Header;
    ULONG StreamId;
    ULONG BufferFrames;
    ULONG PeriodFrames;
    ULONG Session;
    ULONG SessionGeneration;
    ULONG64 Section;
    ULONG64 Event;
    LONGLONG Latency;
} AE_OPEN_STREAM_REPLY;

typedef struct _AE_STREAM_REQUEST
{
    AE_REQUEST_HEADER Header;
    ULONG StreamId;
    ULONG64 Handle;
} AE_STREAM_REQUEST;

/* What AE_SET_ENDPOINT_VOLUME and AE_SET_SESSION change */
#define AE_SET_MASTER_DB        1
#define AE_SET_MASTER_SCALAR    2
#define AE_SET_CHANNEL_DB       3
#define AE_SET_CHANNEL_SCALAR   4
#define AE_SET_MUTE             5
#define AE_SET_VOLUME           6
#define AE_SET_DISPLAY_NAME     7
#define AE_SET_ICON_PATH        8
#define AE_SET_GROUPING         9

typedef struct _AE_SET_ENDPOINT_VOLUME_REQUEST
{
    AE_REQUEST_HEADER Header;
    ULONG Endpoint;
    ULONG What;
    ULONG Channel;
    float Value;
    ULONG Mute;
    GUID Context;
} AE_SET_ENDPOINT_VOLUME_REQUEST;

typedef struct _AE_OPEN_SESSION_REQUEST
{
    AE_REQUEST_HEADER Header;
    ULONG Endpoint;
    GUID SessionGuid;
    ULONG CrossProcess;
} AE_OPEN_SESSION_REQUEST;

typedef struct _AE_OPEN_SESSION_REPLY
{
    AE_REPLY_HEADER Header;
    ULONG Session;
    ULONG Generation;
} AE_OPEN_SESSION_REPLY;

typedef struct _AE_SET_SESSION_REQUEST
{
    AE_REQUEST_HEADER Header;
    ULONG Session;
    ULONG Generation;
    ULONG What;
    ULONG Channel;
    float Value;
    ULONG Mute;
    GUID Guid;
    GUID Context;
    WCHAR Text[AE_PATH_LENGTH];
} AE_SET_SESSION_REQUEST;

typedef struct _AE_SET_DEFAULT_REQUEST
{
    AE_REQUEST_HEADER Header;
    ULONG Endpoint;
    ULONG Role;
} AE_SET_DEFAULT_REQUEST;

typedef union _AE_REQUEST
{
    AE_REQUEST_HEADER Header;
    AE_HELLO_REQUEST Hello;
    AE_OPEN_STREAM_REQUEST OpenStream;
    AE_STREAM_REQUEST Stream;
    AE_SET_ENDPOINT_VOLUME_REQUEST SetEndpointVolume;
    AE_OPEN_SESSION_REQUEST OpenSession;
    AE_SET_SESSION_REQUEST SetSession;
    AE_SET_DEFAULT_REQUEST SetDefault;
} AE_REQUEST;

typedef union _AE_REPLY
{
    AE_REPLY_HEADER Header;
    AE_OPEN_STREAM_REPLY OpenStream;
    AE_OPEN_SESSION_REPLY OpenSession;
} AE_REPLY;
