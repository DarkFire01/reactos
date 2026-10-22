/*
 * PROJECT:     ReactOS Universal Audio Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Audio Device Class 2.0 clocks, control ranges and streaming formats
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "usbaudio.h"

/* Selectors and multipliers a terminal's clock may pass through before its source */
#define USBAUDIO2_MAX_CLOCK_DEPTH       8

/* Subranges a frequency RANGE answer is read for */
#define USBAUDIO2_MAX_SUBRANGES         16

/* The rates a continuous clock range is offered at */
static const ULONG UsbAudio2CommonRates[] =
{
    8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 352800, 384000
};

static
ULONG
UsbAudio2ReadDword(
    _In_reads_bytes_(4) const UCHAR *Bytes)
{
    return Bytes[0] | (Bytes[1] << 8) | (Bytes[2] << 16) | ((ULONG)Bytes[3] << 24);
}

/**
 * @brief
 * The 1.0 subtype a 2.0 entity is handled as. Clock entities are not on the audio path, and
 * effect units and rate converters pass their signal on unchanged.
 */
UCHAR
UsbAudio2MapSubtype(
    _In_ UCHAR Subtype)
{
    switch (Subtype)
    {
        case USB_AUDIO2_PROCESSING_UNIT:
            return USB_AUDIO_PROCESSING_UNIT;
        case USB_AUDIO2_EXTENSION_UNIT:
            return USB_AUDIO_EXTENSION_UNIT;
        case USB_AUDIO2_CLOCK_SOURCE:
        case USB_AUDIO2_CLOCK_SELECTOR:
        case USB_AUDIO2_CLOCK_MULTIPLIER:
            return USBAUDIO_UNIT_CLOCK;
        case USB_AUDIO2_EFFECT_UNIT:
        case USB_AUDIO2_SAMPLE_RATE_CONVERTER:
            return USBAUDIO_UNIT_PASS_THROUGH;
        default:
            return Subtype;
    }
}

/* A terminal, unit or clock of the control interface by its id; every one has it in byte 3 */
static
PUCHAR
UsbAudio2FindEntity(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR Id)
{
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    UCHAR InterfaceNumber;

    Header = UsbAudioGetControlHeader(DeviceExtension->ConfigurationDescriptor, &InterfaceNumber);
    if (!Header)
        return NULL;

    for (Descriptor = UsbAudioNextUnit(DeviceExtension->ConfigurationDescriptor, Header, Header);
         Descriptor;
         Descriptor = UsbAudioNextUnit(DeviceExtension->ConfigurationDescriptor, Header, Descriptor))
    {
        if (((PUCHAR)Descriptor)[3] == Id)
            return (PUCHAR)Descriptor;
    }
    return NULL;
}

/* Follows a clock through selectors and multipliers to the source that drives it */
static
PUCHAR
UsbAudio2FindClockSource(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR ClockId)
{
    PUCHAR Entity;
    UCHAR Selected;
    ULONG Depth;

    for (Depth = 0; Depth < USBAUDIO2_MAX_CLOCK_DEPTH; Depth++)
    {
        Entity = UsbAudio2FindEntity(DeviceExtension, ClockId);
        if (!Entity)
            return NULL;

        switch (Entity[2])
        {
            case USB_AUDIO2_CLOCK_SOURCE:
                return Entity;

            case USB_AUDIO2_CLOCK_SELECTOR:
                /* bNrInPins and the inputs follow the id; the device says which one is picked */
                if (!Entity[4])
                    return NULL;
                Selected = 1;
                UsbAudioClassRequest(DeviceExtension,
                                     URB_FUNCTION_CLASS_INTERFACE,
                                     USB_AUDIO2_CUR,
                                     USB_AUDIO2_CLOCK_SELECTOR_CONTROL << 8,
                                     (USHORT)((ClockId << 8) | DeviceExtension->ControlInterface),
                                     &Selected,
                                     sizeof(Selected),
                                     TRUE);
                if (!Selected || Selected > Entity[4])
                    Selected = 1;
                ClockId = Entity[4 + Selected];
                break;

            case USB_AUDIO2_CLOCK_MULTIPLIER:
                /* the rate it gives is a ratio of its source's, which streams follow the same way */
                ClockId = Entity[4];
                break;

            default:
                return NULL;
        }
    }
    return NULL;
}

static
VOID
UsbAudio2AddFrequency(
    _Inout_ PUSBAUDIO_FORMAT Format,
    _In_ ULONG Frequency)
{
    ULONG Index;

    if (Frequency < 1000 || Format->FrequencyCount == USBAUDIO_MAX_FREQUENCIES)
        return;
    for (Index = 0; Index < Format->FrequencyCount; Index++)
    {
        if (Format->Frequencies[Index] == Frequency)
            return;
    }
    Format->Frequencies[Format->FrequencyCount++] = Frequency;
}

/*
 * The rates of a clock source, from the RANGE of its frequency control. Single rates are taken
 * as they are, and continuous ranges offer the common rates that fall in them.
 */
static
VOID
UsbAudio2ReadFrequencies(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR ClockId,
    _Inout_ PUSBAUDIO_FORMAT Format)
{
    UCHAR Buffer[2 + USBAUDIO2_MAX_SUBRANGES * 12];
    USHORT Value = USB_AUDIO2_SAM_FREQ_CONTROL << 8;
    USHORT Target = (USHORT)((ClockId << 8) | DeviceExtension->ControlInterface);
    ULONG Count = 0, Index, Rate, Minimum, Maximum, Resolution;
    NTSTATUS Status;

    /* The count comes first, so the full answer is asked for with the length it has */
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Status = UsbAudioClassRequest(DeviceExtension, URB_FUNCTION_CLASS_INTERFACE, USB_AUDIO2_RANGE,
                                  Value, Target, Buffer, 2, TRUE);
    if (NT_SUCCESS(Status))
        Count = min((ULONG)(Buffer[0] | (Buffer[1] << 8)), (ULONG)USBAUDIO2_MAX_SUBRANGES);
    if (Count)
    {
        Status = UsbAudioClassRequest(DeviceExtension, URB_FUNCTION_CLASS_INTERFACE, USB_AUDIO2_RANGE,
                                      Value, Target, Buffer, 2 + Count * 12, TRUE);
    }

    /* A clock that gives no range runs at the rate it reports */
    if (!NT_SUCCESS(Status) || !Count)
    {
        Status = UsbAudioClassRequest(DeviceExtension, URB_FUNCTION_CLASS_INTERFACE, USB_AUDIO2_CUR,
                                      Value, Target, Buffer, 4, TRUE);
        if (NT_SUCCESS(Status))
            UsbAudio2AddFrequency(Format, UsbAudio2ReadDword(Buffer));
        return;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Minimum = UsbAudio2ReadDword(&Buffer[2 + Index * 12]);
        Maximum = UsbAudio2ReadDword(&Buffer[6 + Index * 12]);
        Resolution = UsbAudio2ReadDword(&Buffer[10 + Index * 12]);
        if (Minimum >= Maximum)
        {
            UsbAudio2AddFrequency(Format, Minimum);
            continue;
        }

        for (Rate = 0; Rate < ARRAYSIZE(UsbAudio2CommonRates); Rate++)
        {
            if (UsbAudio2CommonRates[Rate] >= Minimum && UsbAudio2CommonRates[Rate] <= Maximum &&
                (!Resolution || !((UsbAudio2CommonRates[Rate] - Minimum) % Resolution)))
            {
                UsbAudio2AddFrequency(Format, UsbAudio2CommonRates[Rate]);
            }
        }
    }
}

/**
 * @brief
 * Fills a format from a 2.0 streaming interface: AS_GENERAL gives the channels and the
 * formats, Type I the sample size, and the clock of the terminal it streams to the rates.
 */
BOOLEAN
UsbAudio2DescribeFormat(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PUCHAR General,
    _In_ PUCHAR Type,
    _Out_ PUSBAUDIO_FORMAT Format)
{
    ULONG Formats = UsbAudio2ReadDword(&General[6]);
    UCHAR Channels = General[10], Subslot = Type[4], Bits = Type[5];
    PUCHAR Terminal, Clock;

    if (General[5] != USB_AUDIO_FORMAT_TYPE_I || Type[3] != USB_AUDIO_FORMAT_TYPE_I)
        return FALSE;
    if (!Channels || !Subslot || Subslot > 4 || Bits > Subslot * 8)
        return FALSE;

    if (Formats & USB_AUDIO2_FORMAT_PCM)
        Format->FormatTag = USB_AUDIO_FORMAT_PCM;
    else if (Formats & USB_AUDIO2_FORMAT_IEEE_FLOAT)
        Format->FormatTag = USB_AUDIO_FORMAT_IEEE_FLOAT;
    else if (Formats & USB_AUDIO2_FORMAT_PCM8)
        Format->FormatTag = USB_AUDIO_FORMAT_PCM8;
    else
        return FALSE;

    /* The terminal names its clock, an input terminal in byte 7 and an output terminal in byte 8 */
    Terminal = UsbAudio2FindEntity(DeviceExtension, General[3]);
    if (!Terminal || (Terminal[2] != USB_AUDIO_INPUT_TERMINAL && Terminal[2] != USB_AUDIO_OUTPUT_TERMINAL))
        return FALSE;
    Clock = UsbAudio2FindClockSource(DeviceExtension, Terminal[(Terminal[2] == USB_AUDIO_INPUT_TERMINAL) ? 7 : 8]);
    if (!Clock)
        return FALSE;

    Format->TerminalLink = General[3];
    Format->Channels = Channels;
    Format->SubframeSize = Subslot;
    Format->BitResolution = Bits;
    Format->ClockId = Clock[3];

    /* Bits 1..0 of bmControls are 3 when the host may set the frequency */
    Format->ClockProgrammable = (Clock[5] & 3) == 3;
    UsbAudio2ReadFrequencies(DeviceExtension, Clock[3], Format);
    return Format->FrequencyCount != 0;
}

/**
 * @brief
 * MIN, MAX or RES of a 2.0 control, named by the 1.0 request for it. They come from one
 * RANGE answer, of which level controls have a single subrange.
 */
NTSTATUS
UsbAudio2GetRangeValue(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ USHORT Value,
    _In_ USHORT Index,
    _In_ ULONG Bytes,
    _In_ UCHAR Request,
    _Out_ PLONG Result)
{
    UCHAR Buffer[2 + 3 * 2];
    ULONG Offset;
    NTSTATUS Status;

    if ((Bytes != 1 && Bytes != 2) || Request < USB_AUDIO_GET_MIN || Request > USB_AUDIO_GET_RES)
        return STATUS_INVALID_PARAMETER;

    Status = UsbAudioClassRequest(DeviceExtension, URB_FUNCTION_CLASS_INTERFACE, USB_AUDIO2_RANGE,
                                  Value, Index, Buffer, 2 + 3 * Bytes, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    /* MIN, MAX and RES follow the subrange count in that order */
    Offset = 2 + (Request - USB_AUDIO_GET_MIN) * Bytes;
    *Result = (Bytes == 2) ? (LONG)(SHORT)(Buffer[Offset] | (Buffer[Offset + 1] << 8)) : (LONG)(CHAR)Buffer[Offset];
    return STATUS_SUCCESS;
}

/* Sets the clock behind a format to a rate, where the host may set it */
NTSTATUS
UsbAudio2SetClockFrequency(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PUSBAUDIO_FORMAT Format,
    _In_ ULONG Frequency)
{
    UCHAR Rate[4];

    if (!Format->ClockId || !Format->ClockProgrammable)
        return STATUS_SUCCESS;

    Rate[0] = (UCHAR)Frequency;
    Rate[1] = (UCHAR)(Frequency >> 8);
    Rate[2] = (UCHAR)(Frequency >> 16);
    Rate[3] = (UCHAR)(Frequency >> 24);
    return UsbAudioClassRequest(DeviceExtension,
                                URB_FUNCTION_CLASS_INTERFACE,
                                USB_AUDIO2_CUR,
                                USB_AUDIO2_SAM_FREQ_CONTROL << 8,
                                (USHORT)((Format->ClockId << 8) | DeviceExtension->ControlInterface),
                                Rate,
                                sizeof(Rate),
                                FALSE);
}
