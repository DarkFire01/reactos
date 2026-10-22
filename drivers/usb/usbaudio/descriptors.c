/*
 * PROJECT:     ReactOS Universal Audio Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Walking the class specific descriptors of an audio function
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "usbaudio.h"

/**
 * @brief
 * The descriptor after Current, or NULL at the end of the configuration or a broken length.
 */
PUSB_COMMON_DESCRIPTOR
UsbAudioNextDescriptor(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ PVOID Current)
{
    PUCHAR End = (PUCHAR)ConfigurationDescriptor + ConfigurationDescriptor->wTotalLength;
    PUSB_COMMON_DESCRIPTOR Descriptor = (PUSB_COMMON_DESCRIPTOR)Current;

    if (!Descriptor->bLength)
        return NULL;

    Descriptor = (PUSB_COMMON_DESCRIPTOR)((PUCHAR)Descriptor + Descriptor->bLength);
    if ((PUCHAR)Descriptor + sizeof(USB_COMMON_DESCRIPTOR) > End ||
        !Descriptor->bLength || (PUCHAR)Descriptor + Descriptor->bLength > End)
    {
        return NULL;
    }
    return Descriptor;
}

/* A 2.0 control interface says so in the bcdADC of its header */
BOOLEAN
UsbAudioIsVersion2Header(
    _In_ PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header)
{
    return Header->bcdADC >= 0x0200;
}

/* The bytes the header spans; 2.0 moved wTotalLength one byte on to make room for bCategory */
ULONG
UsbAudioGetHeaderLength(
    _In_ PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header)
{
    PUCHAR Bytes = (PUCHAR)Header;

    if (UsbAudioIsVersion2Header(Header))
        return Bytes[6] | (Bytes[7] << 8);
    return Header->wTotalLength;
}

/**
 * @brief
 * The class specific header of the audio control interface, which spans all its units.
 */
PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR
UsbAudioGetControlHeader(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _Out_ PUCHAR InterfaceNumber)
{
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header;
    PUSB_INTERFACE_DESCRIPTOR Interface = NULL;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    PUCHAR End = (PUCHAR)ConfigurationDescriptor + ConfigurationDescriptor->wTotalLength;

    *InterfaceNumber = 0;
    for (Descriptor = (PUSB_COMMON_DESCRIPTOR)ConfigurationDescriptor;
         Descriptor;
         Descriptor = UsbAudioNextDescriptor(ConfigurationDescriptor, Descriptor))
    {
        if (Descriptor->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
        {
            Interface = (Descriptor->bLength >= sizeof(USB_INTERFACE_DESCRIPTOR)) ? (PUSB_INTERFACE_DESCRIPTOR)Descriptor : NULL;
            continue;
        }

        if (!Interface || Interface->bInterfaceClass != USB_DEVICE_CLASS_AUDIO ||
            Interface->bInterfaceSubClass != USB_AUDIO_SUBCLASS_CONTROL ||
            Descriptor->bDescriptorType != USB_AUDIO_CS_INTERFACE_DESCRIPTOR_TYPE ||
            Descriptor->bLength < sizeof(USB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR) - 1)
        {
            continue;
        }

        Header = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)Descriptor;
        if (Header->bDescriptorSubtype != USB_AUDIO_HEADER || (PUCHAR)Header + UsbAudioGetHeaderLength(Header) > End)
            continue;

        *InterfaceNumber = Interface->bInterfaceNumber;
        return Header;
    }
    return NULL;
}

/**
 * @brief
 * Whether a class specific descriptor of the control interface is a terminal or unit long
 * enough for every field the driver reads from it.
 */
BOOLEAN
UsbAudioIsValidUnit(
    _In_ PUSB_COMMON_DESCRIPTOR Descriptor,
    _In_ BOOLEAN Version2)
{
    PUCHAR Bytes = (PUCHAR)Descriptor;
    ULONG Length = Descriptor->bLength;

    if (Descriptor->bDescriptorType != USB_AUDIO_CS_INTERFACE_DESCRIPTOR_TYPE || Length < 4)
        return FALSE;

    /* 2.0 terminals carry a clock and wider controls, and feature units four bytes a channel */
    if (Version2)
    {
        switch (Bytes[2])
        {
            case USB_AUDIO_INPUT_TERMINAL:
                return Length >= 17;
            case USB_AUDIO_OUTPUT_TERMINAL:
                return Length >= 12;
            case USB_AUDIO_FEATURE_UNIT:
                return Length >= 10;
            case USB_AUDIO_MIXER_UNIT:
            case USB_AUDIO_SELECTOR_UNIT:
            case USB_AUDIO2_CLOCK_SELECTOR:
                return Length >= 5 && Length >= 5U + Bytes[4];
            case USB_AUDIO2_PROCESSING_UNIT:
            case USB_AUDIO2_EXTENSION_UNIT:
                return Length >= 7 && Length >= 7U + Bytes[6];
            case USB_AUDIO2_EFFECT_UNIT:
            case USB_AUDIO2_SAMPLE_RATE_CONVERTER:
                return Length >= 7;
            case USB_AUDIO2_CLOCK_SOURCE:
                return Length >= 8;
            case USB_AUDIO2_CLOCK_MULTIPLIER:
                return Length >= 6;
            default:
                return FALSE;
        }
    }

    switch (Bytes[2])
    {
        case USB_AUDIO_INPUT_TERMINAL:
            return Length >= sizeof(USB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR);
        case USB_AUDIO_OUTPUT_TERMINAL:
            return Length >= sizeof(USB_AUDIO_CONTROL_OUTPUT_TERMINAL_DESCRIPTOR);
        case USB_AUDIO_FEATURE_UNIT:
            return Length >= FIELD_OFFSET(USB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR, bmaControls) + 1;
        case USB_AUDIO_MIXER_UNIT:
        case USB_AUDIO_SELECTOR_UNIT:
            return Length >= FIELD_OFFSET(USB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR, baSourceID) + Bytes[4];
        case USB_AUDIO_PROCESSING_UNIT:
        case USB_AUDIO_EXTENSION_UNIT:
            return Length >= FIELD_OFFSET(USB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR, baSourceID) &&
                   Length >= FIELD_OFFSET(USB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR, baSourceID) + Bytes[6];
        default:
            return FALSE;
    }
}

/**
 * @brief
 * The valid units and terminals of the control interface, one after another.
 */
PUSB_COMMON_DESCRIPTOR
UsbAudioNextUnit(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header,
    _In_ PVOID Current)
{
    PUSB_COMMON_DESCRIPTOR Descriptor = (PUSB_COMMON_DESCRIPTOR)Current;

    for (;;)
    {
        Descriptor = UsbAudioNextDescriptor(ConfigurationDescriptor, Descriptor);
        if (!Descriptor || (PUCHAR)Descriptor + Descriptor->bLength > (PUCHAR)Header + UsbAudioGetHeaderLength(Header))
            return NULL;
        if (UsbAudioIsValidUnit(Descriptor, UsbAudioIsVersion2Header(Header)))
            return Descriptor;
    }
}

static
BOOLEAN
UsbAudioIsTerminal(
    _In_ PUSB_COMMON_DESCRIPTOR Descriptor)
{
    UCHAR Subtype = ((PUCHAR)Descriptor)[2];

    return Subtype == USB_AUDIO_INPUT_TERMINAL || Subtype == USB_AUDIO_OUTPUT_TERMINAL;
}

/**
 * @brief
 * The Index-th terminal that connects to a streaming interface, or the Index-th that does not.
 */
PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR
UsbAudioGetTerminalByIndex(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ BOOLEAN Streaming,
    _In_ ULONG Index)
{
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR Terminal;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    UCHAR InterfaceNumber;

    Header = UsbAudioGetControlHeader(ConfigurationDescriptor, &InterfaceNumber);
    if (!Header)
        return NULL;

    for (Descriptor = UsbAudioNextUnit(ConfigurationDescriptor, Header, Header);
         Descriptor;
         Descriptor = UsbAudioNextUnit(ConfigurationDescriptor, Header, Descriptor))
    {
        if (!UsbAudioIsTerminal(Descriptor))
            continue;

        Terminal = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)Descriptor;
        if ((Terminal->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE) != Streaming)
            continue;
        if (!Index--)
            return Terminal;
    }
    return NULL;
}

VOID
NTAPI
CountTerminalUnits(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    OUT PULONG NonStreamingTerminalDescriptorCount,
    OUT PULONG TotalTerminalDescriptorCount)
{
    ULONG Streaming = 0, NonStreaming = 0;

    while (UsbAudioGetTerminalByIndex(ConfigurationDescriptor, TRUE, Streaming))
        Streaming++;
    while (UsbAudioGetTerminalByIndex(ConfigurationDescriptor, FALSE, NonStreaming))
        NonStreaming++;

    *NonStreamingTerminalDescriptorCount = NonStreaming;
    *TotalTerminalDescriptorCount = Streaming + NonStreaming;
}

static
ULONG
UsbAudioReadFrequency(
    _In_reads_bytes_(3) const UCHAR *Bytes)
{
    return Bytes[0] | (Bytes[1] << 8) | (Bytes[2] << 16);
}

/* The data range a format is offered with, from what its descriptors gave */
static
VOID
UsbAudioFillDataRange(
    _Inout_ PUSBAUDIO_FORMAT Format)
{
    ULONG Index;

    Format->DataRange.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    Format->DataRange.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    Format->DataRange.DataRange.SubFormat = (Format->FormatTag == USB_AUDIO_FORMAT_IEEE_FLOAT) ?
                                            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    Format->DataRange.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    Format->DataRange.MaximumChannels = Format->Channels;
    Format->DataRange.MinimumBitsPerSample = Format->SubframeSize * 8;
    Format->DataRange.MaximumBitsPerSample = Format->SubframeSize * 8;
    Format->DataRange.MinimumSampleFrequency = MAXULONG;
    Format->DataRange.MaximumSampleFrequency = 0;

    for (Index = 0; Index < Format->FrequencyCount; Index++)
    {
        Format->DataRange.MinimumSampleFrequency = min(Format->DataRange.MinimumSampleFrequency, Format->Frequencies[Index]);
        Format->DataRange.MaximumSampleFrequency = max(Format->DataRange.MaximumSampleFrequency, Format->Frequencies[Index]);
    }
}

/* Fills one format from an alternate setting's class descriptors, if it is one the driver streams */
static
BOOLEAN
UsbAudioDescribeFormat(
    _In_ PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR General,
    _In_ PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR Type,
    _Out_ PUSBAUDIO_FORMAT Format)
{
    ULONG Index, Count;

    if (General->wFormatTag != USB_AUDIO_FORMAT_PCM && General->wFormatTag != USB_AUDIO_FORMAT_PCM8 &&
        General->wFormatTag != USB_AUDIO_FORMAT_IEEE_FLOAT)
    {
        return FALSE;
    }
    if (Type->bFormatType != USB_AUDIO_FORMAT_TYPE_I || !Type->bNrChannels || !Type->bSubframeSize ||
        Type->bSubframeSize > 4 || Type->bBitResolution > Type->bSubframeSize * 8)
    {
        return FALSE;
    }

    /* Continuous ranges give their bounds; discrete ones list every frequency */
    Count = Type->bSamFreqType ? Type->bSamFreqType : 2;
    if (Type->bLength < FIELD_OFFSET(USB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR, tSamFreq) + Count * 3)
        return FALSE;

    Format->FormatTag = General->wFormatTag;
    Format->TerminalLink = General->bTerminalLink;
    Format->Channels = Type->bNrChannels;
    Format->SubframeSize = Type->bSubframeSize;
    Format->BitResolution = Type->bBitResolution;
    Format->Continuous = !Type->bSamFreqType;
    Format->FrequencyCount = min(Count, (ULONG)USBAUDIO_MAX_FREQUENCIES);

    for (Index = 0; Index < Format->FrequencyCount; Index++)
        Format->Frequencies[Index] = UsbAudioReadFrequency(&Type->tSamFreq[Index * 3]);
    return TRUE;
}

/*
 * Walks every streaming alternate setting, counting the usable ones or filling them in. A 2.0
 * function asks its clocks for their rates, so it is talked to on each walk.
 */
static
ULONG
UsbAudioWalkFormats(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _Out_writes_opt_(Capacity) PUSBAUDIO_FORMAT Formats,
    _In_ ULONG Capacity)
{
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor = DeviceExtension->ConfigurationDescriptor;
    BOOLEAN Version2 = DeviceExtension->Uac2;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    PUSB_INTERFACE_DESCRIPTOR Interface = NULL;
    PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR General = NULL;
    PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR Type = NULL;
    PUSB_ENDPOINT_DESCRIPTOR Endpoint = NULL;
    PUSB_ENDPOINT_DESCRIPTOR Feedback = NULL;
    PUCHAR ClassEndpoint = NULL;
    USBAUDIO_FORMAT Format;
    BOOLEAN Usable;
    ULONG Count = 0;

    for (Descriptor = (PUSB_COMMON_DESCRIPTOR)ConfigurationDescriptor; ; Descriptor = UsbAudioNextDescriptor(ConfigurationDescriptor, Descriptor))
    {
        /* An alternate setting ends at the next interface or at the end of the configuration */
        if (!Descriptor || Descriptor->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
        {
            if (Interface && General && Type && Endpoint)
            {
                RtlZeroMemory(&Format, sizeof(Format));
                if (Version2)
                    Usable = UsbAudio2DescribeFormat(DeviceExtension, (PUCHAR)General, (PUCHAR)Type, &Format);
                else
                    Usable = UsbAudioDescribeFormat(General, Type, &Format);
                if (Usable)
                    UsbAudioFillDataRange(&Format);
                if (Usable && Format.DataRange.MaximumSampleFrequency)
                {
                    Format.InterfaceNumber = Interface->bInterfaceNumber;
                    Format.AlternateSetting = Interface->bAlternateSetting;
                    Format.EndpointAddress = Endpoint->bEndpointAddress;
                    Format.EndpointInterval = Endpoint->bInterval;

                    /* A high bandwidth endpoint moves up to three transactions a microframe */
                    Format.MaximumPacketSize = (Endpoint->wMaxPacketSize & 0x7FF) * (1 + ((Endpoint->wMaxPacketSize >> 11) & 3));
                    Format.SyncType = Endpoint->bmAttributes & USB_ENDPOINT_SYNC_MASK;

                    /* The class endpoint descriptor of 2.0 has bmControls ahead of the lock delay */
                    if (ClassEndpoint && Version2)
                    {
                        Format.LockDelayUnits = ClassEndpoint[5];
                        Format.LockDelay = ClassEndpoint[6] | (ClassEndpoint[7] << 8);
                    }
                    else if (ClassEndpoint)
                    {
                        Format.EndpointAttributes = ClassEndpoint[3];
                        Format.LockDelayUnits = ClassEndpoint[4];
                        Format.LockDelay = ClassEndpoint[5] | (ClassEndpoint[6] << 8);
                    }

                    /*
                     * An asynchronous output endpoint reports its rate on a feedback endpoint. 1.0
                     * names it in bSynchAddress; 2.0 endpoints have no such field, and the
                     * feedback endpoint of the setting is the one.
                     */
                    if (Feedback && Format.SyncType == USB_ENDPOINT_SYNC_ASYNCHRONOUS &&
                        !(Endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) &&
                        (Version2 ||
                         (Endpoint->bLength >= USB_AUDIO_ENDPOINT_LENGTH &&
                          ((PUCHAR)Endpoint)[USB_AUDIO_ENDPOINT_SYNCH_ADDRESS] == Feedback->bEndpointAddress)))
                    {
                        Format.FeedbackAddress = Feedback->bEndpointAddress;
                        Format.FeedbackInterval = Feedback->bInterval;
                        Format.FeedbackRefresh = (!Version2 && Feedback->bLength >= USB_AUDIO_ENDPOINT_LENGTH) ?
                                                 ((PUCHAR)Feedback)[USB_AUDIO_ENDPOINT_REFRESH] : 0;
                    }
                    if (Formats && Count < Capacity)
                        Formats[Count] = Format;
                    Count++;
                }
            }

            if (!Descriptor)
                break;

            Interface = (PUSB_INTERFACE_DESCRIPTOR)Descriptor;
            if (Descriptor->bLength < sizeof(USB_INTERFACE_DESCRIPTOR) ||
                Interface->bInterfaceClass != USB_DEVICE_CLASS_AUDIO ||
                Interface->bInterfaceSubClass != USB_AUDIO_SUBCLASS_STREAMING || !Interface->bNumEndpoints)
            {
                Interface = NULL;
            }
            General = NULL;
            Type = NULL;
            Endpoint = NULL;
            Feedback = NULL;
            ClassEndpoint = NULL;
            continue;
        }

        if (!Interface)
            continue;

        /* 2.0 moved the channels into AS_GENERAL, leaving Type I with only the sample size */
        if (Descriptor->bDescriptorType == USB_AUDIO_CS_INTERFACE_DESCRIPTOR_TYPE && Descriptor->bLength >= 3)
        {
            if (((PUCHAR)Descriptor)[2] == USB_AUDIO_AS_GENERAL &&
                Descriptor->bLength >= (Version2 ? 16 : sizeof(USB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR)))
            {
                General = (PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR)Descriptor;
            }
            else if (((PUCHAR)Descriptor)[2] == USB_AUDIO_FORMAT_TYPE && Descriptor->bLength >= (Version2 ? 6 : 8))
            {
                Type = (PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR)Descriptor;
            }
        }
        else if (Descriptor->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE && Descriptor->bLength >= sizeof(USB_ENDPOINT_DESCRIPTOR))
        {
            /* The data endpoint is the isochronous one that is not a synch endpoint */
            if ((((PUSB_ENDPOINT_DESCRIPTOR)Descriptor)->bmAttributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_TYPE_ISOCHRONOUS)
                continue;
            if ((((PUSB_ENDPOINT_DESCRIPTOR)Descriptor)->bmAttributes & 0x30) == 0x10 ||
                (Endpoint && (((PUSB_ENDPOINT_DESCRIPTOR)Descriptor)->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK)))
            {
                if (!Feedback)
                    Feedback = (PUSB_ENDPOINT_DESCRIPTOR)Descriptor;
            }
            else if (!Endpoint)
            {
                Endpoint = (PUSB_ENDPOINT_DESCRIPTOR)Descriptor;
            }
        }
        else if (Descriptor->bDescriptorType == USB_AUDIO_CS_ENDPOINT_DESCRIPTOR_TYPE &&
                 Descriptor->bLength >= (Version2 ? 8 : sizeof(USB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR)) &&
                 Endpoint && !ClassEndpoint)
        {
            ClassEndpoint = (PUCHAR)Descriptor;
        }
    }
    return Count;
}

/**
 * @brief
 * Collects the PCM formats of all streaming interfaces.
 */
NTSTATUS
UsbAudioParseFormats(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    ULONG Count;

    Count = UsbAudioWalkFormats(DeviceExtension, NULL, 0);
    DeviceExtension->FormatCount = 0;
    if (!Count)
        return STATUS_SUCCESS;

    DeviceExtension->Formats = AllocFunction(Count * sizeof(USBAUDIO_FORMAT));
    if (!DeviceExtension->Formats)
        return STATUS_INSUFFICIENT_RESOURCES;

    DeviceExtension->FormatCount = UsbAudioWalkFormats(DeviceExtension, DeviceExtension->Formats, Count);
    return STATUS_SUCCESS;
}
