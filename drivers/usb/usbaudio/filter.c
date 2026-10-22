/*
* PROJECT:     ReactOS Universal Audio Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbaudio/filter.c
* PURPOSE:     USB Audio device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*              Justin Miller (justin.miller@reactos.org)
*/

#include "usbaudio.h"

/* Chains of units longer than this are not followed; real functions stay well below it */
#define USBAUDIO_MAX_CHAIN 32

/* Units a walk has been through, by their one byte id, so a loop in the descriptors ends */
typedef struct
{
    ULONG Seen[256 / 32];
}UNIT_WALK, *PUNIT_WALK;

static
BOOLEAN
UsbAudioVisit(
    _Inout_ PUNIT_WALK Walk,
    _In_ UCHAR Id)
{
    if (Walk->Seen[Id / 32] & (1UL << (Id % 32)))
        return FALSE;
    Walk->Seen[Id / 32] |= 1UL << (Id % 32);
    return TRUE;
}

GUID NodeTypeMicrophone = { STATIC_KSNODETYPE_MICROPHONE };
GUID NodeTypeDesktopMicrophone = { STATIC_KSNODETYPE_DESKTOP_MICROPHONE };
GUID NodeTypePersonalMicrophone = { STATIC_KSNODETYPE_PERSONAL_MICROPHONE };
GUID NodeTypeOmmniMicrophone = { STATIC_KSNODETYPE_OMNI_DIRECTIONAL_MICROPHONE };
GUID NodeTypeArrayMicrophone = { STATIC_KSNODETYPE_MICROPHONE_ARRAY };
GUID NodeTypeProcessingArrayMicrophone = { STATIC_KSNODETYPE_PROCESSING_MICROPHONE_ARRAY };
GUID NodeTypeSpeaker = { STATIC_KSNODETYPE_SPEAKER };
GUID NodeTypeHeadphonesSpeaker = { STATIC_KSNODETYPE_HEADPHONES };
GUID NodeTypeHMDA = { STATIC_KSNODETYPE_HEAD_MOUNTED_DISPLAY_AUDIO };
GUID NodeTypeDesktopSpeaker = { STATIC_KSNODETYPE_DESKTOP_SPEAKER };
GUID NodeTypeRoomSpeaker = { STATIC_KSNODETYPE_ROOM_SPEAKER };
GUID NodeTypeCommunicationSpeaker = { STATIC_KSNODETYPE_COMMUNICATION_SPEAKER };
GUID NodeTypeSubwoofer = { STATIC_KSNODETYPE_LOW_FREQUENCY_EFFECTS_SPEAKER };
GUID NodeTypeLineConnector = { STATIC_KSNODETYPE_LINE_CONNECTOR };
GUID NodeTypeSpdif = { STATIC_KSNODETYPE_SPDIF_INTERFACE };
GUID NodeTypeAnalogConnector = { STATIC_KSNODETYPE_ANALOG_CONNECTOR };
GUID NodeTypeCapture = { STATIC_PINNAME_CAPTURE };
GUID NodeTypePlayback = { STATIC_KSCATEGORY_AUDIO };
GUID GUID_KSCATEGORY_AUDIO = { STATIC_KSCATEGORY_AUDIO };
GUID GUID_KSCATEGORY_RENDER = { STATIC_KSCATEGORY_RENDER };
GUID GUID_KSCATEGORY_CAPTURE = { STATIC_KSCATEGORY_CAPTURE };

KSPIN_INTERFACE StandardPinInterface =
{
     {STATIC_KSINTERFACESETID_Standard},
     KSINTERFACE_STANDARD_STREAMING,
     0
};

KSPIN_MEDIUM StandardPinMedium =
{
     {STATIC_KSMEDIUMSETID_Standard},
     KSMEDIUM_TYPE_ANYINSTANCE,
     0
};

KSDATARANGE BridgePinAudioFormat[] =
{
    {
        {
            sizeof(KSDATAFORMAT),
            0,
            0,
            0,
            {STATIC_KSDATAFORMAT_TYPE_AUDIO},
            {STATIC_KSDATAFORMAT_SUBTYPE_ANALOG},
            {STATIC_KSDATAFORMAT_SPECIFIER_NONE}
        }
    }
};

static PKSDATARANGE BridgePinAudioFormats[] =
{
    &BridgePinAudioFormat[0]
};

static LPWSTR ReferenceString = L"global";

NTSTATUS
NTAPI
USBAudioFilterCreate(
    PKSFILTER Filter,
    PIRP Irp);

static KSFILTER_DISPATCH USBAudioFilterDispatch =
{
    USBAudioFilterCreate,
    NULL,
    NULL,
    NULL
};

static KSPIN_DISPATCH UsbAudioPinDispatch =
{
    USBAudioPinCreate,
    USBAudioPinClose,
    USBAudioPinProcess,
    USBAudioPinReset,
    USBAudioPinSetDataFormat,
    USBAudioPinSetDeviceState,
    NULL,
    NULL,
    NULL,
    NULL
};

NTSTATUS NTAPI FilterAudioControlHandler(IN PIRP Irp, IN PKSIDENTIFIER  Request, IN OUT PVOID  Data);

#define USBAUDIO_CONTROL_PROPERTY(Name, Id, Size) \
static DEFINE_KSPROPERTY_TABLE(Name##PropertyItems) \
{ \
    DEFINE_KSPROPERTY_ITEM(Id, FilterAudioControlHandler, sizeof(KSNODEPROPERTY_AUDIO_CHANNEL), Size, \
                           FilterAudioControlHandler, NULL, 0, NULL, FilterAudioControlHandler, 0) \
}; \
static KSPROPERTY_SET Name##PropertySets[] = \
{ \
    { &KSPROPSETID_Audio, 1, (const KSPROPERTY_ITEM*)&Name##PropertyItems, 0, NULL } \
}; \
static KSAUTOMATION_TABLE Name##AutomationTable = \
{ \
    1, sizeof(KSPROPERTY_ITEM), Name##PropertySets, \
    0, 0, NULL, \
    0, 0, NULL \
}

USBAUDIO_CONTROL_PROPERTY(Mute, KSPROPERTY_AUDIO_MUTE, sizeof(BOOL));
USBAUDIO_CONTROL_PROPERTY(Volume, KSPROPERTY_AUDIO_VOLUMELEVEL, sizeof(LONG));
USBAUDIO_CONTROL_PROPERTY(Bass, KSPROPERTY_AUDIO_BASS, sizeof(LONG));
USBAUDIO_CONTROL_PROPERTY(Mid, KSPROPERTY_AUDIO_MID, sizeof(LONG));
USBAUDIO_CONTROL_PROPERTY(Treble, KSPROPERTY_AUDIO_TREBLE, sizeof(LONG));
USBAUDIO_CONTROL_PROPERTY(Agc, KSPROPERTY_AUDIO_AGC, sizeof(BOOL));
USBAUDIO_CONTROL_PROPERTY(BassBoost, KSPROPERTY_AUDIO_BASS_BOOST, sizeof(BOOL));
USBAUDIO_CONTROL_PROPERTY(Loudness, KSPROPERTY_AUDIO_LOUDNESS, sizeof(BOOL));

NTSTATUS NTAPI FilterNodeEnableHandler(IN PIRP Irp, IN PKSIDENTIFIER Request, IN OUT PVOID Data);

/* Processing and extension units answer whether they are on, a byte like the class control */
static DEFINE_KSPROPERTY_TABLE(EnablePropertyItems)
{
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_TOPOLOGYNODE_ENABLE, FilterNodeEnableHandler, sizeof(KSNODEPROPERTY), sizeof(BOOLEAN),
                           FilterNodeEnableHandler, NULL, 0, NULL, NULL, 0)
};
static KSPROPERTY_SET EnablePropertySets[] =
{
    { &KSPROPSETID_TopologyNode, 1, (const KSPROPERTY_ITEM*)&EnablePropertyItems, 0, NULL }
};
static KSAUTOMATION_TABLE EnableAutomationTable =
{
    1, sizeof(KSPROPERTY_ITEM), EnablePropertySets,
    0, 0, NULL,
    0, 0, NULL
};

/*
 * The feature unit controls that become nodes, in the order of their bits. Levels carry 16 bit
 * values in 1/256 dB or 8 bit ones in 1/4 dB; the rest are one byte switches.
 */
typedef struct
{
    ULONG Bit;
    const GUID *Type;
    const GUID *Name;
    const KSAUTOMATION_TABLE *AutomationTable;
    ULONG Bytes;
    LONG Scale;
}USBAUDIO_CONTROL;

/* KSAUDFNAME_MIDRANGE, which the headers here do not carry */
static const GUID UsbAudioMidrangeName = { 0xA2CBE478, 0xAE84, 0x49A1, { 0x8B, 0x72, 0x4A, 0xD0, 0x9B, 0x78, 0xED, 0x34 } };

static const USBAUDIO_CONTROL UsbAudioControls[] =
{
    { USB_AUDIO_FEATURE_BIT_MUTE,       &KSNODETYPE_MUTE,     &KSNODETYPE_MUTE,     &MuteAutomationTable,      1, 0 },
    { USB_AUDIO_FEATURE_BIT_VOLUME,     &KSNODETYPE_VOLUME,   &KSNODETYPE_VOLUME,   &VolumeAutomationTable,    2, 256 },
    { USB_AUDIO_FEATURE_BIT_BASS,       &KSNODETYPE_TONE,     &KSAUDFNAME_BASS,     &BassAutomationTable,      1, 0x4000 },
    { USB_AUDIO_FEATURE_BIT_MID,        &KSNODETYPE_TONE,     &UsbAudioMidrangeName, &MidAutomationTable,       1, 0x4000 },
    { USB_AUDIO_FEATURE_BIT_TREBLE,     &KSNODETYPE_TONE,     &KSAUDFNAME_TREBLE,   &TrebleAutomationTable,    1, 0x4000 },
    { USB_AUDIO_FEATURE_BIT_AGC,        &KSNODETYPE_AGC,      &KSNODETYPE_AGC,      &AgcAutomationTable,       1, 0 },
    { USB_AUDIO_FEATURE_BIT_BASS_BOOST, &KSNODETYPE_TONE,     &KSNODETYPE_TONE,     &BassBoostAutomationTable, 1, 0 },
    { USB_AUDIO_FEATURE_BIT_LOUDNESS,   &KSNODETYPE_LOUDNESS, &KSNODETYPE_LOUDNESS, &LoudnessAutomationTable,  1, 0 },
};

/* The node type a processing unit stands for; extension units and unknown processes are device specific */
static
const GUID *
UsbAudioGetProcessNodeType(
    _In_ PNODE_CONTEXT Unit)
{
    if (Unit->Subtype == USB_AUDIO_PROCESSING_UNIT)
    {
        switch (((PUSB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR)Unit->Descriptor)->wProcessType)
        {
            case USB_AUDIO_PROCESS_UPDOWN_MIX:
                return &KSNODETYPE_UPDOWN_MIX;
            case USB_AUDIO_PROCESS_PROLOGIC:
                return &KSNODETYPE_PROLOGIC_DECODER;
            case USB_AUDIO_PROCESS_3D_STEREO:
                return &KSNODETYPE_STEREO_WIDE;
            case USB_AUDIO_PROCESS_REVERB:
                return &KSNODETYPE_REVERB;
            case USB_AUDIO_PROCESS_CHORUS:
                return &KSNODETYPE_CHORUS;
            case USB_AUDIO_PROCESS_DYN_RANGE:
                return &KSNODETYPE_DYN_RANGE_COMPRESSOR;
        }
    }
    return &KSNODETYPE_DEV_SPECIFIC;
}

static
const USBAUDIO_CONTROL *
UsbAudioGetControl(
    _In_ ULONG Bit)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(UsbAudioControls); Index++)
    {
        if (UsbAudioControls[Index].Bit == Bit)
            return &UsbAudioControls[Index];
    }
    return NULL;
}

static
PNODE_CONTEXT
UsbAudioFindUnit(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR Id)
{
    ULONG Index;

    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        if (DeviceExtension->NodeContext[Index].Id == Id)
            return &DeviceExtension->NodeContext[Index];
    }
    return NULL;
}

/* The ids a unit takes its signal from */
static
ULONG
UsbAudioGetSources(
    _In_ PNODE_CONTEXT Unit,
    _Out_ const UCHAR **Sources)
{
    PUCHAR Descriptor = (PUCHAR)Unit->Descriptor;
    ULONG Length = Unit->Descriptor->bLength;
    ULONG Offset, Count;

    *Sources = NULL;
    switch (Unit->Subtype)
    {
        case USB_AUDIO_OUTPUT_TERMINAL:
            *Sources = &Descriptor[FIELD_OFFSET(USB_AUDIO_CONTROL_OUTPUT_TERMINAL_DESCRIPTOR, bSourceID)];
            return 1;

        case USB_AUDIO_FEATURE_UNIT:
            *Sources = &Descriptor[FIELD_OFFSET(USB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR, bSourceID)];
            return 1;

        case USB_AUDIO_MIXER_UNIT:
        case USB_AUDIO_SELECTOR_UNIT:
            Offset = FIELD_OFFSET(USB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR, bNrInPins);
            break;

        case USB_AUDIO_PROCESSING_UNIT:
        case USB_AUDIO_EXTENSION_UNIT:
            Offset = FIELD_OFFSET(USB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR, bNrInPins);
            break;

        case USBAUDIO_UNIT_PASS_THROUGH:
            /* a 2.0 effect unit has its source after wEffectType, a rate converter right after its id */
            *Sources = &Descriptor[(Descriptor[2] == USB_AUDIO2_EFFECT_UNIT) ? 6 : 4];
            return 1;

        default:
            return 0;
    }

    if (Offset >= Length)
        return 0;
    Count = min((ULONG)Descriptor[Offset], Length - Offset - 1);
    *Sources = &Descriptor[Offset + 1];
    return Count;
}

static
BOOLEAN
UsbAudioIsStreamingTerminal(
    _In_ PNODE_CONTEXT Unit)
{
    return (Unit->Subtype == USB_AUDIO_INPUT_TERMINAL || Unit->Subtype == USB_AUDIO_OUTPUT_TERMINAL) &&
           ((PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)Unit->Descriptor)->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE;
}

/* Whether the signal of a unit comes from a streaming input terminal somewhere upstream */
static
BOOLEAN
UsbAudioWalkFromStream(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _Inout_ PUNIT_WALK Walk,
    _In_ ULONG Depth)
{
    const UCHAR *Sources;
    PNODE_CONTEXT Source;
    ULONG Count, Index;

    if (Depth > USBAUDIO_MAX_CHAIN || !UsbAudioVisit(Walk, Unit->Id))
        return FALSE;
    if (Unit->Subtype == USB_AUDIO_INPUT_TERMINAL)
        return UsbAudioIsStreamingTerminal(Unit);

    Count = UsbAudioGetSources(Unit, &Sources);
    for (Index = 0; Index < Count; Index++)
    {
        Source = UsbAudioFindUnit(DeviceExtension, Sources[Index]);
        if (Source && UsbAudioWalkFromStream(DeviceExtension, Source, Walk, Depth + 1))
            return TRUE;
    }
    return FALSE;
}

static
BOOLEAN
UsbAudioComesFromStream(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ ULONG Depth)
{
    UNIT_WALK Walk;

    UNREFERENCED_PARAMETER(Depth);
    RtlZeroMemory(&Walk, sizeof(Walk));
    return UsbAudioWalkFromStream(DeviceExtension, Unit, &Walk, 0);
}

/* Whether Target lies upstream of Unit */
static
BOOLEAN
UsbAudioWalkUpstream(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ PNODE_CONTEXT Target,
    _Inout_ PUNIT_WALK Walk,
    _In_ ULONG Depth)
{
    const UCHAR *Sources;
    PNODE_CONTEXT Source;
    ULONG Count, Index;

    if (Depth > USBAUDIO_MAX_CHAIN || !UsbAudioVisit(Walk, Unit->Id))
        return FALSE;

    Count = UsbAudioGetSources(Unit, &Sources);
    for (Index = 0; Index < Count; Index++)
    {
        Source = UsbAudioFindUnit(DeviceExtension, Sources[Index]);
        if (Source == Target || (Source && UsbAudioWalkUpstream(DeviceExtension, Source, Target, Walk, Depth + 1)))
            return TRUE;
    }
    return FALSE;
}

static
BOOLEAN
UsbAudioIsUpstream(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ PNODE_CONTEXT Target,
    _In_ ULONG Depth)
{
    UNIT_WALK Walk;

    UNREFERENCED_PARAMETER(Depth);
    RtlZeroMemory(&Walk, sizeof(Walk));
    return UsbAudioWalkUpstream(DeviceExtension, Unit, Target, &Walk, 0);
}

/* Whether a terminal's signal ends up in a streaming output terminal, i.e. is recorded */
static
BOOLEAN
UsbAudioIsRecorded(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit)
{
    ULONG Index;

    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        if (DeviceExtension->NodeContext[Index].Subtype == USB_AUDIO_OUTPUT_TERMINAL &&
            UsbAudioIsStreamingTerminal(&DeviceExtension->NodeContext[Index]) &&
            UsbAudioIsUpstream(DeviceExtension, &DeviceExtension->NodeContext[Index], Unit, 0))
        {
            return TRUE;
        }
    }
    return FALSE;
}

/* Whether a mixer input belongs in the topology: analog paths beside a stream stay out of it */
static
BOOLEAN
UsbAudioIsWantedInput(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ UCHAR SourceId)
{
    const UCHAR *Sources;
    PNODE_CONTEXT Source;
    ULONG Count, Index;
    BOOLEAN AnyStream = FALSE;

    if (Unit->Subtype != USB_AUDIO_MIXER_UNIT)
        return TRUE;

    Count = UsbAudioGetSources(Unit, &Sources);
    for (Index = 0; Index < Count && !AnyStream; Index++)
    {
        Source = UsbAudioFindUnit(DeviceExtension, Sources[Index]);
        AnyStream = Source && UsbAudioComesFromStream(DeviceExtension, Source, 0);
    }

    Source = UsbAudioFindUnit(DeviceExtension, SourceId);
    return Source && (!AnyStream || UsbAudioComesFromStream(DeviceExtension, Source, 0));
}

/* The node a unit's signal leaves from; units without nodes hand on their first source's */
static
ULONG
UsbAudioGetOutputNode(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ ULONG Depth)
{
    const UCHAR *Sources;
    PNODE_CONTEXT Source;

    /* the depth limit also ends a unit that names itself as its source */
    if (Depth > USBAUDIO_MAX_CHAIN)
        return MAXULONG;
    if (Unit->NodeCount)
        return Unit->Nodes[Unit->NodeCount - 1];

    if (!UsbAudioGetSources(Unit, &Sources))
        return MAXULONG;
    Source = UsbAudioFindUnit(DeviceExtension, Sources[0]);
    return Source ? UsbAudioGetOutputNode(DeviceExtension, Source, Depth + 1) : MAXULONG;
}

/* The controls of a feature unit for its master channel and each logical channel */
static
VOID
UsbAudioReadFeatureControls(
    _In_ PNODE_CONTEXT Unit)
{
    PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR Feature = (PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR)Unit->Descriptor;
    ULONG Size = Feature->bControlSize, Entries, Index, Pair, Bits;
    PUCHAR Control;

    /*
     * 2.0 has no bControlSize: each channel has four bytes from byte 5 on, two bits a control,
     * and a pair that is not zero means the control is there.
     */
    if (Unit->Uac2)
    {
        Entries = (Feature->bLength - 6) / 4;
        Unit->Channels = Entries ? min(Entries - 1, (ULONG)USBAUDIO_MAX_CHANNELS) : 0;
        for (Index = 0; Index < min(Entries, (ULONG)USBAUDIO_MAX_CHANNELS + 1); Index++)
        {
            Control = (PUCHAR)Feature + 5 + Index * 4;
            Bits = Control[0] | (Control[1] << 8) | (Control[2] << 16) | ((ULONG)Control[3] << 24);
            Unit->Controls[Index] = 0;
            for (Pair = 0; Pair < USB_AUDIO_FEATURE_CONTROLS; Pair++)
            {
                if ((Bits >> (Pair * 2)) & 3)
                    Unit->Controls[Index] |= (USHORT)(1 << Pair);
            }
        }
        return;
    }

    if (!Size || Feature->bLength < FIELD_OFFSET(USB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR, bmaControls) + Size)
        return;

    /* bmaControls is followed by iFeature */
    Entries = (Feature->bLength - FIELD_OFFSET(USB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR, bmaControls) - 1) / Size;
    Unit->Channels = Entries ? min(Entries - 1, (ULONG)USBAUDIO_MAX_CHANNELS) : 0;

    for (Index = 0; Index < min(Entries, (ULONG)USBAUDIO_MAX_CHANNELS + 1); Index++)
    {
        Control = &Feature->bmaControls[Index * Size];
        Unit->Controls[Index] = Control[0] | ((Size > 1) ? (Control[1] << 8) : 0);
    }
}

/*
 * Whether a processing or extension unit has its enable control: bit 0 of bmControls, which
 * follows the sources, bNrChannels, wChannelConfig, iChannelNames and bControlSize.
 */
static
VOID
UsbAudioReadProcessControls(
    _In_ PNODE_CONTEXT Unit)
{
    PUCHAR Descriptor = (PUCHAR)Unit->Descriptor;
    ULONG Length = Unit->Descriptor->bLength;
    ULONG Offset = FIELD_OFFSET(USB_AUDIO_CONTROL_PROCESSING_UNIT_DESCRIPTOR, bNrInPins);

    if (Offset >= Length)
        return;

    /* 2.0 has a four byte channel config and no bControlSize; its enable control is bits 1..0 */
    if (Unit->Uac2)
    {
        Offset += 1 + Descriptor[Offset] + 6;
        if (Offset < Length)
            Unit->HasEnable = (Descriptor[Offset] & 3) != 0;
        return;
    }

    Offset += 1 + Descriptor[Offset] + 4;
    if (Offset + 1 < Length && Descriptor[Offset])
        Unit->HasEnable = (Descriptor[Offset + 1] & 1) != 0;
}

/* Whether the master or any logical channel of a feature unit has a control */
static
BOOLEAN
UsbAudioHasControl(
    _In_ PNODE_CONTEXT Unit,
    _In_ USHORT Control)
{
    ULONG Channel;

    for (Channel = 0; Channel <= Unit->Channels; Channel++)
    {
        if (Unit->Controls[Channel] & Control)
            return TRUE;
    }
    return FALSE;
}

/* Collects every terminal and unit of the control interface */
static
NTSTATUS
UsbAudioCollectUnits(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    ULONG Count = 0, Index;
    UCHAR InterfaceNumber;
    BOOLEAN Version2;

    Header = UsbAudioGetControlHeader(DeviceExtension->ConfigurationDescriptor, &InterfaceNumber);
    if (!Header)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Version2 = UsbAudioIsVersion2Header(Header);

    for (Descriptor = UsbAudioNextUnit(DeviceExtension->ConfigurationDescriptor, Header, Header);
         Descriptor;
         Descriptor = UsbAudioNextUnit(DeviceExtension->ConfigurationDescriptor, Header, Descriptor))
    {
        Count++;
    }

    DeviceExtension->NodeContext = AllocFunction(max(Count, 1UL) * sizeof(NODE_CONTEXT));
    if (!DeviceExtension->NodeContext)
        return STATUS_INSUFFICIENT_RESOURCES;

    Count = 0;
    for (Descriptor = UsbAudioNextUnit(DeviceExtension->ConfigurationDescriptor, Header, Header);
         Descriptor;
         Descriptor = UsbAudioNextUnit(DeviceExtension->ConfigurationDescriptor, Header, Descriptor))
    {
        /* Terminals and units all carry their id right after the subtype */
        DeviceExtension->NodeContext[Count].Descriptor = Descriptor;
        DeviceExtension->NodeContext[Count].Uac2 = Version2;
        DeviceExtension->NodeContext[Count].Subtype = Version2 ? UsbAudio2MapSubtype(((PUCHAR)Descriptor)[2]) : ((PUCHAR)Descriptor)[2];
        DeviceExtension->NodeContext[Count].Id = ((PUCHAR)Descriptor)[3];
        for (Index = 0; Index < USB_AUDIO_FEATURE_CONTROLS; Index++)
            DeviceExtension->NodeContext[Count].ControlNodes[Index] = MAXULONG;
        if (DeviceExtension->NodeContext[Count].Subtype == USB_AUDIO_FEATURE_UNIT)
            UsbAudioReadFeatureControls(&DeviceExtension->NodeContext[Count]);
        else if (DeviceExtension->NodeContext[Count].Subtype == USB_AUDIO_PROCESSING_UNIT ||
                 DeviceExtension->NodeContext[Count].Subtype == USB_AUDIO_EXTENSION_UNIT)
            UsbAudioReadProcessControls(&DeviceExtension->NodeContext[Count]);
        Count++;
    }
    DeviceExtension->NodeContextCount = Count;
    return STATUS_SUCCESS;
}

static
VOID
UsbAudioAddNode(
    _Inout_ PKSFILTER_DESCRIPTOR FilterDescriptor,
    _Inout_ PNODE_CONTEXT Unit,
    _In_ const GUID *Type,
    _In_opt_ const KSAUTOMATION_TABLE *AutomationTable)
{
    PKSNODE_DESCRIPTOR Node = (PKSNODE_DESCRIPTOR)&FilterDescriptor->NodeDescriptors[FilterDescriptor->NodeDescriptorsCount];

    if (Unit->NodeCount == USBAUDIO_MAX_UNIT_NODES)
        return;

    Node->Type = Type;
    Node->Name = Type;
    Node->AutomationTable = AutomationTable;
    Unit->Nodes[Unit->NodeCount++] = FilterDescriptor->NodeDescriptorsCount++;
}

static
VOID
UsbAudioAddConnection(
    _Inout_ PKSFILTER_DESCRIPTOR FilterDescriptor,
    _In_ ULONG FromNode,
    _In_ ULONG FromPin,
    _In_ ULONG ToNode,
    _In_ ULONG ToPin)
{
    PKSTOPOLOGY_CONNECTION Connection = (PKSTOPOLOGY_CONNECTION)&FilterDescriptor->Connections[FilterDescriptor->ConnectionsCount++];

    Connection->FromNode = FromNode;
    Connection->FromNodePin = FromPin;
    Connection->ToNode = ToNode;
    Connection->ToNodePin = ToPin;
}

/* The filter pin of a terminal: streaming terminals come first, then the bridge pins */
static
ULONG
UsbAudioGetTerminalPin(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Terminal)
{
    BOOLEAN Streaming = UsbAudioIsStreamingTerminal(Terminal);
    ULONG Streams = 0, Bridges = 0, Index;
    PNODE_CONTEXT Unit;

    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        Unit = &DeviceExtension->NodeContext[Index];
        if (Unit->Subtype != USB_AUDIO_INPUT_TERMINAL && Unit->Subtype != USB_AUDIO_OUTPUT_TERMINAL)
            continue;
        if (Unit == Terminal)
            break;
        if (UsbAudioIsStreamingTerminal(Unit))
            Streams++;
        else
            Bridges++;
    }

    if (Streaming)
        return Streams;

    /* bridge pins follow every streaming pin */
    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        Unit = &DeviceExtension->NodeContext[Index];
        if ((Unit->Subtype == USB_AUDIO_INPUT_TERMINAL || Unit->Subtype == USB_AUDIO_OUTPUT_TERMINAL) &&
            UsbAudioIsStreamingTerminal(Unit) && Unit > Terminal)
        {
            Streams++;
        }
    }
    return Streams + Bridges;
}

/*
 * A playback path runs stream pin, SRC, feature nodes, DAC, bridge pin; a recording path
 * runs bridge pin, ADC, feature nodes, SRC, stream pin. Terminals off those paths get no node.
 */
NTSTATUS
BuildUSBAudioFilterTopology(
    PKSDEVICE Device,
    PKSFILTER_DESCRIPTOR FilterDescriptor)
{
    PDEVICE_EXTENSION DeviceExtension = Device->Context;
    PNODE_CONTEXT Unit, Source;
    const UCHAR *Sources;
    ULONG Index, Input, Count, Inputs, MaxNodes = 0, MaxConnections = 0, From;
    NTSTATUS Status;

    Status = UsbAudioCollectUnits(DeviceExtension);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        Unit = &DeviceExtension->NodeContext[Index];
        MaxNodes += 1 + ARRAYSIZE(UsbAudioControls);
        MaxConnections += 2 + UsbAudioGetSources(Unit, &Sources);
    }

    FilterDescriptor->NodeDescriptors = AllocFunction(max(MaxNodes, 1UL) * sizeof(KSNODE_DESCRIPTOR));
    FilterDescriptor->Connections = AllocFunction(max(MaxConnections, 1UL) * sizeof(KSTOPOLOGY_CONNECTION));
    if (!FilterDescriptor->NodeDescriptors || !FilterDescriptor->Connections)
        return STATUS_INSUFFICIENT_RESOURCES;
    FilterDescriptor->NodeDescriptorSize = sizeof(KSNODE_DESCRIPTOR);

    /* Nodes first, in descriptor order */
    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        Unit = &DeviceExtension->NodeContext[Index];
        switch (Unit->Subtype)
        {
            case USB_AUDIO_INPUT_TERMINAL:
                if (UsbAudioIsStreamingTerminal(Unit))
                    UsbAudioAddNode(FilterDescriptor, Unit, &KSNODETYPE_SRC, NULL);
                else if (UsbAudioIsRecorded(DeviceExtension, Unit))
                    UsbAudioAddNode(FilterDescriptor, Unit, &KSNODETYPE_ADC, NULL);
                break;

            case USB_AUDIO_OUTPUT_TERMINAL:
                if (UsbAudioIsStreamingTerminal(Unit))
                    UsbAudioAddNode(FilterDescriptor, Unit, &KSNODETYPE_SRC, NULL);
                else if (UsbAudioComesFromStream(DeviceExtension, Unit, 0))
                    UsbAudioAddNode(FilterDescriptor, Unit, &KSNODETYPE_DAC, NULL);
                break;

            case USB_AUDIO_FEATURE_UNIT:
                /* One node per control a property stands behind, in the order of their bits */
                for (Count = 0; Count < ARRAYSIZE(UsbAudioControls); Count++)
                {
                    if (!UsbAudioHasControl(Unit, 1 << UsbAudioControls[Count].Bit))
                        continue;
                    Unit->ControlNodes[UsbAudioControls[Count].Bit] = FilterDescriptor->NodeDescriptorsCount;
                    UsbAudioAddNode(FilterDescriptor, Unit, UsbAudioControls[Count].Type, UsbAudioControls[Count].AutomationTable);
                    ((PKSNODE_DESCRIPTOR)FilterDescriptor->NodeDescriptors)[FilterDescriptor->NodeDescriptorsCount - 1].Name = UsbAudioControls[Count].Name;
                }
                break;

            case USB_AUDIO_MIXER_UNIT:
                UsbAudioAddNode(FilterDescriptor, Unit, &KSNODETYPE_SUM, NULL);
                break;

            case USB_AUDIO_SELECTOR_UNIT:
                UsbAudioAddNode(FilterDescriptor, Unit, &KSNODETYPE_MUX, NULL);
                break;

            case USB_AUDIO_PROCESSING_UNIT:
            case USB_AUDIO_EXTENSION_UNIT:
                UsbAudioAddNode(FilterDescriptor, Unit, UsbAudioGetProcessNodeType(Unit), &EnableAutomationTable);
                break;
        }
    }

    /* Then the connections into each unit, and between the nodes of one unit */
    for (Index = 0; Index < DeviceExtension->NodeContextCount; Index++)
    {
        Unit = &DeviceExtension->NodeContext[Index];
        if (!Unit->NodeCount)
            continue;

        for (Count = 1; Count < Unit->NodeCount; Count++)
            UsbAudioAddConnection(FilterDescriptor, Unit->Nodes[Count - 1], KSNODEPIN_STANDARD_OUT, Unit->Nodes[Count], KSNODEPIN_STANDARD_IN);

        if (Unit->Subtype == USB_AUDIO_INPUT_TERMINAL)
        {
            UsbAudioAddConnection(FilterDescriptor, KSFILTER_NODE, UsbAudioGetTerminalPin(DeviceExtension, Unit), Unit->Nodes[0], KSNODEPIN_STANDARD_IN);
            continue;
        }

        Inputs = 0;
        Count = UsbAudioGetSources(Unit, &Sources);
        for (Input = 0; Input < Count; Input++)
        {
            Source = UsbAudioFindUnit(DeviceExtension, Sources[Input]);
            if (!Source || !UsbAudioIsWantedInput(DeviceExtension, Unit, Sources[Input]))
                continue;
            From = UsbAudioGetOutputNode(DeviceExtension, Source, 0);
            if (From == MAXULONG)
                continue;
            UsbAudioAddConnection(FilterDescriptor, From, KSNODEPIN_STANDARD_OUT, Unit->Nodes[0], KSNODEPIN_STANDARD_IN + Inputs);
            Inputs++;
        }

        if (Unit->Subtype == USB_AUDIO_OUTPUT_TERMINAL)
            UsbAudioAddConnection(FilterDescriptor, Unit->Nodes[0], KSNODEPIN_STANDARD_OUT, KSFILTER_NODE, UsbAudioGetTerminalPin(DeviceExtension, Unit));
    }

    DPRINT1("BuildUSBAudioFilterTopology %lu units, %lu nodes, %lu connections\n",
            DeviceExtension->NodeContextCount, FilterDescriptor->NodeDescriptorsCount, FilterDescriptor->ConnectionsCount);
    return STATUS_SUCCESS;
}

/* The feature unit and control behind a node */
static
PNODE_CONTEXT
UsbAudioGetFeatureForNode(
    _In_ PIRP Irp,
    _In_ ULONG NodeId,
    _Out_ PDEVICE_EXTENSION *DeviceExtension,
    _Out_ const USBAUDIO_CONTROL **Control)
{
    PKSFILTER Filter = KsGetFilterFromIrp(Irp);
    PNODE_CONTEXT Unit;
    ULONG Index, Bit;

    *DeviceExtension = NULL;
    *Control = NULL;
    if (!Filter)
        return NULL;

    *DeviceExtension = ((PFILTER_CONTEXT)Filter->Context)->DeviceExtension;
    for (Index = 0; Index < (*DeviceExtension)->NodeContextCount; Index++)
    {
        Unit = &(*DeviceExtension)->NodeContext[Index];
        if (Unit->Subtype != USB_AUDIO_FEATURE_UNIT)
            continue;
        for (Bit = 0; Bit < USB_AUDIO_FEATURE_CONTROLS; Bit++)
        {
            if (Unit->ControlNodes[Bit] == NodeId)
            {
                *Control = UsbAudioGetControl(Bit);
                return *Control ? Unit : NULL;
            }
        }
    }
    return NULL;
}

/* Channels a control is reported with: each logical channel if any has it, else one master */
static
ULONG
UsbAudioGetControlChannels(
    _In_ PNODE_CONTEXT Unit,
    _In_ USHORT Control)
{
    ULONG Channel;

    for (Channel = 1; Channel <= Unit->Channels; Channel++)
    {
        if (Unit->Controls[Channel] & Control)
            return Unit->Channels;
    }
    return 1;
}

/* The USB channel a KS channel index goes to, or MAXULONG past the last one */
static
ULONG
UsbAudioMapChannel(
    _In_ PNODE_CONTEXT Unit,
    _In_ USHORT Control,
    _In_ ULONG Channel)
{
    if (Channel >= UsbAudioGetControlChannels(Unit, Control))
        return MAXULONG;
    if (Channel + 1 <= Unit->Channels && (Unit->Controls[Channel + 1] & Control))
        return Channel + 1;
    if (Unit->Controls[0] & Control)
        return 0;
    return MAXULONG;
}

/* Reads or writes one control of a feature unit, as a sign extended value */
static
NTSTATUS
UsbAudioFeatureRequest(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ const USBAUDIO_CONTROL *Control,
    _In_ UCHAR Request,
    _In_ ULONG Channel,
    _Inout_ PLONG Value)
{
    UCHAR Bytes[2];
    NTSTATUS Status;

    /* 2.0 answers MIN, MAX and RES together, with one RANGE request */
    if (Unit->Uac2 && Request != USB_AUDIO_GET_CUR && Request != USB_AUDIO_SET_CUR)
    {
        return UsbAudio2GetRangeValue(DeviceExtension,
                                      (USHORT)(((Control->Bit + 1) << 8) | Channel),
                                      (USHORT)((Unit->Id << 8) | DeviceExtension->ControlInterface),
                                      Control->Bytes,
                                      Request,
                                      Value);
    }

    Bytes[0] = (UCHAR)*Value;
    Bytes[1] = (UCHAR)(*Value >> 8);
    Status = UsbAudioClassRequest(DeviceExtension,
                                  URB_FUNCTION_CLASS_INTERFACE,
                                  Request,
                                  (USHORT)(((Control->Bit + 1) << 8) | Channel),
                                  (USHORT)((Unit->Id << 8) | DeviceExtension->ControlInterface),
                                  Bytes,
                                  Control->Bytes,
                                  (Request & 0x80) != 0);
    if (NT_SUCCESS(Status) && (Request & 0x80))
        *Value = (Control->Bytes == 2) ? (LONG)(SHORT)(Bytes[0] | (Bytes[1] << 8)) : (LONG)(CHAR)Bytes[0];
    return Status;
}

/* Fills as much of a basic support answer as the caller's buffer holds */
static
NTSTATUS
UsbAudioBasicSupport(
    _In_ PIRP Irp,
    _Out_ PVOID Data,
    _In_ ULONG VarType,
    _In_reads_opt_(Members) const KSPROPERTY_STEPPING_LONG *Ranges,
    _In_ ULONG Members)
{
    ULONG Length = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG Full = sizeof(KSPROPERTY_DESCRIPTION) + (Members ? sizeof(KSPROPERTY_MEMBERSHEADER) + Members * sizeof(KSPROPERTY_STEPPING_LONG) : 0);
    PKSPROPERTY_DESCRIPTION Description = (PKSPROPERTY_DESCRIPTION)Data;
    PKSPROPERTY_MEMBERSHEADER Header;
    ULONG Access = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;

    if (Length < sizeof(KSPROPERTY_DESCRIPTION))
    {
        if (Length < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        *(PULONG)Data = Access;
        Irp->IoStatus.Information = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    Description->AccessFlags = Access;
    Description->DescriptionSize = Full;
    Description->PropTypeSet.Set = KSPROPTYPESETID_General;
    Description->PropTypeSet.Id = VarType;
    Description->PropTypeSet.Flags = 0;
    Description->MembersListCount = Members ? 1 : 0;
    Description->Reserved = 0;
    Irp->IoStatus.Information = sizeof(KSPROPERTY_DESCRIPTION);

    if (!Members || Length < Full)
        return STATUS_SUCCESS;

    /* A single channel does not claim to be multichannel; the mixer insists on two or more then */
    Header = (PKSPROPERTY_MEMBERSHEADER)(Description + 1);
    Header->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
    Header->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
    Header->MembersCount = Members;
    Header->Flags = (Members > 1) ? KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL : 0;
    RtlCopyMemory(Header + 1, Ranges, Members * sizeof(KSPROPERTY_STEPPING_LONG));
    Irp->IoStatus.Information = Full;
    return STATUS_SUCCESS;
}

/* Range of a level control in KS units, 1/65536 dB, or of a switch, 0 to 1 */
static
VOID
UsbAudioGetControlRange(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PNODE_CONTEXT Unit,
    _In_ const USBAUDIO_CONTROL *Control,
    _In_ ULONG UsbChannel,
    _Out_ PKSPROPERTY_STEPPING_LONG Range)
{
    LONG Minimum, Maximum, Resolution = 1;

    Range->Reserved = 0;
    if (!Control->Scale)
    {
        Range->SteppingDelta = 1;
        Range->Bounds.SignedMinimum = 0;
        Range->Bounds.SignedMaximum = 1;
        return;
    }

    Minimum = (Control->Bytes == 2) ? -96 * 256 : -48;
    Maximum = 0;
    UsbAudioFeatureRequest(DeviceExtension, Unit, Control, USB_AUDIO_GET_MIN, UsbChannel, &Minimum);
    UsbAudioFeatureRequest(DeviceExtension, Unit, Control, USB_AUDIO_GET_MAX, UsbChannel, &Maximum);
    UsbAudioFeatureRequest(DeviceExtension, Unit, Control, USB_AUDIO_GET_RES, UsbChannel, &Resolution);

    /* The minimum may be the silence value, which is not a level */
    if (Control->Bytes == 2 && Minimum == USB_AUDIO_VOLUME_SILENCE)
        Minimum++;
    if (Maximum < Minimum)
        Maximum = Minimum;

    Range->SteppingDelta = (ULONG)max(Resolution & ((Control->Bytes == 2) ? 0xFFFF : 0xFF), 1) * Control->Scale;
    Range->Bounds.SignedMinimum = Minimum * Control->Scale;
    Range->Bounds.SignedMaximum = Maximum * Control->Scale;
}

NTSTATUS
NTAPI
FilterAudioControlHandler(
    IN PIRP Irp,
    IN PKSIDENTIFIER  Request,
    IN OUT PVOID  Data)
{
    PKSNODEPROPERTY_AUDIO_CHANNEL Property = (PKSNODEPROPERTY_AUDIO_CHANNEL)Request;
    KSPROPERTY_STEPPING_LONG Ranges[USBAUDIO_MAX_CHANNELS];
    const USBAUDIO_CONTROL *Control;
    PDEVICE_EXTENSION DeviceExtension;
    PNODE_CONTEXT Unit;
    ULONG UsbChannel, Channels, Index;
    USHORT Mask;
    LONG Value;
    NTSTATUS Status;

    Unit = UsbAudioGetFeatureForNode(Irp, Property->NodeProperty.NodeId, &DeviceExtension, &Control);
    if (!Unit)
        return STATUS_INVALID_PARAMETER;
    Mask = (USHORT)(1 << Control->Bit);

    /* Basic support needs only the node, not the channel */
    if (Request->Flags & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        Channels = min(UsbAudioGetControlChannels(Unit, Mask), (ULONG)USBAUDIO_MAX_CHANNELS);
        for (Index = 0; Index < Channels; Index++)
        {
            UsbChannel = UsbAudioMapChannel(Unit, Mask, Index);
            UsbAudioGetControlRange(DeviceExtension, Unit, Control, (UsbChannel == MAXULONG) ? 0 : UsbChannel, &Ranges[Index]);
        }
        return UsbAudioBasicSupport(Irp, Data, Control->Scale ? VT_I4 : VT_BOOL, Ranges, Channels);
    }

    UsbChannel = UsbAudioMapChannel(Unit, Mask, Property->Channel);
    if (UsbChannel == MAXULONG)
        return (Request->Flags & KSPROPERTY_TYPE_GET) ? STATUS_INVALID_DEVICE_REQUEST : STATUS_INVALID_PARAMETER;

    if (Request->Flags & KSPROPERTY_TYPE_GET)
    {
        Value = 0;
        Status = UsbAudioFeatureRequest(DeviceExtension, Unit, Control, USB_AUDIO_GET_CUR, UsbChannel, &Value);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Control->Scale)
        {
            UsbAudioGetControlRange(DeviceExtension, Unit, Control, UsbChannel, &Ranges[0]);
            if (Control->Bytes == 2 && Value == USB_AUDIO_VOLUME_SILENCE)
                *(PLONG)Data = Ranges[0].Bounds.SignedMinimum;
            else
                *(PLONG)Data = Value * Control->Scale;
        }
        else
        {
            *(PBOOL)Data = Value ? TRUE : FALSE;
        }
        Irp->IoStatus.Information = sizeof(LONG);
        return STATUS_SUCCESS;
    }

    /* Levels outside what the device takes are brought into its range */
    if (Control->Scale)
    {
        UsbAudioGetControlRange(DeviceExtension, Unit, Control, UsbChannel, &Ranges[0]);
        Value = min(max(*(PLONG)Data, Ranges[0].Bounds.SignedMinimum), Ranges[0].Bounds.SignedMaximum) / Control->Scale;
    }
    else
    {
        Value = *(PBOOL)Data ? 1 : 0;
    }

    Status = UsbAudioFeatureRequest(DeviceExtension, Unit, Control, USB_AUDIO_SET_CUR, UsbChannel, &Value);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = sizeof(LONG);
    return Status;
}

/* Reads or switches the enable control of a processing or extension unit */
NTSTATUS
NTAPI
FilterNodeEnableHandler(
    IN PIRP Irp,
    IN PKSIDENTIFIER Request,
    IN OUT PVOID Data)
{
    PKSNODEPROPERTY Property = (PKSNODEPROPERTY)Request;
    PKSFILTER Filter = KsGetFilterFromIrp(Irp);
    PDEVICE_EXTENSION DeviceExtension;
    PNODE_CONTEXT Unit = NULL;
    ULONG Index;
    UCHAR Value;
    NTSTATUS Status;

    if (!Filter)
        return STATUS_INVALID_PARAMETER;

    DeviceExtension = ((PFILTER_CONTEXT)Filter->Context)->DeviceExtension;
    for (Index = 0; Index < DeviceExtension->NodeContextCount && !Unit; Index++)
    {
        if ((DeviceExtension->NodeContext[Index].Subtype == USB_AUDIO_PROCESSING_UNIT ||
             DeviceExtension->NodeContext[Index].Subtype == USB_AUDIO_EXTENSION_UNIT) &&
            DeviceExtension->NodeContext[Index].NodeCount &&
            DeviceExtension->NodeContext[Index].Nodes[0] == Property->NodeId)
        {
            Unit = &DeviceExtension->NodeContext[Index];
        }
    }
    if (!Unit)
        return STATUS_INVALID_PARAMETER;

    /* A unit that cannot be switched reads as off and refuses to be switched */
    if (Request->Flags & KSPROPERTY_TYPE_GET)
    {
        Value = 0;
        if (Unit->HasEnable)
        {
            Status = UsbAudioClassRequest(DeviceExtension,
                                          URB_FUNCTION_CLASS_INTERFACE,
                                          USB_AUDIO_GET_CUR,
                                          USB_AUDIO_ENABLE_CONTROL << 8,
                                          (USHORT)((Unit->Id << 8) | DeviceExtension->ControlInterface),
                                          &Value,
                                          sizeof(Value),
                                          TRUE);
            if (!NT_SUCCESS(Status))
                return Status;
        }
        *(PBOOLEAN)Data = Value ? TRUE : FALSE;
        Irp->IoStatus.Information = sizeof(BOOLEAN);
        return STATUS_SUCCESS;
    }

    if (!Unit->HasEnable)
        return STATUS_INVALID_PARAMETER;

    Value = *(PBOOLEAN)Data ? 1 : 0;
    Status = UsbAudioClassRequest(DeviceExtension,
                                  URB_FUNCTION_CLASS_INTERFACE,
                                  USB_AUDIO_SET_CUR,
                                  USB_AUDIO_ENABLE_CONTROL << 8,
                                  (USHORT)((Unit->Id << 8) | DeviceExtension->ControlInterface),
                                  &Value,
                                  sizeof(Value),
                                  FALSE);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = sizeof(BOOLEAN);
    return Status;
}

NTSTATUS
NTAPI
USBAudioFilterCreate(
    PKSFILTER Filter,
    PIRP Irp)
{
    PKSFILTERFACTORY FilterFactory;
    PKSDEVICE Device;
    PFILTER_CONTEXT FilterContext;

    FilterFactory = KsGetParent(Filter);
    if (FilterFactory == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    Device = KsGetParent(FilterFactory);
    if (Device == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    /* alloc filter context */
    FilterContext = AllocFunction(sizeof(FILTER_CONTEXT));
    if (FilterContext == NULL)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init context */
    FilterContext->DeviceExtension = Device->Context;
    FilterContext->LowerDevice = Device->NextDeviceObject;
    Filter->Context = FilterContext;

    DPRINT("USBAudioFilterCreate FilterContext %p LowerDevice %p DeviceExtension %p\n", FilterContext, FilterContext->LowerDevice, FilterContext->DeviceExtension);
    KsAddItemToObjectBag(Filter->Bag, FilterContext, ExFreePool);
    return STATUS_SUCCESS;
}

LPGUID
UsbAudioGetPinCategoryFromTerminalDescriptor(
    IN PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR TerminalDescriptor)
{
    switch (TerminalDescriptor->wTerminalType)
    {
        case USB_AUDIO_MICROPHONE_TERMINAL_TYPE:
            return &NodeTypeMicrophone;
        case USB_AUDIO_DESKTOP_MICROPHONE_TERMINAL_TYPE:
            return &NodeTypeDesktopMicrophone;
        case USB_AUDIO_PERSONAL_MICROPHONE_TERMINAL_TYPE:
            return &NodeTypePersonalMicrophone;
        case USB_AUDIO_OMMNI_MICROPHONE_TERMINAL_TYPE:
            return &NodeTypeOmmniMicrophone;
        case USB_AUDIO_ARRAY_MICROPHONE_TERMINAL_TYPE:
            return &NodeTypeArrayMicrophone;
        case USB_AUDIO_ARRAY_PROCESSING_MICROPHONE_TERMINAL_TYPE:
            return &NodeTypeProcessingArrayMicrophone;
        case USB_AUDIO_SPEAKER_TERMINAL_TYPE:
            return &NodeTypeSpeaker;
        case USB_HEADPHONES_SPEAKER_TERMINAL_TYPE:
            return &NodeTypeHeadphonesSpeaker;
        case USB_AUDIO_HMDA_TERMINAL_TYPE:
            return &NodeTypeHMDA;
        case USB_AUDIO_DESKTOP_SPEAKER_TERMINAL_TYPE:
            return &NodeTypeDesktopSpeaker;
        case USB_AUDIO_ROOM_SPEAKER_TERMINAL_TYPE:
            return &NodeTypeRoomSpeaker;
        case USB_AUDIO_COMMUNICATION_SPEAKER_TERMINAL_TYPE:
            return &NodeTypeCommunicationSpeaker;
        case USB_AUDIO_SUBWOOFER_TERMINAL_TYPE:
            return &NodeTypeSubwoofer;
        case USB_AUDIO_LINE_CONNECTOR_TERMINAL_TYPE:
            return &NodeTypeLineConnector;
        case USB_AUDIO_SPDIF_TERMINAL_TYPE:
            return &NodeTypeSpdif;

        case USB_AUDIO_HEADSET_TERMINAL_TYPE:
            /* the headset's microphone where sound comes in, its earpieces where it goes out */
            return (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_INPUT_TERMINAL) ? &NodeTypeMicrophone : &NodeTypeHeadphonesSpeaker;

        case USB_AUDIO_STREAMING_TERMINAL_TYPE:
            return (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_OUTPUT_TERMINAL) ? &NodeTypeCapture : &NodeTypePlayback;
    }

    /* anything else is an analog jack of some kind */
    if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_INPUT_TERMINAL)
        return (TerminalDescriptor->wTerminalType & 0xFF00) == 0x0200 ? &NodeTypeMicrophone : &NodeTypeAnalogConnector;
    return (TerminalDescriptor->wTerminalType & 0xFF00) == 0x0300 ? &NodeTypeSpeaker : &NodeTypeAnalogConnector;
}

/* The data ranges of a streaming terminal, one per alternate setting that carries it */
static
NTSTATUS
UsbAudioGetDataRanges(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ UCHAR TerminalId,
    _Out_ const PKSDATARANGE **DataRanges,
    _Out_ PULONG DataRangesCount)
{
    PKSDATARANGE *Ranges;
    ULONG Index, Count = 0;

    for (Index = 0; Index < DeviceExtension->FormatCount; Index++)
    {
        if (DeviceExtension->Formats[Index].TerminalLink == TerminalId)
            Count++;
    }

    Ranges = AllocFunction(max(Count, 1UL) * sizeof(PKSDATARANGE));
    if (!Ranges)
        return STATUS_INSUFFICIENT_RESOURCES;

    Count = 0;
    for (Index = 0; Index < DeviceExtension->FormatCount; Index++)
    {
        if (DeviceExtension->Formats[Index].TerminalLink == TerminalId)
            Ranges[Count++] = (PKSDATARANGE)&DeviceExtension->Formats[Index].DataRange;
    }

    *DataRanges = (const PKSDATARANGE *)Ranges;
    *DataRangesCount = Count;
    return STATUS_SUCCESS;
}

NTSTATUS
USBAudioPinBuildDescriptors(
    PKSDEVICE Device,
    PKSPIN_DESCRIPTOR_EX *PinDescriptors,
    PULONG PinDescriptorsCount,
    PULONG PinDescriptorSize,
    PBOOLEAN Renders,
    PBOOLEAN Captures)
{
    PDEVICE_EXTENSION DeviceExtension;
    PKSPIN_DESCRIPTOR_EX Pins;
    ULONG TotalTerminalDescriptorCount = 0;
    ULONG NonStreamingTerminalDescriptorCount = 0;
    ULONG StreamingCount;
    ULONG MidiCount;
    ULONG Index = 0;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR TerminalDescriptor = NULL;
    NTSTATUS Status;

    /* get device extension */
    DeviceExtension = Device->Context;
    *Renders = FALSE;
    *Captures = FALSE;

    CountTerminalUnits(DeviceExtension->ConfigurationDescriptor, &NonStreamingTerminalDescriptorCount, &TotalTerminalDescriptorCount);
    StreamingCount = TotalTerminalDescriptorCount - NonStreamingTerminalDescriptorCount;
    DPRINT("TotalTerminalDescriptorCount %lu NonStreamingTerminalDescriptorCount %lu\n", TotalTerminalDescriptorCount, NonStreamingTerminalDescriptorCount);

    /* MIDI pins, one per cable, follow the terminals */
    MidiCount = UsbMidiGetPinCount(DeviceExtension);

    /* allocate pins */
    Pins = AllocFunction(sizeof(KSPIN_DESCRIPTOR_EX) * max(TotalTerminalDescriptorCount + MidiCount, 1UL));
    if (!Pins)
    {
        /* no memory*/
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Index = 0; Index < TotalTerminalDescriptorCount + MidiCount; Index++)
    {
        Pins[Index].PinDescriptor.InterfacesCount = 1;
        Pins[Index].PinDescriptor.Interfaces = &StandardPinInterface;
        Pins[Index].PinDescriptor.MediumsCount = 1;
        Pins[Index].PinDescriptor.Mediums = &StandardPinMedium;
        if (Index >= TotalTerminalDescriptorCount)
            continue;

        if (Index < StreamingCount)
        {
            /* irp sink pins*/
            TerminalDescriptor = UsbAudioGetTerminalByIndex(DeviceExtension->ConfigurationDescriptor, TRUE, Index);
            ASSERT(TerminalDescriptor != NULL);

            Pins[Index].Dispatch = &UsbAudioPinDispatch;
            Pins[Index].PinDescriptor.Category = UsbAudioGetPinCategoryFromTerminalDescriptor(TerminalDescriptor);
            Status = UsbAudioGetDataRanges(DeviceExtension, TerminalDescriptor->bTerminalID,
                                           &Pins[Index].PinDescriptor.DataRanges, &Pins[Index].PinDescriptor.DataRangesCount);
            if (!NT_SUCCESS(Status))
                return Status;

            /* A streaming output terminal is where recorded audio leaves the device */
            if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_OUTPUT_TERMINAL)
            {
                Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
                Pins[Index].PinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
                *Captures |= Pins[Index].PinDescriptor.DataRangesCount != 0;
            }
            else
            {
                Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
                Pins[Index].PinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
                *Renders |= Pins[Index].PinDescriptor.DataRangesCount != 0;
            }

            Pins[Index].Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY;
            Pins[Index].IntersectHandler = UsbAudioPinDataIntersect;
            Pins[Index].InstancesPossible = 1;
        }
        else
        {
            /* bridge pins */
            TerminalDescriptor = UsbAudioGetTerminalByIndex(DeviceExtension->ConfigurationDescriptor, FALSE, Index - StreamingCount);
            ASSERT(TerminalDescriptor != NULL);

            Pins[Index].PinDescriptor.DataRanges = BridgePinAudioFormats;
            Pins[Index].PinDescriptor.DataRangesCount = 1;
            Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_BRIDGE;
            Pins[Index].PinDescriptor.Category = UsbAudioGetPinCategoryFromTerminalDescriptor(TerminalDescriptor);
            Pins[Index].PinDescriptor.DataFlow = (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_INPUT_TERMINAL) ?
                                                 KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
        }
    }

    UsbMidiInitPinDescriptors(DeviceExtension, &Pins[TotalTerminalDescriptorCount], TotalTerminalDescriptorCount, MidiCount);

    *PinDescriptors = Pins;
    *PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);
    *PinDescriptorsCount = TotalTerminalDescriptorCount + MidiCount;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBAudioGetDescriptor(
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR DescriptorType,
    IN ULONG DescriptorLength,
    IN UCHAR DescriptorIndex,
    IN LANGID LanguageId,
    OUT PVOID *OutDescriptor)
{
    PURB Urb;
    NTSTATUS Status;
    PVOID Descriptor;

    /* sanity checks */
    ASSERT(DeviceObject);
    ASSERT(OutDescriptor);
    ASSERT(DescriptorLength);

    //
    // first allocate descriptor buffer
    //
    Descriptor = AllocFunction(DescriptorLength);
    if (!Descriptor)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* allocate urb */
    Urb = (PURB)AllocFunction(sizeof(URB));
    if (!Urb)
    {
        /* no memory */
        FreeFunction(Descriptor);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* initialize urb */
    UsbBuildGetDescriptorRequest(Urb,
        sizeof(Urb->UrbControlDescriptorRequest),
        DescriptorType,
        DescriptorIndex,
        LanguageId,
        Descriptor,
        NULL,
        DescriptorLength,
        NULL);

    /* submit urb */
    Status = SubmitUrbSync(DeviceObject, Urb);

    /* free urb */
    FreeFunction(Urb);

    if (NT_SUCCESS(Status))
    {
        /* store result */
        *OutDescriptor = Descriptor;
    }
    else
    {
        /* failed */
        FreeFunction(Descriptor);
    }

    /* done */
    return Status;
}

NTSTATUS
NTAPI
USBAudioGetStringDescriptor(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG DescriptorLength,
    IN UCHAR DescriptorIndex,
    IN LANGID LanguageId,
    OUT PVOID *OutDescriptor)
{
    NTSTATUS Status;

    /* retrieve descriptor */
    Status = USBAudioGetDescriptor(DeviceObject, USB_STRING_DESCRIPTOR_TYPE, DescriptorLength, DescriptorIndex, LanguageId, OutDescriptor);
    if (!NT_SUCCESS(Status))
    {
        // failed
        return Status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
USBAudioRegCreateMediaCategoriesKey(
    IN PUNICODE_STRING Name,
    OUT PHANDLE OutHandle)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING DestinationString;
    HANDLE Handle;

    /* initialize root name*/
    RtlInitUnicodeString(&DestinationString, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\MediaCategories\\");

    /* initialize object attributes */
    InitializeObjectAttributes(&ObjectAttributes, &DestinationString, OBJ_CASE_INSENSITIVE | OBJ_OPENIF | OBJ_KERNEL_HANDLE, NULL, NULL);

    /* create the key */
    Status = ZwOpenKey(&Handle, KEY_ALL_ACCESS, &ObjectAttributes);
    if (NT_SUCCESS(Status))
    {
        /* initialize object attributes */
        InitializeObjectAttributes(&ObjectAttributes, Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Handle, NULL);

        Status = ZwCreateKey(OutHandle, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, 0, NULL);
        ZwClose(Handle);

    }
    return Status;
}

/* The product name sits in a string descriptor behind a two byte header */
static
VOID
UsbAudioPublishName(
    _In_ const GUID *NameGuid,
    _In_ PUSB_STRING_DESCRIPTOR Descriptor)
{
    WCHAR Name[MAXIMUM_USB_STRING_LENGTH + 1];
    UNICODE_STRING GuidString, ValueName;
    ULONG Length;
    HANDLE Key;

    if (Descriptor->bLength < 2 || Descriptor->bDescriptorType != USB_STRING_DESCRIPTOR_TYPE)
        return;

    Length = min((ULONG)(Descriptor->bLength - 2) / sizeof(WCHAR), (ULONG)MAXIMUM_USB_STRING_LENGTH);
    RtlCopyMemory(Name, Descriptor->bString, Length * sizeof(WCHAR));
    Name[Length] = UNICODE_NULL;

    if (!NT_SUCCESS(RtlStringFromGUID(NameGuid, &GuidString)))
        return;
    if (NT_SUCCESS(USBAudioRegCreateMediaCategoriesKey(&GuidString, &Key)))
    {
        RtlInitUnicodeString(&ValueName, L"Name");
        ZwSetValueKey(Key, &ValueName, 0, REG_SZ, Name, (Length + 1) * sizeof(WCHAR));
        ZwClose(Key);
    }
    RtlFreeUnicodeString(&GuidString);
}

NTSTATUS
USBAudioInitComponentId(
    PKSDEVICE Device,
    IN PKSCOMPONENTID ComponentId)
{
    PDEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;
    PUSB_STRING_DESCRIPTOR DescriptionBuffer;

    /* get device extension */
    DeviceExtension = Device->Context;

    /* init component id */
    ComponentId->Component = KSCOMPONENTID_USBAUDIO;
    ComponentId->Version = HIBYTE(DeviceExtension->DeviceDescriptor->bcdDevice);
    ComponentId->Revision = LOBYTE(DeviceExtension->DeviceDescriptor->bcdDevice);

    INIT_USBAUDIO_MID(&ComponentId->Manufacturer, DeviceExtension->DeviceDescriptor->idVendor);
    INIT_USBAUDIO_PID(&ComponentId->Product, DeviceExtension->DeviceDescriptor->idProduct);

    if (DeviceExtension->DeviceDescriptor->iProduct)
    {
        Status = USBAudioGetStringDescriptor(DeviceExtension->LowerDevice, 100 * sizeof(WCHAR), DeviceExtension->DeviceDescriptor->iProduct, 0x0409 /* FIXME */, (PVOID*)&DescriptionBuffer);
        if (NT_SUCCESS(Status))
        {
            INIT_USBAUDIO_PRODUCT_NAME(&ComponentId->Name, DeviceExtension->DeviceDescriptor->idVendor, DeviceExtension->DeviceDescriptor->idProduct, 0);
            UsbAudioPublishName(&ComponentId->Name, DescriptionBuffer);
            FreeFunction(DescriptionBuffer);
        }
    }
    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
USBAudioCreateFilterContext(
    PKSDEVICE Device)
{
    PKSFILTER_DESCRIPTOR FilterDescriptor;
    PKSCOMPONENTID ComponentId;
    NTSTATUS Status;
    BOOLEAN Renders, Captures;
    GUID *Categories;

    /* allocate descriptor */
    FilterDescriptor = AllocFunction(sizeof(KSFILTER_DESCRIPTOR));
    Categories = AllocFunction(3 * sizeof(GUID));
    if (!FilterDescriptor || !Categories)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init filter descriptor*/
    FilterDescriptor->Version = KSFILTER_DESCRIPTOR_VERSION;
    FilterDescriptor->Flags = 0;
    FilterDescriptor->ReferenceGuid = &KSNAME_Filter;
    FilterDescriptor->Dispatch = &USBAudioFilterDispatch;

    /* init component id*/
    ComponentId = AllocFunction(sizeof(KSCOMPONENTID));
    if (!ComponentId)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = USBAudioInitComponentId(Device, ComponentId);
    if (!NT_SUCCESS(Status))
    {
        /* failed*/
        FreeFunction(ComponentId);
        return Status;
    }
    FilterDescriptor->ComponentId = ComponentId;

    /* build pin descriptors */
    Status = USBAudioPinBuildDescriptors(Device, (PKSPIN_DESCRIPTOR_EX *)&FilterDescriptor->PinDescriptors, &FilterDescriptor->PinDescriptorsCount, &FilterDescriptor->PinDescriptorSize, &Renders, &Captures);
    if (!NT_SUCCESS(Status))
    {
        /* failed*/
        FreeFunction(ComponentId);
        return Status;
    }

    /* The filter shows up where its streams are */
    Categories[FilterDescriptor->CategoriesCount++] = GUID_KSCATEGORY_AUDIO;
    if (Renders)
        Categories[FilterDescriptor->CategoriesCount++] = GUID_KSCATEGORY_RENDER;
    if (Captures)
        Categories[FilterDescriptor->CategoriesCount++] = GUID_KSCATEGORY_CAPTURE;
    FilterDescriptor->Categories = Categories;

    /* build topology */
    Status = BuildUSBAudioFilterTopology(Device, FilterDescriptor);
    if (!NT_SUCCESS(Status))
    {
        /* failed*/
        FreeFunction(ComponentId);
        return Status;
    }

    /* lets create the filter */
    Status = KsCreateFilterFactory(Device->FunctionalDeviceObject, FilterDescriptor, ReferenceString, NULL, KSCREATE_ITEM_FREEONSTOP, NULL, NULL, NULL);
    DPRINT1("KsCreateFilterFactory: %x\n", Status);

    return Status;
}
