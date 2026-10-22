/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Sample formats, channel mapping and rate conversion
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "engine.h"

#include <math.h>

#define AE_SPEAKER_FL   SPEAKER_FRONT_LEFT
#define AE_SPEAKER_FR   SPEAKER_FRONT_RIGHT
#define AE_SPEAKER_FC   SPEAKER_FRONT_CENTER
#define AE_SPEAKER_LFE  SPEAKER_LOW_FREQUENCY
#define AE_SPEAKER_BL   SPEAKER_BACK_LEFT
#define AE_SPEAKER_BR   SPEAKER_BACK_RIGHT
#define AE_SPEAKER_FLC  SPEAKER_FRONT_LEFT_OF_CENTER
#define AE_SPEAKER_FRC  SPEAKER_FRONT_RIGHT_OF_CENTER
#define AE_SPEAKER_BC   SPEAKER_BACK_CENTER
#define AE_SPEAKER_SL   SPEAKER_SIDE_LEFT
#define AE_SPEAKER_SR   SPEAKER_SIDE_RIGHT

/* Half power, for a channel spread over two speakers */
#define AE_HALF_POWER   0.70710678f

ULONG
AeDefaultChannelMask(
    _In_ ULONG Channels)
{
    switch (Channels)
    {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 3: return KSAUDIO_SPEAKER_STEREO | AE_SPEAKER_FC;
        case 4: return KSAUDIO_SPEAKER_QUAD;
        case 5: return KSAUDIO_SPEAKER_QUAD | AE_SPEAKER_FC;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 7: return KSAUDIO_SPEAKER_5POINT1 | AE_SPEAKER_BC;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return 0;
    }
}

BOOL
AeDecodeFormat(
    _In_ const WAVEFORMATEX *Format,
    _Out_ PAE_FORMAT Decoded)
{
    const WAVEFORMATEXTENSIBLE *Extensible = (const WAVEFORMATEXTENSIBLE *)Format;

    ZeroMemory(Decoded, sizeof(*Decoded));
    if (!Format->nChannels || Format->nChannels > AE_MAX_CHANNELS ||
        Format->nSamplesPerSec < AE_MIN_RATE || Format->nSamplesPerSec > AE_MAX_RATE ||
        !Format->wBitsPerSample || (Format->wBitsPerSample % 8))
    {
        return FALSE;
    }

    Decoded->Channels = Format->nChannels;
    Decoded->SamplesPerSec = Format->nSamplesPerSec;
    Decoded->ContainerBits = Format->wBitsPerSample;
    Decoded->ValidBits = Format->wBitsPerSample;
    Decoded->ChannelMask = AeDefaultChannelMask(Format->nChannels);

    switch (Format->wFormatTag)
    {
        case WAVE_FORMAT_PCM:
            Decoded->SampleType = AeSamplePcm;
            break;

        case WAVE_FORMAT_IEEE_FLOAT:
            Decoded->SampleType = AeSampleFloat;
            break;

        case WAVE_FORMAT_EXTENSIBLE:
            if (Format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
                return FALSE;
            if (IsEqualGUID(Extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
                Decoded->SampleType = AeSamplePcm;
            else if (IsEqualGUID(Extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                Decoded->SampleType = AeSampleFloat;
            else
                return FALSE;

            if (Extensible->Samples.wValidBitsPerSample &&
                Extensible->Samples.wValidBitsPerSample <= Format->wBitsPerSample)
            {
                Decoded->ValidBits = Extensible->Samples.wValidBitsPerSample;
            }
            if (Extensible->dwChannelMask)
                Decoded->ChannelMask = Extensible->dwChannelMask;
            break;

        default:
            return FALSE;
    }

    if (Decoded->SampleType == AeSamplePcm && Decoded->ContainerBits > 32)
        return FALSE;
    if (Decoded->SampleType == AeSampleFloat && Decoded->ContainerBits != 32 && Decoded->ContainerBits != 64)
        return FALSE;

    /* The block has to be exactly one sample per channel */
    Decoded->BlockAlign = Decoded->Channels * Decoded->ContainerBits / 8;
    return Format->nBlockAlign == Decoded->BlockAlign;
}

VOID
AeMakeFloatFormat(
    _In_ ULONG SamplesPerSec,
    _In_ ULONG Channels,
    _In_ ULONG ChannelMask,
    _Out_ WAVEFORMATEXTENSIBLE *Format)
{
    ZeroMemory(Format, sizeof(*Format));
    Format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    Format->Format.nChannels = (WORD)Channels;
    Format->Format.nSamplesPerSec = SamplesPerSec;
    Format->Format.wBitsPerSample = 32;
    Format->Format.nBlockAlign = (WORD)(Channels * 4);
    Format->Format.nAvgBytesPerSec = SamplesPerSec * Channels * 4;
    Format->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    Format->Samples.wValidBitsPerSample = 32;
    Format->dwChannelMask = ChannelMask;
    Format->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

static
LONG
AeReadInteger(
    _In_ const BYTE *Sample,
    _In_ ULONG Bytes)
{
    switch (Bytes)
    {
        case 1: return ((LONG)Sample[0] - 128) << 24;
        case 2: return (LONG)(*(const SHORT *)Sample) << 16;
        case 3: return (LONG)(((ULONG)Sample[0] << 8) | ((ULONG)Sample[1] << 16) | ((ULONG)Sample[2] << 24));
        default: return *(const LONG *)Sample;
    }
}

VOID
AeToFloat(
    _In_ const AE_FORMAT *Format,
    _In_reads_bytes_(Frames * Format->BlockAlign) const BYTE *Source,
    _Out_writes_(Frames * Format->Channels) float *Destination,
    _In_ ULONG Frames)
{
    ULONG Samples = Frames * Format->Channels;
    ULONG Bytes = Format->ContainerBits / 8;
    ULONG Index;

    if (Format->SampleType == AeSampleFloat)
    {
        if (Bytes == 4)
        {
            CopyMemory(Destination, Source, Samples * sizeof(float));
            return;
        }
        for (Index = 0; Index < Samples; Index++)
            Destination[Index] = (float)((const double *)Source)[Index];
        return;
    }

    /* Every integer width is read into the top of a 32 bit sample */
    for (Index = 0; Index < Samples; Index++)
        Destination[Index] = (float)AeReadInteger(Source + Index * Bytes, Bytes) * (1.0f / 2147483648.0f);
}

VOID
AeFromFloat(
    _In_ const AE_FORMAT *Format,
    _In_reads_(Frames * Format->Channels) const float *Source,
    _Out_writes_bytes_(Frames * Format->BlockAlign) BYTE *Destination,
    _In_ ULONG Frames)
{
    ULONG Samples = Frames * Format->Channels;
    ULONG Bytes = Format->ContainerBits / 8;
    ULONG Index;
    double Value;
    LONG Integer;
    BYTE *Sample;

    if (Format->SampleType == AeSampleFloat)
    {
        if (Bytes == 4)
        {
            CopyMemory(Destination, Source, Samples * sizeof(float));
            return;
        }
        for (Index = 0; Index < Samples; Index++)
            ((double *)Destination)[Index] = Source[Index];
        return;
    }

    for (Index = 0; Index < Samples; Index++)
    {
        Value = Source[Index];
        if (Value > 1.0)
            Value = 1.0;
        else if (Value < -1.0)
            Value = -1.0;

        Sample = Destination + Index * Bytes;
        switch (Bytes)
        {
            case 1:
                Sample[0] = (BYTE)(LONG)floor(Value * 127.0 + 128.5);
                break;
            case 2:
                *(SHORT *)Sample = (SHORT)(LONG)floor(Value * 32767.0 + 0.5);
                break;
            case 3:
                Integer = (LONG)floor(Value * 8388607.0 + 0.5);
                Sample[0] = (BYTE)Integer;
                Sample[1] = (BYTE)(Integer >> 8);
                Sample[2] = (BYTE)(Integer >> 16);
                break;
            default:
                *(LONG *)Sample = (LONG)floor(Value * 2147483647.0 + 0.5);
                break;
        }
    }
}

/* The channel of a mask a speaker position lands in, or -1 */
static
LONG
AeChannelOf(
    _In_ ULONG Mask,
    _In_ ULONG Speaker)
{
    LONG Channel = 0;
    ULONG Bit;

    if (!(Mask & Speaker))
        return -1;
    for (Bit = 1; Bit < Speaker; Bit <<= 1)
    {
        if (Mask & Bit)
            Channel++;
    }
    return Channel;
}

VOID
CAeChannelMap::Initialize(
    _In_ ULONG InChannels,
    _In_ ULONG InMask,
    _In_ ULONG OutChannels,
    _In_ ULONG OutMask)
{
    ULONG In, Speaker, Position;
    LONG Target, Left, Right;

    ZeroMemory(m_Matrix, sizeof(m_Matrix));
    m_InChannels = InChannels;
    m_OutChannels = OutChannels;
    m_Identity = (InChannels == OutChannels) && (InMask == OutMask);
    if (m_Identity)
        return;

    if (!InMask)
        InMask = AeDefaultChannelMask(InChannels);
    if (!OutMask)
        OutMask = AeDefaultChannelMask(OutChannels);

    /* One output: an even mix of the fronts */
    if (OutChannels == 1)
    {
        for (In = 0; In < InChannels; In++)
            m_Matrix[0][In] = 1.0f / InChannels;
        return;
    }

    /* A mask may name more speakers than there are channels; those past the last one are dropped */
    Left = AeChannelOf(OutMask, AE_SPEAKER_FL);
    Right = AeChannelOf(OutMask, AE_SPEAKER_FR);
    if (Left >= (LONG)OutChannels)
        Left = -1;
    if (Right >= (LONG)OutChannels)
        Right = -1;

    /* One input: the same signal on both fronts */
    if (InChannels == 1 && Left >= 0 && Right >= 0)
    {
        m_Matrix[Left][0] = 1.0f;
        m_Matrix[Right][0] = 1.0f;
        return;
    }

    In = 0;
    for (Position = 0; Position < 32 && In < InChannels; Position++)
    {
        Speaker = 1UL << Position;
        if (!(InMask & Speaker))
            continue;

        Target = AeChannelOf(OutMask, Speaker);
        if (Target >= (LONG)OutChannels)
            Target = -1;
        if (Target >= 0)
        {
            m_Matrix[Target][In] = 1.0f;
        }
        else if (Left >= 0 && Right >= 0)
        {
            /* A speaker the output lacks folds into the side it is on */
            switch (Speaker)
            {
                case AE_SPEAKER_FC:
                case AE_SPEAKER_BC:
                    m_Matrix[Left][In] = AE_HALF_POWER;
                    m_Matrix[Right][In] = AE_HALF_POWER;
                    break;
                case AE_SPEAKER_BL:
                case AE_SPEAKER_SL:
                    Target = AeChannelOf(OutMask, Speaker == AE_SPEAKER_BL ? AE_SPEAKER_SL : AE_SPEAKER_BL);
                    m_Matrix[Target >= 0 ? Target : Left][In] = Target >= 0 ? 1.0f : AE_HALF_POWER;
                    break;
                case AE_SPEAKER_BR:
                case AE_SPEAKER_SR:
                    Target = AeChannelOf(OutMask, Speaker == AE_SPEAKER_BR ? AE_SPEAKER_SR : AE_SPEAKER_BR);
                    m_Matrix[Target >= 0 ? Target : Right][In] = Target >= 0 ? 1.0f : AE_HALF_POWER;
                    break;
                case AE_SPEAKER_FLC:
                    m_Matrix[Left][In] = 1.0f;
                    break;
                case AE_SPEAKER_FRC:
                    m_Matrix[Right][In] = 1.0f;
                    break;
                default:
                    /* Low frequency and the top speakers are left out */
                    break;
            }
        }
        In++;
    }

    /* Inputs past the mask go to the outputs with the same index */
    for (; In < InChannels && In < OutChannels; In++)
        m_Matrix[In][In] = 1.0f;
}

VOID
CAeChannelMap::Apply(
    _In_reads_(Frames * m_InChannels) const float *Source,
    _Out_writes_(Frames * m_OutChannels) float *Destination,
    _In_ ULONG Frames) const
{
    ULONG Frame, In, Out;
    float Sum;

    if (m_Identity)
    {
        CopyMemory(Destination, Source, Frames * m_InChannels * sizeof(float));
        return;
    }

    for (Frame = 0; Frame < Frames; Frame++)
    {
        for (Out = 0; Out < m_OutChannels; Out++)
        {
            Sum = 0.0f;
            for (In = 0; In < m_InChannels; In++)
                Sum += m_Matrix[Out][In] * Source[In];
            Destination[Out] = Sum;
        }
        Source += m_InChannels;
        Destination += m_OutChannels;
    }
}

CAeResampler::CAeResampler()
    : m_Fifo(NULL), m_Capacity(0), m_Count(0), m_Position(1.0), m_Step(1.0),
      m_Channels(0), m_InRate(0), m_OutRate(0)
{
}

CAeResampler::~CAeResampler()
{
    if (m_Fifo)
        HeapFree(GetProcessHeap(), 0, m_Fifo);
}

BOOL
CAeResampler::Initialize(
    _In_ ULONG Channels,
    _In_ ULONG InRate,
    _In_ ULONG OutRate)
{
    m_Channels = Channels;
    m_InRate = InRate;
    m_OutRate = OutRate;
    m_Step = (double)InRate / (double)OutRate;
    if (InRate == OutRate)
        return TRUE;

    /* Room for a quarter second of input, more than any engine period asks for */
    m_Capacity = max(InRate / 4, 1024);
    m_Fifo = (float *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)m_Capacity * Channels * sizeof(float));
    Reset();
    return m_Fifo != NULL;
}

VOID
CAeResampler::Reset()
{
    if (!m_Fifo)
        return;

    /* One frame of silence before the first input lets the first output interpolate */
    ZeroMemory(m_Fifo, m_Channels * sizeof(float));
    m_Count = 1;
    m_Position = 1.0;
}

ULONG
CAeResampler::Space() const
{
    return m_Fifo ? m_Capacity - m_Count : 0;
}

VOID
CAeResampler::Push(
    _In_reads_(Frames * m_Channels) const float *Source,
    _In_ ULONG Frames)
{
    Frames = min(Frames, Space());
    CopyMemory(m_Fifo + (SIZE_T)m_Count * m_Channels, Source, (SIZE_T)Frames * m_Channels * sizeof(float));
    m_Count += Frames;
}

ULONG
CAeResampler::InputNeeded(
    _In_ ULONG Frames) const
{
    double Last = m_Position + (double)Frames * m_Step;
    ULONG Needed = (ULONG)Last + 3;

    return (Needed > m_Count) ? Needed - m_Count : 0;
}

ULONG
CAeResampler::Pull(
    _Out_writes_(MaxFrames * m_Channels) float *Destination,
    _In_ ULONG MaxFrames)
{
    ULONG Produced = 0;
    ULONG Base, Channel, Drop;
    const float *P0, *P1, *P2, *P3;
    float T;

    while (Produced < MaxFrames)
    {
        Base = (ULONG)m_Position;
        if (Base + 2 >= m_Count)
            break;

        T = (float)(m_Position - Base);
        P0 = m_Fifo + (SIZE_T)(Base - 1) * m_Channels;
        P1 = P0 + m_Channels;
        P2 = P1 + m_Channels;
        P3 = P2 + m_Channels;
        for (Channel = 0; Channel < m_Channels; Channel++)
        {
            /* Catmull-Rom through the two frames around the point and one on each side */
            Destination[Channel] = P1[Channel] + 0.5f * T * (P2[Channel] - P0[Channel] +
                T * (2.0f * P0[Channel] - 5.0f * P1[Channel] + 4.0f * P2[Channel] - P3[Channel] +
                T * (3.0f * (P1[Channel] - P2[Channel]) + P3[Channel] - P0[Channel])));
        }

        Destination += m_Channels;
        m_Position += m_Step;
        Produced++;
    }

    /* Keep one frame before the point for the next interpolation */
    Base = (ULONG)m_Position;
    if (Base > 1)
    {
        Drop = min(Base - 1, m_Count);
        MoveMemory(m_Fifo, m_Fifo + (SIZE_T)Drop * m_Channels, (SIZE_T)(m_Count - Drop) * m_Channels * sizeof(float));
        m_Count -= Drop;
        m_Position -= Drop;
    }
    return Produced;
}
