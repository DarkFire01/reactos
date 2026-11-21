/*
 * PROJECT:     ReactOS Qualcomm audio (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WaveRT render miniport and stream
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "tables.hpp"

#define QCA_LOG(...) DbgPrint("[qcomaudio] " __VA_ARGS__)

#define QCA_TAG                     'aMcQ'

#ifdef _MSC_VER
#pragma code_seg("PAGE")
#endif

NTSTATUS
NTAPI
CreateMiniportWaveQcom(PUNKNOWN *Unknown, REFCLSID, PUNKNOWN OuterUnknown, POOL_TYPE PoolType)
{
    PAGED_CODE();
    STD_CREATE_BODY_(CMiniportWaveQcom, Unknown, OuterUnknown, PoolType, PMINIPORTWAVERT);
}

CMiniportWaveQcom::~CMiniportWaveQcom()
{
    PAGED_CODE();
    if (Port)
        Port->Release();
}

STDMETHODIMP
CMiniportWaveQcom::NonDelegatingQueryInterface(REFIID Interface, PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
        *Object = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
        *Object = PVOID(PMINIPORTWAVERT(this));
    else
        *Object = NULL;

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcom::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();
    *Description = &WaveMiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcom::DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE, ULONG, PVOID, PULONG)
{
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcom::Init(PUNKNOWN, PRESOURCELIST, PPORTWAVERT PortWaveRt)
{
    PAGED_CODE();

    Port = PortWaveRt;
    Port->AddRef();
    StreamCount = 0;

    QCA_LOG("wave miniport init, q6 core %s\n", QcQ6CoreIsUp() ? "up" : "not up yet");
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcom::NewStream(PMINIPORTWAVERTSTREAM *OutStream, PPORTWAVERTSTREAM PortStream,
                            ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();

    QCA_LOG("new stream on pin %lu%s\n", Pin, Capture ? " (capture)" : "");

    if (Capture || Pin != PIN_WAVE_RENDER_SINK)
        return STATUS_INVALID_PARAMETER;

    /* The dsp graph carries one playback session */
    if (StreamCount)
    {
        QCA_LOG("stream rejected, a playback session is already open\n");
        return STATUS_DEVICE_BUSY;
    }

    CMiniportWaveQcomStream *Stream = new(NonPagedPool) CMiniportWaveQcomStream(NULL);
    if (!Stream)
        return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS Status = Stream->Init(this, PortStream, DataFormat);
    if (!NT_SUCCESS(Status))
    {
        delete Stream;
        return Status;
    }

    *OutStream = PMINIPORTWAVERTSTREAM(Stream);
    (*OutStream)->AddRef();
    StreamCount++;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcom::GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription)
{
    PAGED_CODE();

    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = InterfaceTypeUndefined;
    DeviceDescription->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

/* ---- stream ---- */

CMiniportWaveQcomStream::~CMiniportWaveQcomStream()
{
    PAGED_CODE();

    if (Running)
        QcQ6PlaybackStop();

    FreeSharedBuffer();

    if (Miniport && Miniport->StreamCount)
        Miniport->StreamCount -= 1;
    if (PortStream)
        PortStream->Release();
}

STDMETHODIMP
CMiniportWaveQcomStream::NonDelegatingQueryInterface(REFIID Interface, PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification))
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    else
        *Object = NULL;

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
CMiniportWaveQcomStream::Init(CMiniportWaveQcom *Parent, PPORTWAVERTSTREAM Stream, PKSDATAFORMAT Format)
{
    PAGED_CODE();

    Miniport = Parent;
    PortStream = Stream;
    PortStream->AddRef();
    State = KSSTATE_STOP;
    Running = FALSE;
    BufferMdl = NULL;
    BufferVa = NULL;
    BufferBytes = 0;
    BufferPhys = 0;
    Periods = 0;
    PeriodBytes = 0;
    PeriodsPerNotification = 0;
    KeInitializeSpinLock(&EventLock);
    RtlZeroMemory(Events, sizeof(Events));

    return SetFormat(Format);
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::SetFormat(PKSDATAFORMAT Format)
{
    PAGED_CODE();

    if (Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX))
        return STATUS_INVALID_PARAMETER;

    PWAVEFORMATEX Wfx = &((PKSDATAFORMAT_WAVEFORMATEX)Format)->WaveFormatEx;
    if (Wfx->nChannels < 1 || Wfx->nChannels > QCA_MAX_CHANNELS ||
        Wfx->wBitsPerSample != 16 ||
        Wfx->nSamplesPerSec < QCA_MIN_RATE || Wfx->nSamplesPerSec > QCA_MAX_RATE)
    {
        QCA_LOG("format rejected: %lu Hz %u ch %u bit, tag 0x%x\n",
                Wfx->nSamplesPerSec, Wfx->nChannels, Wfx->wBitsPerSample, Wfx->wFormatTag);
        return STATUS_INVALID_PARAMETER;
    }

    /* The dsp session is configured once at run, it cannot change format underneath */
    if (Running)
        return STATUS_INVALID_DEVICE_STATE;

    SampleRate = Wfx->nSamplesPerSec;
    Channels = Wfx->nChannels;
    BitsPerSample = Wfx->wBitsPerSample;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::SetState(KSSTATE NewState)
{
    PAGED_CODE();

    if (NewState == KSSTATE_RUN && !Running)
    {
        NTSTATUS Status;

        if (!BufferPhys || !PeriodBytes)
            return STATUS_INVALID_DEVICE_STATE;

        Status = QcQ6PlaybackStart(BufferPhys, BufferBytes, PeriodBytes,
                                   SampleRate, Channels, BitsPerSample,
                                   PeriodDone, this);
        if (!NT_SUCCESS(Status))
        {
            QCA_LOG("q6 playback start failed 0x%08lx (q6 core %s)\n",
                    Status, QcQ6CoreIsUp() ? "up" : "down");
            return Status;
        }

        Running = TRUE;
        QCA_LOG("stream run: %lu Hz %lu ch, %lu periods of %lu bytes\n",
                SampleRate, Channels, Periods, PeriodBytes);
    }
    else if (NewState != KSSTATE_RUN && Running)
    {
        /* The graph restarts from the top of the buffer on the next run */
        QcQ6PlaybackStop();
        Running = FALSE;
    }

    State = NewState;
    return STATUS_SUCCESS;
}

#ifdef _MSC_VER
#pragma code_seg()
#endif

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::GetPosition(PKSAUDIO_POSITION Position)
{
    ULONGLONG Pos = 0;

    /* The dsp reports total bytes rendered; the ring position is that modulo size */
    if (BufferBytes && Running)
        Pos = QcQ6PlaybackPosition() % BufferBytes;

    Position->PlayOffset = Pos;
    Position->WriteOffset = Pos;
    return STATUS_SUCCESS;
}

#ifdef _MSC_VER
#pragma code_seg("PAGE")
#endif

/* The dsp reads the buffer by physical address, so it is contiguous and not cached */
NTSTATUS
CMiniportWaveQcomStream::AllocateSharedBuffer(ULONG RequestedSize, ULONG NotificationCount)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    ULONG Size;

    PAGED_CODE();

    if (Running)
        return STATUS_INVALID_DEVICE_STATE;

    FreeSharedBuffer();

    if (NotificationCount < 1)
        NotificationCount = 2;
    if (NotificationCount > QCA_MAX_EVENTS)
        NotificationCount = QCA_MAX_EVENTS;

    /* Two dsp periods per notification keep one period queued while the client refills */
    Size = ROUND_TO_PAGES(max(RequestedSize, (ULONG)PAGE_SIZE));
    Periods = NotificationCount * 2;
    PeriodBytes = (Size / Periods) & ~7UL;
    if (!PeriodBytes)
        return STATUS_INVALID_PARAMETER;
    PeriodsPerNotification = 2;

    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFF;
    Boundary.QuadPart = 0;
    BufferVa = MmAllocateContiguousMemorySpecifyCache(Size, Low, High, Boundary, MmWriteCombined);
    if (!BufferVa)
        return STATUS_INSUFFICIENT_RESOURCES;

    BufferMdl = IoAllocateMdl(BufferVa, Size, FALSE, FALSE, NULL);
    if (!BufferMdl)
    {
        MmFreeContiguousMemorySpecifyCache(BufferVa, Size, MmWriteCombined);
        BufferVa = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(BufferMdl);

    RtlZeroMemory(BufferVa, Size);
    BufferBytes = Size;
    BufferPhys = MmGetPhysicalAddress(BufferVa).QuadPart;

    QCA_LOG("buffer %lu bytes at 0x%I64x, %lu periods\n", BufferBytes, BufferPhys, Periods);
    return STATUS_SUCCESS;
}

VOID
CMiniportWaveQcomStream::FreeSharedBuffer()
{
    PAGED_CODE();

    if (BufferMdl)
    {
        IoFreeMdl(BufferMdl);
        BufferMdl = NULL;
    }
    if (BufferVa)
    {
        MmFreeContiguousMemorySpecifyCache(BufferVa, BufferBytes, MmWriteCombined);
        BufferVa = NULL;
    }
    BufferBytes = 0;
    BufferPhys = 0;
    PeriodBytes = 0;
    Periods = 0;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::AllocateAudioBuffer(ULONG RequestedSize, PMDL *AudioBufferMdl,
                                             ULONG *ActualSize, ULONG *OffsetFromFirstPage,
                                             MEMORY_CACHING_TYPE *CacheType)
{
    return AllocateBufferWithNotification(0, RequestedSize, AudioBufferMdl, ActualSize,
                                          OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::AllocateBufferWithNotification(ULONG NotificationCount, ULONG RequestedSize,
                                                        PMDL *AudioBufferMdl, ULONG *ActualSize,
                                                        ULONG *OffsetFromFirstPage,
                                                        MEMORY_CACHING_TYPE *CacheType)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = AllocateSharedBuffer(RequestedSize, NotificationCount);
    if (!NT_SUCCESS(Status))
        return Status;

    *AudioBufferMdl = BufferMdl;
    *ActualSize = BufferBytes;
    *OffsetFromFirstPage = 0;
    *CacheType = MmWriteCombined;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CMiniportWaveQcomStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG)
{
    PAGED_CODE();

    if (AudioBufferMdl == BufferMdl)
        FreeSharedBuffer();
}

STDMETHODIMP_(VOID)
CMiniportWaveQcomStream::FreeBufferWithNotification(PMDL AudioBufferMdl, ULONG BufferSize)
{
    FreeAudioBuffer(AudioBufferMdl, BufferSize);
}

#ifdef _MSC_VER
#pragma code_seg()
#endif

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::RegisterNotificationEvent(PKEVENT NotificationEvent)
{
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&EventLock, &OldIrql);
    for (i = 0; i < QCA_MAX_EVENTS; i++)
    {
        if (!Events[i])
        {
            Events[i] = NotificationEvent;
            Status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&EventLock, OldIrql);
    return Status;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::UnregisterNotificationEvent(PKEVENT NotificationEvent)
{
    NTSTATUS Status = STATUS_NOT_FOUND;
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&EventLock, &OldIrql);
    for (i = 0; i < QCA_MAX_EVENTS; i++)
    {
        if (Events[i] == NotificationEvent)
        {
            Events[i] = NULL;
            Status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&EventLock, OldIrql);
    return Status;
}

#ifdef _MSC_VER
#pragma code_seg("PAGE")
#endif

STDMETHODIMP_(VOID)
CMiniportWaveQcomStream::GetHWLatency(KSRTAUDIO_HWLATENCY *Latency)
{
    PAGED_CODE();
    Latency->FifoSize = PeriodBytes;
    Latency->ChipsetDelay = 0;
    Latency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::GetPositionRegister(KSRTAUDIO_HWREGISTER *)
{
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveQcomStream::GetClockRegister(KSRTAUDIO_HWREGISTER *)
{
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

#ifdef _MSC_VER
#pragma code_seg()
#endif

/* Runs at DISPATCH_LEVEL from the dsp write done, wakes the client each half buffer */
VOID
NTAPI
CMiniportWaveQcomStream::PeriodDone(PVOID Context, ULONG Period)
{
    CMiniportWaveQcomStream *Stream = (CMiniportWaveQcomStream *)Context;
    ULONG i;

    if (!Stream->PeriodsPerNotification || (Period + 1) % Stream->PeriodsPerNotification)
        return;

    KeAcquireSpinLockAtDpcLevel(&Stream->EventLock);
    for (i = 0; i < QCA_MAX_EVENTS; i++)
    {
        if (Stream->Events[i])
            KeSetEvent(Stream->Events[i], IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLockFromDpcLevel(&Stream->EventLock);
}
