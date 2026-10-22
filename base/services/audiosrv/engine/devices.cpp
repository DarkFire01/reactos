/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Endpoint threads that mix streams to the wave devices and capture from them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "engine.h"

#include <math.h>

/* Meters show the loudest sample of the last ten engine periods */
#define AE_METER_PERIODS    10

typedef struct _AE_METER
{
    float Periods[AE_METER_PERIODS][AE_MAX_CHANNELS];
    ULONG Next;
} AE_METER;

static AE_METER AeEndpointMeters[AE_MAX_ENDPOINTS];
static AE_METER AeSessionMeters[AE_MAX_SESSIONS];

static
LONGLONG
AeNow100ns(VOID)
{
    static LARGE_INTEGER Frequency;
    LARGE_INTEGER Counter;

    if (!Frequency.QuadPart)
        QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Counter);
    return (LONGLONG)((double)Counter.QuadPart * 10000000.0 / (double)Frequency.QuadPart);
}

/* The stream's ring as one or two runs from Position, Frames long in all */
static
VOID
AeRingRuns(
    _In_ PAE_STREAM Stream,
    _In_ LONGLONG Position,
    _In_ ULONG Frames,
    _Out_ ULONG *FirstOffset,
    _Out_ ULONG *FirstFrames)
{
    ULONG Start = (ULONG)((ULONGLONG)Position % Stream->BufferFrames);

    *FirstOffset = Start * Stream->FrameBytes;
    *FirstFrames = min(Frames, Stream->BufferFrames - Start);
}

/* Reads Frames client frames from the ring into float, with the stream's volume applied */
static
VOID
AeReadRing(
    _In_ PAE_STREAM Stream,
    _In_ ULONG Frames,
    _Out_writes_(Frames * Stream->Format.Channels) float *Destination)
{
    PAE_STREAM_SHARED Shared = Stream->Shared;
    ULONG Offset, First, Frame, Channel;
    float Volume[AE_MAX_CHANNELS];
    float *Sample;

    AeRingRuns(Stream, Shared->ReadPosition, Frames, &Offset, &First);
    AeToFloat(&Stream->Format, Stream->Ring + Offset, Destination, First);
    if (First < Frames)
        AeToFloat(&Stream->Format, Stream->Ring, Destination + First * Stream->Format.Channels, Frames - First);

    /* Stream volumes come from the client; anything outside 0 to 1 is not a volume */
    for (Channel = 0; Channel < Stream->Format.Channels; Channel++)
        Volume[Channel] = (Shared->Volume[Channel] >= 0.0f) ? min(Shared->Volume[Channel], 1.0f) : 0.0f;

    Sample = Destination;
    for (Frame = 0; Frame < Frames; Frame++)
    {
        for (Channel = 0; Channel < Stream->Format.Channels; Channel++)
            *Sample++ *= Volume[Channel];
    }
    InterlockedExchange64(&Shared->ReadPosition, Shared->ReadPosition + Frames);
}

/**
 * @brief
 * Takes up to Frames frames of a render stream in the endpoint's mix format; the engine lock
 * must be held.
 *
 * @return
 * The frames produced; fewer than asked means the client fell behind.
 */
ULONG
AeRenderPull(
    _In_ PAE_STREAM Stream,
    _Out_writes_(Frames * AeEndpoints[Stream->Endpoint].MixFormat.Channels) float *Destination,
    _In_ ULONG Frames)
{
    PAE_STREAM_SHARED Shared = Stream->Shared;
    ULONG OutChannels = Stream->ChannelMap.OutChannels();
    LONGLONG Available = InterlockedCompareExchange64(&Shared->WritePosition, 0, 0) - Shared->ReadPosition;
    ULONG Produced = 0;
    ULONG Take;

    if (Available < 0 || Available > Stream->BufferFrames)
        Available = 0;

    if (Stream->Resampler.IsPassThrough())
    {
        Take = (ULONG)min((LONGLONG)Frames, Available);
        Take = min(Take, Stream->ScratchFrames);
        AeReadRing(Stream, Take, Stream->Scratch);
        Stream->ChannelMap.Apply(Stream->Scratch, Destination, Take);
        return Take;
    }

    while (Produced < Frames)
    {
        Produced += Stream->Resampler.Pull(Destination + (SIZE_T)Produced * OutChannels, Frames - Produced);
        if (Produced == Frames)
            break;

        Take = Stream->Resampler.InputNeeded(Frames - Produced);
        Take = (ULONG)min((LONGLONG)Take, Available);
        Take = min(Take, min(Stream->ScratchFrames, Stream->Resampler.Space()));
        if (!Take)
            break;

        AeReadRing(Stream, Take, Stream->Scratch);
        Stream->ChannelMap.Apply(Stream->Scratch, Stream->Converted, Take);
        Stream->Resampler.Push(Stream->Converted, Take);
        Available -= Take;
    }
    return Produced;
}

/* Writes float frames in the client's format into the ring, losing what does not fit */
static
VOID
AeWriteRing(
    _In_ PAE_STREAM Stream,
    _In_reads_(Frames * Stream->Format.Channels) const float *Source,
    _In_ ULONG Frames)
{
    PAE_STREAM_SHARED Shared = Stream->Shared;
    LONGLONG Used = Shared->WritePosition - InterlockedCompareExchange64(&Shared->ReadPosition, 0, 0);
    ULONG Space, Offset, First;

    Space = (Used < 0 || Used >= Stream->BufferFrames) ? 0 : Stream->BufferFrames - (ULONG)Used;
    if (Frames > Space)
    {
        InterlockedExchange(&Shared->Discontinuity, TRUE);
        Frames = Space;
    }
    if (!Frames)
        return;

    AeRingRuns(Stream, Shared->WritePosition, Frames, &Offset, &First);
    AeFromFloat(&Stream->Format, Source, Stream->Ring + Offset, First);
    if (First < Frames)
        AeFromFloat(&Stream->Format, Source + First * Stream->Format.Channels, Stream->Ring, Frames - First);
    InterlockedExchange64(&Shared->WritePosition, Shared->WritePosition + Frames);
}

/**
 * @brief
 * Gives a capture or loopback stream frames in the endpoint's mix format; the engine lock must
 * be held.
 */
VOID
AeCapturePush(
    _In_ PAE_STREAM Stream,
    _In_reads_(Frames * Channels) const float *Source,
    _In_ ULONG Channels,
    _In_ ULONG Frames,
    _In_ BOOL Silent)
{
    ULONG Done = 0, Chunk, Pulled;

    UNREFERENCED_PARAMETER(Channels);

    if (!Stream->Running)
        return;
    Stream->Shared->Silent = Silent;

    while (Done < Frames)
    {
        Chunk = min(Frames - Done, Stream->ScratchFrames);
        Stream->ChannelMap.Apply(Source + (SIZE_T)Done * Stream->ChannelMap.InChannels(), Stream->Scratch, Chunk);
        Done += Chunk;

        if (Stream->Resampler.IsPassThrough())
        {
            AeWriteRing(Stream, Stream->Scratch, Chunk);
            continue;
        }

        Stream->Resampler.Push(Stream->Scratch, min(Chunk, Stream->Resampler.Space()));
        for (;;)
        {
            Pulled = Stream->Resampler.Pull(Stream->Converted, Stream->ScratchFrames);
            if (!Pulled)
                break;
            AeWriteRing(Stream, Stream->Converted, Pulled);
        }
    }

    InterlockedExchange64(&Stream->Shared->DevicePositionTime, AeNow100ns());
    InterlockedExchange64(&Stream->Shared->DevicePosition, Stream->Shared->WritePosition);
}

static
float
AePeak(
    _In_reads_(Frames * Channels) const float *Samples,
    _In_ ULONG Channels,
    _In_ ULONG Frames,
    _Out_writes_(Channels) float *ChannelPeaks)
{
    ULONG Frame, Channel;
    float Value, Peak = 0.0f;

    for (Channel = 0; Channel < Channels; Channel++)
        ChannelPeaks[Channel] = 0.0f;
    for (Frame = 0; Frame < Frames; Frame++)
    {
        for (Channel = 0; Channel < Channels; Channel++)
        {
            Value = fabsf(*Samples++);
            if (Value > ChannelPeaks[Channel])
                ChannelPeaks[Channel] = Value;
        }
    }
    for (Channel = 0; Channel < Channels; Channel++)
        Peak = max(Peak, min(ChannelPeaks[Channel], 1.0f));
    return Peak;
}

/* Adds one period's channel peaks to a meter and publishes the loudest of its window */
static
VOID
AeMeterAdd(
    _Inout_ AE_METER *Meter,
    _In_reads_(Channels) const float *ChannelPeaks,
    _In_ ULONG Channels,
    _Out_ float *Peak,
    _Out_writes_(AE_MAX_CHANNELS) float *PeakChannel)
{
    ULONG Period, Channel;
    float Value, Loudest = 0.0f;

    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
        Meter->Periods[Meter->Next][Channel] = (Channel < Channels) ? ChannelPeaks[Channel] : 0.0f;
    Meter->Next = (Meter->Next + 1) % AE_METER_PERIODS;

    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
    {
        Value = 0.0f;
        for (Period = 0; Period < AE_METER_PERIODS; Period++)
            Value = max(Value, Meter->Periods[Period][Channel]);
        PeakChannel[Channel] = min(Value, 1.0f);
        Loudest = max(Loudest, PeakChannel[Channel]);
    }
    *Peak = Loudest;
}

/* Gain of a stream from its session; channel volumes of the session follow mix channels */
static
VOID
AeAddWithSessionGain(
    _In_ PAE_STREAM Stream,
    _In_reads_(Frames * Channels) const float *Source,
    _Inout_updates_(Frames * Channels) float *Mix,
    _In_ ULONG Channels,
    _In_ ULONG Frames)
{
    PAE_SESSION_STATE Session = &AeState->Sessions[Stream->Session];
    float Gain[AE_MAX_CHANNELS];
    float Value;
    ULONG Frame, Channel;

    for (Channel = 0; Channel < Channels; Channel++)
    {
        Gain[Channel] = Session->Mute ? 0.0f : Session->Volume;
        if (Channel < Session->Channels)
            Gain[Channel] *= Session->ChannelVolume[Channel];
    }

    for (Frame = 0; Frame < Frames; Frame++)
    {
        for (Channel = 0; Channel < Channels; Channel++)
        {
            Value = *Source++ * Gain[Channel];
            *Mix++ += Value;
        }
    }
}

static
VOID
AeAddToLoopback(
    _In_reads_(Frames * Channels) const float *Source,
    _Inout_updates_(Frames * Channels) float *Accumulator,
    _In_ float Gain,
    _In_ ULONG Channels,
    _In_ ULONG Frames)
{
    ULONG Samples = Frames * Channels, Index;

    for (Index = 0; Index < Samples; Index++)
        Accumulator[Index] += Source[Index] * Gain;
}

/**
 * @brief
 * Mixes one engine period of an endpoint into Mix and hands the result to its loopback streams.
 */
static
VOID
AeMixRenderPeriod(
    _In_ PAE_ENDPOINT Device,
    _Out_writes_(Device->PeriodFrames * Device->MixFormat.Channels) float *Mix,
    _Out_writes_(Device->PeriodFrames * Device->MixFormat.Channels) float *Temp)
{
    ULONG Frames = Device->PeriodFrames;
    ULONG Channels = Device->MixFormat.Channels;
    ULONG Samples = Frames * Channels;
    PAE_ENDPOINT_STATE State = &AeState->Endpoints[Device->Index];
    PAE_SESSION_STATE Session;
    PAE_STREAM Stream, Loopback;
    PLIST_ENTRY Entry;
    float ChannelPeaks[AE_MAX_CHANNELS];
    float SessionPeaks[AE_MAX_SESSIONS][AE_MAX_CHANNELS];
    float SessionGain;
    ULONG Got, Slot, Index, Channel;
    BOOL Audible = FALSE;
    LONGLONG Now = AeNow100ns();

    ZeroMemory(Mix, Samples * sizeof(float));
    ZeroMemory(SessionPeaks, sizeof(SessionPeaks));

    EnterCriticalSection(&AeLock);

    for (Slot = 0; Slot < AE_MAX_LOOPBACKS; Slot++)
    {
        Loopback = AeLoopbacks[Slot];
        if (Loopback && Loopback->Endpoint == Device->Index)
            ZeroMemory(Loopback->Accumulator, Samples * sizeof(float));
    }

    for (Entry = AeStreams.Flink; Entry != &AeStreams; Entry = Entry->Flink)
    {
        Stream = CONTAINING_RECORD(Entry, AE_STREAM, Link);
        if (Stream->Kind != AE_STREAM_RENDER || Stream->Endpoint != Device->Index || !Stream->Running)
            continue;

        Got = AeRenderPull(Stream, Temp, Frames);
        if (Got < Frames)
            ZeroMemory(Temp + (SIZE_T)Got * Channels, (Frames - Got) * Channels * sizeof(float));
        if (Got)
            Audible = TRUE;

        /* A session meters the loudest of its streams */
        Session = &AeState->Sessions[Stream->Session];
        AeAddWithSessionGain(Stream, Temp, Mix, Channels, Frames);
        AePeak(Temp, Channels, Frames, ChannelPeaks);
        SessionGain = Session->Mute ? 0.0f : Session->Volume;
        for (Channel = 0; Channel < Channels; Channel++)
        {
            SessionPeaks[Stream->Session][Channel] = max(SessionPeaks[Stream->Session][Channel],
                                                         ChannelPeaks[Channel] * SessionGain);
        }

        /* Which process loopbacks this stream feeds is worked out again when the set changes */
        if (Stream->LoopbackStamp != (ULONG)AeLoopbackStamp)
        {
            Stream->LoopbackMask = 0;
            for (Slot = 0; Slot < AE_MAX_LOOPBACKS; Slot++)
            {
                if (AeLoopbacks[Slot] && AeStreamMatchesLoopback(Stream, AeLoopbacks[Slot]))
                    Stream->LoopbackMask |= 1UL << Slot;
            }
            Stream->LoopbackStamp = (ULONG)AeLoopbackStamp;
        }
        for (Slot = 0; Slot < AE_MAX_LOOPBACKS; Slot++)
        {
            Loopback = AeLoopbacks[Slot];
            if ((Stream->LoopbackMask & (1UL << Slot)) && Loopback && Loopback->Endpoint == Device->Index)
                AeAddToLoopback(Temp, Loopback->Accumulator, SessionGain, Channels, Frames);
        }

        /* What the client hears now left the ring a device latency ago */
        InterlockedExchange64(&Stream->Shared->DevicePositionTime, Now);
        InterlockedExchange64(&Stream->Shared->DevicePosition, max(Stream->Shared->ReadPosition - Stream->LatencyFrames, 0));
        AeSignalStream(Stream);
    }

    /* Loopback is the mix before the endpoint volume, as the speakers would get it at full volume */
    for (Entry = AeStreams.Flink; Entry != &AeStreams; Entry = Entry->Flink)
    {
        Stream = CONTAINING_RECORD(Entry, AE_STREAM, Link);
        if (Stream->Kind != AE_STREAM_LOOPBACK || Stream->Endpoint != Device->Index || !Stream->Running)
            continue;
        AeCapturePush(Stream, Mix, Channels, Frames, !Audible);
        AeSignalStream(Stream);
    }
    for (Slot = 0; Slot < AE_MAX_LOOPBACKS; Slot++)
    {
        Loopback = AeLoopbacks[Slot];
        if (!Loopback || Loopback->Endpoint != Device->Index || !Loopback->Running)
            continue;
        AeCapturePush(Loopback, Loopback->Accumulator, Channels, Frames, FALSE);
        AeSignalStream(Loopback);
    }

    for (Index = 0; Index < AE_MAX_SESSIONS; Index++)
    {
        Session = &AeState->Sessions[Index];
        if (Session->InUse && Session->Endpoint == Device->Index)
            AeMeterAdd(&AeSessionMeters[Index], SessionPeaks[Index], Channels, &Session->Peak, Session->PeakChannel);
    }

    /* The endpoint meter reads the mix before the endpoint volume */
    AePeak(Mix, Channels, Frames, ChannelPeaks);
    AeMeterAdd(&AeEndpointMeters[Device->Index], ChannelPeaks, Channels, &State->PeakMaster, State->PeakChannel);

    for (Index = 0; Index < Samples; Index++)
        Mix[Index] *= Device->ChannelGain[Index % Channels];

    LeaveCriticalSection(&AeLock);
}

/* Decides under the lock whether an idle endpoint thread ends, so a new stream starts a new one */
static
BOOL
AeEndpointShouldExit(
    _In_ PAE_ENDPOINT Device)
{
    BOOL Exit = FALSE;

    EnterCriticalSection(&AeLock);
    if (Device->StopRequested ||
        (!Device->StreamCount && GetTickCount() - Device->IdleSince >= AE_IDLE_CLOSE_MS))
    {
        CloseHandle(Device->Thread);
        Device->Thread = NULL;
        Exit = TRUE;

        /* Meters fall to nothing once the device stops */
        AeState->Endpoints[Device->Index].PeakMaster = 0.0f;
        ZeroMemory(AeState->Endpoints[Device->Index].PeakChannel, sizeof(AeState->Endpoints[Device->Index].PeakChannel));
        ZeroMemory(&AeEndpointMeters[Device->Index], sizeof(AeEndpointMeters[Device->Index]));
    }
    LeaveCriticalSection(&AeLock);
    return Exit;
}

static
HANDLE
AeCreatePeriodTimer(VOID)
{
    LARGE_INTEGER Due;
    HANDLE Timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    Due.QuadPart = -AE_PERIOD_100NS;
    if (Timer)
        SetWaitableTimer(Timer, &Due, AE_PERIOD_100NS / 10000, NULL, NULL, FALSE);
    return Timer;
}

static
DWORD
WINAPI
AeRenderThread(
    _In_ LPVOID Context)
{
    PAE_ENDPOINT Device = (PAE_ENDPOINT)Context;
    ULONG Samples = Device->PeriodFrames * Device->MixFormat.Channels;
    ULONG BufferBytes = Device->PeriodFrames * Device->DeviceFormat.BlockAlign;
    WAVEHDR Headers[AE_DEVICE_BUFFERS];
    WAVEFORMATEX Format = Device->DeviceFormatEx.Format;
    AE_WAVE Wave;
    BOOL Open = FALSE;
    HANDLE Done, Timer = NULL;
    float *Mix, *Temp;
    BYTE *Buffers;
    ULONG Index;

    Mix = (float *)HeapAlloc(GetProcessHeap(), 0, Samples * sizeof(float) * 2);
    Buffers = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BufferBytes * AE_DEVICE_BUFFERS);
    Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Mix || !Buffers || !Done)
        goto Cleanup;
    Temp = Mix + Samples;

    /* Wave drivers take plain PCM more readily than the extensible form; a thread that is just
       ending may still hold the device for a moment */
    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.cbSize = 0;
    for (Index = 0; Device->Present && Index < AE_OPEN_ATTEMPTS; Index++)
    {
        Open = AeWaveOpen(AE_FLOW_RENDER, Device->WaveDevice, &Format, Done, &Wave) == MMSYSERR_NOERROR;
        if (Open)
            break;
        Sleep(AE_OPEN_RETRY_MS);
    }

    if (Open)
    {
        for (Index = 0; Index < AE_DEVICE_BUFFERS; Index++)
        {
            ZeroMemory(&Headers[Index], sizeof(Headers[Index]));
            Headers[Index].lpData = (LPSTR)(Buffers + Index * BufferBytes);
            Headers[Index].dwBufferLength = BufferBytes;
            AeWavePrepare(&Wave, &Headers[Index]);
            Headers[Index].dwFlags |= WHDR_DONE;
        }
    }
    else
    {
        /* No device: the clients still get their periods, and the mix goes nowhere */
        Timer = AeCreatePeriodTimer();
    }

    while (!AeEndpointShouldExit(Device))
    {
        WaitForSingleObject(Open ? Done : Timer, 200);

        if (!Open)
        {
            AeMixRenderPeriod(Device, Mix, Temp);
            continue;
        }

        for (Index = 0; Index < AE_DEVICE_BUFFERS; Index++)
        {
            if (!(Headers[Index].dwFlags & WHDR_DONE))
                continue;
            AeMixRenderPeriod(Device, Mix, Temp);
            AeFromFloat(&Device->DeviceFormat, Mix, (BYTE *)Headers[Index].lpData, Device->PeriodFrames);
            Headers[Index].dwFlags &= ~WHDR_DONE;
            AeWaveSubmit(&Wave, &Headers[Index]);
        }
    }

    if (Open)
    {
        AeWaveReset(&Wave);
        for (Index = 0; Index < AE_DEVICE_BUFFERS; Index++)
            AeWaveUnprepare(&Wave, &Headers[Index]);
        AeWaveClose(&Wave);
    }

Cleanup:
    if (Timer)
        CloseHandle(Timer);
    if (Done)
        CloseHandle(Done);
    if (Buffers)
        HeapFree(GetProcessHeap(), 0, Buffers);
    if (Mix)
        HeapFree(GetProcessHeap(), 0, Mix);
    return 0;
}

/* Hands one captured period to the endpoint's capture streams */
static
VOID
AeDistributeCapture(
    _In_ PAE_ENDPOINT Device,
    _Inout_updates_(Frames * Device->MixFormat.Channels) float *Samples,
    _In_ ULONG Frames)
{
    PAE_ENDPOINT_STATE State = &AeState->Endpoints[Device->Index];
    ULONG Channels = Device->MixFormat.Channels;
    ULONG Count = Frames * Channels, Index;
    float ChannelPeaks[AE_MAX_CHANNELS];
    PLIST_ENTRY Entry;
    PAE_STREAM Stream;

    for (Index = 0; Index < Count; Index++)
        Samples[Index] *= Device->ChannelGain[Index % Channels];

    EnterCriticalSection(&AeLock);
    AePeak(Samples, Channels, Frames, ChannelPeaks);
    AeMeterAdd(&AeEndpointMeters[Device->Index], ChannelPeaks, Channels, &State->PeakMaster, State->PeakChannel);
    for (Entry = AeStreams.Flink; Entry != &AeStreams; Entry = Entry->Flink)
    {
        Stream = CONTAINING_RECORD(Entry, AE_STREAM, Link);
        if (Stream->Kind != AE_STREAM_CAPTURE || Stream->Endpoint != Device->Index || !Stream->Running)
            continue;
        AeCapturePush(Stream, Samples, Channels, Frames, FALSE);
        AeSignalStream(Stream);
    }
    LeaveCriticalSection(&AeLock);
}

static
DWORD
WINAPI
AeCaptureThread(
    _In_ LPVOID Context)
{
    PAE_ENDPOINT Device = (PAE_ENDPOINT)Context;
    ULONG Samples = Device->PeriodFrames * Device->MixFormat.Channels;
    ULONG BufferBytes = Device->PeriodFrames * Device->DeviceFormat.BlockAlign;
    WAVEHDR Headers[AE_DEVICE_BUFFERS];
    WAVEFORMATEX Format = Device->DeviceFormatEx.Format;
    AE_WAVE Wave;
    BOOL Open = FALSE;
    HANDLE Done, Timer = NULL;
    BYTE *Buffers;
    float *Converted;
    ULONG Index, Frames;

    Converted = (float *)HeapAlloc(GetProcessHeap(), 0, Samples * sizeof(float));
    Buffers = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BufferBytes * AE_DEVICE_BUFFERS);
    Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Converted || !Buffers || !Done)
        goto Cleanup;

    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.cbSize = 0;
    for (Index = 0; Device->Present && Index < AE_OPEN_ATTEMPTS; Index++)
    {
        Open = AeWaveOpen(AE_FLOW_CAPTURE, Device->WaveDevice, &Format, Done, &Wave) == MMSYSERR_NOERROR;
        if (Open)
            break;
        Sleep(AE_OPEN_RETRY_MS);
    }

    if (Open)
    {
        for (Index = 0; Index < AE_DEVICE_BUFFERS; Index++)
        {
            ZeroMemory(&Headers[Index], sizeof(Headers[Index]));
            Headers[Index].lpData = (LPSTR)(Buffers + Index * BufferBytes);
            Headers[Index].dwBufferLength = BufferBytes;
            AeWavePrepare(&Wave, &Headers[Index]);
            AeWaveSubmit(&Wave, &Headers[Index]);
        }
        AeWaveStart(&Wave);
    }
    else
    {
        /* No device: capture clients get silence at the engine rate */
        Timer = AeCreatePeriodTimer();
    }

    while (!AeEndpointShouldExit(Device))
    {
        WaitForSingleObject(Open ? Done : Timer, 200);

        if (!Open)
        {
            ZeroMemory(Converted, Samples * sizeof(float));
            AeDistributeCapture(Device, Converted, Device->PeriodFrames);
            continue;
        }

        for (Index = 0; Index < AE_DEVICE_BUFFERS; Index++)
        {
            if (!(Headers[Index].dwFlags & WHDR_DONE))
                continue;

            Frames = min(Headers[Index].dwBytesRecorded / Device->DeviceFormat.BlockAlign, Device->PeriodFrames);
            if (Frames)
            {
                AeToFloat(&Device->DeviceFormat, (const BYTE *)Headers[Index].lpData, Converted, Frames);
                AeDistributeCapture(Device, Converted, Frames);
            }
            Headers[Index].dwFlags &= ~WHDR_DONE;
            AeWaveSubmit(&Wave, &Headers[Index]);
        }
    }

    if (Open)
    {
        AeWaveReset(&Wave);
        for (Index = 0; Index < AE_DEVICE_BUFFERS; Index++)
            AeWaveUnprepare(&Wave, &Headers[Index]);
        AeWaveClose(&Wave);
    }

Cleanup:
    if (Timer)
        CloseHandle(Timer);
    if (Done)
        CloseHandle(Done);
    if (Buffers)
        HeapFree(GetProcessHeap(), 0, Buffers);
    if (Converted)
        HeapFree(GetProcessHeap(), 0, Converted);
    return 0;
}

/**
 * @brief
 * Counts a stream on an endpoint and starts its thread if none runs; the engine lock must be held.
 */
VOID
AeEndpointStreamAdded(
    _In_ ULONG Endpoint)
{
    PAE_ENDPOINT Device = &AeEndpoints[Endpoint];

    Device->StreamCount++;
    if (Device->Thread)
        return;

    Device->StopRequested = FALSE;
    Device->Thread = CreateThread(NULL, 0,
                                  Device->Flow == AE_FLOW_RENDER ? AeRenderThread : AeCaptureThread,
                                  Device, 0, NULL);
    if (Device->Thread)
        SetThreadPriority(Device->Thread, THREAD_PRIORITY_TIME_CRITICAL);
}

VOID
AeEndpointStreamRemoved(
    _In_ ULONG Endpoint)
{
    PAE_ENDPOINT Device = &AeEndpoints[Endpoint];

    if (Device->StreamCount && !--Device->StreamCount)
        Device->IdleSince = GetTickCount();
}

VOID
AeStopAllDevices(VOID)
{
    ULONG Index;

    EnterCriticalSection(&AeLock);
    for (Index = 0; Index < AeEndpointCount; Index++)
        AeEndpoints[Index].StopRequested = TRUE;
    LeaveCriticalSection(&AeLock);
}
