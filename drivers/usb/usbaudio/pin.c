/*
* PROJECT:     ReactOS Universal Audio Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbaudio/pin.c
* PURPOSE:     USB Audio device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*              Justin Miller (justin.miller@reactos.org)
*/

#include "usbaudio.h"

/* Full speed packets are one millisecond frames, high speed ones a number of 125 microsecond microframes */
#define USBAUDIO_FRAMES_PER_SECOND      1000
#define USBAUDIO_MICROFRAMES_PER_FRAME  8

/* Render keeps this many transfers with the device, capture this many */
#define USBAUDIO_RENDER_TRANSFERS       6
#define USBAUDIO_CAPTURE_TRANSFERS      8

/* Captured audio waits this long, in milliseconds, for the client before the oldest is lost */
#define USBAUDIO_CAPTURE_RING_MS        500

/* How long stopping waits for the device to give its transfers back */
#define USBAUDIO_STOP_TIMEOUT_MS        5000

/* The streaming terminal of a pin, which streams come first in the pin list */
static
UCHAR
UsbAudioGetPinTerminal(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ ULONG PinId)
{
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR Terminal;

    Terminal = UsbAudioGetTerminalByIndex(DeviceExtension->ConfigurationDescriptor, TRUE, PinId);
    return Terminal ? Terminal->bTerminalID : 0;
}

static
BOOLEAN
UsbAudioSupportsRate(
    _In_ PUSBAUDIO_FORMAT Format,
    _In_ ULONG Rate)
{
    ULONG Index;

    if (Format->Continuous)
        return Rate >= Format->DataRange.MinimumSampleFrequency && Rate <= Format->DataRange.MaximumSampleFrequency;

    for (Index = 0; Index < Format->FrequencyCount; Index++)
    {
        if (Format->Frequencies[Index] == Rate)
            return TRUE;
    }
    return FALSE;
}

/* The highest rate a format runs at between Minimum and Maximum, or 0 */
static
ULONG
UsbAudioBestRate(
    _In_ PUSBAUDIO_FORMAT Format,
    _In_ ULONG Minimum,
    _In_ ULONG Maximum)
{
    ULONG Index, Best = 0;

    if (Format->Continuous)
    {
        Best = min(Maximum, Format->DataRange.MaximumSampleFrequency);
        return (Best >= max(Minimum, Format->DataRange.MinimumSampleFrequency)) ? Best : 0;
    }

    for (Index = 0; Index < Format->FrequencyCount; Index++)
    {
        if (Format->Frequencies[Index] >= Minimum && Format->Frequencies[Index] <= Maximum)
            Best = max(Best, Format->Frequencies[Index]);
    }
    return Best;
}

static
BOOLEAN
UsbAudioIsWildcardOr(
    _In_ const GUID *Guid,
    _In_ const GUID *Expected)
{
    return IsEqualGUIDAligned(Guid, &GUID_NULL) || IsEqualGUIDAligned(Guid, Expected);
}

/* The format of a terminal that streams a connection format exactly */
static
PUSBAUDIO_FORMAT
UsbAudioFindFormat(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR TerminalId,
    _In_ PKSDATAFORMAT DataFormat)
{
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
    PWAVEFORMATEXTENSIBLE Extensible = (PWAVEFORMATEXTENSIBLE)&WaveFormat->WaveFormatEx;
    PUSBAUDIO_FORMAT Format;
    BOOLEAN Float;
    ULONG Index;

    if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(&DataFormat->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(&DataFormat->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        return NULL;
    }

    switch (WaveFormat->WaveFormatEx.wFormatTag)
    {
        case WAVE_FORMAT_PCM:
            Float = FALSE;
            break;
        case WAVE_FORMAT_IEEE_FLOAT:
            Float = TRUE;
            break;
        case WAVE_FORMAT_EXTENSIBLE:
            if (DataFormat->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE))
                return NULL;
            Float = IsEqualGUIDAligned(&Extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            if (!Float && !IsEqualGUIDAligned(&Extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
                return NULL;
            break;
        default:
            return NULL;
    }

    for (Index = 0; Index < DeviceExtension->FormatCount; Index++)
    {
        Format = &DeviceExtension->Formats[Index];
        if (Format->TerminalLink != TerminalId || (Format->FormatTag == USB_AUDIO_FORMAT_IEEE_FLOAT) != Float)
            continue;
        if (Format->Channels != WaveFormat->WaveFormatEx.nChannels ||
            Format->SubframeSize * 8 != WaveFormat->WaveFormatEx.wBitsPerSample ||
            !UsbAudioSupportsRate(Format, WaveFormat->WaveFormatEx.nSamplesPerSec))
        {
            continue;
        }

        /* Frames are laid out as the device sends them, and the byte rate has to agree */
        if (WaveFormat->WaveFormatEx.nBlockAlign != Format->Channels * Format->SubframeSize ||
            WaveFormat->WaveFormatEx.nAvgBytesPerSec != WaveFormat->WaveFormatEx.nSamplesPerSec * WaveFormat->WaveFormatEx.nBlockAlign ||
            WaveFormat->WaveFormatEx.nSamplesPerSec < 1000 || WaveFormat->WaveFormatEx.nSamplesPerSec > 384000)
        {
            continue;
        }
        return Format;
    }
    return NULL;
}

static
ULONG
UsbAudioDefaultChannelMask(
    _In_ ULONG Channels)
{
    switch (Channels)
    {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 4: return KSAUDIO_SPEAKER_QUAD;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1;
        default: return 0;
    }
}

/*
 * Picks, among the terminal's formats the client's range allows, the one with the highest
 * rate, then the most channels, then the most bits, and describes it at that rate.
 */
NTSTATUS
NTAPI
UsbAudioPinDataIntersect(
    _In_  PVOID        Context,
    _In_  PIRP         Irp,
    _In_  PKSP_PIN     Pin,
    _In_  PKSDATARANGE DataRange,
    _In_  PKSDATARANGE MatchingDataRange,
    _In_  ULONG        DataBufferSize,
    _Out_ PVOID        Data,
    _Out_ PULONG       DataSize)
{
    PKSFILTER Filter;
    PDEVICE_EXTENSION DeviceExtension;
    PKSDATARANGE_AUDIO ClientRange = (PKSDATARANGE_AUDIO)DataRange;
    PKSDATAFORMAT_WAVEFORMATEX DataFormat;
    PWAVEFORMATEXTENSIBLE Extensible;
    PUSBAUDIO_FORMAT Format, Best = NULL;
    ULONG Index, Rate, BestRate = 0, MinRate, MaxRate, MaxChannels, MinBits, MaxBits, Size;
    BOOLEAN Range, Float, Extended;
    UCHAR TerminalId;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(MatchingDataRange);

    Filter = KsGetFilterFromIrp(Irp);
    if (!Filter)
        return STATUS_NO_MATCH;
    DeviceExtension = ((PFILTER_CONTEXT)Filter->Context)->DeviceExtension;
    TerminalId = UsbAudioGetPinTerminal(DeviceExtension, Pin->PinId);

    if (!UsbAudioIsWildcardOr(&DataRange->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) ||
        !UsbAudioIsWildcardOr(&DataRange->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        return STATUS_NO_MATCH;
    }

    /* The client gives either a range or one exact format */
    Range = DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO);
    if (Range)
    {
        MinRate = ClientRange->MinimumSampleFrequency;
        MaxRate = ClientRange->MaximumSampleFrequency;
        MaxChannels = ClientRange->MaximumChannels;
        MinBits = ClientRange->MinimumBitsPerSample;
        MaxBits = ClientRange->MaximumBitsPerSample;
    }
    else if (DataRange->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX))
    {
        DataFormat = (PKSDATAFORMAT_WAVEFORMATEX)DataRange;
        MinRate = MaxRate = DataFormat->WaveFormatEx.nSamplesPerSec;
        MaxChannels = DataFormat->WaveFormatEx.nChannels;
        MinBits = MaxBits = DataFormat->WaveFormatEx.wBitsPerSample;
    }
    else
    {
        return STATUS_NO_MATCH;
    }

    for (Index = 0; Index < DeviceExtension->FormatCount; Index++)
    {
        Format = &DeviceExtension->Formats[Index];
        Float = (Format->FormatTag == USB_AUDIO_FORMAT_IEEE_FLOAT);
        if (Format->TerminalLink != TerminalId ||
            !UsbAudioIsWildcardOr(&DataRange->SubFormat, Float ? &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : &KSDATAFORMAT_SUBTYPE_PCM))
        {
            continue;
        }
        if (Format->Channels > MaxChannels || (!Range && Format->Channels != MaxChannels) ||
            Format->SubframeSize * 8 < MinBits || Format->SubframeSize * 8 > MaxBits)
        {
            continue;
        }

        Rate = UsbAudioBestRate(Format, MinRate, MaxRate);
        if (!Rate)
            continue;

        if (!Best || Rate > BestRate ||
            (Rate == BestRate && (Format->Channels > Best->Channels ||
                                  (Format->Channels == Best->Channels && Format->BitResolution > Best->BitResolution))))
        {
            Best = Format;
            BestRate = Rate;
        }
    }

    if (!Best)
        return STATUS_NO_MATCH;

    Extended = Best->SubframeSize > 2 || Best->Channels > 2;
    Size = Extended ? sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) : sizeof(KSDATAFORMAT_WAVEFORMATEX);
    *DataSize = Size;
    if (!DataBufferSize)
        return STATUS_BUFFER_OVERFLOW;
    if (DataBufferSize < Size)
        return STATUS_BUFFER_TOO_SMALL;

    DataFormat = (PKSDATAFORMAT_WAVEFORMATEX)Data;
    DataFormat->DataFormat.FormatSize = Size;
    DataFormat->DataFormat.Flags = 0;
    DataFormat->DataFormat.Reserved = 0;
    DataFormat->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    DataFormat->DataFormat.SubFormat = Best->DataRange.DataRange.SubFormat;
    DataFormat->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    DataFormat->DataFormat.SampleSize = Best->SubframeSize * Best->Channels;
    DataFormat->WaveFormatEx.wFormatTag = (Best->FormatTag == USB_AUDIO_FORMAT_IEEE_FLOAT) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    DataFormat->WaveFormatEx.nChannels = Best->Channels;
    DataFormat->WaveFormatEx.nSamplesPerSec = BestRate;
    DataFormat->WaveFormatEx.wBitsPerSample = Best->SubframeSize * 8;
    DataFormat->WaveFormatEx.nBlockAlign = Best->SubframeSize * Best->Channels;
    DataFormat->WaveFormatEx.nAvgBytesPerSec = BestRate * DataFormat->WaveFormatEx.nBlockAlign;
    DataFormat->WaveFormatEx.cbSize = 0;

    if (Extended)
    {
        Extensible = (PWAVEFORMATEXTENSIBLE)&DataFormat->WaveFormatEx;
        Extensible->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        Extensible->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        Extensible->Samples.wValidBitsPerSample = Best->BitResolution ? Best->BitResolution : Best->SubframeSize * 8;
        Extensible->dwChannelMask = UsbAudioDefaultChannelMask(Best->Channels);
        Extensible->SubFormat = Best->DataRange.DataRange.SubFormat;
    }
    return STATUS_SUCCESS;
}

/* Sends a request that targets the pin's endpoint, such as its sampling frequency */
static
NTSTATUS
UsbAudioSetSampleRate(
    _In_ PPIN_CONTEXT PinContext,
    _In_ BOOLEAN Capture)
{
    PUSBAUDIO_FORMAT Format = PinContext->Format;
    UCHAR Rate[3];
    NTSTATUS Status;

    /* 2.0 sets the rate on the clock behind the terminal, not on the endpoint */
    if (PinContext->DeviceExtension->Uac2)
    {
        Status = UsbAudio2SetClockFrequency(PinContext->DeviceExtension, Format, PinContext->SampleRate);
        if (!NT_SUCCESS(Status))
            DPRINT1("UsbAudioSetSampleRate clock %u to %lu failed with %x\n", Format->ClockId, PinContext->SampleRate, Status);
        return (Format->FrequencyCount == 1) ? STATUS_SUCCESS : Status;
    }

    /* Endpoints announce the control; a capture endpoint with a choice of rates needs it anyway */
    if (!(Format->EndpointAttributes & USB_AUDIO_EP_SAMPLING_FREQ) && !(Capture && Format->FrequencyCount > 1))
        return STATUS_SUCCESS;

    Rate[0] = (UCHAR)(PinContext->SampleRate);
    Rate[1] = (UCHAR)(PinContext->SampleRate >> 8);
    Rate[2] = (UCHAR)(PinContext->SampleRate >> 16);
    Status = UsbAudioClassRequest(PinContext->DeviceExtension,
                                  URB_FUNCTION_CLASS_ENDPOINT,
                                  USB_AUDIO_SET_CUR,
                                  USB_AUDIO_SAMPLING_FREQ_CONTROL << 8,
                                  Format->EndpointAddress,
                                  Rate,
                                  sizeof(Rate),
                                  FALSE);
    if (!NT_SUCCESS(Status))
        DPRINT1("UsbAudioSetSampleRate %lu failed with %x\n", PinContext->SampleRate, Status);

    /* A device with one rate has nothing to set */
    return (Format->FrequencyCount == 1) ? STATUS_SUCCESS : Status;
}

/* Selects an alternate setting of the pin's streaming interface and finds the data pipe in it */
static
NTSTATUS
UsbAudioSelectAlternate(
    _In_ PPIN_CONTEXT PinContext,
    _In_ UCHAR AlternateSetting)
{
    PDEVICE_EXTENSION DeviceExtension = PinContext->DeviceExtension;
    PUSB_INTERFACE_DESCRIPTOR Interface;
    PUSBD_INTERFACE_INFORMATION Information;
    ULONG Size, Index;
    NTSTATUS Status;
    PURB Urb;

    Interface = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor,
                                                    DeviceExtension->ConfigurationDescriptor,
                                                    PinContext->Format->InterfaceNumber,
                                                    AlternateSetting, -1, -1, -1);
    if (!Interface)
        return STATUS_INVALID_PARAMETER;

    Size = GET_SELECT_INTERFACE_REQUEST_SIZE(Interface->bNumEndpoints);
    Urb = AllocFunction(Size);
    if (!Urb)
        return STATUS_INSUFFICIENT_RESOURCES;

    UsbBuildSelectInterfaceRequest(Urb, (USHORT)Size, DeviceExtension->ConfigurationHandle,
                                   Interface->bInterfaceNumber, Interface->bAlternateSetting);
    Information = &Urb->UrbSelectInterface.Interface;
    Information->Length = (USHORT)GET_USBD_INTERFACE_SIZE(Interface->bNumEndpoints);
    for (Index = 0; Index < Interface->bNumEndpoints; Index++)
        Information->Pipes[Index].MaximumTransferSize = 500 * max((ULONG)PinContext->Format->MaximumPacketSize, 1UL);

    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    PinContext->PipeHandle = NULL;
    PinContext->FeedbackPipe = NULL;
    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Information->NumberOfPipes; Index++)
        {
            if (Information->Pipes[Index].EndpointAddress == PinContext->Format->EndpointAddress)
                PinContext->PipeHandle = Information->Pipes[Index].PipeHandle;
            else if (PinContext->Format->FeedbackAddress &&
                     Information->Pipes[Index].EndpointAddress == PinContext->Format->FeedbackAddress)
                PinContext->FeedbackPipe = Information->Pipes[Index].PipeHandle;
        }
    }

    DPRINT1("UsbAudioSelectAlternate interface %u alternate %u status %x pipe %p\n",
            Interface->bInterfaceNumber, AlternateSetting, Status, PinContext->PipeHandle);
    FreeFunction(Urb);
    return Status;
}

/* Takes back every transfer the device holds for the pin; FALSE if some never came back */
static
BOOLEAN
UsbAudioAbortPipe(
    _In_ PPIN_CONTEXT PinContext)
{
    struct _URB_PIPE_REQUEST *Urb;
    LARGE_INTEGER Timeout;

    Urb = AllocFunction(sizeof(*Urb));
    if (Urb)
    {
        if (PinContext->PipeHandle)
        {
            Urb->Hdr.Length = sizeof(*Urb);
            Urb->Hdr.Function = URB_FUNCTION_ABORT_PIPE;
            Urb->PipeHandle = PinContext->PipeHandle;
            SubmitUrbSync(PinContext->DeviceExtension->LowerDevice, (PURB)Urb);
        }
        if (PinContext->FeedbackPipe)
        {
            RtlZeroMemory(Urb, sizeof(*Urb));
            Urb->Hdr.Length = sizeof(*Urb);
            Urb->Hdr.Function = URB_FUNCTION_ABORT_PIPE;
            Urb->PipeHandle = PinContext->FeedbackPipe;
            SubmitUrbSync(PinContext->DeviceExtension->LowerDevice, (PURB)Urb);
        }
        FreeFunction(Urb);
    }

    Timeout.QuadPart = -10000LL * USBAUDIO_STOP_TIMEOUT_MS;
    if (KeWaitForSingleObject(&PinContext->Idle, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        DPRINT1("UsbAudioAbortPipe %ld transfers did not come back\n", PinContext->Pending);
        return FALSE;
    }
    return TRUE;
}

NTSTATUS
NTAPI
UsbAudioRenderComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context);

NTSTATUS
NTAPI
UsbAudioCaptureComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context);

/*
 * Hands a transfer to the device while the pin streams. Counted is set for a transfer the
 * device just gave back, which is still counted as pending.
 */
static
VOID
UsbAudioSubmitTransfer(
    _In_ PPIN_CONTEXT PinContext,
    _In_ PPIN_TRANSFER Transfer,
    _In_ BOOLEAN Capture,
    _In_ BOOLEAN Counted)
{
    PURB Urb = Transfer->Urb;
    PIO_STACK_LOCATION IoStack;
    ULONG Index, Offset = 0;
    KIRQL OldIrql;

    RtlZeroMemory(Urb, GET_ISO_URB_SIZE(Transfer->Packets));
    Urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    Urb->UrbIsochronousTransfer.Hdr.Length = (USHORT)GET_ISO_URB_SIZE(Transfer->Packets);
    Urb->UrbIsochronousTransfer.PipeHandle = PinContext->PipeHandle;
    Urb->UrbIsochronousTransfer.TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
                                                (Capture ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : USBD_TRANSFER_DIRECTION_OUT);
    Urb->UrbIsochronousTransfer.TransferBuffer = Transfer->Buffer;
    Urb->UrbIsochronousTransfer.NumberOfPackets = Transfer->Packets;

    /* Capture packets each get a full packet of room; render packets are as long as they were filled */
    for (Index = 0; Index < Transfer->Packets; Index++)
    {
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = Offset;
        Offset += Capture ? PinContext->Format->MaximumPacketSize : PinContext->PacketBytes[Index];
    }
    Urb->UrbIsochronousTransfer.TransferBufferLength = Offset;

    IoReuseIrp(Transfer->Irp, STATUS_SUCCESS);
    IoStack = IoGetNextIrpStackLocation(Transfer->Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    IoStack->Parameters.Others.Argument1 = Urb;
    IoSetCompletionRoutine(Transfer->Irp, Capture ? UsbAudioCaptureComplete : UsbAudioRenderComplete, Transfer, TRUE, TRUE, TRUE);

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    if (!PinContext->Streaming)
    {
        /* a stopping pin takes nothing more to the device */
        InsertTailList(&PinContext->FreeTransfers, &Transfer->Entry);
        if (Counted && !--PinContext->Pending)
            KeSetEvent(&PinContext->Idle, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&PinContext->Lock, OldIrql);
        return;
    }
    if (!Counted && !PinContext->Pending++)
        KeClearEvent(&PinContext->Idle);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);

    IoCallDriver(PinContext->LowerDevice, Transfer->Irp);
}

/*
 * Takes back a transfer from the device. The pin is asked to process before the transfer stops
 * being pending, as once nothing is pending the pin may be gone.
 */
static
VOID
UsbAudioTransferDone(
    _In_ PPIN_CONTEXT PinContext,
    _In_ PPIN_TRANSFER Transfer)
{
    PKSPIN Pin = Transfer->Pin;
    KIRQL OldIrql;

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    InsertTailList(&PinContext->FreeTransfers, &Transfer->Entry);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);

    KsPinAttemptProcessing(Pin, TRUE);

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    if (!--PinContext->Pending)
        KeSetEvent(&PinContext->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);
}

/* Puts back a transfer that never went to the device */
static
VOID
UsbAudioReleaseTransfer(
    _In_ PPIN_CONTEXT PinContext,
    _In_ PPIN_TRANSFER Transfer)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    InsertTailList(&PinContext->FreeTransfers, &Transfer->Entry);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);
}

NTSTATUS
NTAPI
UsbAudioRenderComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    PPIN_TRANSFER Transfer = Context;
    PPIN_CONTEXT PinContext = Transfer->Pin->Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!NT_SUCCESS(Irp->IoStatus.Status))
        DPRINT("UsbAudioRenderComplete failed with %x\n", Irp->IoStatus.Status);

    /* a free transfer means room for more data */
    UsbAudioTransferDone(PinContext, Transfer);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
UsbAudioFeedbackComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context);

/* Asks the device for its rate once more; it answers every 2^bRefresh milliseconds */
static
VOID
UsbAudioSubmitFeedback(
    _In_ PPIN_CONTEXT PinContext)
{
    PURB Urb = PinContext->FeedbackUrb;
    PIO_STACK_LOCATION IoStack;
    ULONG Index;

    /* One frame of feedback packets, four bytes each, which at high speed may be several */
    RtlZeroMemory(Urb, GET_ISO_URB_SIZE(PinContext->FeedbackPackets));
    Urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    Urb->UrbIsochronousTransfer.Hdr.Length = (USHORT)GET_ISO_URB_SIZE(PinContext->FeedbackPackets);
    Urb->UrbIsochronousTransfer.PipeHandle = PinContext->FeedbackPipe;
    Urb->UrbIsochronousTransfer.TransferFlags = USBD_START_ISO_TRANSFER_ASAP | USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    Urb->UrbIsochronousTransfer.TransferBuffer = PinContext->FeedbackBuffer;
    Urb->UrbIsochronousTransfer.TransferBufferLength = 4 * PinContext->FeedbackPackets;
    Urb->UrbIsochronousTransfer.NumberOfPackets = PinContext->FeedbackPackets;
    for (Index = 0; Index < PinContext->FeedbackPackets; Index++)
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = Index * 4;

    IoReuseIrp(PinContext->FeedbackIrp, STATUS_SUCCESS);
    IoStack = IoGetNextIrpStackLocation(PinContext->FeedbackIrp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    IoStack->Parameters.Others.Argument1 = Urb;
    IoSetCompletionRoutine(PinContext->FeedbackIrp, UsbAudioFeedbackComplete, PinContext, TRUE, TRUE, TRUE);
    IoCallDriver(PinContext->LowerDevice, PinContext->FeedbackIrp);
}

/*
 * Keeps the rate the device asks for, within a tenth of the nominal one. Full speed devices say
 * frames a frame in 10.14 and high speed ones frames a microframe in 16.16; it is kept as frames
 * a packet in 16.16.
 */
NTSTATUS
NTAPI
UsbAudioFeedbackComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    PPIN_CONTEXT PinContext = Context;
    struct _URB_ISOCH_TRANSFER *Urb = &PinContext->FeedbackUrb->UrbIsochronousTransfer;
    ULONG Nominal, Value, Index;
    PUCHAR Bytes;
    BOOLEAN Again;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* The newest packet that came back holds the value */
    for (Index = Urb->NumberOfPackets; NT_SUCCESS(Irp->IoStatus.Status) && Index > 0; Index--)
    {
        if (!USBD_SUCCESS(Urb->IsoPacket[Index - 1].Status) || Urb->IsoPacket[Index - 1].Length < 3)
            continue;

        Bytes = PinContext->FeedbackBuffer + Urb->IsoPacket[Index - 1].Offset;
        Value = Bytes[0] | (Bytes[1] << 8) | (Bytes[2] << 16);
        if (PinContext->DeviceExtension->HighSpeed)
        {
            if (Urb->IsoPacket[Index - 1].Length >= 4)
                Value |= (ULONG)Bytes[3] << 24;
            Value *= PinContext->ServiceMicroframes;
        }
        else
        {
            Value <<= USBAUDIO_FEEDBACK_SHIFT - USB_AUDIO_FEEDBACK_SHIFT;
        }

        Nominal = (ULONG)(((ULONGLONG)PinContext->SampleRate << USBAUDIO_FEEDBACK_SHIFT) / PinContext->PacketsPerSecond);
        Value = min(max(Value, Nominal - Nominal / 10), Nominal + Nominal / 10);
        InterlockedExchange(&PinContext->FeedbackRate, (LONG)Value);
        break;
    }

    /* the feedback stops with the stream, and on a failure that is not the device saying nothing yet */
    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    Again = PinContext->Streaming && Irp->IoStatus.Status != STATUS_CANCELLED &&
            Irp->IoStatus.Status != STATUS_NO_SUCH_DEVICE && Irp->IoStatus.Status != STATUS_DEVICE_NOT_CONNECTED;
    if (!Again && !--PinContext->Pending)
        KeSetEvent(&PinContext->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);

    if (Again)
        UsbAudioSubmitFeedback(PinContext);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Starts reading the device's rate feedback, counted as pending like any transfer */
static
VOID
UsbAudioStartFeedback(
    _In_ PPIN_CONTEXT PinContext)
{
    KIRQL OldIrql;

    if (!PinContext->FeedbackPipe || !PinContext->FeedbackIrp)
        return;

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    if (!PinContext->Pending++)
        KeClearEvent(&PinContext->Idle);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);
    UsbAudioSubmitFeedback(PinContext);
}

/* Adds captured bytes to the ring, dropping the oldest when the client falls behind */
static
VOID
UsbAudioRingWrite(
    _In_ PPIN_CONTEXT PinContext,
    _In_reads_bytes_(Length) PUCHAR Data,
    _In_ ULONG Length)
{
    ULONG Write, Chunk;

    if (Length > PinContext->RingSize)
    {
        Data += Length - PinContext->RingSize;
        Length = PinContext->RingSize;
    }
    if (PinContext->RingCount + Length > PinContext->RingSize)
    {
        Chunk = PinContext->RingCount + Length - PinContext->RingSize;
        PinContext->RingRead = (PinContext->RingRead + Chunk) % PinContext->RingSize;
        PinContext->RingCount -= Chunk;
        PinContext->Discontinuity = TRUE;
    }

    Write = (PinContext->RingRead + PinContext->RingCount) % PinContext->RingSize;
    Chunk = min(Length, PinContext->RingSize - Write);
    RtlCopyMemory(PinContext->Ring + Write, Data, Chunk);
    RtlCopyMemory(PinContext->Ring, Data + Chunk, Length - Chunk);
    PinContext->RingCount += Length;
}

NTSTATUS
NTAPI
UsbAudioCaptureComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    PPIN_TRANSFER Transfer = Context;
    PPIN_CONTEXT PinContext = Transfer->Pin->Context;
    struct _URB_ISOCH_TRANSFER *Urb = &Transfer->Urb->UrbIsochronousTransfer;
    ULONG Index, Length;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Packets that failed or came back empty leave a gap the client is told about */
    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    for (Index = 0; NT_SUCCESS(Irp->IoStatus.Status) && Index < Urb->NumberOfPackets; Index++)
    {
        Length = min(Urb->IsoPacket[Index].Length, (ULONG)PinContext->Format->MaximumPacketSize);
        if (!USBD_SUCCESS(Urb->IsoPacket[Index].Status) || !Length)
        {
            PinContext->Discontinuity = TRUE;
            continue;
        }
        UsbAudioRingWrite(PinContext, Transfer->Buffer + Urb->IsoPacket[Index].Offset, Length);
    }
    if (!NT_SUCCESS(Irp->IoStatus.Status))
        PinContext->Discontinuity = TRUE;
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);

    /* A failed transfer goes back too, unless the device is gone or the pipe was aborted */
    if (Irp->IoStatus.Status != STATUS_CANCELLED && Irp->IoStatus.Status != STATUS_NO_SUCH_DEVICE &&
        Irp->IoStatus.Status != STATUS_DEVICE_NOT_CONNECTED && Irp->IoStatus.Status != STATUS_DEVICE_REMOVED)
    {
        KsPinAttemptProcessing(Transfer->Pin, TRUE);
        UsbAudioSubmitTransfer(PinContext, Transfer, TRUE, TRUE);
    }
    else
    {
        UsbAudioTransferDone(PinContext, Transfer);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Bytes of the next render packet, carrying the fraction of a frame from packet to packet */
static
ULONG
UsbAudioNextPacketBytes(
    _In_ PPIN_CONTEXT PinContext)
{
    ULONG Frames = PinContext->SampleRate / PinContext->PacketsPerSecond;
    ULONG Rate = (ULONG)PinContext->FeedbackRate;

    if (Rate)
    {
        /* the device says how many frames it takes a packet */
        PinContext->FeedbackRemainder += Rate;
        Frames = PinContext->FeedbackRemainder >> USBAUDIO_FEEDBACK_SHIFT;
        PinContext->FeedbackRemainder &= (1 << USBAUDIO_FEEDBACK_SHIFT) - 1;
    }
    else
    {
        PinContext->FrameRemainder += PinContext->SampleRate % PinContext->PacketsPerSecond;
        if (PinContext->FrameRemainder >= PinContext->PacketsPerSecond)
        {
            PinContext->FrameRemainder -= PinContext->PacketsPerSecond;
            Frames++;
        }
    }
    return min(Frames * PinContext->FrameBytes,
               PinContext->Format->MaximumPacketSize - PinContext->Format->MaximumPacketSize % PinContext->FrameBytes);
}

static
PPIN_TRANSFER
UsbAudioTakeFreeTransfer(
    _In_ PPIN_CONTEXT PinContext)
{
    PPIN_TRANSFER Transfer = NULL;
    KIRQL OldIrql;

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    if (!IsListEmpty(&PinContext->FreeTransfers))
        Transfer = CONTAINING_RECORD(RemoveHeadList(&PinContext->FreeTransfers), PIN_TRANSFER, Entry);
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);
    return Transfer;
}

/* Sends the transfer being filled; an unfinished packet is made up with silence */
static
VOID
UsbAudioSubmitPartial(
    _In_ PPIN_CONTEXT PinContext)
{
    PPIN_TRANSFER Transfer = PinContext->Partial;
    ULONG PacketsPerFrame = PinContext->PacketsPerSecond / USBAUDIO_FRAMES_PER_SECOND;
    ULONG Missing;

    if (PinContext->PacketFill)
    {
        Missing = PinContext->PacketBytes[Transfer->Packets] - PinContext->PacketFill;
        RtlZeroMemory(Transfer->Buffer + Transfer->Filled, Missing);
        Transfer->Filled += Missing;
        Transfer->Packets++;
        PinContext->PacketFill = 0;
    }

    /* A high speed transfer covers whole frames, so the rest of the last one is silence */
    while (Transfer->Packets && Transfer->Packets % PacketsPerFrame)
    {
        PinContext->PacketBytes[Transfer->Packets] = UsbAudioNextPacketBytes(PinContext);
        RtlZeroMemory(Transfer->Buffer + Transfer->Filled, PinContext->PacketBytes[Transfer->Packets]);
        Transfer->Filled += PinContext->PacketBytes[Transfer->Packets];
        Transfer->Packets++;
    }

    PinContext->Partial = NULL;
    if (Transfer->Packets)
        UsbAudioSubmitTransfer(PinContext, Transfer, FALSE, FALSE);
    else
        UsbAudioReleaseTransfer(PinContext, Transfer);
}

/*
 * Copies the client's frames into transfers of one millisecond packets. A transfer goes to
 * the device once its packets are full, or as it is when the device has run dry.
 */
static
NTSTATUS
PinRenderProcess(
    IN PKSPIN Pin)
{
    PPIN_CONTEXT PinContext = Pin->Context;
    PKSSTREAM_POINTER Leading;
    PPIN_TRANSFER Transfer;
    ULONG Copy;
    KIRQL OldIrql;
    LONG Pending;

    if (!PinContext->Streaming)
        return STATUS_PENDING;

    Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    while (Leading)
    {
        if (!PinContext->Partial)
        {
            PinContext->Partial = UsbAudioTakeFreeTransfer(PinContext);
            if (!PinContext->Partial)
                break;
            PinContext->Partial->Packets = 0;
            PinContext->Partial->Filled = 0;
            PinContext->PacketFill = 0;
        }
        Transfer = PinContext->Partial;

        if (!PinContext->PacketFill)
            PinContext->PacketBytes[Transfer->Packets] = UsbAudioNextPacketBytes(PinContext);

        /* A device with a lock delay hears silence while its clock settles */
        if (PinContext->SilencePackets && !PinContext->PacketFill)
        {
            RtlZeroMemory(Transfer->Buffer + Transfer->Filled, PinContext->PacketBytes[Transfer->Packets]);
            Transfer->Filled += PinContext->PacketBytes[Transfer->Packets];
            Transfer->Packets++;
            PinContext->SilencePackets--;
            if (Transfer->Packets == PinContext->PacketsPerTransfer)
            {
                PinContext->Partial = NULL;
                UsbAudioSubmitTransfer(PinContext, Transfer, FALSE, FALSE);
            }
            continue;
        }

        Copy = min(PinContext->PacketBytes[Transfer->Packets] - PinContext->PacketFill, Leading->OffsetIn.Remaining);
        RtlCopyMemory(Transfer->Buffer + Transfer->Filled, Leading->OffsetIn.Data, Copy);
        Transfer->Filled += Copy;
        PinContext->PacketFill += Copy;

        if (PinContext->PacketFill == PinContext->PacketBytes[Transfer->Packets])
        {
            Transfer->Packets++;
            PinContext->PacketFill = 0;
            if (Transfer->Packets == PinContext->PacketsPerTransfer)
            {
                PinContext->Partial = NULL;
                UsbAudioSubmitTransfer(PinContext, Transfer, FALSE, FALSE);
            }
        }

        /* moving past the end of a frame hands it back and goes on to the next one */
        if (!NT_SUCCESS(KsStreamPointerAdvanceOffsets(Leading, Copy, 0, FALSE)))
            Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    }

    if (Leading)
        KsStreamPointerUnlock(Leading, FALSE);

    /* With nothing left at the device, what is there is better heard now than later */
    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    Pending = PinContext->Pending;
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);
    if (!Pending && PinContext->Partial)
        UsbAudioSubmitPartial(PinContext);

    return STATUS_PENDING;
}

/* Moves captured bytes from the ring into the client's frames */
static
NTSTATUS
PinCaptureProcess(
    IN PKSPIN Pin)
{
    PPIN_CONTEXT PinContext = Pin->Context;
    PKSSTREAM_POINTER Leading;
    ULONG Copy, Chunk;
    KIRQL OldIrql;

    Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    while (Leading)
    {
        KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
        Copy = min(PinContext->RingCount, Leading->OffsetOut.Remaining);

        /* whole frames only, so a sample never straddles two buffers */
        Copy -= Copy % PinContext->FrameBytes;
        if (!Copy)
        {
            KeReleaseSpinLock(&PinContext->Lock, OldIrql);
            if (Leading->OffsetOut.Remaining >= PinContext->FrameBytes)
                break;

            /* the client's buffer has no room for a whole frame, so it goes back as it is */
            if (!NT_SUCCESS(KsStreamPointerAdvanceOffsets(Leading, 0, 0, TRUE)))
                Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
            continue;
        }

        Chunk = min(Copy, PinContext->RingSize - PinContext->RingRead);
        RtlCopyMemory(Leading->OffsetOut.Data, PinContext->Ring + PinContext->RingRead, Chunk);
        RtlCopyMemory(Leading->OffsetOut.Data + Chunk, PinContext->Ring, Copy - Chunk);
        PinContext->RingRead = (PinContext->RingRead + Copy) % PinContext->RingSize;
        PinContext->RingCount -= Copy;
        if (PinContext->Discontinuity)
        {
            Leading->StreamHeader->OptionsFlags |= KSSTREAM_HEADER_OPTIONSF_DATADISCONTINUITY;
            PinContext->Discontinuity = FALSE;
        }
        KeReleaseSpinLock(&PinContext->Lock, OldIrql);

        if (!NT_SUCCESS(KsStreamPointerAdvanceOffsets(Leading, 0, Copy, FALSE)))
            Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    }

    if (Leading)
        KsStreamPointerUnlock(Leading, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBAudioPinProcess(
    _In_ PKSPIN Pin)
{
    if (!Pin->Context)
        return STATUS_PENDING;

    if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
        return PinCaptureProcess(Pin);
    return PinRenderProcess(Pin);
}

/* Microframes between the packets of a high speed endpoint, as the host controller reads bInterval */
static
ULONG
UsbAudioServiceMicroframes(
    _In_ UCHAR Interval)
{
    return (Interval == 0 || Interval > 4) ? USBAUDIO_MICROFRAMES_PER_FRAME : (1UL << (Interval - 1));
}

/*
 * Packets come once a frame at full speed and once a service interval at high speed. A transfer
 * covers the same time either way, and a feedback transfer one frame.
 */
static
VOID
UsbAudioSetPacing(
    _Inout_ PPIN_CONTEXT PinContext)
{
    PUSBAUDIO_FORMAT Format = PinContext->Format;

    if (PinContext->DeviceExtension->HighSpeed)
    {
        PinContext->ServiceMicroframes = UsbAudioServiceMicroframes(Format->EndpointInterval);
        PinContext->PacketsPerSecond = USBAUDIO_FRAMES_PER_SECOND * USBAUDIO_MICROFRAMES_PER_FRAME / PinContext->ServiceMicroframes;
        PinContext->FeedbackPackets = USBAUDIO_MICROFRAMES_PER_FRAME / UsbAudioServiceMicroframes(Format->FeedbackInterval);
    }
    else
    {
        PinContext->ServiceMicroframes = USBAUDIO_MICROFRAMES_PER_FRAME;
        PinContext->PacketsPerSecond = USBAUDIO_FRAMES_PER_SECOND;
        PinContext->FeedbackPackets = 1;
    }
    PinContext->PacketsPerTransfer = min(PinContext->PacketsPerSecond * USBAUDIO_TRANSFER_MS / USBAUDIO_FRAMES_PER_SECOND,
                                         (ULONG)USBAUDIO_MAX_PACKETS_PER_TRANSFER);
}

/* Allocates the transfers of a pin, which it keeps until it is closed */
static
NTSTATUS
UsbAudioAllocateTransfers(
    _In_ PKSPIN Pin,
    _In_ PPIN_CONTEXT PinContext)
{
    PPIN_TRANSFER Transfer;
    BOOLEAN Capture = (Pin->DataFlow == KSPIN_DATAFLOW_OUT);
    ULONG Index;

    PinContext->TransferCount = Capture ? USBAUDIO_CAPTURE_TRANSFERS : USBAUDIO_RENDER_TRANSFERS;
    PinContext->Transfers = AllocFunction(PinContext->TransferCount * sizeof(PIN_TRANSFER));
    if (!PinContext->Transfers)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < PinContext->TransferCount; Index++)
    {
        Transfer = &PinContext->Transfers[Index];
        Transfer->Pin = Pin;
        Transfer->Packets = PinContext->PacketsPerTransfer;
        Transfer->Irp = IoAllocateIrp(PinContext->LowerDevice->StackSize, FALSE);
        Transfer->Urb = AllocFunction(GET_ISO_URB_SIZE(PinContext->PacketsPerTransfer));
        Transfer->Buffer = AllocFunction(PinContext->PacketsPerTransfer * PinContext->Format->MaximumPacketSize);
        if (!Transfer->Irp || !Transfer->Urb || !Transfer->Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        InsertTailList(&PinContext->FreeTransfers, &Transfer->Entry);
    }

    if (!Capture && PinContext->Format->FeedbackAddress)
    {
        PinContext->FeedbackIrp = IoAllocateIrp(PinContext->LowerDevice->StackSize, FALSE);
        PinContext->FeedbackUrb = AllocFunction(GET_ISO_URB_SIZE(PinContext->FeedbackPackets));
        PinContext->FeedbackBuffer = AllocFunction(4 * PinContext->FeedbackPackets);
        if (!PinContext->FeedbackIrp || !PinContext->FeedbackUrb || !PinContext->FeedbackBuffer)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Capture)
    {
        PinContext->RingSize = (ULONG)((ULONGLONG)PinContext->FrameBytes * PinContext->SampleRate * USBAUDIO_CAPTURE_RING_MS / 1000);
        PinContext->RingSize -= PinContext->RingSize % PinContext->FrameBytes;
        PinContext->Ring = PinContext->RingSize ? AllocFunction(PinContext->RingSize) : NULL;
        if (!PinContext->Ring)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

static
VOID
UsbAudioFreeTransfers(
    _In_ PPIN_CONTEXT PinContext)
{
    ULONG Index;

    for (Index = 0; PinContext->Transfers && Index < PinContext->TransferCount; Index++)
    {
        if (PinContext->Transfers[Index].Irp)
            IoFreeIrp(PinContext->Transfers[Index].Irp);
        if (PinContext->Transfers[Index].Urb)
            FreeFunction(PinContext->Transfers[Index].Urb);
        if (PinContext->Transfers[Index].Buffer)
            FreeFunction(PinContext->Transfers[Index].Buffer);
    }
    if (PinContext->Transfers)
        FreeFunction(PinContext->Transfers);
    if (PinContext->Ring)
        FreeFunction(PinContext->Ring);
    if (PinContext->FeedbackIrp)
        IoFreeIrp(PinContext->FeedbackIrp);
    if (PinContext->FeedbackUrb)
        FreeFunction(PinContext->FeedbackUrb);
    if (PinContext->FeedbackBuffer)
        FreeFunction(PinContext->FeedbackBuffer);
    PinContext->Transfers = NULL;
    PinContext->Ring = NULL;
    PinContext->FeedbackIrp = NULL;
    PinContext->FeedbackUrb = NULL;
    PinContext->FeedbackBuffer = NULL;
}

NTSTATUS
NTAPI
USBAudioPinCreate(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PKSFILTER Filter;
    PFILTER_CONTEXT FilterContext;
    PPIN_CONTEXT PinContext;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;
    PUSBAUDIO_FORMAT Format;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Irp);

    Filter = KsPinGetParentFilter(Pin);
    if (Filter == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    /* get filter context */
    FilterContext = Filter->Context;

    /* choose correct dataformat */
    Format = UsbAudioFindFormat(FilterContext->DeviceExtension,
                                UsbAudioGetPinTerminal(FilterContext->DeviceExtension, Pin->Id),
                                Pin->ConnectionFormat);
    if (!Format)
    {
        /* no format match */
        return STATUS_NO_MATCH;
    }

    /* allocate pin context */
    PinContext = AllocFunction(sizeof(PIN_CONTEXT));
    if (!PinContext)
    {
        /* no memory*/
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init pin context */
    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)Pin->ConnectionFormat;
    PinContext->DeviceExtension = FilterContext->DeviceExtension;
    PinContext->LowerDevice = FilterContext->LowerDevice;
    PinContext->Format = Format;
    PinContext->SampleRate = WaveFormat->WaveFormatEx.nSamplesPerSec;
    PinContext->FrameBytes = Format->Channels * Format->SubframeSize;
    UsbAudioSetPacing(PinContext);
    InitializeListHead(&PinContext->FreeTransfers);
    KeInitializeSpinLock(&PinContext->Lock);
    KeInitializeEvent(&PinContext->Idle, NotificationEvent, TRUE);

    Status = UsbAudioAllocateTransfers(Pin, PinContext);
    if (!NT_SUCCESS(Status))
    {
        UsbAudioFreeTransfers(PinContext);
        FreeFunction(PinContext);
        return Status;
    }

    /* store pin context*/
    Pin->Context = PinContext;
    DPRINT1("USBAudioPinCreate pin %lu interface %u alternate %u rate %lu bytes per frame %lu\n",
            Pin->Id, Format->InterfaceNumber, Format->AlternateSetting, PinContext->SampleRate, PinContext->FrameBytes);
    return STATUS_SUCCESS;
}

/*
 * Stops the transfers of a pin and waits until the device gave them all back. The processing
 * mutex is held throughout, so no process call can send one more after the stop.
 */
static
BOOLEAN
UsbAudioStopStreaming(
    _In_ PKSPIN Pin,
    _In_ PPIN_CONTEXT PinContext)
{
    BOOLEAN AllBack;
    KIRQL OldIrql;

    KsPinAcquireProcessingMutex(Pin);

    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    PinContext->Streaming = FALSE;
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);

    AllBack = UsbAudioAbortPipe(PinContext);

    /* audio that never reached the device is dropped with the stream */
    if (PinContext->Partial)
    {
        UsbAudioReleaseTransfer(PinContext, PinContext->Partial);
        PinContext->Partial = NULL;
    }
    PinContext->PacketFill = 0;
    PinContext->FrameRemainder = 0;
    PinContext->FeedbackRemainder = 0;
    PinContext->FeedbackRate = 0;
    PinContext->SilencePackets = 0;
    KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
    PinContext->RingRead = 0;
    PinContext->RingCount = 0;
    KeReleaseSpinLock(&PinContext->Lock, OldIrql);

    KsPinReleaseProcessingMutex(Pin);
    return AllBack;
}

NTSTATUS
NTAPI
USBAudioPinClose(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PPIN_CONTEXT PinContext = Pin->Context;
    BOOLEAN AllBack = TRUE;

    UNREFERENCED_PARAMETER(Irp);

    if (!PinContext)
        return STATUS_SUCCESS;

    if (PinContext->Streaming || PinContext->Pending)
        AllBack = UsbAudioStopStreaming(Pin, PinContext);
    if (PinContext->PipeHandle)
        UsbAudioSelectAlternate(PinContext, 0);

    /* A process call already running finishes before the context goes */
    KsPinAcquireProcessingMutex(Pin);
    Pin->Context = NULL;
    KsPinReleaseProcessingMutex(Pin);

    /* Transfers the device never gave back still belong to it, so they and their pin context stay */
    if (!AllBack)
    {
        DPRINT1("USBAudioPinClose leaking pin context %p, the device kept transfers\n", PinContext);
        return STATUS_SUCCESS;
    }

    UsbAudioFreeTransfers(PinContext);
    FreeFunction(PinContext);
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBAudioPinReset(
    _In_ PKSPIN Pin)
{
    /* Nothing is held back from a flushed stream; frames already copied go out as they are */
    UNREFERENCED_PARAMETER(Pin);
}

NTSTATUS
NTAPI
USBAudioPinSetDataFormat(
    _In_ PKSPIN Pin,
    _In_opt_ PKSDATAFORMAT OldFormat,
    _In_opt_ PKSMULTIPLE_ITEM OldAttributeList,
    _In_ const KSDATARANGE* DataRange,
    _In_opt_ const KSATTRIBUTE_LIST* AttributeRange)
{
    PKSFILTER Filter = KsPinGetParentFilter(Pin);
    PDEVICE_EXTENSION DeviceExtension;

    UNREFERENCED_PARAMETER(OldAttributeList);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(AttributeRange);

    if (!Filter)
        return STATUS_INVALID_PARAMETER;
    DeviceExtension = ((PFILTER_CONTEXT)Filter->Context)->DeviceExtension;

    if (!UsbAudioFindFormat(DeviceExtension, UsbAudioGetPinTerminal(DeviceExtension, Pin->Id), Pin->ConnectionFormat))
        return STATUS_NO_MATCH;

    /* A pin keeps the format it was created with once its streaming interface is set up */
    if (OldFormat && Pin->Context && ((PPIN_CONTEXT)Pin->Context)->PipeHandle)
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}

/*
 * The streaming interface gets its bandwidth on the way to acquire and gives it back on the
 * way to stop. Transfers flow only while the pin runs.
 */
NTSTATUS
NTAPI
USBAudioPinSetDeviceState(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState)
{
    PPIN_CONTEXT PinContext = Pin->Context;
    BOOLEAN Capture = (Pin->DataFlow == KSPIN_DATAFLOW_OUT);
    PPIN_TRANSFER Transfer;
    NTSTATUS Status;
    KIRQL OldIrql;

    DPRINT1("USBAudioPinSetDeviceState pin %lu from %u to %u\n", Pin->Id, FromState, ToState);
    if (!PinContext || ToState == FromState)
        return STATUS_SUCCESS;

    if (FromState == KSSTATE_RUN)
        UsbAudioStopStreaming(Pin, PinContext);

    if (FromState == KSSTATE_STOP)
    {
        Status = UsbAudioSelectAlternate(PinContext, PinContext->Format->AlternateSetting);
        if (NT_SUCCESS(Status) && !PinContext->PipeHandle)
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        if (NT_SUCCESS(Status))
            Status = UsbAudioSetSampleRate(PinContext, Capture);
        if (!NT_SUCCESS(Status))
        {
            UsbAudioSelectAlternate(PinContext, 0);
            return Status;
        }
    }

    if (ToState == KSSTATE_STOP)
    {
        UsbAudioSelectAlternate(PinContext, 0);
        PinContext->PipeHandle = NULL;
    }

    if (ToState == KSSTATE_RUN)
    {
        /* Lock delay is given in milliseconds or in samples; any delay is at least one packet */
        PinContext->SilencePackets = 0;
        if (!Capture && PinContext->Format->LockDelay)
        {
            if (PinContext->Format->LockDelayUnits == USB_AUDIO_LOCK_DELAY_MS)
                PinContext->SilencePackets = PinContext->Format->LockDelay * PinContext->PacketsPerSecond / USBAUDIO_FRAMES_PER_SECOND;
            else if (PinContext->Format->LockDelayUnits == USB_AUDIO_LOCK_DELAY_SAMPLES)
                PinContext->SilencePackets = PinContext->Format->LockDelay / max(PinContext->SampleRate / PinContext->PacketsPerSecond, 1UL);
            PinContext->SilencePackets = max(PinContext->SilencePackets, 1UL);
        }

        KeAcquireSpinLock(&PinContext->Lock, &OldIrql);
        PinContext->Streaming = TRUE;
        KeReleaseSpinLock(&PinContext->Lock, OldIrql);

        if (!Capture)
            UsbAudioStartFeedback(PinContext);

        /* Capture keeps every transfer at the device; render sends them as data comes */
        if (Capture)
        {
            while ((Transfer = UsbAudioTakeFreeTransfer(PinContext)))
            {
                Transfer->Packets = PinContext->PacketsPerTransfer;
                UsbAudioSubmitTransfer(PinContext, Transfer, TRUE, FALSE);
            }
        }
    }
    return STATUS_SUCCESS;
}
