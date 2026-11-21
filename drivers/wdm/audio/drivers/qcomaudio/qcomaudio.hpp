/*
 * PROJECT:     ReactOS Qualcomm audio (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WaveRT speaker endpoint over the Q6 audio dsp
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _QCOMAUDIO_HPP_
#define _QCOMAUDIO_HPP_

/*
 * The audio dsp is reached over APR once qcsubsys has booted it and the Q6
 * core answers. This adapter registers a render WaveRT filter wired to a
 * topology speaker endpoint so the audio stack sees a speaker. The stream
 * shares a contiguous buffer with the dsp; running it drives the Q6 playback
 * graph, and each period the dsp finishes signals the client's event.
 */

/* The placement new/delete come from the stdunk lib; the headers only
   declare them when the global _NEW_DELETE_OPERATORS_ is not set. */
PVOID __cdecl operator new(size_t Size, POOL_TYPE PoolType);
PVOID __cdecl operator new(size_t Size, POOL_TYPE PoolType, ULONG Tag);

/* Wave render filter pins */
#define PIN_WAVE_RENDER_SINK        0
#define PIN_WAVE_RENDER_SOURCE      1

/* Wave render nodes */
#define NODE_WAVE_DAC               0

/* Topology filter pins. The microphone pin and its capture side exist for the
   call endpoint pair; the earpiece joins once its codec is driven */
#define PIN_TOPO_WAVE_IN            0
#define PIN_TOPO_SPEAKER            1
#define PIN_TOPO_MICROPHONE         2
#define PIN_TOPO_CAPTURE_OUT        3

/* Topology nodes */
#define NODE_TOPO_VOLUME            0
#define NODE_TOPO_MUTE              1

#define MAX_MINIPORTS               2

/* Notification events a stream can hold at once */
#define QCA_MAX_EVENTS              4

/* The speaker volume range in 1/65536 dB, the amp moves in quarter dB steps */
#define QCA_VOLUME_MIN              (-14 * 65536)
#define QCA_VOLUME_MAX              0
#define QCA_VOLUME_STEP             (65536 / 4)

typedef VOID (NTAPI *PQCA_PERIOD_DONE)(PVOID Context, ULONG Period);

extern "C" BOOLEAN NTAPI QcQ6CoreIsUp(VOID);
extern "C" NTSTATUS NTAPI QcQ6PlaybackStart(ULONGLONG BufferPhys, ULONG BufferBytes,
                                            ULONG PeriodBytes, ULONG Rate, ULONG Channels, ULONG Bits,
                                            PQCA_PERIOD_DONE PeriodDone, PVOID Context);
extern "C" VOID NTAPI QcQ6PlaybackStop(VOID);
extern "C" ULONGLONG NTAPI QcQ6PlaybackPosition(VOID);
extern "C" NTSTATUS NTAPI QcQ6VoiceStart(VOID);
extern "C" VOID NTAPI QcQ6VoiceStop(VOID);
extern "C" NTSTATUS NTAPI QcQ6VoiceSetTxMute(BOOLEAN Mute, ULONG RampMs);
extern "C" VOID NTAPI QcAmpSetVolume(LONG Level);
extern "C" VOID NTAPI QcAmpSetMute(BOOLEAN Mute);

/* ntddk.h export, wdm.h leaves it out */
extern "C" NTKERNELAPI PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress(PVOID BaseAddress);

NTSTATUS NTAPI CreateMiniportWaveQcom(PUNKNOWN *Unknown, REFCLSID, PUNKNOWN OuterUnknown, POOL_TYPE PoolType);
NTSTATUS NTAPI CreateMiniportTopologyQcom(PUNKNOWN *Unknown, REFCLSID, PUNKNOWN OuterUnknown, POOL_TYPE PoolType);

NTSTATUS NTAPI TopoPropertyVolume(PPCPROPERTY_REQUEST Request);
NTSTATUS NTAPI TopoPropertyMute(PPCPROPERTY_REQUEST Request);

/* Cellular call audio, KSPROPSETID_TelephonyControl on the wave filter and
   KSPROPSETID_TelephonyTopology on the topology filter */
NTSTATUS NTAPI WavePropertyTelephony(PPCPROPERTY_REQUEST Request);
VOID NTAPI TelephonyInitialize(VOID);
NTSTATUS TopoBasicSupport(PPCPROPERTY_REQUEST Request, ULONG VariantType, BOOLEAN Ranged);
NTSTATUS NTAPI TopoPropertyTelephony(PPCPROPERTY_REQUEST Request);

class CMiniportWaveQcomStream;

class CMiniportWaveQcom : public IMiniportWaveRT,
                          public CUnknown
{
private:
    PPORTWAVERT Port;
    ULONG StreamCount;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveQcom);
    ~CMiniportWaveQcom();

    IMP_IMiniportWaveRT;

    friend class CMiniportWaveQcomStream;
};

class CMiniportWaveQcomStream : public IMiniportWaveRTStreamNotification,
                                public CUnknown
{
private:
    CMiniportWaveQcom *Miniport;
    PPORTWAVERTSTREAM PortStream;
    PMDL BufferMdl;
    PVOID BufferVa;
    ULONG BufferBytes;
    ULONGLONG BufferPhys;
    ULONG Periods;
    ULONG PeriodBytes;
    ULONG PeriodsPerNotification;
    KSSTATE State;
    BOOLEAN Running;
    ULONG SampleRate;
    ULONG Channels;
    ULONG BitsPerSample;
    KSPIN_LOCK EventLock;
    PKEVENT Events[QCA_MAX_EVENTS];

    NTSTATUS AllocateSharedBuffer(ULONG RequestedSize, ULONG NotificationCount);
    VOID FreeSharedBuffer();
    static VOID NTAPI PeriodDone(PVOID Context, ULONG Period);

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveQcomStream);
    ~CMiniportWaveQcomStream();

    IMP_IMiniportWaveRTStream;

    STDMETHODIMP_(NTSTATUS) AllocateBufferWithNotification(ULONG NotificationCount, ULONG RequestedSize,
                                                           PMDL *AudioBufferMdl, ULONG *ActualSize,
                                                           ULONG *OffsetFromFirstPage,
                                                           MEMORY_CACHING_TYPE *CacheType);
    STDMETHODIMP_(VOID) FreeBufferWithNotification(PMDL AudioBufferMdl, ULONG BufferSize);
    STDMETHODIMP_(NTSTATUS) RegisterNotificationEvent(PKEVENT NotificationEvent);
    STDMETHODIMP_(NTSTATUS) UnregisterNotificationEvent(PKEVENT NotificationEvent);

    NTSTATUS Init(CMiniportWaveQcom *Parent, PPORTWAVERTSTREAM Stream, PKSDATAFORMAT Format);

    friend class CMiniportWaveQcom;
};

class CMiniportTopologyQcom : public IMiniportTopology,
                              public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopologyQcom);
    ~CMiniportTopologyQcom();

    IMP_IMiniportTopology;
};

#endif /* _QCOMAUDIO_HPP_ */
