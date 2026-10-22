#pragma once

#include <ntddk.h>
#include <portcls.h>
#include <ksmedia.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <usbioctl.h>
#include <usb.h>
#include <usbdlib.h>
#include <debug.h>

#define USBAUDIO_TAG 'AbsU'

/* Class specific descriptor types of the USB Device Class Definition for Audio Devices 1.0 */
#define USB_AUDIO_CS_INTERFACE_DESCRIPTOR_TYPE  (0x24)
#define USB_AUDIO_CS_ENDPOINT_DESCRIPTOR_TYPE   (0x25)
#define USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE USB_AUDIO_CS_INTERFACE_DESCRIPTOR_TYPE

/* Interface subclasses */
#define USB_AUDIO_SUBCLASS_CONTROL              (0x01)
#define USB_AUDIO_SUBCLASS_STREAMING            (0x02)
#define USB_AUDIO_SUBCLASS_MIDI_STREAMING       (0x03)

/* Audio control interface descriptor subtypes */
#define USB_AUDIO_HEADER                        (0x01)
#define USB_AUDIO_INPUT_TERMINAL                (0x02)
#define USB_AUDIO_OUTPUT_TERMINAL               (0x03)
#define USB_AUDIO_MIXER_UNIT                    (0x04)
#define USB_AUDIO_SELECTOR_UNIT                 (0x05)
#define USB_AUDIO_FEATURE_UNIT                  (0x06)
#define USB_AUDIO_PROCESSING_UNIT               (0x07)
#define USB_AUDIO_EXTENSION_UNIT                (0x08)

/* Audio Device Class 2.0: the control interface protocol, and the subtypes that differ from 1.0 */
#define USB_AUDIO_PROTOCOL_2_0                  (0x20)
#define USB_AUDIO2_EFFECT_UNIT                  (0x07)
#define USB_AUDIO2_PROCESSING_UNIT              (0x08)
#define USB_AUDIO2_EXTENSION_UNIT               (0x09)
#define USB_AUDIO2_CLOCK_SOURCE                 (0x0A)
#define USB_AUDIO2_CLOCK_SELECTOR               (0x0B)
#define USB_AUDIO2_CLOCK_MULTIPLIER             (0x0C)
#define USB_AUDIO2_SAMPLE_RATE_CONVERTER        (0x0D)

/* 2.0 entities with no 1.0 counterpart, as subtypes of the driver's own */
#define USBAUDIO_UNIT_CLOCK                     (0x80)
#define USBAUDIO_UNIT_PASS_THROUGH              (0x81)

/* 2.0 requests, whose direction is in bmRequestType, and the clock controls */
#define USB_AUDIO2_CUR                          (0x01)
#define USB_AUDIO2_RANGE                        (0x02)
#define USB_AUDIO2_SAM_FREQ_CONTROL             (0x01)
#define USB_AUDIO2_CLOCK_SELECTOR_CONTROL       (0x01)

/* 2.0 Type I formats, bits of bmFormats */
#define USB_AUDIO2_FORMAT_PCM                   (0x00000001)
#define USB_AUDIO2_FORMAT_PCM8                  (0x00000002)
#define USB_AUDIO2_FORMAT_IEEE_FLOAT            (0x00000004)

/* Audio streaming interface descriptor subtypes */
#define USB_AUDIO_AS_GENERAL                    (0x01)
#define USB_AUDIO_FORMAT_TYPE                   (0x02)
#define USB_AUDIO_FORMAT_TYPE_I                 (0x01)

/* Terminal types, section 2.2 of the terminal types definition */
#define USB_AUDIO_STREAMING_TERMINAL_TYPE       (0x0101)

#define USB_AUDIO_MICROPHONE_TERMINAL_TYPE      (0x0201)
#define USB_AUDIO_DESKTOP_MICROPHONE_TERMINAL_TYPE (0x0202)
#define USB_AUDIO_PERSONAL_MICROPHONE_TERMINAL_TYPE (0x0203)
#define USB_AUDIO_OMMNI_MICROPHONE_TERMINAL_TYPE (0x0204)
#define USB_AUDIO_ARRAY_MICROPHONE_TERMINAL_TYPE (0x0205)
#define USB_AUDIO_ARRAY_PROCESSING_MICROPHONE_TERMINAL_TYPE (0x0206)

#define USB_AUDIO_SPEAKER_TERMINAL_TYPE         (0x0301)
#define USB_HEADPHONES_SPEAKER_TERMINAL_TYPE    (0x0302)
#define USB_AUDIO_HMDA_TERMINAL_TYPE            (0x0303)
#define USB_AUDIO_DESKTOP_SPEAKER_TERMINAL_TYPE (0x0304)
#define USB_AUDIO_ROOM_SPEAKER_TERMINAL_TYPE    (0x0305)
#define USB_AUDIO_COMMUNICATION_SPEAKER_TERMINAL_TYPE (0x0306)
#define USB_AUDIO_SUBWOOFER_TERMINAL_TYPE       (0x0307)

#define USB_AUDIO_HEADSET_TERMINAL_TYPE         (0x0402)
#define USB_AUDIO_LINE_CONNECTOR_TERMINAL_TYPE  (0x0603)
#define USB_AUDIO_SPDIF_TERMINAL_TYPE           (0x0605)
#define USB_AUDIO_UNDEFINED_TERMINAL_TYPE       (0xFFFF)

/* Format tags of type I streams */
#define USB_AUDIO_FORMAT_PCM                    (0x0001)
#define USB_AUDIO_FORMAT_PCM8                   (0x0002)
#define USB_AUDIO_FORMAT_IEEE_FLOAT             (0x0003)

/* Feature unit controls, as bits of bmaControls; a control's selector is its bit plus one */
#define USB_AUDIO_FEATURE_MUTE                  (0x0001)
#define USB_AUDIO_FEATURE_VOLUME                (0x0002)
#define USB_AUDIO_FEATURE_CONTROLS              (10)

#define USB_AUDIO_FEATURE_BIT_MUTE              (0)
#define USB_AUDIO_FEATURE_BIT_VOLUME            (1)
#define USB_AUDIO_FEATURE_BIT_BASS              (2)
#define USB_AUDIO_FEATURE_BIT_MID               (3)
#define USB_AUDIO_FEATURE_BIT_TREBLE            (4)
#define USB_AUDIO_FEATURE_BIT_EQUALIZER         (5)
#define USB_AUDIO_FEATURE_BIT_AGC               (6)
#define USB_AUDIO_FEATURE_BIT_DELAY             (7)
#define USB_AUDIO_FEATURE_BIT_BASS_BOOST        (8)
#define USB_AUDIO_FEATURE_BIT_LOUDNESS          (9)

/* Process types of processing units */
#define USB_AUDIO_PROCESS_UPDOWN_MIX            (0x01)
#define USB_AUDIO_PROCESS_PROLOGIC              (0x02)
#define USB_AUDIO_PROCESS_3D_STEREO             (0x03)
#define USB_AUDIO_PROCESS_REVERB                (0x04)
#define USB_AUDIO_PROCESS_CHORUS                (0x05)
#define USB_AUDIO_PROCESS_DYN_RANGE             (0x06)

/* Control selectors */
#define USB_AUDIO_ENABLE_CONTROL                (0x01)
#define USB_AUDIO_MUTE_CONTROL                  (0x01)
#define USB_AUDIO_VOLUME_CONTROL                (0x02)
#define USB_AUDIO_SAMPLING_FREQ_CONTROL         (0x01)

/* Class specific requests */
#define USB_AUDIO_SET_CUR                       (0x01)
#define USB_AUDIO_GET_CUR                       (0x81)
#define USB_AUDIO_GET_MIN                       (0x82)
#define USB_AUDIO_GET_MAX                       (0x83)
#define USB_AUDIO_GET_RES                       (0x84)

/* Class specific endpoint attributes, and lock delay units */
#define USB_AUDIO_EP_SAMPLING_FREQ              (0x01)
#define USB_AUDIO_LOCK_DELAY_MS                 (0x01)
#define USB_AUDIO_LOCK_DELAY_SAMPLES            (0x02)

/* Synchronization types of isochronous endpoints, bits 2 and 3 of bmAttributes */
#define USB_ENDPOINT_SYNC_MASK                  (0x0C)
#define USB_ENDPOINT_SYNC_ASYNCHRONOUS          (0x04)

/* Audio endpoint descriptors carry bRefresh and bSynchAddress after the standard fields */
#define USB_AUDIO_ENDPOINT_LENGTH               (9)
#define USB_AUDIO_ENDPOINT_REFRESH              (7)
#define USB_AUDIO_ENDPOINT_SYNCH_ADDRESS        (8)

/* Feedback values are frames per frame of the bus in 10.14 fixed point */
#define USB_AUDIO_FEEDBACK_SHIFT                (14)

/* Silence in a USB volume control, in 1/256 dB */
#define USB_AUDIO_VOLUME_SILENCE                ((SHORT)0x8000)

/* Logical channels a feature unit is followed for, past its master channel */
#define USBAUDIO_MAX_CHANNELS                   (8)

/* Nodes one descriptor may turn into */
#define USBAUDIO_MAX_UNIT_NODES                 (33)

/* Sampling frequencies kept per streaming format */
#define USBAUDIO_MAX_FREQUENCIES                (32)

/* Each isochronous transfer covers ten milliseconds: one packet a frame at full speed, up to eight at high speed */
#define USBAUDIO_TRANSFER_MS                    (10)
#define USBAUDIO_MAX_PACKETS_PER_TRANSFER       (80)

/* Rate feedback is kept as frames per packet in 16.16 */
#define USBAUDIO_FEEDBACK_SHIFT                 (16)

/* MIDI streaming interface descriptor subtypes, and the class endpoint one */
#define USB_MIDI_IN_JACK                        (0x02)
#define USB_MIDI_OUT_JACK                       (0x03)
#define USB_MIDI_GENERAL                        (0x01)

/* Cables one MIDI endpoint is followed for, and the bytes kept for each input cable */
#define USBMIDI_MAX_CABLES                      (16)
#define USBMIDI_IN_RING_BYTES                   (1024)

/* Bulk transfers of MIDI event packets: kept per output pin, and shared by the device's input */
#define USBMIDI_OUT_TRANSFERS                   (4)
#define USBMIDI_IN_TRANSFERS                    (2)
#define USBMIDI_TRANSFER_BYTES                  (512)

#include <pshpack1.h>

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    USHORT bcdADC;
    USHORT wTotalLength;
    UCHAR bInCollection;
    UCHAR baInterfaceNr;
}USB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR, *PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bTerminalID;
    USHORT wTerminalType;
    UCHAR bAssocTerminal;
    UCHAR bSourceID;
    UCHAR iTerminal;
}USB_AUDIO_CONTROL_OUTPUT_TERMINAL_DESCRIPTOR, *PUSB_AUDIO_CONTROL_OUTPUT_TERMINAL_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bTerminalID;
    USHORT wTerminalType;
    UCHAR bAssocTerminal;
    UCHAR bNrChannels;
    USHORT wChannelConfig;
    UCHAR iChannelNames;
    UCHAR iTerminal;
}USB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR, *PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bUnitID;
    UCHAR bSourceID;
    UCHAR bControlSize;
    UCHAR bmaControls[1];
}USB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR, *PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR;

/* Mixer, selector, processing and extension units: their sources follow bNrInPins */
typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bUnitID;
    UCHAR bNrInPins;
    UCHAR baSourceID[1];
}USB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR, *PUSB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR,
 USB_AUDIO_CONTROL_SELECTOR_UNIT_DESCRIPTOR, *PUSB_AUDIO_CONTROL_SELECTOR_UNIT_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bUnitID;
    USHORT wProcessType;
    UCHAR bNrInPins;
    UCHAR baSourceID[1];
}USB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR, *PUSB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bTerminalLink;
    UCHAR bDelay;
    USHORT wFormatTag;
}USB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR, *PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bFormatType;
    UCHAR bNrChannels;
    UCHAR bSubframeSize;
    UCHAR bBitResolution;
    UCHAR bSamFreqType;
    UCHAR tSamFreq[1];
}USB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR, *PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR;

typedef struct
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bDescriptorSubtype;
    UCHAR bmAttributes;
    UCHAR bLockDelayUnits;
    USHORT wLockDelay;
}USB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR, *PUSB_AUDIO_STREAMING_ENDPOINT_DESCRIPTOR;

#include <poppack.h>

/* A terminal or unit of the audio function and the KS nodes it became */
typedef struct
{
    PUSB_COMMON_DESCRIPTOR Descriptor;
    UCHAR Subtype;
    UCHAR Id;
    ULONG NodeCount;
    ULONG Nodes[USBAUDIO_MAX_UNIT_NODES];

    /* Feature units: logical channels and the controls of the master and of each channel */
    ULONG Channels;
    USHORT Controls[USBAUDIO_MAX_CHANNELS + 1];
    ULONG ControlNodes[USB_AUDIO_FEATURE_CONTROLS];

    /* The unit is described the 2.0 way, and Subtype is mapped to the 1.0 numbering */
    BOOLEAN Uac2;

    /* Processing and extension units: whether the unit can be switched on and off */
    BOOLEAN HasEnable;
}NODE_CONTEXT, *PNODE_CONTEXT;

/* One alternate setting of a streaming interface that carries PCM */
typedef struct
{
    KSDATARANGE_AUDIO DataRange;
    UCHAR TerminalLink;
    UCHAR InterfaceNumber;
    UCHAR AlternateSetting;
    UCHAR Channels;
    UCHAR SubframeSize;
    UCHAR BitResolution;
    UCHAR EndpointAddress;
    UCHAR EndpointInterval;
    UCHAR EndpointAttributes;
    UCHAR SyncType;
    UCHAR FeedbackAddress;
    UCHAR FeedbackRefresh;
    UCHAR FeedbackInterval;
    UCHAR LockDelayUnits;
    USHORT LockDelay;
    USHORT MaximumPacketSize;
    USHORT FormatTag;
    BOOLEAN Continuous;
    ULONG FrequencyCount;
    ULONG Frequencies[USBAUDIO_MAX_FREQUENCIES];

    /* 2.0: the clock source behind the terminal, and whether its frequency can be set */
    UCHAR ClockId;
    BOOLEAN ClockProgrammable;
}USBAUDIO_FORMAT, *PUSBAUDIO_FORMAT;

/* One bulk transfer of USB MIDI event packets */
typedef struct
{
    LIST_ENTRY Entry;
    PVOID Context;
    PIRP Irp;
    PURB Urb;
    PUCHAR Buffer;
    ULONG Filled;
}MIDI_TRANSFER, *PMIDI_TRANSFER;

/* Bytes from the device for one input cable, waiting for that cable's pin */
typedef struct
{
    PKSPIN Pin;
    ULONG Read;
    ULONG Count;
    UCHAR Ring[USBMIDI_IN_RING_BYTES];
}MIDI_IN_CABLE, *PMIDI_IN_CABLE;

typedef struct __DEVICE_EXTENSION__
{
    PDEVICE_OBJECT LowerDevice;                                  /* lower device*/
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;       /* usb configuration descriptor */
    PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;                     /* usb device descriptor */
    PUSBD_INTERFACE_INFORMATION InterfaceInfo;                   /* interface information */
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;               /* configuration handle */
    PNODE_CONTEXT NodeContext;                                   /* node context */
    ULONG NodeContextCount;                                      /* node context count */
    UCHAR ControlInterface;                                      /* audio control interface number */
    PUSBAUDIO_FORMAT Formats;                                    /* streaming formats */
    ULONG FormatCount;                                           /* streaming format count */
    KEVENT ControlLock;                                          /* serializes control requests */
    BOOLEAN Uac2;                                                /* the function follows Audio Device Class 2.0 */
    BOOLEAN HighSpeed;                                           /* the device runs at high speed */

    /* MIDI streaming: the bulk pipes, the cables each carries, and the first MIDI pin */
    USBD_PIPE_HANDLE MidiOutPipe;
    USBD_PIPE_HANDLE MidiInPipe;
    USHORT MidiOutPacketSize;
    USHORT MidiInPacketSize;
    UCHAR MidiOutCables;
    UCHAR MidiInCables;
    ULONG MidiPinBase;

    /* MIDI input is read once for the device and handed to the pin of each cable */
    KSPIN_LOCK MidiInLock;
    ULONG MidiInUsers;
    LONG MidiInPending;
    KEVENT MidiInIdle;
    PMIDI_TRANSFER MidiInTransfers;
    PMIDI_IN_CABLE MidiIn;
}DEVICE_EXTENSION, *PDEVICE_EXTENSION;

typedef struct
{
    PDEVICE_EXTENSION DeviceExtension;                           /* device extension */
    PDEVICE_OBJECT LowerDevice;                                  /* lower device*/

}FILTER_CONTEXT, *PFILTER_CONTEXT;

/* One isochronous transfer of a pin, reused for as long as the pin streams */
typedef struct
{
    LIST_ENTRY Entry;
    PKSPIN Pin;
    PIRP Irp;
    PURB Urb;
    PUCHAR Buffer;
    ULONG Packets;
    ULONG Filled;
}PIN_TRANSFER, *PPIN_TRANSFER;

typedef struct
{
    PDEVICE_EXTENSION DeviceExtension;                           /* device extension */
    PDEVICE_OBJECT LowerDevice;                                  /* lower device*/
    PUSBAUDIO_FORMAT Format;                                     /* streaming format of the pin */
    ULONG SampleRate;                                            /* frames per second */
    ULONG FrameBytes;                                            /* bytes of one frame */
    USBD_PIPE_HANDLE PipeHandle;                                 /* isochronous pipe */
    USBD_PIPE_HANDLE FeedbackPipe;                               /* render: the device's rate feedback, if it sends one */
    PIRP FeedbackIrp;                                            /* render: reads the feedback */
    PURB FeedbackUrb;                                            /* render: feedback transfer */
    PUCHAR FeedbackBuffer;                                       /* render: feedback value */
    volatile LONG FeedbackRate;                                  /* render: frames per packet in 16.16, 0 without feedback */
    ULONG FeedbackRemainder;                                     /* render: fraction of a frame in 16.16 */
    ULONG PacketsPerSecond;                                      /* service intervals of the endpoint each second */
    ULONG ServiceMicroframes;                                    /* high speed: microframes between packets */
    ULONG FeedbackPackets;                                       /* render: packets of one feedback transfer */
    ULONG SilencePackets;                                        /* render: silent packets still to send before the client's data */
    ULONG PacketsPerTransfer;                                    /* packets in one transfer */
    ULONG FrameRemainder;                                        /* fraction of a frame carried between packets */

    PPIN_TRANSFER Transfers;                                     /* transfers of the pin */
    ULONG TransferCount;                                         /* transfer count */
    KSPIN_LOCK Lock;                                             /* protects the lists and counters below */
    LIST_ENTRY FreeTransfers;                                    /* transfers not with the device */
    PPIN_TRANSFER Partial;                                       /* render transfer being filled */
    ULONG PacketBytes[USBAUDIO_MAX_PACKETS_PER_TRANSFER];        /* render: bytes of each packet of the partial transfer */
    ULONG PacketFill;                                            /* render: bytes in the packet being filled */
    LONG Pending;                                                /* transfers with the device */
    KEVENT Idle;                                                 /* set when no transfer is with the device */
    BOOLEAN Streaming;                                           /* transfers may be submitted */

    PUCHAR Ring;                                                 /* capture: data not yet handed out */
    ULONG RingSize;                                              /* capture: ring size */
    ULONG RingRead;                                              /* capture: read offset */
    ULONG RingCount;                                             /* capture: bytes in the ring */
    BOOLEAN Discontinuity;                                       /* capture: audio was lost before the next byte */
}PIN_CONTEXT, *PPIN_CONTEXT;

/* A MIDI pin: one cable of the device, in either direction */
typedef struct
{
    PDEVICE_EXTENSION DeviceExtension;
    PKSPIN Pin;
    UCHAR Cable;
    BOOLEAN Streaming;
    KSPIN_LOCK Lock;
    LIST_ENTRY FreeTransfers;
    LONG Pending;
    KEVENT Idle;
    MIDI_TRANSFER Transfers[USBMIDI_OUT_TRANSFERS];
    PMIDI_TRANSFER Partial;

    /* Output reads the client's KS music records a byte at a time */
    UCHAR Header[sizeof(KSMUSICFORMAT)];
    ULONG HeaderFill;
    ULONG DataLeft;
    ULONG PadLeft;

    /* and puts the bytes of each message together into event packets */
    UCHAR Status;
    UCHAR Message[3];
    UCHAR Fill;
    UCHAR Need;
    BOOLEAN SysEx;
}MIDI_PIN_CONTEXT, *PMIDI_PIN_CONTEXT;

/* descriptors.c */
PUSB_COMMON_DESCRIPTOR
UsbAudioNextDescriptor(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ PVOID Current);

BOOLEAN
UsbAudioIsValidUnit(
    _In_ PUSB_COMMON_DESCRIPTOR Descriptor,
    _In_ BOOLEAN Version2);

BOOLEAN
UsbAudioIsVersion2Header(
    _In_ PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header);

ULONG
UsbAudioGetHeaderLength(
    _In_ PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header);

PUSB_COMMON_DESCRIPTOR
UsbAudioNextUnit(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header,
    _In_ PVOID Current);

PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR
UsbAudioGetControlHeader(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _Out_ PUCHAR InterfaceNumber);

NTSTATUS
UsbAudioParseFormats(
    _In_ PDEVICE_EXTENSION DeviceExtension);

PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR
UsbAudioGetTerminalByIndex(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ BOOLEAN Streaming,
    _In_ ULONG Index);

VOID
NTAPI
CountTerminalUnits(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    OUT PULONG NonStreamingTerminalDescriptorCount,
    OUT PULONG TotalTerminalDescriptorCount);

/* filter.c */

NTSTATUS
NTAPI
USBAudioCreateFilterContext(
    PKSDEVICE Device);

/* pool.c */
PVOID
NTAPI
AllocFunction(
    IN ULONG ItemSize);

VOID
NTAPI
FreeFunction(
    IN PVOID Item);

/* usbaudio.c */

NTSTATUS
SubmitUrbSync(
    IN PDEVICE_OBJECT Device,
    IN PURB Urb);

NTSTATUS
UsbAudioClassRequest(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ USHORT Function,
    _In_ UCHAR Request,
    _In_ USHORT Value,
    _In_ USHORT Index,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN In);

NTSTATUS
NTAPI
USBAudioAddDevice(
  _In_ PKSDEVICE Device
);

NTSTATUS
NTAPI
USBAudioPnPStart(
  _In_     PKSDEVICE         Device,
  _In_     PIRP              Irp,
  _In_opt_ PCM_RESOURCE_LIST TranslatedResourceList,
  _In_opt_ PCM_RESOURCE_LIST UntranslatedResourceList
);

NTSTATUS
NTAPI
USBAudioPnPQueryStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBAudioPnPCancelStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBAudioPnPStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

NTSTATUS
NTAPI
USBAudioPnPQueryRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBAudioPnPCancelRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

VOID
NTAPI
USBAudioPnPRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

NTSTATUS
NTAPI
USBAudioPnPQueryCapabilities(
  _In_    PKSDEVICE            Device,
  _In_    PIRP                 Irp,
  _Inout_ PDEVICE_CAPABILITIES Capabilities
);

VOID
NTAPI
USBAudioPnPSurpriseRemoval(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
);

NTSTATUS
NTAPI
USBAudioPnPQueryPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE DeviceTo,
  _In_ DEVICE_POWER_STATE DeviceFrom,
  _In_ SYSTEM_POWER_STATE SystemTo,
  _In_ SYSTEM_POWER_STATE SystemFrom,
  _In_ POWER_ACTION       Action
);

VOID
NTAPI
USBAudioPnPSetPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE To,
  _In_ DEVICE_POWER_STATE From
);

/* pin.c*/

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
    _Out_ PULONG       DataSize);

NTSTATUS
NTAPI
USBAudioPinCreate(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
USBAudioPinClose(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
USBAudioPinProcess(
    _In_ PKSPIN Pin);

VOID
NTAPI
USBAudioPinReset(
    _In_ PKSPIN Pin);

NTSTATUS
NTAPI
USBAudioPinSetDataFormat(
    _In_ PKSPIN Pin,
    _In_opt_ PKSDATAFORMAT OldFormat,
    _In_opt_ PKSMULTIPLE_ITEM OldAttributeList,
    _In_ const KSDATARANGE* DataRange,
    _In_opt_ const KSATTRIBUTE_LIST* AttributeRange);

NTSTATUS
NTAPI
USBAudioPinSetDeviceState(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState);

/* midi.c */
VOID
UsbMidiSetupInterface(
    _Inout_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PUSBD_INTERFACE_INFORMATION Interface);

ULONG
UsbMidiGetPinCount(
    _In_ PDEVICE_EXTENSION DeviceExtension);

VOID
UsbMidiInitPinDescriptors(
    _Inout_ PDEVICE_EXTENSION DeviceExtension,
    _Out_writes_(Count) PKSPIN_DESCRIPTOR_EX Pins,
    _In_ ULONG Base,
    _In_ ULONG Count);

VOID
UsbMidiFreeDevice(
    _Inout_ PDEVICE_EXTENSION DeviceExtension);

/* uac2.c */
UCHAR
UsbAudio2MapSubtype(
    _In_ UCHAR Subtype);

BOOLEAN
UsbAudio2DescribeFormat(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PUCHAR General,
    _In_ PUCHAR Type,
    _Out_ PUSBAUDIO_FORMAT Format);

NTSTATUS
UsbAudio2GetRangeValue(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ USHORT Value,
    _In_ USHORT Index,
    _In_ ULONG Bytes,
    _In_ UCHAR Request,
    _Out_ PLONG Result);

NTSTATUS
UsbAudio2SetClockFrequency(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PUSBAUDIO_FORMAT Format,
    _In_ ULONG Frequency);
