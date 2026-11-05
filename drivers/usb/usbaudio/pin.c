/*
* PROJECT:     ReactOS Universal Audio Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbaudio/pin.c
* PURPOSE:     USB Audio device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#define NDEBUG
#include "usbaudio.h"

#define PACKET_COUNT 16
#define IRP_CAPTURE_COUNT 16
/* up to RENDER_IRP_COUNT * RENDER_PACKETS_PER_URB frames are scheduled, this has to stay
 * below the 1024 entries of the frame list */
#define RENDER_IRP_COUNT (32)
/* render transfers carry up to 20ms of audio, always in whole packets */
#define RENDER_PACKETS_PER_URB (20)
/* below this number of pending render transfers the available packets are sent without
 * waiting for a full transfer, so the device does not run dry */
#define RENDER_LOW_WATER_IRPS (4)
/* audio buffered before streaming (re)starts, absorbs the jitter of the client */
#define RENDER_PREBUFFER_MS (200)
/* streaming starts at the latest this long after the first data arrived, so sounds shorter
 * than the prebuffer play too. It is longer than the prebuffer, a client delivering in real
 * time fills the prebuffer before */
#define RENDER_START_TIMEOUT_MS (RENDER_PREBUFFER_MS + 50)


NTSTATUS
UsbAudioAllocCaptureUrbIso(
    IN USBD_PIPE_HANDLE PipeHandle,
    IN ULONG MaxPacketSize,
    IN PVOID Buffer,
    IN ULONG BufferLength,
    OUT PURB * OutUrb)
{
    PURB Urb;
    ULONG UrbSize;
    ULONG Index;

    /* calculate urb size*/
    UrbSize = GET_ISO_URB_SIZE(PACKET_COUNT);

    /* allocate urb */
    Urb = AllocFunction(UrbSize);
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init urb */
    Urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    Urb->UrbIsochronousTransfer.Hdr.Length = UrbSize;
    Urb->UrbIsochronousTransfer.PipeHandle = PipeHandle;
    Urb->UrbIsochronousTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_START_ISO_TRANSFER_ASAP;
    Urb->UrbIsochronousTransfer.TransferBufferLength = BufferLength;
    Urb->UrbIsochronousTransfer.TransferBuffer = Buffer;
    Urb->UrbIsochronousTransfer.NumberOfPackets = PACKET_COUNT;

    for (Index = 0; Index < PACKET_COUNT; Index++)
    {
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = Index * MaxPacketSize;
    }

    *OutUrb = Urb;
    return STATUS_SUCCESS;

}

static
ULONG
UsbAudioGetSampleFrequency(
    IN PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR FormatDescriptor,
    IN ULONG Index)
{
    /* tSamFreq entries are 3 byte little endian values */
    return FormatDescriptor->tSamFreq[Index * 3] |
           FormatDescriptor->tSamFreq[(Index * 3) + 1] << 8 |
           FormatDescriptor->tSamFreq[(Index * 3) + 2] << 16;
}

static
BOOLEAN
UsbAudioIsSampleFrequencySupported(
    IN PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR FormatDescriptor,
    IN ULONG SampleFrequency)
{
    ULONG Index;

    if (FormatDescriptor->bSamFreqType == 0)
    {
        /* continuous sampling frequency, tSamFreq holds the lower and the upper bound */
        return SampleFrequency >= UsbAudioGetSampleFrequency(FormatDescriptor, 0) &&
               SampleFrequency <= UsbAudioGetSampleFrequency(FormatDescriptor, 1);
    }

    /* discrete sampling frequencies */
    for (Index = 0; Index < FormatDescriptor->bSamFreqType; Index++)
    {
        if (UsbAudioGetSampleFrequency(FormatDescriptor, Index) == SampleFrequency)
            return TRUE;
    }

    return FALSE;
}

static
PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR
UsbAudioGetFormatTypeDescriptor(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor)
{
    PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR StreamingInterfaceDescriptor;
    PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR FormatDescriptor;

    /* the format type descriptor follows the AS_GENERAL descriptor of the interface */
    StreamingInterfaceDescriptor = (PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, InterfaceDescriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
    if (!StreamingInterfaceDescriptor)
        return NULL;

    FormatDescriptor = (PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR)((ULONG_PTR)StreamingInterfaceDescriptor + StreamingInterfaceDescriptor->bLength);
    if (FormatDescriptor->bDescriptorType != USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE ||
        FormatDescriptor->bDescriptorSubtype != USB_AUDIO_STREAMING_FORMAT_TYPE ||
        FormatDescriptor->bFormatType != USB_AUDIO_FORMAT_TYPE_I)
    {
        return NULL;
    }

    return FormatDescriptor;
}

static
PUSB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR
UsbAudioGetStreamingEndpointDescriptor(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor,
    IN UCHAR EndpointAddress)
{
    PUSB_COMMON_DESCRIPTOR Descriptor;
    PUSB_COMMON_DESCRIPTOR NextDescriptor;
    ULONG_PTR End;

    End = (ULONG_PTR)ConfigurationDescriptor + ConfigurationDescriptor->wTotalLength;
    Descriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)InterfaceDescriptor + InterfaceDescriptor->bLength);

    /* search the endpoints of this alternate setting */
    while ((ULONG_PTR)Descriptor + sizeof(USB_COMMON_DESCRIPTOR) <= End &&
           Descriptor->bLength != 0 &&
           Descriptor->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE)
    {
        NextDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)Descriptor + Descriptor->bLength);

        if (Descriptor->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE &&
            ((PUSB_ENDPOINT_DESCRIPTOR)Descriptor)->bEndpointAddress == EndpointAddress)
        {
            /* the class-specific endpoint descriptor follows the standard endpoint descriptor */
            if ((ULONG_PTR)NextDescriptor + sizeof(USB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR) <= End &&
                NextDescriptor->bDescriptorType == USB_AUDIO_ENDPOINT_DESCRIPTOR_TYPE &&
                ((PUSB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR)NextDescriptor)->bDescriptorSubtype == USB_AUDIO_STREAMING_EP_GENERAL)
            {
                return (PUSB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR)NextDescriptor;
            }

            return NULL;
        }

        Descriptor = NextDescriptor;
    }

    return NULL;
}

NTSTATUS
UsbAudioSetFormat(
    IN PKSPIN Pin,
    IN const KSDATAFORMAT * DataFormat)
{
    PURB Urb;
    PUCHAR SampleRateBuffer;
    NTSTATUS Status;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormatEx;
    PFILTER_CONTEXT FilterContext;
    PKSFILTER Filter;
    ULONG FormatIndex;
    PPIN_CONTEXT PinContext;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR FormatDescriptor;
    PUSB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR EndpointDescriptor;
    UCHAR EndpointAddress;

    DPRINT1("UsbAudioSetFormat entered\n");

    if (!IsEqualGUIDAligned(&DataFormat->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(&DataFormat->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(&DataFormat->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        /* not supported yet*/
        UNIMPLEMENTED;
        return STATUS_INVALID_PARAMETER;
    }

    /* choose correct dataformat */
    FormatIndex = GetDataRangeIndexForFormat(DataFormat, Pin->Descriptor->PinDescriptor.DataRanges, Pin->Descriptor->PinDescriptor.DataRangesCount);
    if (FormatIndex == MAXULONG)
    {
        /* no format match */
        DPRINT1("UsbAudioSetFormat no matching format found\n");
        return STATUS_NO_MATCH;
    }

    DPRINT1("UsbAudioSetFormat FormatIndex %u\n", FormatIndex);

    Filter = KsPinGetParentFilter(Pin);
    if (Filter == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    /* get filter context */
    FilterContext = Filter->Context;

    if (!Pin->Context)
    {
        /* allocate pin context */
        PinContext = AllocFunction(sizeof(PIN_CONTEXT));
        if (!PinContext)
        {
            /* no memory*/
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* store pin context*/
        Pin->Context = PinContext;
    }
    else
    {
        PinContext = (PPIN_CONTEXT)Pin->Context;
    }

    ConfigurationDescriptor = FilterContext->DeviceExtension->ConfigurationDescriptor;

    Status = USBAudioSelectAudioStreamingInterface(Pin, PinContext, FilterContext->DeviceExtension,
        ConfigurationDescriptor, FormatIndex);
    if (!NT_SUCCESS(Status))
        return Status;

    /* validate the connection format against the type I format descriptor of the selected alternate setting */
    FormatDescriptor = UsbAudioGetFormatTypeDescriptor(ConfigurationDescriptor, PinContext->InterfaceDescriptor);
    if (!FormatDescriptor)
    {
        DPRINT1("UsbAudioSetFormat no type I format descriptor\n");
        return STATUS_INVALID_PARAMETER;
    }

    WaveFormatEx = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;

    /* samples are stored MSB justified in subframes of bSubframeSize bytes (USB Audio Data Formats 1.0, 2.2) */
    if (WaveFormatEx->WaveFormatEx.wFormatTag != WAVE_FORMAT_PCM ||
        WaveFormatEx->WaveFormatEx.nChannels != FormatDescriptor->bNrChannels ||
        WaveFormatEx->WaveFormatEx.wBitsPerSample != FormatDescriptor->bSubframeSize * 8 ||
        WaveFormatEx->WaveFormatEx.nBlockAlign != FormatDescriptor->bNrChannels * FormatDescriptor->bSubframeSize ||
        !UsbAudioIsSampleFrequencySupported(FormatDescriptor, WaveFormatEx->WaveFormatEx.nSamplesPerSec))
    {
        DPRINT1("UsbAudioSetFormat format mismatch Channels %u BitsPerSample %u BlockAlign %u SamplesPerSec %u\n",
                WaveFormatEx->WaveFormatEx.nChannels,
                WaveFormatEx->WaveFormatEx.wBitsPerSample,
                WaveFormatEx->WaveFormatEx.nBlockAlign,
                WaveFormatEx->WaveFormatEx.nSamplesPerSec);
        return STATUS_NO_MATCH;
    }

    /* the sampling frequency control is optional, only send SET_CUR when the endpoint supports it
     * (USB Audio 1.0, 4.6.1.2 and 5.2.3.2.3.1) */
    EndpointAddress = PinContext->InterfaceInfo->Pipes[0].EndpointAddress;
    EndpointDescriptor = UsbAudioGetStreamingEndpointDescriptor(ConfigurationDescriptor, PinContext->InterfaceDescriptor, EndpointAddress);
    if (!EndpointDescriptor || !(EndpointDescriptor->bmAttributes & USB_AUDIO_EP_SAMPLING_FREQ_CONTROL))
    {
        DPRINT1("UsbAudioSetFormat endpoint %x has no sampling frequency control\n", EndpointAddress);
        return STATUS_SUCCESS;
    }

    /* allocate sample rate buffer */
    SampleRateBuffer = AllocFunction(sizeof(ULONG));
    if (!SampleRateBuffer)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* tSampleFreq is a 3 byte little endian value */
    SampleRateBuffer[2] = (WaveFormatEx->WaveFormatEx.nSamplesPerSec >> 16) & 0xFF;
    SampleRateBuffer[1] = (WaveFormatEx->WaveFormatEx.nSamplesPerSec >> 8) & 0xFF;
    SampleRateBuffer[0] = (WaveFormatEx->WaveFormatEx.nSamplesPerSec >> 0) & 0xFF;

    /* allocate urb */
    Urb = AllocFunction(sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    if (!Urb)
    {
        /* no memory */
        FreeFunction(SampleRateBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UsbBuildVendorRequest(Urb,
        URB_FUNCTION_CLASS_ENDPOINT,
        sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
        USBD_TRANSFER_DIRECTION_OUT,
        0,
        USB_AUDIO_SET_CUR,
        USB_AUDIO_SAMPLING_FREQ_CONTROL << 8,
        EndpointAddress,
        SampleRateBuffer,
        NULL,
        3,
        NULL);

    /* submit urb */
    Status = SubmitUrbSync(FilterContext->LowerDevice, Urb);

    DPRINT1("USBAudioPinSetDataFormat Pin %p Status %x\n", Pin, Status);
    FreeFunction(Urb);
    FreeFunction(SampleRateBuffer);
    return Status;
}

NTSTATUS
USBAudioSelectAudioStreamingInterface(
    IN PKSPIN Pin,
    IN PPIN_CONTEXT PinContext,
    IN PDEVICE_EXTENSION DeviceExtension,
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN ULONG FormatDescriptorIndex)
{
    PURB Urb;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    NTSTATUS Status;
    ULONG Found, Index;

    PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR StreamingInterfaceDescriptor;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR TerminalDescriptor = NULL;

    /* search for terminal descriptor of that irp sink / irp source */
    TerminalDescriptor = UsbAudioGetStreamingTerminalDescriptorByIndex(DeviceExtension->ConfigurationDescriptor, Pin->Id);
    ASSERT(TerminalDescriptor != NULL);

    /* grab interface descriptor */
    InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    if (!InterfaceDescriptor)
    {
        /* no such interface */
        return STATUS_INVALID_PARAMETER;
    }

    Found = FALSE;
    Index = 0;

    /* selects the interface which has an audio streaming interface descriptor associated to the input / output terminal at the given format index */
    while (InterfaceDescriptor != NULL)
    {
        if (InterfaceDescriptor->bInterfaceSubClass == 0x02 /* AUDIO_STREAMING */ && InterfaceDescriptor->bNumEndpoints > 0)
        {
            StreamingInterfaceDescriptor = (PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, InterfaceDescriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (StreamingInterfaceDescriptor != NULL)
            {
                ASSERT(StreamingInterfaceDescriptor->bDescriptorSubtype == 0x01);
                ASSERT(StreamingInterfaceDescriptor->wFormatTag == WAVE_FORMAT_PCM);
                if (StreamingInterfaceDescriptor->bTerminalLink == TerminalDescriptor->bTerminalID)
                {
                    if (FormatDescriptorIndex == Index)
                    {
                        Found = TRUE;
                        break;
                    }
                    Index++;
                }
            }
        }
        InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)InterfaceDescriptor + InterfaceDescriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    }

    if (!Found)
    {
        /* no such interface */
        DPRINT1("No Interface found\n");
        return STATUS_INVALID_PARAMETER;
    }

    Urb = AllocFunction(GET_SELECT_INTERFACE_REQUEST_SIZE(InterfaceDescriptor->bNumEndpoints));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

     /* now prepare interface urb */
     UsbBuildSelectInterfaceRequest(Urb, GET_SELECT_INTERFACE_REQUEST_SIZE(InterfaceDescriptor->bNumEndpoints), DeviceExtension->ConfigurationHandle, InterfaceDescriptor->bInterfaceNumber, InterfaceDescriptor->bAlternateSetting);

     /* now select the interface */
     Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);

     DPRINT1("USBAudioSelectAudioStreamingInterface Status %x UrbStatus %x InterfaceNumber %x AlternateSetting %x\n", Status, Urb->UrbSelectInterface.Hdr.Status, InterfaceDescriptor->bInterfaceNumber, InterfaceDescriptor->bAlternateSetting);

     /* did it succeeed */
     if (NT_SUCCESS(Status))
     {
         /* free old interface info */
         if (PinContext->InterfaceInfo)
         {
             /* free old info */
             FreeFunction(PinContext->InterfaceInfo);
         }

         /* alloc interface info */
         PinContext->InterfaceInfo = AllocFunction(Urb->UrbSelectInterface.Interface.Length);
         if (PinContext->InterfaceInfo == NULL)
         {
             /* no memory */
             FreeFunction(Urb);
             return STATUS_INSUFFICIENT_RESOURCES;
         }

         /* copy interface info */
         RtlCopyMemory(PinContext->InterfaceInfo, &Urb->UrbSelectInterface.Interface, Urb->UrbSelectInterface.Interface.Length);
         PinContext->InterfaceDescriptor = InterfaceDescriptor;
    }

     /* free urb */
     FreeFunction(Urb);
     return Status;
}

VOID
NTAPI
CaptureGateOnWorkItem(
    _In_ PVOID Context)
{
    PKSPIN Pin;
    PPIN_CONTEXT PinContext;
    PKSGATE Gate;
    ULONG Count;

    /* get pin */
    Pin = Context;

    /* get pin context */
    PinContext = Pin->Context;

    do
    {
        /* acquire processing mutex */
        KsPinAcquireProcessingMutex(Pin);

        if (!PinContext->StopStreaming)
        {
            /* get pin control gate */
            Gate = KsPinGetAndGate(Pin);

            /* turn input on */
            KsGateTurnInputOn(Gate);

            /* schedule processing */
            KsPinAttemptProcessing(Pin, TRUE);
        }

        /* release processing mutex */
        KsPinReleaseProcessingMutex(Pin);

        /* decrement worker count */
        Count = KsDecrementCountedWorker(PinContext->CaptureWorker);
    } while (Count);
}

/* returns the length of the next render packet. A full speed audio endpoint is serviced once per
 * 1ms frame and every packet has to carry the samples of exactly one frame, e.g. 44.1kHz is sent as
 * nine packets of 44 samples followed by one of 45 samples (USB Audio 1.0, 3.7.2.1 and 3.7.2.2) */
static
ULONG
UsbAudioGetRenderPacketLength(
    IN PKSDATAFORMAT_WAVEFORMATEX WaveFormat,
    IN ULONG MaximumPacketSize,
    IN OUT PULONG SampleRemainder)
{
    ULONG Samples;
    ULONG MaximumSamples;

    *SampleRemainder += WaveFormat->WaveFormatEx.nSamplesPerSec;
    Samples = *SampleRemainder / USB_AUDIO_FRAMES_PER_SECOND;
    *SampleRemainder %= USB_AUDIO_FRAMES_PER_SECOND;

    /* never exceed wMaxPacketSize, the remaining samples are carried to the next packet */
    MaximumSamples = MaximumPacketSize / WaveFormat->WaveFormatEx.nBlockAlign;
    if (Samples > MaximumSamples)
    {
        *SampleRemainder += (Samples - MaximumSamples) * USB_AUDIO_FRAMES_PER_SECOND;
        Samples = MaximumSamples;
    }

    return Samples * WaveFormat->WaveFormatEx.nBlockAlign;
}

NTSTATUS
RenderInitializeUrbAndIrp(
    IN PKSPIN Pin,
    IN PPIN_CONTEXT PinContext,
    IN OUT PIRP Irp,
    IN PVOID TransferBuffer,
    IN ULONG TransferBufferSize,
    IN ULONG MaximumPacketSize,
    IN ULONG PacketCount)
{
    ULONG Index;
    ULONG Offset;
    ULONG UnusedSamples;
    PURB Urb;
    PIO_STACK_LOCATION IoStack;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;

    /* initialize irp */
    IoInitializeIrp(Irp, IoSizeOfIrp(PinContext->DeviceExtension->LowerDevice->StackSize), PinContext->DeviceExtension->LowerDevice->StackSize);

    /* set irp members */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    Irp->Flags = 0;
    Irp->UserBuffer = NULL;

    /* the transfer buffer belongs to the irp, keep it across IoInitializeIrp */
    Irp->Tail.Overlay.DriverContext[2] = TransferBuffer;

    /* init stack location */
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->DeviceObject = PinContext->DeviceExtension->LowerDevice;
    IoStack->Parameters.Others.Argument2 = NULL;
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

    /* set completion routine */
    IoSetCompletionRoutine(Irp, UsbAudioRenderComplete, Pin, TRUE, TRUE, TRUE);

    /* lets allocate urb */
    Urb = (PURB)AllocFunction(GET_ISO_URB_SIZE(PacketCount));
    if (!Urb)
    {
        /* no memory */
        DPRINT1("No Memory PacketCount %u\n", PacketCount);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init urb */
    Urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    Urb->UrbIsochronousTransfer.Hdr.Length = GET_ISO_URB_SIZE(PacketCount);
    Urb->UrbIsochronousTransfer.PipeHandle = PinContext->InterfaceInfo->Pipes[0].PipeHandle;
    Urb->UrbIsochronousTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_OUT | USBD_START_ISO_TRANSFER_ASAP;
    Urb->UrbIsochronousTransfer.TransferBufferLength = TransferBufferSize;
    Urb->UrbIsochronousTransfer.TransferBuffer = TransferBuffer;
    Urb->UrbIsochronousTransfer.NumberOfPackets = PacketCount;
    Urb->UrbIsochronousTransfer.StartFrame = 0;

    /* the length of an OUT packet is given by the offset of the next packet */
    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)Pin->ConnectionFormat;
    Offset = 0;

    for (Index = 0; Index < PacketCount; Index++)
    {
        ASSERT(Offset < TransferBufferSize);
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = Offset;
        Offset += UsbAudioGetRenderPacketLength(WaveFormat, MaximumPacketSize, &PinContext->RenderSampleRemainder);

    }

    if (Offset > TransferBufferSize)
    {
        /* the last packet is short, carry the samples it did not get over to the next transfer */
        UnusedSamples = (Offset - TransferBufferSize) / WaveFormat->WaveFormatEx.nBlockAlign;
        PinContext->RenderSampleRemainder += UnusedSamples * USB_AUDIO_FRAMES_PER_SECOND;
    }
    else if (Offset < TransferBufferSize)
    {
        /* the transfer is limited by the maximum number of iso packets */
        Urb->UrbIsochronousTransfer.TransferBufferLength = Offset;
    }

    /* store urb */
    IoStack->Parameters.Others.Argument1 = Urb;
    Irp->Tail.Overlay.DriverContext[0] = Urb;


    /* done */
    return STATUS_SUCCESS;
}

/* reports the data rates of the stream which ran dry. The client delivers too slow if the
 * received rate is below the format rate, transfers complete too early if the sent rate is above */
static
VOID
UsbAudioRenderReportUnderrun(
    IN PKSPIN Pin,
    IN PPIN_CONTEXT PinContext)
{
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;
    ULONGLONG Elapsed;

    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)Pin->ConnectionFormat;

    /* interrupt time is in 100ns units */
    Elapsed = (KeQueryInterruptTime() - PinContext->RenderRunStartTime) / 10000;
    if (Elapsed == 0)
        Elapsed = 1;

    /* a USB frame lasts 1ms, both clocks have to advance at the same rate */
    DPRINT1("UsbAudioRenderPump underrun after %I64u ms / %u USB frames: received %I64u bytes (%I64u B/s) sent %I64u bytes (%I64u B/s) format %u B/s\n",
            Elapsed,
            PinContext->RenderRunEndFrame - PinContext->RenderRunStartFrame,
            PinContext->RenderBytesIn, PinContext->RenderBytesIn * 1000 / Elapsed,
            PinContext->RenderBytesOut, PinContext->RenderBytesOut * 1000 / Elapsed,
            WaveFormat->WaveFormatEx.nAvgBytesPerSec);
}

static
VOID
UsbAudioRenderPump(
    IN PKSPIN Pin);

/* starts streaming of a sound which is shorter than the prebuffer */
static
VOID
NTAPI
UsbAudioRenderStartDpc(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    PKSPIN Pin = DeferredContext;
    PPIN_CONTEXT PinContext = Pin->Context;

    KeAcquireSpinLockAtDpcLevel(&PinContext->IrpListLock);
    PinContext->RenderStartTimerArmed = FALSE;
    if (!PinContext->RenderStarted)
        PinContext->RenderForceStart = TRUE;
    KeReleaseSpinLockFromDpcLevel(&PinContext->IrpListLock);

    UsbAudioRenderPump(Pin);
}

/* sends the audio data of the render fifo. Every transfer consists of whole packets, so the
 * device receives the samples of exactly one frame in every packet, independent of the size
 * of the stream frames the client sends */
static
VOID
UsbAudioRenderSubmit(
    IN PKSPIN Pin)
{
    PPIN_CONTEXT PinContext;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;
    ULONG MaximumPacketSize;
    ULONG SampleRemainder;
    ULONG PacketCount;
    ULONG PacketLength;
    ULONG Length;
    ULONG Copy;
    PUCHAR TransferBuffer;
    PLIST_ENTRY Entry;
    PIRP Irp;
    KIRQL OldLevel;
    NTSTATUS Status;
    LONG Pending;

    PinContext = Pin->Context;
    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)Pin->ConnectionFormat;
    MaximumPacketSize = PinContext->InterfaceInfo->Pipes[0].MaximumPacketSize;

    while (TRUE)
    {
        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

        if (PinContext->StopStreaming || IsListEmpty(&PinContext->IrpListHead))
        {
            /* no irp available */
            KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
            break;
        }

        /* calculate the size of the next transfer from the packets available in the fifo */
        SampleRemainder = PinContext->RenderSampleRemainder;
        Length = 0;
        for (PacketCount = 0; PacketCount < PinContext->RenderPacketsPerUrb; PacketCount++)
        {
            PacketLength = UsbAudioGetRenderPacketLength(WaveFormat, MaximumPacketSize, &SampleRemainder);
            if (Length + PacketLength > PinContext->RenderUrbBufferSize ||
                Length + PacketLength > PinContext->BufferLength)
                break;

            Length += PacketLength;
        }

        Pending = RENDER_IRP_COUNT - PinContext->StoppedStreamingIrps;

        if (PinContext->RenderStarted && Pending == 0 && Length == 0)
        {
            /* all transfers completed and no packet is left, the device ran dry */
            UsbAudioRenderReportUnderrun(Pin, PinContext);
            PinContext->RenderStarted = FALSE;
            PinContext->RenderForceStart = FALSE;
        }

        if (!PinContext->RenderStarted)
        {
            if (PinContext->BufferLength < PinContext->RenderPrebufferSize && !PinContext->RenderForceStart)
            {
                /* fill the prebuffer first, the timer starts the stream of a short sound */
                if (PinContext->BufferLength && !PinContext->RenderStartTimerArmed)
                {
                    LARGE_INTEGER DueTime;

                    DueTime.QuadPart = -10000LL * RENDER_START_TIMEOUT_MS;
                    PinContext->RenderStartTimerArmed = TRUE;
                    KeSetTimer(&PinContext->RenderStartTimer, DueTime, &PinContext->RenderStartDpc);
                }
                KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
                break;
            }

            /* start streaming */
            PinContext->RenderStarted = TRUE;
            PinContext->RenderForceStart = FALSE;
            PinContext->RenderRunStartTime = KeQueryInterruptTime();
            PinContext->RenderRunStartFrame = 0;
            PinContext->RenderBytesIn = PinContext->BufferLength;
            PinContext->RenderBytesOut = 0;
        }

        if (Length == 0 || (PacketCount < PinContext->RenderPacketsPerUrb && Pending >= RENDER_LOW_WATER_IRPS))
        {
            /* wait for more audio data, unless the device is about to run dry */
            KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
            break;
        }
        PinContext->RenderBytesOut += Length;

        Entry = RemoveHeadList(&PinContext->IrpListHead);
        Irp = (PIRP)CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        InterlockedDecrement(&PinContext->StoppedStreamingIrps);

        /* copy the audio data out of the fifo, it may wrap */
        TransferBuffer = Irp->Tail.Overlay.DriverContext[2];
        Copy = min(Length, PinContext->BufferSize - PinContext->BufferOffset);
        RtlCopyMemory(TransferBuffer, &PinContext->Buffer[PinContext->BufferOffset], Copy);
        RtlCopyMemory(&TransferBuffer[Copy], PinContext->Buffer, Length - Copy);

        PinContext->BufferOffset = (PinContext->BufferOffset + Length) % PinContext->BufferSize;
        PinContext->BufferLength -= Length;

        /* packet lengths are committed with the fifo update, so they always match the data */
        Status = RenderInitializeUrbAndIrp(Pin, PinContext, Irp, TransferBuffer, Length, MaximumPacketSize, PacketCount);
        if (!NT_SUCCESS(Status))
        {
            /* the audio data is lost, keep the irp */
            InsertHeadList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);
            InterlockedIncrement(&PinContext->StoppedStreamingIrps);
            KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
            break;
        }

        Irp->Tail.Overlay.DriverContext[1] = NULL;

        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        /* space was freed in the fifo, continue with the stream frame which did not fit */
        if (InterlockedExchange(&PinContext->RenderWaitingForSpace, FALSE))
        {
            KsPinAttemptProcessing(Pin, TRUE);
        }

        DPRINT("UsbAudioRenderPump sending Irp %p PacketCount %u Length %u\n", Irp, PacketCount, Length);
        IoCallDriver(PinContext->LowerDevice, Irp);
    }
}

/* the pump runs from the pin worker, the completion routine and the start timer. Only one
 * instance submits at a time, so the transfers reach usbport in the order of the audio data.
 * A caller finding the pump active lets that instance run another round */
static
VOID
UsbAudioRenderPump(
    IN PKSPIN Pin)
{
    PPIN_CONTEXT PinContext = Pin->Context;

    if (InterlockedIncrement(&PinContext->RenderPumpRequests) != 1)
        return;

    do
    {
        UsbAudioRenderSubmit(Pin);
    } while (InterlockedDecrement(&PinContext->RenderPumpRequests) != 0);
}

VOID
CaptureInitializeUrbAndIrp(
    IN PKSPIN Pin,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PURB Urb;
    PUCHAR TransferBuffer;
    ULONG Index;
    PPIN_CONTEXT PinContext;

    /* get pin context */
    PinContext = Pin->Context;

    /* backup urb and transferbuffer */
    Urb = Irp->Tail.Overlay.DriverContext[0];
    TransferBuffer = Urb->UrbIsochronousTransfer.TransferBuffer;
    ASSERT(TransferBuffer);

    /* initialize irp */
    IoInitializeIrp(Irp, IoSizeOfIrp(PinContext->DeviceExtension->LowerDevice->StackSize), PinContext->DeviceExtension->LowerDevice->StackSize);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    Irp->Flags = 0;
    Irp->UserBuffer = NULL;
    Irp->Tail.Overlay.DriverContext[0] = Urb;
    Irp->Tail.Overlay.DriverContext[1] = NULL;

    /* init stack location */
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->DeviceObject = PinContext->DeviceExtension->LowerDevice;
    IoStack->Parameters.Others.Argument1 = Urb;
    IoStack->Parameters.Others.Argument2 = NULL;
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

    IoSetCompletionRoutine(Irp, UsbAudioCaptureComplete, Pin, TRUE, TRUE, TRUE);

    RtlZeroMemory(Urb, GET_ISO_URB_SIZE(PACKET_COUNT));

    /* init urb */
    Urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    Urb->UrbIsochronousTransfer.Hdr.Length = GET_ISO_URB_SIZE(PACKET_COUNT);
    Urb->UrbIsochronousTransfer.PipeHandle = PinContext->InterfaceInfo->Pipes[0].PipeHandle;
    Urb->UrbIsochronousTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_START_ISO_TRANSFER_ASAP;
    Urb->UrbIsochronousTransfer.TransferBufferLength = PinContext->MaximumPacketSize * PACKET_COUNT;
    Urb->UrbIsochronousTransfer.TransferBuffer = TransferBuffer;
    Urb->UrbIsochronousTransfer.NumberOfPackets = PACKET_COUNT;
    Urb->UrbIsochronousTransfer.StartFrame = 0;

    for (Index = 0; Index < PACKET_COUNT; Index++)
    {
        Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = Index * PinContext->MaximumPacketSize;
    }
}


VOID
NTAPI
CaptureAvoidPipeStarvationWorker(
    _In_ PVOID Context)
{
    PKSPIN Pin;
    PPIN_CONTEXT PinContext;
    KIRQL OldLevel;
    PLIST_ENTRY CurEntry;
    PIRP Irp;

    /* get pin */
    Pin = Context;

    /* get pin context */
    PinContext = Pin->Context;

    /* acquire spin lock */
    KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    if (!IsListEmpty(&PinContext->IrpListHead))
    {
        /* remove entry from list */
        CurEntry = RemoveHeadList(&PinContext->IrpListHead);

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        /* get irp offset */
        Irp = (PIRP)CONTAINING_RECORD(CurEntry, IRP, Tail.Overlay.ListEntry);

        /* reinitialize irp and urb */
        CaptureInitializeUrbAndIrp(Pin, Irp);

        KsDecrementCountedWorker(PinContext->StarvationWorker);

        /* call driver */
        IoCallDriver(PinContext->DeviceExtension->LowerDevice, Irp);
    }
    else
    {
        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        KsDecrementCountedWorker(PinContext->StarvationWorker);
    }
}



NTSTATUS
InitCapturePin(
    IN PKSPIN Pin)
{
    NTSTATUS Status;
    ULONG Index;
    ULONG BufferSize;
    ULONG MaximumPacketSize;
    PIRP Irp;
    PURB Urb;
    PPIN_CONTEXT PinContext;
    PIO_STACK_LOCATION IoStack;
    PKSALLOCATOR_FRAMING_EX Framing;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;
    PKSGATE Gate;

    /* set sample rate */
    Status = UsbAudioSetFormat(Pin, Pin->ConnectionFormat);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        return Status;
    }

    /* get pin context */
    PinContext = Pin->Context;

    /* lets get maximum packet size */
    MaximumPacketSize = PinContext->MaximumPacketSize = PinContext->InterfaceInfo->Pipes[0].MaximumPacketSize;

    /* compute data format length */
    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)Pin->ConnectionFormat;
    /* FIXME support non PCM formats */
    ASSERT(IsEqualGUIDAligned(&WaveFormat->DataFormat.MajorFormat, &KSDATAFORMAT_TYPE_AUDIO));
    ASSERT(IsEqualGUIDAligned(&WaveFormat->DataFormat.SubFormat, &KSDATAFORMAT_SUBTYPE_PCM));
    ASSERT(IsEqualGUIDAligned(&WaveFormat->DataFormat.Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX));
    PinContext->DataPacketLength = WaveFormat->WaveFormatEx.wBitsPerSample * WaveFormat->WaveFormatEx.nChannels * WaveFormat->WaveFormatEx.nBlockAlign;

    /* initialize work item for capture worker */
    ExInitializeWorkItem(&PinContext->CaptureWorkItem, CaptureGateOnWorkItem, (PVOID)Pin);

    /* initialize completion event */
    KeInitializeEvent(&PinContext->StoppedStreamingEvent, NotificationEvent, FALSE);

    /* register worker */
    Status = KsRegisterCountedWorker(CriticalWorkQueue, &PinContext->CaptureWorkItem, &PinContext->CaptureWorker);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        return Status;
    }

    /* initialize work item */
    ExInitializeWorkItem(&PinContext->StarvationWorkItem, CaptureAvoidPipeStarvationWorker, (PVOID)Pin);

    /* register worker */
    Status = KsRegisterCountedWorker(CriticalWorkQueue, &PinContext->StarvationWorkItem, &PinContext->StarvationWorker);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        KsUnregisterWorker(PinContext->CaptureWorker);
    }
    DPRINT1("InitCapturePin MaximumPacketSize %u\n", MaximumPacketSize);

    /* lets edit framing struct */
    Framing = (PKSALLOCATOR_FRAMING_EX)Pin->Descriptor->AllocatorFraming;
    Framing->FramingItem[0].PhysicalRange.MinFrameSize =
        Framing->FramingItem[0].PhysicalRange.MaxFrameSize =
        Framing->FramingItem[0].FramingRange.Range.MinFrameSize =
        Framing->FramingItem[0].FramingRange.Range.MaxFrameSize =
    MaximumPacketSize;

    /* calculate buffer size 8 irps * 64 iso packets * max packet size */
    BufferSize = IRP_CAPTURE_COUNT * PACKET_COUNT * MaximumPacketSize;

    /* allocate pin capture buffer */
    PinContext->BufferSize = BufferSize;
    PinContext->Buffer = AllocFunction(BufferSize);
    if (!PinContext->Buffer)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KsAddItemToObjectBag(Pin->Bag, PinContext->Buffer, ExFreePool);

    /* allocate pin capture buffer */
    PinContext->CaptureBufferLength = BufferSize * 3;
    PinContext->CaptureBuffer = AllocFunction(PinContext->CaptureBufferLength);
    if (!PinContext->CaptureBuffer)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KsAddItemToObjectBag(Pin->Bag, PinContext->CaptureBuffer, ExFreePool);

    /* init irps */
    for (Index = 0; Index < IRP_CAPTURE_COUNT; Index++)
    {
        /* allocate irp */
        Irp = AllocFunction(IoSizeOfIrp(PinContext->DeviceExtension->LowerDevice->StackSize));
        if (!Irp)
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* initialize irp */
        IoInitializeIrp(Irp, IoSizeOfIrp(PinContext->DeviceExtension->LowerDevice->StackSize), PinContext->DeviceExtension->LowerDevice->StackSize);

        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        Irp->Flags = 0;
        Irp->UserBuffer = NULL;

        IoStack = IoGetNextIrpStackLocation(Irp);
        IoStack->DeviceObject = PinContext->DeviceExtension->LowerDevice;
        IoStack->Parameters.Others.Argument2 = NULL;
        IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

        IoSetCompletionRoutine(Irp, UsbAudioCaptureComplete, Pin, TRUE, TRUE, TRUE);

        /* insert into irp list */
        InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);

        /* add to object bag*/
        KsAddItemToObjectBag(Pin->Bag, Irp, ExFreePool);

        /* alloc urb */
        Status = UsbAudioAllocCaptureUrbIso(PinContext->DeviceExtension->InterfaceInfo->Pipes[0].PipeHandle,
                                            MaximumPacketSize,
                                            &PinContext->Buffer[MaximumPacketSize * PACKET_COUNT * Index],
                                            MaximumPacketSize * PACKET_COUNT,
                                            &Urb);

        DPRINT1("InitCapturePin Irp %p Urb %p\n", Irp, Urb);

        if (NT_SUCCESS(Status))
        {
            /* get next stack location */
            IoStack = IoGetNextIrpStackLocation(Irp);

            /* store urb */
            IoStack->Parameters.Others.Argument1 = Urb;
            Irp->Tail.Overlay.DriverContext[0] = Urb;
        }
        else
        {
            /* failed */
            return Status;
        }
    }

    /* get process control gate */
    Gate = KsPinGetAndGate(Pin);

    /* turn input off */
    KsGateTurnInputOff(Gate);

    return Status;
}

NTSTATUS
InitStreamPin(
    IN PKSPIN Pin)
{
    ULONG Index;
    PIRP Irp;
    PPIN_CONTEXT PinContext;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormatEx;
    PIO_STACK_LOCATION IoStack;
    ULONG MaximumPacketLength;
    PVOID TransferBuffer;

    DPRINT1("InitStreamPin\n");

    /* get pin context */
    PinContext = Pin->Context;

    /* allocate 1 sec buffer */
    WaveFormatEx = (PKSDATAFORMAT_WAVEFORMATEX)Pin->ConnectionFormat;
    PinContext->Buffer = AllocFunction(WaveFormatEx->WaveFormatEx.nAvgBytesPerSec);
    if (!PinContext->Buffer)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* the 1 sec buffer is the render fifo, BufferOffset is the read position, BufferLength the filled bytes */
    PinContext->BufferSize = WaveFormatEx->WaveFormatEx.nAvgBytesPerSec;
    PinContext->BufferOffset = 0;
    PinContext->BufferLength = 0;
    PinContext->RenderSampleRemainder = 0;
    PinContext->RenderWaitingForSpace = FALSE;
    PinContext->RenderStarted = FALSE;
    PinContext->RenderForceStart = FALSE;
    PinContext->RenderStartTimerArmed = FALSE;
    PinContext->RenderPumpRequests = 0;
    PinContext->RenderPrebufferSize = WaveFormatEx->WaveFormatEx.nAvgBytesPerSec / 1000 * RENDER_PREBUFFER_MS;
    PinContext->RenderPrebufferSize -= PinContext->RenderPrebufferSize % WaveFormatEx->WaveFormatEx.nBlockAlign;
    KeInitializeTimer(&PinContext->RenderStartTimer);
    KeInitializeDpc(&PinContext->RenderStartDpc, UsbAudioRenderStartDpc, Pin);
    KsAddItemToObjectBag(Pin->Bag, PinContext->Buffer, ExFreePool);

    /* every render irp has its own page as transfer buffer. A page is physically contiguous,
     * so no packet spans two pages which the host controller would have to bounce.
     * A packet carries at most one sample more than nSamplesPerSec / 1000 */
    MaximumPacketLength = (WaveFormatEx->WaveFormatEx.nSamplesPerSec / USB_AUDIO_FRAMES_PER_SECOND + 1) *
                          WaveFormatEx->WaveFormatEx.nBlockAlign;
    if (MaximumPacketLength == 0 || MaximumPacketLength > PAGE_SIZE)
    {
        DPRINT1("InitStreamPin invalid packet length %u\n", MaximumPacketLength);
        return STATUS_INVALID_PARAMETER;
    }

    PinContext->RenderUrbBufferSize = PAGE_SIZE;
    PinContext->RenderPacketsPerUrb = min(RENDER_PACKETS_PER_URB, PAGE_SIZE / MaximumPacketLength);

    /* initialize completion event */
    KeInitializeEvent(&PinContext->StoppedStreamingEvent, SynchronizationEvent, FALSE);

    /* init irp count */
    PinContext->StoppedStreamingIrps = RENDER_IRP_COUNT;

    /* init irps */
    for (Index = 0; Index < RENDER_IRP_COUNT; Index++)
    {
        /* allocate irp */
        Irp = AllocFunction(IoSizeOfIrp(PinContext->DeviceExtension->LowerDevice->StackSize));
        if (!Irp)
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DPRINT1("InitStreamPin Irp %p\n", Irp);

        /* initialize irp */
        IoInitializeIrp(Irp, IoSizeOfIrp(PinContext->DeviceExtension->LowerDevice->StackSize), PinContext->DeviceExtension->LowerDevice->StackSize);

        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        Irp->Flags = 0;
        Irp->UserBuffer = NULL;

        IoStack = IoGetNextIrpStackLocation(Irp);
        IoStack->DeviceObject = PinContext->DeviceExtension->LowerDevice;
        IoStack->Parameters.Others.Argument2 = NULL;
        IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

        IoSetCompletionRoutine(Irp, UsbAudioRenderComplete, Pin, TRUE, TRUE, TRUE);

        /* add to object bag*/
        KsAddItemToObjectBag(Pin->Bag, Irp, ExFreePool);

        /* assign the transfer buffer of the irp, a page sized allocation is page aligned */
        TransferBuffer = AllocFunction(PinContext->RenderUrbBufferSize);
        if (!TransferBuffer)
        {
            /* no memory */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        KsAddItemToObjectBag(Pin->Bag, TransferBuffer, ExFreePool);
        Irp->Tail.Overlay.DriverContext[2] = TransferBuffer;

        /* insert into irp list */
        InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);
    }

    return STATUS_SUCCESS;
}

ULONG
GetDataRangeIndexForFormat(
    IN const KSDATARANGE * ConnectionFormat,
    IN const PKSDATARANGE * DataRanges,
    IN ULONG DataRangesCount)
{
    ULONG Index;
    PKSDATARANGE CurrentDataRange;
    PKSDATARANGE_AUDIO CurrentAudioDataRange;
    PKSDATAFORMAT_WAVEFORMATEX ConnectionDataFormat;

    if (ConnectionFormat->FormatSize != sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX))
    {
        /* unsupported connection format */
        DPRINT1("GetDataRangeIndexForFormat expected KSDATARANGE_AUDIO\n");
        return MAXULONG;
    }

    /* cast to right type */
    ConnectionDataFormat = (PKSDATAFORMAT_WAVEFORMATEX)ConnectionFormat;

    for (Index = 0; Index < DataRangesCount; Index++)
    {
         /* get current data range */
         CurrentDataRange = DataRanges[Index];

         /* compare guids */
         if (!IsEqualGUIDAligned(&CurrentDataRange->MajorFormat, &ConnectionFormat->MajorFormat) ||
             !IsEqualGUIDAligned(&CurrentDataRange->SubFormat, &ConnectionFormat->SubFormat) ||
             !IsEqualGUIDAligned(&CurrentDataRange->Specifier, &ConnectionFormat->Specifier))
         {
             /* no match */
             DPRINT1("No GUID Match\n");
             continue;
         }

         /* all pin data ranges are KSDATARANGE_AUDIO */
         CurrentAudioDataRange = (PKSDATARANGE_AUDIO)CurrentDataRange;

         /* check if number of channel match */
         if (CurrentAudioDataRange->MaximumChannels != ConnectionDataFormat->WaveFormatEx.nChannels)
         {
             /* number of channels mismatch */
             DPRINT1("NoChannelMatch %u\n", ConnectionDataFormat->WaveFormatEx.nChannels);
             continue;
         }

         if (CurrentAudioDataRange->MinimumSampleFrequency > ConnectionDataFormat->WaveFormatEx.nSamplesPerSec)
         {
             /* channel frequency too low */
             DPRINT1("Minimum Sample freq too low %u\n", ConnectionDataFormat->WaveFormatEx.nSamplesPerSec);
             continue;
         }

         if (CurrentAudioDataRange->MaximumSampleFrequency < ConnectionDataFormat->WaveFormatEx.nSamplesPerSec)
         {
             /* channel frequency too high */
             DPRINT1("MaxFrequency too high %u\n", ConnectionDataFormat->WaveFormatEx.nSamplesPerSec);
             continue;
         }

         if (CurrentAudioDataRange->MinimumBitsPerSample > ConnectionDataFormat->WaveFormatEx.wBitsPerSample ||
             CurrentAudioDataRange->MaximumBitsPerSample < ConnectionDataFormat->WaveFormatEx.wBitsPerSample)
         {
             /* sample size mismatch */
             DPRINT1("NoBitsPerSampleMatch %u\n", ConnectionDataFormat->WaveFormatEx.wBitsPerSample);
             continue;
         }

         return Index;
    }

    /* no datarange found */
    return MAXULONG;
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
    NTSTATUS Status;
    ULONG FormatIndex;

    Filter = KsPinGetParentFilter(Pin);
    if (Filter == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    /* get filter context */
    FilterContext = Filter->Context;

    if (!Pin->Context)
    {
        /* allocate pin context */
        PinContext = AllocFunction(sizeof(PIN_CONTEXT));
        if (!PinContext)
        {
            /* no memory*/
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* store pin context*/
        Pin->Context = PinContext;
    }
    else
    {
        PinContext = (PPIN_CONTEXT)Pin->Context;
    }


    /* init pin context */
    PinContext->DeviceExtension = FilterContext->DeviceExtension;
    PinContext->LowerDevice = FilterContext->LowerDevice;
    InitializeListHead(&PinContext->IrpListHead);
    InitializeListHead(&PinContext->DoneIrpListHead);
    KeInitializeSpinLock(&PinContext->IrpListLock);

    /* lets edit allocator framing struct */
    Status = _KsEdit(Pin->Bag, (PVOID*)&Pin->Descriptor, sizeof(KSPIN_DESCRIPTOR_EX), sizeof(KSPIN_DESCRIPTOR_EX), USBAUDIO_TAG);
    if (NT_SUCCESS(Status))
    {
        Status = _KsEdit(Pin->Bag, (PVOID*)&Pin->Descriptor->AllocatorFraming, sizeof(KSALLOCATOR_FRAMING_EX), sizeof(KSALLOCATOR_FRAMING_EX), USBAUDIO_TAG);
        ASSERT(Status == STATUS_SUCCESS);
    }

    /* choose correct dataformat */
    FormatIndex = GetDataRangeIndexForFormat(Pin->ConnectionFormat, Pin->Descriptor->PinDescriptor.DataRanges, Pin->Descriptor->PinDescriptor.DataRangesCount);
    if (FormatIndex == MAXULONG)
    {
        /* no format match */
        return STATUS_NO_MATCH;
    }

    if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
    {
        /* init capture pin */
        Status = InitCapturePin(Pin);
    }
    else
    {
        /* audio streaming pin*/
        Status = InitStreamPin(Pin);
    }

    return Status;
}

NTSTATUS
NTAPI
USBAudioPinClose(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PPIN_CONTEXT PinContext;
    PURB Urb;
    NTSTATUS Status;
    //ULONG InterfaceIndex;

    DPRINT1("USBAudioPinClose entered\n");

    /* get pin context */
    PinContext = Pin->Context;

    if (!PinContext->StopStreaming)
    {
        /* set stop streaming flag */
        PinContext->StopStreaming = TRUE;

        if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
        {
           if (PinContext->StoppedStreamingIrps != IRP_CAPTURE_COUNT)
           {
                /* wait for completion */
                KeWaitForSingleObject(&PinContext->StoppedStreamingEvent, Executive, KernelMode, FALSE, NULL);
           }
        }
        else
        {
           if (PinContext->StoppedStreamingIrps != RENDER_IRP_COUNT)
           {
                /* wait for completion */
                KeWaitForSingleObject(&PinContext->StoppedStreamingEvent, Executive, KernelMode, FALSE, NULL);
           }
        }
    }

    if (Pin->DataFlow == KSPIN_DATAFLOW_IN)
    {
        /* the start timer must not fire after the pin is gone */
        KeCancelTimer(&PinContext->RenderStartTimer);
        KeFlushQueuedDpcs();
    }

    /* now abort pipe*/
    Urb = AllocFunction(sizeof(URB));
    if (!Urb)
    {
        /* no memory */
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Urb->UrbHeader.Length   = sizeof(struct _URB_PIPE_REQUEST);
    Urb->UrbHeader.Function = URB_FUNCTION_ABORT_PIPE;
    Urb->UrbPipeRequest.PipeHandle = PinContext->InterfaceInfo->Pipes[0].PipeHandle;
    Status = SubmitUrbSync(PinContext->DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        DPRINT1("USBVideoPinClose Status %x\n", Status);
        FreeFunction(Urb);
        Irp->IoStatus.Status = Status;
        return Status;
    }
#if 0
    Status = USBAudioGetInterfaceDescriptorForStreaming(Pin, &InterfaceIndex);
    ASSERT(NT_SUCCESS(Status));
    UsbBuildSelectInterfaceRequest(Urb,
        GET_SELECT_INTERFACE_REQUEST_SIZE(1),
        PinContext->DeviceExtension->ConfigurationHandle,
        InterfaceIndex,
        0);

    Status = SubmitUrbSync(PinContext->DeviceExtension->LowerDevice, Urb);
    FreeFunction(Urb);
 #endif
    DPRINT1("USBAudioPinClose Status %x\n", Status);
    return Status;
}

/* reports render transfers which were not played completely. The start frame assigned by
 * usbport has to continue the previous transfer while the stream runs */
static
VOID
UsbAudioRenderCheckUrb(
    IN PPIN_CONTEXT PinContext,
    IN PURB Urb)
{
    ULONG Index;
    ULONG Late = 0;
    ULONG Failed = 0;
    ULONG FailedStatus = 0;
    ULONG StartFrame;
    ULONG NumberOfPackets;
    USBD_STATUS PacketStatus;

    StartFrame = Urb->UrbIsochronousTransfer.StartFrame;
    NumberOfPackets = Urb->UrbIsochronousTransfer.NumberOfPackets;

    for (Index = 0; Index < NumberOfPackets; Index++)
    {
        PacketStatus = Urb->UrbIsochronousTransfer.IsoPacket[Index].Status;
        if (PacketStatus == USBD_STATUS_ISO_NOT_ACCESSED_LATE ||
            PacketStatus == USBD_STATUS_NOT_ACCESSED)
        {
            Late++;
        }
        else if (!USBD_SUCCESS(PacketStatus))
        {
            Failed++;
            FailedStatus = PacketStatus;
        }
    }

    /* the first completed transfer of a run gives the start of the USB clock measurement */
    if (!PinContext->RenderRunStartFrame)
    {
        PinContext->RenderRunStartFrame = StartFrame;
        PinContext->RenderRunEndFrame = StartFrame;
    }

    /* transfers may complete out of order, track the end of the latest one */
    if ((LONG)(StartFrame + NumberOfPackets - PinContext->RenderRunEndFrame) > 0)
        PinContext->RenderRunEndFrame = StartFrame + NumberOfPackets;

    if (!USBD_SUCCESS(Urb->UrbHeader.Status) || Late || Failed)
    {
        DPRINT1("UsbAudioRenderComplete Status %x StartFrame %x Expected %x Packets %u Late %u Failed %u (%x) Pending %u\n",
                Urb->UrbHeader.Status, StartFrame, PinContext->RenderNextFrame, NumberOfPackets,
                Late, Failed, FailedStatus, RENDER_IRP_COUNT - PinContext->StoppedStreamingIrps);
    }

    /* transfers complete in order, the next one starts behind this one */
    PinContext->RenderNextFrame = StartFrame + NumberOfPackets;
}

NTSTATUS
NTAPI
UsbAudioRenderComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    PKSPIN Pin;
    PPIN_CONTEXT PinContext;
    KIRQL OldLevel;
    NTSTATUS Status;
    PURB Urb;
    BOOLEAN StopStreaming;

    DPRINT("UsbAudioRenderComplete Irp %p\n", Irp);

    /* get pin context */
    Pin = Context;
    PinContext = Pin->Context;

    /* get status */
    Status = Irp->IoStatus.Status;
    if (!NT_SUCCESS(Status))
    {
        /* the stream frames were already completed when their data was copied to the fifo */
        DPRINT1("UsbAudioRenderComplete failed with %x\n", Status);
    }

    /* get urb */
    Urb = Irp->Tail.Overlay.DriverContext[0];

    /* acquire lock */
    KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    UsbAudioRenderCheckUrb(PinContext, Urb);

    /* and free it */
    FreeFunction(Urb);

    /* insert entry into ready list */
    InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);
    InterlockedIncrement(&PinContext->StoppedStreamingIrps);
    StopStreaming = PinContext->StopStreaming && PinContext->StoppedStreamingIrps == RENDER_IRP_COUNT;

    /* release lock */
    KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

    if (StopStreaming)
    {
        /* the last pending transfer completed, the pin may be closed */
        KeSetEvent(&PinContext->StoppedStreamingEvent, IO_NO_INCREMENT, FALSE);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /* keep the device fed with the data in the fifo */
    UsbAudioRenderPump(Pin);

    /* done */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
UsbAudioCaptureComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    PKSPIN Pin;
    PPIN_CONTEXT PinContext;
    KIRQL OldLevel;
    PURB Urb;
    ULONG PacketIndex, PacketLength, PacketOffset;
    BOOLEAN InComplete = FALSE;
    /* get pin context */
    Pin = Context;
    PinContext = Pin->Context;

    /* get urb */
    Urb = Irp->Tail.Overlay.DriverContext[0];

    /* acquire lock */
    KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    if (!NT_SUCCESS(Urb->UrbIsochronousTransfer.Hdr.Status))
    {
        DPRINT1("UsbAudioCaptureComplete Irp %p Urb %p Status %x Packet Status %x\n", Irp, Urb, Urb->UrbIsochronousTransfer.Hdr.Status, Urb->UrbIsochronousTransfer.IsoPacket[0].Status);

        /* insert entry into ready list */
        InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        KsIncrementCountedWorker(PinContext->StarvationWorker);
    }
    else
    {
        DPRINT("UsbAudioCaptureComplete Irp %p\n", Irp);

        for(PacketIndex = 0; PacketIndex < Urb->UrbIsochronousTransfer.NumberOfPackets; PacketIndex++)
        {
            PacketOffset = Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex].Offset;
            PacketLength = Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex].Length;
            PacketLength = min(PacketLength, PinContext->CaptureBufferLength - PinContext->CaptureBufferOffset);
            PacketLength = min(PacketLength, PinContext->DataPacketLength);
            RtlCopyMemory(&PinContext->CaptureBuffer[PinContext->CaptureBufferOffset],
                &((PUCHAR)Urb->UrbIsochronousTransfer.TransferBuffer)[PacketOffset],
                PacketLength);
            PinContext->CaptureBufferOffset += PacketLength;
            if (PinContext->CaptureBufferOffset == PinContext->CaptureBufferLength)
            {
                InComplete = PacketIndex + 1 < Urb->UrbIsochronousTransfer.NumberOfPackets;
                break;
            }
        }

        if (InComplete)
        {
            /* insert entry into done list */
            Irp->Tail.Overlay.DriverContext[1] = UlongToPtr(Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex + 1].Offset);
            InsertTailList(&PinContext->DoneIrpListHead, &Irp->Tail.Overlay.ListEntry);
        }
        else
        {
            /* insert entry into ready list */
            InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);
        }

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        KsIncrementCountedWorker(PinContext->CaptureWorker);
    }

    /* done */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

PIRP
PinGetIrpFromReadyList(
    IN PKSPIN Pin)
{
    PPIN_CONTEXT PinContext;
    PLIST_ENTRY CurEntry;
    KIRQL OldLevel;
    PIRP Irp = NULL;

    /* get pin context */
    PinContext = Pin->Context;

    /* acquire spin lock */
    KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    if (!IsListEmpty(&PinContext->IrpListHead))
    {
        /* remove entry from list */
        CurEntry = RemoveHeadList(&PinContext->IrpListHead);

        /* get irp offset */
        Irp = (PIRP)CONTAINING_RECORD(CurEntry, IRP, Tail.Overlay.ListEntry);
    }

    InterlockedDecrement(&PinContext->StoppedStreamingIrps);

    /* release lock */
    KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

    return Irp;
}

NTSTATUS
PinRenderProcess(
    IN PKSPIN Pin)
{
    PKSSTREAM_POINTER LeadingStreamPointer;
    PPIN_CONTEXT PinContext;
    ULONG Used, Valid, Available, Free, WriteOffset, Copy, Part;
    PUCHAR Data;
    KIRQL OldLevel;

    DPRINT("PinRenderProcess entered\n");

    /* get pin context */
    PinContext = Pin->Context;

    if (PinContext->StopStreaming)
    {
        DPRINT1("PinRenderProcess StopStreaming == TRUE\n");

        /* the irps in the list are idle and already counted as stopped */
        if (PinContext->StoppedStreamingIrps == RENDER_IRP_COUNT)
        {
            DPRINT1("PinRenderProcess signaling event\n");
            KeSetEvent(&PinContext->StoppedStreamingEvent, 0, FALSE);
        }
        return STATUS_SUCCESS;
    }



    /* copy the stream frames into the render fifo */
    while (TRUE)
    {
        LeadingStreamPointer = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
        if (LeadingStreamPointer == NULL)
        {
            DPRINT("PinRenderProcess NoData\n");
            break;
        }

        Data = LeadingStreamPointer->StreamHeader->Data;
        if (NULL == Data)
        {
            KsStreamPointerUnlock(LeadingStreamPointer, FALSE);
            DPRINT("PinRenderProcess NoData\n");
            break;
        }

        /* only DataUsed bytes of the frame contain audio data */
        ASSERT(LeadingStreamPointer->Offset->Count >= LeadingStreamPointer->Offset->Remaining);
        Used = LeadingStreamPointer->Offset->Count - LeadingStreamPointer->Offset->Remaining;
        Valid = min(LeadingStreamPointer->StreamHeader->DataUsed, LeadingStreamPointer->Offset->Count);
        Available = (Valid > Used) ? Valid - Used : 0;

        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);
        Free = PinContext->BufferSize - PinContext->BufferLength;
        WriteOffset = (PinContext->BufferOffset + PinContext->BufferLength) % PinContext->BufferSize;
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        /* the free part of the fifo is only written here, the pump only reads the filled part */
        Copy = min(Available, Free);
        if (Copy)
        {
            Part = min(Copy, PinContext->BufferSize - WriteOffset);
            RtlCopyMemory(&PinContext->Buffer[WriteOffset], &Data[Used], Part);
            RtlCopyMemory(PinContext->Buffer, &Data[Used + Part], Copy - Part);

            KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);
            PinContext->BufferLength += Copy;
            PinContext->RenderBytesIn += Copy;
            KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
        }

        if (Copy < Available)
        {
            /* the fifo is full, continue with this frame when transfers completed */
            InterlockedExchange(&PinContext->RenderWaitingForSpace, TRUE);
            KsStreamPointerAdvanceOffsets(LeadingStreamPointer, Copy, 0, FALSE);
            KsStreamPointerUnlock(LeadingStreamPointer, FALSE);
            break;
        }

        /* the whole frame is in the fifo, complete it */
        KsStreamPointerAdvanceOffsetsAndUnlock(LeadingStreamPointer, LeadingStreamPointer->Offset->Remaining, 0, TRUE);
    }

    /* send the audio data */
    UsbAudioRenderPump(Pin);

    return STATUS_SUCCESS;
}

NTSTATUS
PinCaptureProcess(
    IN PKSPIN Pin)
{
    PKSSTREAM_POINTER LeadingStreamPointer;
    KIRQL OldLevel;
    PPIN_CONTEXT PinContext;
    PLIST_ENTRY CurEntry;
    PIRP Irp;
    PURB Urb;
    ULONG PacketIndex, CopiedBytes, PacketLength, PacketOffset;
    PUCHAR TransferBuffer, OutBuffer;
    ULONG Offset, Length, TargetOffset;
    PKSGATE Gate;

    DPRINT("PinCaptureProcess entered\n");

    /* get pin context */
    PinContext = Pin->Context;

    if (PinContext->StopStreaming)
    {
        DPRINT1("PinCaptureProcess StopStreaming == TRUE\n");

        /* acquire spin lock */
        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

        while (!IsListEmpty(&PinContext->DoneIrpListHead))
        {
            /* remove entry from list */
            CurEntry = RemoveHeadList(&PinContext->DoneIrpListHead);

            /* increment stopped irp count */
            InterlockedIncrement(&PinContext->StoppedStreamingIrps);
        }

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        if (PinContext->StoppedStreamingIrps == IRP_CAPTURE_COUNT)
        {
            DPRINT1("PinCaptureProcess entered signaling event\n");
            KeSetEvent(&PinContext->StoppedStreamingEvent, 0, FALSE);
        }
        return STATUS_SUCCESS;
    }


    LeadingStreamPointer = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    if (LeadingStreamPointer == NULL || LeadingStreamPointer->StreamHeader->Data == NULL)
    {
        DPRINT1("NoData available\n");
        /* unlock */
        if (LeadingStreamPointer)
            KsStreamPointerUnlock(LeadingStreamPointer, FALSE);

        /* get process control gate */
        Gate = KsPinGetAndGate(Pin);

        /* shutdown processing */
        KsGateTurnInputOff(Gate);
        return STATUS_SUCCESS;
    }

    /* get pin context */
    PinContext = Pin->Context;

    /* acquire spin lock */
    KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    /* get target buffer */
    OutBuffer = (PUCHAR)LeadingStreamPointer->StreamHeader->Data;

    /* calculate length */
    Length = min(LeadingStreamPointer->OffsetOut.Remaining, PinContext->CaptureBufferOffset);

    /* target buffer offset */
    TargetOffset = LeadingStreamPointer->OffsetOut.Count - LeadingStreamPointer->OffsetOut.Remaining;

    /* copy audio bytes */
    RtlCopyMemory(&OutBuffer[TargetOffset], PinContext->CaptureBuffer, Length);

    /* move capture buffer */
    RtlMoveMemory(PinContext->CaptureBuffer, &PinContext->CaptureBuffer[Length], PinContext->CaptureBufferOffset - Length);

    /* decrement offset*/
    PinContext->CaptureBufferOffset -= Length;

    /* release lock */
    KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

    if (Length == LeadingStreamPointer->OffsetOut.Remaining)
    {
        DPRINT("Ejecting StreamPointer %p\n", LeadingStreamPointer);
        KsStreamPointerAdvanceOffsetsAndUnlock(LeadingStreamPointer, 0, Length, TRUE);

        /* acquire spin lock */
        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);
    }
    else
    {
        /* adjust offsets */
        KsStreamPointerAdvanceOffsets(LeadingStreamPointer, 0, Length, FALSE);

        /* release lock */
        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);
    }

    while (!IsListEmpty(&PinContext->DoneIrpListHead))
    {
        /* remove entry from list */
        CurEntry = RemoveHeadList(&PinContext->DoneIrpListHead);

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        /* get irp offset */
        Irp = (PIRP)CONTAINING_RECORD(CurEntry, IRP, Tail.Overlay.ListEntry);

        /* get urb from irp */
        Urb = (PURB)Irp->Tail.Overlay.DriverContext[0];
        ASSERT(Urb);

        Offset = PtrToUlong(Irp->Tail.Overlay.DriverContext[1]);

        /* get transfer buffer */
        TransferBuffer = Urb->UrbIsochronousTransfer.TransferBuffer;

        /* get target buffer */
        OutBuffer = (PUCHAR)LeadingStreamPointer->StreamHeader->Data;

        /* calculate length */
        Length = min(LeadingStreamPointer->OffsetOut.Remaining, Urb->UrbIsochronousTransfer.TransferBufferLength - Offset);

        /* target buffer offset */
        TargetOffset = LeadingStreamPointer->OffsetOut.Count - LeadingStreamPointer->OffsetOut.Remaining;

        DPRINT("Irp %p Remaining %u Offset %u TransferBufferLength %u Length %u\n", Irp, LeadingStreamPointer->OffsetOut.Remaining, Offset, Urb->UrbIsochronousTransfer.TransferBufferLength, Length );
        PacketIndex = 0;
        do
        {
            if (Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex].Offset >= Offset)
                break;

            PacketIndex++;
        } while (PacketIndex < Urb->UrbIsochronousTransfer.NumberOfPackets);

        /* sanity check */
        ASSERT(PacketIndex < Urb->UrbIsochronousTransfer.NumberOfPackets);

        /* copy each packet extra */
        CopiedBytes = 0;
        for(; PacketIndex < Urb->UrbIsochronousTransfer.NumberOfPackets; PacketIndex++)
        {
            /* copy audio bytes */
            PacketLength = Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex].Length;
            if (PacketLength == 0)
            {
                // zero packet
                PacketLength = min(PinContext->DataPacketLength, Length - CopiedBytes);
                RtlZeroMemory(&OutBuffer[TargetOffset], PacketLength);
            }
            else
            {
                PacketLength = min(PinContext->DataPacketLength, Length - CopiedBytes);
                PacketOffset = Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex].Offset;
                RtlCopyMemory((PUCHAR)&OutBuffer[TargetOffset], &TransferBuffer[PacketOffset], PacketLength);
            }
            CopiedBytes += PacketLength;
            TargetOffset += PacketLength;
            if (CopiedBytes >= Length)
                break;
        }

        /* sanity check */
        ASSERT(CopiedBytes != 0);
        ASSERT(CopiedBytes <= Length);

        //DPRINT1("Irp %p Urb %p OutBuffer %p TransferBuffer %p Offset %lu Remaining %lu TransferBufferLength %lu Length %lu\n", Irp, Urb, OutBuffer, TransferBuffer, Offset, LeadingStreamPointer->OffsetOut.Remaining, Urb->UrbIsochronousTransfer.TransferBufferLength, Length);
        if (CopiedBytes == LeadingStreamPointer->OffsetOut.Remaining)
        {
            DPRINT("Ejecting StreamPointer %p\n", LeadingStreamPointer);
            KsStreamPointerAdvanceOffsetsAndUnlock(LeadingStreamPointer, 0, CopiedBytes, TRUE);

            /* acquire spin lock */
            KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

            if (PacketIndex + 1 < Urb->UrbIsochronousTransfer.NumberOfPackets)
            {
                /* adjust offset */
                Irp->Tail.Overlay.DriverContext[1] = UlongToPtr(Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex + 1].Offset);

                /* reinsert into processed list */
                InsertHeadList(&PinContext->DoneIrpListHead, &Irp->Tail.Overlay.ListEntry);

                /* release lock */
                KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
            }
            else
            {
                /* finished irp and intermediate irp */
                InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);

                /* release lock */
                KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
            }

            /* reaquire lock */
            KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);
        }
        else
        {
            /* adjust offsets */
            KsStreamPointerAdvanceOffsets(LeadingStreamPointer, 0, CopiedBytes, FALSE);

            /* release lock */
            KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

            if (PacketIndex + 1 < Urb->UrbIsochronousTransfer.NumberOfPackets)
            {
                /* adjust offset */
                Irp->Tail.Overlay.DriverContext[1] = UlongToPtr(Urb->UrbIsochronousTransfer.IsoPacket[PacketIndex+1].Offset);

                /* reinsert into processed list */
                InsertHeadList(&PinContext->DoneIrpListHead, &Irp->Tail.Overlay.ListEntry);
            }
            else
            {
                /* finished intermediate irp */
                InsertTailList(&PinContext->IrpListHead, &Irp->Tail.Overlay.ListEntry);
            }
        }

        if (LeadingStreamPointer->StreamHeader->Data == NULL)
        {
            /* no more work to be done*/
            break;
        }
    }
    DPRINT("PinCaptureProcess process ready list\n");
    while (!IsListEmpty(&PinContext->IrpListHead))
    {
        /* remove entry from list */
        CurEntry = RemoveHeadList(&PinContext->IrpListHead);

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        /* get irp offset */
        Irp = (PIRP)CONTAINING_RECORD(CurEntry, IRP, Tail.Overlay.ListEntry);

        /* reinitialize irp and urb */
        CaptureInitializeUrbAndIrp(Pin, Irp);

        IoCallDriver(PinContext->DeviceExtension->LowerDevice, Irp);

        /* acquire spin lock */
        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    }

    /* release lock */
    KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

    if (LeadingStreamPointer != NULL)
        KsStreamPointerUnlock(LeadingStreamPointer, FALSE);

    /* get process control gate */
    Gate = KsPinGetAndGate(Pin);

    /* shutdown processing */
    KsGateTurnInputOff(Gate);

    DPRINT("PinCaptureProcess done\n");

    return STATUS_PENDING;
}


NTSTATUS
NTAPI
USBAudioPinProcess(
    _In_ PKSPIN Pin)
{
    NTSTATUS Status;

    if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
    {
        Status = PinCaptureProcess(Pin);
    }
    else
    {
        Status = PinRenderProcess(Pin);
    }

    return Status;
}


VOID
NTAPI
USBAudioPinReset(
    _In_ PKSPIN Pin)
{
    UNIMPLEMENTED;
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
    /* DataRange is the matching range of the pin, the new format is in Pin->ConnectionFormat */
    return UsbAudioSetFormat(Pin, Pin->ConnectionFormat);
}

NTSTATUS
StartCaptureIsocTransfer(
    IN PKSPIN Pin)
{
    PPIN_CONTEXT PinContext;
    PLIST_ENTRY CurEntry;
    PIRP Irp;
    KIRQL OldLevel;

    /* get pin context */
    PinContext = Pin->Context;

    DPRINT("StartCaptureIsocTransfer entered\n");

    /* acquire spin lock */
    KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);

    while(!IsListEmpty(&PinContext->IrpListHead))
    {
        /* remove entry from list */
        CurEntry = RemoveHeadList(&PinContext->IrpListHead);

        /* get irp offset */
        Irp = (PIRP)CONTAINING_RECORD(CurEntry, IRP, Tail.Overlay.ListEntry);

        /* release lock */
        KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);

        /* reinitialize irp and urb */
        CaptureInitializeUrbAndIrp(Pin, Irp);

        DPRINT("StartCaptureIsocTransfer Irp %p\n", Irp);
        IoCallDriver(PinContext->DeviceExtension->LowerDevice, Irp);

        /* acquire spin lock */
        KeAcquireSpinLock(&PinContext->IrpListLock, &OldLevel);
    }

    /* release lock */
    KeReleaseSpinLock(&PinContext->IrpListLock, OldLevel);
    DPRINT("StartCaptureIsocTransfer done\n");
    return STATUS_SUCCESS;
}

NTSTATUS
CapturePinStateChange(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (FromState != ToState)
    {
        if (ToState)
        {
            if (ToState == KSSTATE_PAUSE)
            {
                if (FromState == KSSTATE_RUN)
                {
                    /* wait until pin processing is finished*/
                    UNIMPLEMENTED;
                }
            }
            else
            {
                if (ToState == KSSTATE_RUN)
                {
                    Status = StartCaptureIsocTransfer(Pin);
                }
            }
        }
    }
    return Status;
}

NTSTATUS
RenderPinStateChange(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PPIN_CONTEXT PinContext;

    /* get pin context */
    PinContext = Pin->Context;

    if (FromState != ToState)
    {
        if (ToState)
        {
            if (ToState == KSSTATE_PAUSE)
            {
                if (FromState == KSSTATE_RUN)
                {
                    /* wait until pin processing is finished*/
                    UNIMPLEMENTED;
                }
            }
            else
            {
                if (ToState == KSSTATE_STOP)
                {
                    PinContext->StopStreaming = TRUE;
                }
            }
        }
    }
    return Status;
}



NTSTATUS
NTAPI
USBAudioPinSetDeviceState(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState)
{
    NTSTATUS Status;

    if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
    {
        /* handle capture state changes */
        Status = CapturePinStateChange(Pin, ToState, FromState);
    }
    else
    {
        Status = RenderPinStateChange(Pin, ToState, FromState);
    }

    return Status;
}


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
    PKSPIN_DESCRIPTOR_EX PinDescriptor;
    PKSDATAFORMAT_WAVEFORMATEX DataFormat;
    PKSDATARANGE_AUDIO DataRangeAudio;

    /* get filter from irp*/
    Filter = KsGetFilterFromIrp(Irp);
    if (!Filter)
    {
        /* no match*/
        return STATUS_NO_MATCH;
    }

    /* get pin descriptor */
    PinDescriptor = (PKSPIN_DESCRIPTOR_EX)&Filter->Descriptor->PinDescriptors[Pin->PinId];

    *DataSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    if (DataBufferSize == 0)
    {
        /* buffer too small */
        return STATUS_BUFFER_OVERFLOW;
    }

    /* sanity checks*/
    ASSERT(PinDescriptor->PinDescriptor.DataRangesCount >= 0);
    ASSERT(PinDescriptor->PinDescriptor.DataRanges[0]->FormatSize == sizeof(KSDATARANGE_AUDIO));

    DataRangeAudio = (PKSDATARANGE_AUDIO)PinDescriptor->PinDescriptor.DataRanges[0];

    DataFormat = Data;
    DataFormat->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    DataFormat->WaveFormatEx.nChannels = DataRangeAudio->MaximumChannels;
    DataFormat->WaveFormatEx.nSamplesPerSec = DataRangeAudio->MaximumSampleFrequency;
    DataFormat->WaveFormatEx.nAvgBytesPerSec = DataRangeAudio->MaximumSampleFrequency * (DataRangeAudio->MaximumBitsPerSample / 8) * DataRangeAudio->MaximumChannels;
    DataFormat->WaveFormatEx.nBlockAlign = (DataRangeAudio->MaximumBitsPerSample / 8) * DataRangeAudio->MaximumChannels;
    DataFormat->WaveFormatEx.wBitsPerSample = DataRangeAudio->MaximumBitsPerSample;
    DataFormat->WaveFormatEx.cbSize = 0;

    DataFormat->DataFormat.FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX);
    DataFormat->DataFormat.Flags = 0;
    DataFormat->DataFormat.Reserved = 0;
    DataFormat->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    DataFormat->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    DataFormat->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    DataFormat->DataFormat.SampleSize = (DataRangeAudio->MaximumBitsPerSample / 8) * DataRangeAudio->MaximumChannels;

    return STATUS_SUCCESS;
}
