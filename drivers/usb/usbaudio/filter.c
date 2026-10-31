/*
* PROJECT:     ReactOS Universal Audio Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbaudio/filter.c
* PURPOSE:     USB Audio device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/

#include "usbaudio.h"

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
GUID NodeTypeCapture = { STATIC_PINNAME_CAPTURE };
GUID NodeTypePlayback = { STATIC_KSCATEGORY_AUDIO };
GUID GUID_KSCATEGORY_AUDIO = { STATIC_KSCATEGORY_AUDIO };

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

NTSTATUS NTAPI FilterAudioVolumeHandler(IN PIRP Irp, IN PKSIDENTIFIER  Request, IN OUT PVOID  Data);
NTSTATUS NTAPI FilterAudioMuteHandler(IN PIRP Irp, IN PKSIDENTIFIER  Request, IN OUT PVOID  Data);

DEFINE_KSPROPERTY_TABLE_AUDIO_VOLUME(FilterAudioVolumePropertySet, FilterAudioVolumeHandler);
DEFINE_KSPROPERTY_TABLE_AUDIO_MUTE(FilterAudioMutePropertySet, FilterAudioMuteHandler);


static KSPROPERTY_SET FilterAudioVolumePropertySetArray[] =
{
    {
        &KSPROPSETID_Audio,
        sizeof(FilterAudioVolumePropertySet) / sizeof(KSPROPERTY_ITEM),
        (const KSPROPERTY_ITEM*)&FilterAudioVolumePropertySet,
        0,
        NULL
    }
};

static KSPROPERTY_SET FilterAudioMutePropertySetArray[] =
{
    {
        &KSPROPSETID_Audio,
        sizeof(FilterAudioMutePropertySet) / sizeof(KSPROPERTY_ITEM),
        (const KSPROPERTY_ITEM*)&FilterAudioMutePropertySet,
        0,
        NULL
    }
};

NTSTATUS
UsbAudioGetSetProperty(
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR Request,
    IN USHORT Value,
    IN USHORT Index,
    IN PVOID TransferBuffer,
    IN ULONG TransferBufferLength,
    IN ULONG TransferFlags)
{
    PURB Urb;
    NTSTATUS Status;

    /* allocate urb */
    Urb = AllocFunction(sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* format urb */
    UsbBuildVendorRequest(Urb,
        URB_FUNCTION_CLASS_INTERFACE,
        sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
        TransferFlags,
        0,
        Request,
        Value,
        Index,
        TransferBuffer,
        NULL,
        TransferBufferLength,
        NULL);

    /* submit urb */
    Status = SubmitUrbSync(DeviceObject, Urb);

    FreeFunction(Urb);
    return Status;
}

PNODE_CONTEXT
FindNodeContextWithNode(
    IN PNODE_CONTEXT NodeContext,
    IN ULONG NodeContextCount,
    IN ULONG NodeId)
{
    ULONG Index, NodeIndex;
    for (Index = 0; Index < NodeContextCount; Index++)
    {
        for (NodeIndex = 0; NodeIndex < NodeContext[Index].NodeCount; NodeIndex++)
        {
            if (NodeContext[Index].Nodes[NodeIndex] == NodeId)
            {
                return &NodeContext[Index];
            }
        }
    }
    return NULL;
}


static
ULONG
UsbAudioGetFeatureUnitControlCount(
    IN PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor)
{
    if (FeatureUnitDescriptor->bControlSize == 0 || FeatureUnitDescriptor->bLength < 7)
        return 0;

    /* bmaControls holds the controls of the master channel followed by the controls of each logical channel */
    return (FeatureUnitDescriptor->bLength - 7) / FeatureUnitDescriptor->bControlSize;
}

static
ULONG
UsbAudioGetFeatureUnitControls(
    IN PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor,
    IN ULONG Channel)
{
    PUCHAR Controls;
    ULONG Value = 0;
    ULONG Index;

    if (Channel >= UsbAudioGetFeatureUnitControlCount(FeatureUnitDescriptor))
        return 0;

    /* each bmaControls element is bControlSize bytes, little endian */
    Controls = &FeatureUnitDescriptor->bmaControls[Channel * FeatureUnitDescriptor->bControlSize];
    for (Index = 0; Index < min(FeatureUnitDescriptor->bControlSize, sizeof(ULONG)); Index++)
    {
        Value |= (ULONG)Controls[Index] << (Index * 8);
    }

    return Value;
}

/* maps a KS channel to the feature unit channel which implements the control or returns MAXULONG */
static
ULONG
UsbAudioGetFeatureUnitChannel(
    IN PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor,
    IN ULONG KsChannel,
    IN ULONG Control)
{
    if (KsChannel == MAXULONG)
    {
        /* request for all channels, prefer the master channel */
        if (UsbAudioGetFeatureUnitControls(FeatureUnitDescriptor, 0) & Control)
            return 0;
        else
            return MAXULONG;
    }

    /* KS channels are zero based, the logical channels of the feature unit start at 1 */
    if (UsbAudioGetFeatureUnitControls(FeatureUnitDescriptor, KsChannel + 1) & Control)
        return KsChannel + 1;

    return MAXULONG;
}

static
NTSTATUS
UsbAudioFeatureUnitRequest(
    IN PDEVICE_EXTENSION DeviceExtension,
    IN PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor,
    IN UCHAR Request,
    IN UCHAR Control,
    IN ULONG Channel,
    IN PVOID TransferBuffer,
    IN ULONG TransferBufferLength)
{
    /* wValue: control selector and channel number, wIndex: unit id and audio control interface (USB Audio 1.0, 5.2.2.4) */
    return UsbAudioGetSetProperty(DeviceExtension->LowerDevice,
                                  Request,
                                  (Control << 8) | (UCHAR)Channel,
                                  (FeatureUnitDescriptor->bUnitID << 8) | DeviceExtension->AudioControlInterfaceNumber,
                                  TransferBuffer,
                                  TransferBufferLength,
                                  (Request & 0x80) ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT);
}

static
NTSTATUS
UsbAudioGetVolumeRange(
    IN PDEVICE_EXTENSION DeviceExtension,
    IN PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor,
    IN ULONG Channel,
    IN PSHORT TransferBuffer,
    OUT PLONG Minimum,
    OUT PLONG Maximum,
    OUT PLONG Resolution)
{
    NTSTATUS Status;

    /* volume values are signed 16 bit in 1/256 dB (USB Audio 1.0, 5.2.2.4.3.2) */
    Status = UsbAudioFeatureUnitRequest(DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_GET_MIN, USB_AUDIO_VOLUME_CONTROL, Channel, TransferBuffer, sizeof(SHORT));
    if (!NT_SUCCESS(Status))
        return Status;
    *Minimum = *TransferBuffer;

    Status = UsbAudioFeatureUnitRequest(DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_GET_MAX, USB_AUDIO_VOLUME_CONTROL, Channel, TransferBuffer, sizeof(SHORT));
    if (!NT_SUCCESS(Status))
        return Status;
    *Maximum = *TransferBuffer;

    Status = UsbAudioFeatureUnitRequest(DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_GET_RES, USB_AUDIO_VOLUME_CONTROL, Channel, TransferBuffer, sizeof(SHORT));
    if (!NT_SUCCESS(Status))
        return Status;
    *Resolution = (USHORT)*TransferBuffer;

    if (*Minimum > *Maximum)
        return STATUS_DEVICE_DATA_ERROR;

    if (*Resolution == 0)
        *Resolution = 1;

    return STATUS_SUCCESS;
}

static
SHORT
UsbAudioClampVolume(
    IN LONG Value,
    IN LONG Minimum,
    IN LONG Maximum,
    IN LONG Resolution)
{
    if (Value < Minimum)
        Value = Minimum;

    if (Value > Maximum)
        Value = Maximum;

    /* align to the volume steps of the device */
    return (SHORT)(Minimum + ((Value - Minimum) / Resolution) * Resolution);
}

NTSTATUS
NTAPI
FilterAudioMuteHandler(
    IN PIRP Irp,
    IN PKSIDENTIFIER  Request,
    IN OUT PVOID  Data)
{
    PKSNODEPROPERTY_AUDIO_CHANNEL Property;
    PKSFILTER Filter;
    PFILTER_CONTEXT FilterContext;
    PNODE_CONTEXT NodeContext;
    PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor;
    PUCHAR TransferBuffer;
    ULONG Channel;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    /* get filter from irp */
    Filter = KsGetFilterFromIrp(Irp);

    if (Filter)
    {
        /* get property */
        Property = (PKSNODEPROPERTY_AUDIO_CHANNEL)Request;

        /* get filter context */
        FilterContext = (PFILTER_CONTEXT)Filter->Context;

        /* search for node context */
        NodeContext = FindNodeContextWithNode(FilterContext->DeviceExtension->NodeContext, FilterContext->DeviceExtension->NodeContextCount, Property->NodeProperty.NodeId);
        if (NodeContext)
        {
            FeatureUnitDescriptor = (PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR)NodeContext->Descriptor;

            Channel = UsbAudioGetFeatureUnitChannel(FeatureUnitDescriptor, Property->Channel, USB_AUDIO_FU_CONTROL_MUTE);
            if (Channel == MAXULONG)
            {
                /* channel has no mute control */
                return STATUS_INVALID_PARAMETER;
            }

            TransferBuffer = AllocFunction(sizeof(UCHAR));
            if (!TransferBuffer)
            {
                /* no memory */
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            if (Property->NodeProperty.Property.Flags & KSPROPERTY_TYPE_GET)
            {
                Status = UsbAudioFeatureUnitRequest(FilterContext->DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_GET_CUR, USB_AUDIO_MUTE_CONTROL, Channel, TransferBuffer, sizeof(UCHAR));
                if (NT_SUCCESS(Status))
                {
                    *(PBOOL)Data = (*TransferBuffer != 0);
                    Irp->IoStatus.Information = sizeof(BOOL);
                }
            }
            else
            {
                *TransferBuffer = *(PBOOL)Data ? 1 : 0;
                Status = UsbAudioFeatureUnitRequest(FilterContext->DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_SET_CUR, USB_AUDIO_MUTE_CONTROL, Channel, TransferBuffer, sizeof(UCHAR));
                if (NT_SUCCESS(Status))
                {
                    Irp->IoStatus.Information = sizeof(BOOL);
                }
            }

            FreeFunction(TransferBuffer);
        }
    }
    return Status;
}

NTSTATUS
NTAPI
FilterAudioVolumeHandler(
    IN PIRP Irp,
    IN PKSIDENTIFIER  Request,
    IN OUT PVOID  Data)
{
    PKSNODEPROPERTY_AUDIO_CHANNEL Property;
    PKSFILTER Filter;
    PFILTER_CONTEXT FilterContext;
    PNODE_CONTEXT NodeContext;
    PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor;
    PSHORT TransferBuffer;
    LONG Minimum, Maximum, Resolution;
    ULONG Channel;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    /* get filter from irp */
    Filter = KsGetFilterFromIrp(Irp);
    //DPRINT("FilterAudioVolumeHandler Filter %p\n", Filter);

    if (Filter)
    {
        /* get property */
        Property = (PKSNODEPROPERTY_AUDIO_CHANNEL)Request;

        /* get filter context */
        FilterContext = (PFILTER_CONTEXT)Filter->Context;

        /* search for node context */
        NodeContext = FindNodeContextWithNode(FilterContext->DeviceExtension->NodeContext, FilterContext->DeviceExtension->NodeContextCount, Property->NodeProperty.NodeId);
        if (NodeContext)
        {
            FeatureUnitDescriptor = (PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR)NodeContext->Descriptor;

            Channel = UsbAudioGetFeatureUnitChannel(FeatureUnitDescriptor, Property->Channel, USB_AUDIO_FU_CONTROL_VOLUME);
            if (Channel == MAXULONG)
            {
                /* channel has no volume control */
                return STATUS_INVALID_PARAMETER;
            }

            TransferBuffer = AllocFunction(sizeof(SHORT));
            if (!TransferBuffer)
            {
                /* no memory */
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            /* KS volume is in 1/65536 dB, USB volume in 1/256 dB */
            if (Property->NodeProperty.Property.Flags & KSPROPERTY_TYPE_GET)
            {
                Status = UsbAudioFeatureUnitRequest(FilterContext->DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_GET_CUR, USB_AUDIO_VOLUME_CONTROL, Channel, TransferBuffer, sizeof(SHORT));
                if (NT_SUCCESS(Status))
                {
                    *(PLONG)Data = (LONG)*TransferBuffer * 256;
                    Irp->IoStatus.Information = sizeof(LONG);
                }
            }
            else
            {
                /* never send a volume outside of the range of the device */
                Status = UsbAudioGetVolumeRange(FilterContext->DeviceExtension, FeatureUnitDescriptor, Channel, TransferBuffer, &Minimum, &Maximum, &Resolution);
                if (NT_SUCCESS(Status))
                {
                    *TransferBuffer = UsbAudioClampVolume(*(PLONG)Data / 256, Minimum, Maximum, Resolution);

                    Status = UsbAudioFeatureUnitRequest(FilterContext->DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_SET_CUR, USB_AUDIO_VOLUME_CONTROL, Channel, TransferBuffer, sizeof(SHORT));
                    if (NT_SUCCESS(Status))
                    {
                        /* store number of bytes transferred*/
                        Irp->IoStatus.Information = sizeof(LONG);
                    }
                }
                else
                {
                    DPRINT1("FilterAudioVolumeHandler failed to get volume range of channel %u Status %x\n", Channel, Status);
                }
            }

            /* free transfer buffer */
            FreeFunction(TransferBuffer);
        }
    }
    return Status;
}

VOID
USBAudioSetVolume(
    PDEVICE_EXTENSION DeviceExtension,
    PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor)
{
    PSHORT TransferBuffer;
    NTSTATUS Status;
    ULONG Channel, ChannelCount;
    LONG Minimum, Maximum, Resolution, Volume;

    TransferBuffer = AllocFunction(sizeof(SHORT));
    if (!TransferBuffer)
        return;

    /* set a moderate default volume on every channel with a volume control */
    ChannelCount = UsbAudioGetFeatureUnitControlCount(FeatureUnitDescriptor);
    for (Channel = 0; Channel < ChannelCount; Channel++)
    {
        if (!(UsbAudioGetFeatureUnitControls(FeatureUnitDescriptor, Channel) & USB_AUDIO_FU_CONTROL_VOLUME))
            continue;

        Status = UsbAudioGetVolumeRange(DeviceExtension, FeatureUnitDescriptor, Channel, TransferBuffer, &Minimum, &Maximum, &Resolution);
        if (!NT_SUCCESS(Status))
        {
            /* leave the volume of the device untouched */
            DPRINT1("USBAudioSetVolume failed to get volume range of channel %u Status %x\n", Channel, Status);
            continue;
        }

        /* three quarters of the range, but never amplify above 0 dB */
        Volume = Minimum + ((Maximum - Minimum) * 3) / 4;
        if (Volume > 0 && Minimum <= 0)
            Volume = 0;

        *TransferBuffer = UsbAudioClampVolume(Volume, Minimum, Maximum, Resolution);

        Status = UsbAudioFeatureUnitRequest(DeviceExtension, FeatureUnitDescriptor, USB_AUDIO_SET_CUR, USB_AUDIO_VOLUME_CONTROL, Channel, TransferBuffer, sizeof(SHORT));
        DPRINT1("USBAudioSetVolume Channel %u Min %d Max %d Res %d Volume %d Status %x\n",
                Channel, Minimum, Maximum, Resolution, *TransferBuffer, Status);
    }

    FreeFunction(TransferBuffer);
}


ULONG
CountTopologyComponents(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    OUT PULONG OutDescriptorCount)
{
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR InputTerminalDescriptor;
    PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor;
    PUSB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR MixerUnitDescriptor;
    ULONG NodeCount = 0, Length, Index;
    ULONG DescriptorCount = 0;
    UCHAR Value;

    for (Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x01) /* AUDIO_CONTROL */
        {
            InterfaceHeaderDescriptor = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (InterfaceHeaderDescriptor != NULL)
            {
                CommonDescriptor = USBD_ParseDescriptors(InterfaceHeaderDescriptor, InterfaceHeaderDescriptor->wTotalLength, (PVOID)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength), USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
                while (CommonDescriptor)
                {
                    InputTerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                    if (InputTerminalDescriptor->bDescriptorSubtype == 0x02 /* INPUT TERMINAL*/ || InputTerminalDescriptor->bDescriptorSubtype == 0x03 /* OUTPUT_TERMINAL*/)
                    {
                        NodeCount++;
                        DescriptorCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x06 /* FEATURE_UNIT*/)
                    {
                        FeatureUnitDescriptor = (PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR)InputTerminalDescriptor;
                        DescriptorCount++;

                        /* get controls from all channels*/
                        Value = 0;
                        Length = UsbAudioGetFeatureUnitControlCount(FeatureUnitDescriptor);
                        for (Index = 0; Index < Length; Index++)
                        {
                            Value |= UsbAudioGetFeatureUnitControls(FeatureUnitDescriptor, Index);
                        }

                        if (Value & 0x01) /* MUTE*/
                            NodeCount++;
                        if (Value & 0x02) /* VOLUME */
                            NodeCount++;
                        if (Value & 0x04) /* BASS */
                            NodeCount++;
                        if (Value & 0x08) /* MID */
                            NodeCount++;
                        if (Value & 0x10) /* TREBLE */
                            NodeCount++;
                        if (Value & 0x20) /* GRAPHIC EQUALIZER */
                            NodeCount++;
                        if (Value & 0x40) /* AUTOMATIC GAIN */
                            NodeCount++;
                        if (Value & 0x80) /* DELAY */
                            NodeCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x04 /* MIXER_UNIT */)
                    {
                        MixerUnitDescriptor = (PUSB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR)InputTerminalDescriptor;
                        DescriptorCount++;
                        NodeCount += MixerUnitDescriptor->bNrInPins + 1; /* KSNODETYPE_SUPERMIX for each source pin and KSNODETYPE_SUM for target */
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x05 /* SELECTOR_UNIT */)
                    {
                        DescriptorCount++;
                        NodeCount++;
                    }
                    else
                    {
                        UNIMPLEMENTED;
                    }
                    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
                    if ((ULONG_PTR)CommonDescriptor >= ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
                        break;
                }
            }
        }
    }
    *OutDescriptorCount = DescriptorCount;
    return NodeCount;
}

PNODE_CONTEXT
FindNodeContextWithId(
    IN PNODE_CONTEXT NodeContext,
    IN ULONG NodeContextCount,
    IN UCHAR TerminalId)
{
    ULONG Index;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR TerminalDescriptor;

    for (Index = 0; Index < NodeContextCount; Index++)
    {
        TerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)NodeContext[Index].Descriptor;
        if (TerminalDescriptor->bTerminalID == TerminalId)
            return &NodeContext[Index];
    }
    return NULL;
}

NTSTATUS
BuildUSBAudioFilterTopology(
    PKSDEVICE Device,
    PKSFILTER_DESCRIPTOR FilterDescriptor)
{
    PDEVICE_EXTENSION DeviceExtension;
    ULONG NodeCount, Index, DescriptorCount, StreamingTerminalIndex, NonStreamingTerminalDescriptorCount, TotalTerminalDescriptorCount, StreamingTerminalPinOffset, ControlDescriptorCount, Length;
    UCHAR Value;
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR InputTerminalDescriptor;
    PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR FeatureUnitDescriptor;
    PUSB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR MixerUnitDescriptor;
    PUSB_AUDIO_CONTROL_OUTPUT_TERMINAL_DESCRIPTOR OutputTerminalDescriptor;
    PUSB_AUDIO_CONTROL_SELECTOR_UNIT_DESCRIPTOR SelectorUnitDescriptor;
    PKSNODE_DESCRIPTOR NodeDescriptors;
    PNODE_CONTEXT NodeContext, PreviousNodeContext;
    PKSTOPOLOGY_CONNECTION Connections;
    PKSAUTOMATION_TABLE AutomationTable;

    /* get device extension */
    DeviceExtension = Device->Context;

    /* count topology nodes */
    NodeCount = CountTopologyComponents(DeviceExtension->ConfigurationDescriptor, &ControlDescriptorCount);

    /* init node descriptors*/
    FilterDescriptor->NodeDescriptors = NodeDescriptors = AllocFunction(NodeCount * sizeof(KSNODE_DESCRIPTOR));
    if (FilterDescriptor->NodeDescriptors == NULL)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    FilterDescriptor->NodeDescriptorSize = sizeof(KSNODE_DESCRIPTOR);

    DeviceExtension->NodeContext = NodeContext = AllocFunction(sizeof(NODE_CONTEXT) * ControlDescriptorCount);
    if (!NodeContext)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DeviceExtension->NodeContextCount = ControlDescriptorCount;
    DescriptorCount = 0;

    /* first enumerate all topology nodes */
    for (Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x01) /* AUDIO_CONTROL */
        {
            InterfaceHeaderDescriptor = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)USBD_ParseDescriptors(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (InterfaceHeaderDescriptor != NULL)
            {
                CommonDescriptor = USBD_ParseDescriptors(InterfaceHeaderDescriptor, InterfaceHeaderDescriptor->wTotalLength, (PVOID)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength), USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
                while (CommonDescriptor)
                {
                    InputTerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                    if (InputTerminalDescriptor->bDescriptorSubtype == 0x02 /* INPUT TERMINAL*/)
                    {
                        if (InputTerminalDescriptor->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_DAC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_DAC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else if ((InputTerminalDescriptor->wTerminalType & 0xFF00) == 0x200)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;


                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else if ((InputTerminalDescriptor->wTerminalType & 0xFF00) == 0x300)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else if ((InputTerminalDescriptor->wTerminalType & 0xFF00) == 0x400)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else
                        {
                            DPRINT1("Unexpected input terminal type %x\n", InputTerminalDescriptor->wTerminalType);
                        }
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x03 /* OUTPUT_TERMINAL*/)
                    {
                        if (InputTerminalDescriptor->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_ADC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_ADC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else if ((InputTerminalDescriptor->wTerminalType & 0xFF00) == 0x300)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else if ((InputTerminalDescriptor->wTerminalType & 0xFF00) == 0x400)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SRC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                            NodeContext[DescriptorCount].NodeCount = 1;
                            NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                            DescriptorCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        else
                        {
                            DPRINT1("Unexpected output terminal type %x\n", InputTerminalDescriptor->wTerminalType);
                        }
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x06 /* FEATURE_UNIT*/)
                    {
                        FeatureUnitDescriptor = (PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR)CommonDescriptor;

                        /* get controls from all channels*/
                        Value = 0;
                        Length = UsbAudioGetFeatureUnitControlCount(FeatureUnitDescriptor);
                        for (Index = 0; Index < Length; Index++)
                        {
                            Value |= UsbAudioGetFeatureUnitControls(FeatureUnitDescriptor, Index);
                        }

                        if (Value & 0x01) /* MUTE*/
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_MUTE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_MUTE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));
                            if (AutomationTable)
                            {
                                AutomationTable->PropertySets = FilterAudioMutePropertySetArray;
                                AutomationTable->PropertySetsCount = 1;
                                AutomationTable->PropertyItemSize = sizeof(KSPROPERTY_ITEM);
                            }

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        if (Value & 0x02) /* VOLUME */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_VOLUME;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_VOLUME;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));
                            if (AutomationTable)
                            {
                                AutomationTable->PropertySets = FilterAudioVolumePropertySetArray;
                                AutomationTable->PropertySetsCount = 1;
                                AutomationTable->PropertyItemSize = sizeof(KSPROPERTY_ITEM);
                            }

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                            USBAudioSetVolume(DeviceExtension, FeatureUnitDescriptor);
                        }

                        if (Value & 0x04) /* BASS */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }

                        if (Value & 0x08) /* MID */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }

                        if (Value & 0x10) /* TREBLE */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;


                            FilterDescriptor->NodeDescriptorsCount++;
                        }

                        if (Value & 0x20) /* GRAPHIC EQUALIZER */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }

                        if (Value & 0x40) /* AUTOMATIC GAIN */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_AGC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_AGC;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;


                            FilterDescriptor->NodeDescriptorsCount++;
                        }

                        if (Value & 0x80) /* DELAY */
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_TONE;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }
                        NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                        DescriptorCount++;

                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x04 /* MIXER_UNIT */)
                    {
                        MixerUnitDescriptor = (PUSB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR)CommonDescriptor;
                        for (Index = 0; Index < MixerUnitDescriptor->bNrInPins; Index++)
                        {
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SUPERMIX;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SUPERMIX;
                            NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                            /* insert into node context*/
                            NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                            NodeContext[DescriptorCount].NodeCount++;

                            FilterDescriptor->NodeDescriptorsCount++;
                        }

                        NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_SUM;
                        NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_SUM;
                        NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                        /* insert into node context*/
                        NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount] = FilterDescriptor->NodeDescriptorsCount;
                        NodeContext[DescriptorCount].NodeCount++;
                        NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                        DescriptorCount++;

                        FilterDescriptor->NodeDescriptorsCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x05 /* SELECTOR UNIT */)
                    {
                        NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Type = &KSNODETYPE_MUX;
                        NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].Name = &KSNODETYPE_MUX;
                        NodeDescriptors[FilterDescriptor->NodeDescriptorsCount].AutomationTable = AllocFunction(sizeof(KSAUTOMATION_TABLE));

                        /* insert into node context*/
                        NodeContext[DescriptorCount].Descriptor = CommonDescriptor;
                        NodeContext[DescriptorCount].NodeCount = 1;
                        NodeContext[DescriptorCount].Nodes[0] = FilterDescriptor->NodeDescriptorsCount;
                        DescriptorCount++;
                        FilterDescriptor->NodeDescriptorsCount++;
                    }
                    else
                    {
                        UNIMPLEMENTED;
                    }
                    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
                    if ((ULONG_PTR)CommonDescriptor >= ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
                        break;
                }
            }
        }
    }

    /* FIXME determine connections count*/
    FilterDescriptor->Connections = Connections = AllocFunction(sizeof(KSTOPOLOGY_CONNECTION) * FilterDescriptor->NodeDescriptorsCount * 2);
    if (!FilterDescriptor->Connections)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    FilterDescriptor->ConnectionsCount = 0;

    /* now build connections array */
    DescriptorCount = 0;
    StreamingTerminalIndex = 0;
    NodeCount = 0;

    CountTerminalUnits(DeviceExtension->ConfigurationDescriptor, &NonStreamingTerminalDescriptorCount, &TotalTerminalDescriptorCount);
    StreamingTerminalPinOffset = TotalTerminalDescriptorCount - NonStreamingTerminalDescriptorCount;

    for (Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(DeviceExtension->ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x01) /* AUDIO_CONTROL */
        {
            InterfaceHeaderDescriptor = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)USBD_ParseDescriptors(DeviceExtension->ConfigurationDescriptor, DeviceExtension->ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (InterfaceHeaderDescriptor != NULL)
            {
                CommonDescriptor = USBD_ParseDescriptors(InterfaceHeaderDescriptor, InterfaceHeaderDescriptor->wTotalLength, (PVOID)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength), USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
                while (CommonDescriptor)
                {
                    InputTerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                    if (InputTerminalDescriptor->bDescriptorSubtype == 0x02 /* INPUT TERMINAL*/)
                    {
                        if (InputTerminalDescriptor->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                             Connections[FilterDescriptor->ConnectionsCount].FromNode = KSFILTER_NODE;
                             Connections[FilterDescriptor->ConnectionsCount].FromNodePin = StreamingTerminalIndex;
                             Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                             Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[0];
                             FilterDescriptor->ConnectionsCount++;
                             StreamingTerminalIndex++;

                        }
                        else
                        {
                            Connections[FilterDescriptor->ConnectionsCount].FromNode = KSFILTER_NODE;
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = StreamingTerminalPinOffset;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[0];
                            FilterDescriptor->ConnectionsCount++;
                            StreamingTerminalPinOffset++;
                        }
                        DescriptorCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x03 /* OUTPUT_TERMINAL*/)
                    {
                        OutputTerminalDescriptor = (PUSB_AUDIO_CONTROL_OUTPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                        PreviousNodeContext = FindNodeContextWithId(NodeContext, ControlDescriptorCount, OutputTerminalDescriptor->bSourceID);
                        if (PreviousNodeContext)
                        {
                            Connections[FilterDescriptor->ConnectionsCount].FromNode = PreviousNodeContext->Nodes[PreviousNodeContext->NodeCount - 1];
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[0];
                            FilterDescriptor->ConnectionsCount++;
                        }

                        if (InputTerminalDescriptor->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                            Connections[FilterDescriptor->ConnectionsCount].FromNode = NodeContext[DescriptorCount].Nodes[0];
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = StreamingTerminalIndex;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = KSFILTER_NODE;
                            FilterDescriptor->ConnectionsCount++;
                            StreamingTerminalIndex++;
                        }
                        else
                        {
                            Connections[FilterDescriptor->ConnectionsCount].FromNode = NodeContext[DescriptorCount].Nodes[0];
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = StreamingTerminalPinOffset;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = KSFILTER_NODE;
                            FilterDescriptor->ConnectionsCount++;

                            StreamingTerminalPinOffset++;
                        }
                        DescriptorCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x06 /* FEATURE_UNIT*/)
                    {
                        FeatureUnitDescriptor = (PUSB_AUDIO_CONTROL_FEATURE_UNIT_DESCRIPTOR)InputTerminalDescriptor;
                        PreviousNodeContext = FindNodeContextWithId(NodeContext, ControlDescriptorCount, FeatureUnitDescriptor->bSourceID);
                        if (PreviousNodeContext)
                        {
                            Connections[FilterDescriptor->ConnectionsCount].FromNode = PreviousNodeContext->Nodes[PreviousNodeContext->NodeCount-1];
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[0];
                            FilterDescriptor->ConnectionsCount++;
                        }
                        for (Index = 1; Index < NodeContext[DescriptorCount].NodeCount; Index++)
                        {
                            Connections[FilterDescriptor->ConnectionsCount].FromNode = NodeContext[DescriptorCount].Nodes[Index - 1];
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[Index];
                            FilterDescriptor->ConnectionsCount++;
                        }

                        DescriptorCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x04 /* MIXER_UNIT */)
                    {
                        MixerUnitDescriptor = (PUSB_AUDIO_CONTROL_MIXER_UNIT_DESCRIPTOR)InputTerminalDescriptor;
                        for (Index = 0; Index < MixerUnitDescriptor->bNrInPins; Index++)
                        {
                            Value = MixerUnitDescriptor->baSourceID[Index];
                            PreviousNodeContext = FindNodeContextWithId(NodeContext, ControlDescriptorCount, Value);
                            if (PreviousNodeContext)
                            {
                                Connections[FilterDescriptor->ConnectionsCount].FromNode = PreviousNodeContext->Nodes[PreviousNodeContext->NodeCount - 1];
                                Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                                Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                                Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[Index];
                                FilterDescriptor->ConnectionsCount++;
                            }

                            Connections[FilterDescriptor->ConnectionsCount].FromNode = NodeContext[DescriptorCount].Nodes[Index];
                            Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                            Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1 + Index;
                            Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[NodeContext[DescriptorCount].NodeCount-1];
                            FilterDescriptor->ConnectionsCount++;
                        }
                        DescriptorCount++;
                    }
                    else if (InputTerminalDescriptor->bDescriptorSubtype == 0x05 /* SELECTOR_UNIT */)
                    {
                        SelectorUnitDescriptor = (PUSB_AUDIO_CONTROL_SELECTOR_UNIT_DESCRIPTOR)InputTerminalDescriptor;
                        for (Index = 0; Index < SelectorUnitDescriptor->bNrInPins; Index++)
                        {
                            Value = SelectorUnitDescriptor->baSourceID[Index];
                            PreviousNodeContext = FindNodeContextWithId(NodeContext, ControlDescriptorCount, Value);
                            if (PreviousNodeContext)
                            {
                                Connections[FilterDescriptor->ConnectionsCount].FromNode = PreviousNodeContext->Nodes[PreviousNodeContext->NodeCount - 1];
                                Connections[FilterDescriptor->ConnectionsCount].FromNodePin = 0;
                                Connections[FilterDescriptor->ConnectionsCount].ToNodePin = 1;
                                Connections[FilterDescriptor->ConnectionsCount].ToNode = NodeContext[DescriptorCount].Nodes[0];
                                FilterDescriptor->ConnectionsCount++;
                            }
                        }
                        DescriptorCount++;
                    }
                    else
                    {
                        UNIMPLEMENTED;
                    }
                    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
                    if ((ULONG_PTR)CommonDescriptor >= ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
                        break;
                }
            }
        }
    }



    return STATUS_SUCCESS;
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
    FilterContext->InterfaceInfo = FilterContext->DeviceExtension->InterfaceInfo;
    Filter->Context = FilterContext;

    DPRINT("USBAudioFilterCreate FilterContext %p LowerDevice %p DeviceExtension %p\n", FilterContext, FilterContext->LowerDevice, FilterContext->DeviceExtension);
    KsAddItemToObjectBag(Filter->Bag, FilterContext, ExFreePool);
    return STATUS_SUCCESS;
}


VOID
NTAPI
CountTerminalUnits(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    OUT PULONG NonStreamingTerminalDescriptorCount,
    OUT PULONG TotalTerminalDescriptorCount)
{
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR InputTerminalDescriptor;
    ULONG NonStreamingTerminalCount = 0;
    ULONG TotalTerminalCount = 0;

    for(Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
        Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x01) /* AUDIO_CONTROL */
        {
            InterfaceHeaderDescriptor = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (InterfaceHeaderDescriptor != NULL)
            {
                CommonDescriptor = USBD_ParseDescriptors(InterfaceHeaderDescriptor, InterfaceHeaderDescriptor->wTotalLength, (PVOID)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength), USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
                while (CommonDescriptor)
                {
                    InputTerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                    if (InputTerminalDescriptor->bDescriptorSubtype == 0x02 /* INPUT TERMINAL*/ || InputTerminalDescriptor->bDescriptorSubtype == 0x03 /* OUTPUT_TERMINAL*/)
                    {
                        if (InputTerminalDescriptor->wTerminalType != USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                            NonStreamingTerminalCount++;
                        }
                        TotalTerminalCount++;
                    }
                    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
                    if ((ULONG_PTR)CommonDescriptor >= ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
                        break;
                }
            }
        }
        else if (Descriptor->bInterfaceSubClass == 0x03) /* MIDI_STREAMING */
        {
            UNIMPLEMENTED;
        }
    }
    *NonStreamingTerminalDescriptorCount = NonStreamingTerminalCount;
    *TotalTerminalDescriptorCount = TotalTerminalCount;
}

LPGUID
UsbAudioGetPinCategoryFromTerminalDescriptor(
    IN PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR TerminalDescriptor)
{
    if (TerminalDescriptor->wTerminalType == USB_AUDIO_MICROPHONE_TERMINAL_TYPE)
        return &NodeTypeMicrophone;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_DESKTOP_MICROPHONE_TERMINAL_TYPE)
        return &NodeTypeDesktopMicrophone;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_PERSONAL_MICROPHONE_TERMINAL_TYPE)
        return &NodeTypePersonalMicrophone;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_OMMNI_MICROPHONE_TERMINAL_TYPE)
        return &NodeTypeOmmniMicrophone;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_ARRAY_MICROPHONE_TERMINAL_TYPE)
        return &NodeTypeArrayMicrophone;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_ARRAY_PROCESSING_MICROPHONE_TERMINAL_TYPE)
        return &NodeTypeProcessingArrayMicrophone;

    /* playback types */
    if (TerminalDescriptor->wTerminalType == USB_AUDIO_SPEAKER_TERMINAL_TYPE)
        return &NodeTypeSpeaker;
    else if (TerminalDescriptor->wTerminalType == USB_HEADPHONES_SPEAKER_TERMINAL_TYPE)
        return &NodeTypeHeadphonesSpeaker;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_HMDA_TERMINAL_TYPE)
        return &NodeTypeHMDA;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_DESKTOP_SPEAKER_TERMINAL_TYPE)
        return &NodeTypeDesktopSpeaker;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_ROOM_SPEAKER_TERMINAL_TYPE)
        return &NodeTypeRoomSpeaker;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_COMMUNICATION_SPEAKER_TERMINAL_TYPE)
        return &NodeTypeCommunicationSpeaker;
    else if (TerminalDescriptor->wTerminalType == USB_AUDIO_SUBWOOFER_TERMINAL_TYPE)
        return &NodeTypeSubwoofer;

    if (TerminalDescriptor->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE)
    {
        if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_OUTPUT_TERMINAL)
            return &NodeTypeCapture;
        else if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_INPUT_TERMINAL)
            return &NodeTypePlayback;

    }
    return NULL;
}

PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR
UsbAudioGetStreamingTerminalDescriptorByIndex(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN ULONG Index)
{
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR InputTerminalDescriptor;
    ULONG TerminalCount = 0;

    for (Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x01) /* AUDIO_CONTROL */
        {
            InterfaceHeaderDescriptor = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (InterfaceHeaderDescriptor != NULL)
            {
                CommonDescriptor = USBD_ParseDescriptors(InterfaceHeaderDescriptor, InterfaceHeaderDescriptor->wTotalLength, (PVOID)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength), USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
                while (CommonDescriptor)
                {
                    InputTerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                    if (InputTerminalDescriptor->bDescriptorSubtype == 0x02 /* INPUT TERMINAL*/ || InputTerminalDescriptor->bDescriptorSubtype == 0x03 /* OUTPUT_TERMINAL*/)
                    {
                        if (InputTerminalDescriptor->wTerminalType == USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                            if (TerminalCount == Index)
                            {
                                return InputTerminalDescriptor;
                            }
                            TerminalCount++;
                        }
                    }
                    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
                    if ((ULONG_PTR)CommonDescriptor >= ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
                        break;
                }
            }
        }
    }
    return NULL;
}

PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR
UsbAudioGetNonStreamingTerminalDescriptorByIndex(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN ULONG Index)
{

    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR InterfaceHeaderDescriptor;
    PUSB_COMMON_DESCRIPTOR CommonDescriptor;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR InputTerminalDescriptor;
    ULONG TerminalCount = 0;

    for (Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x01) /* AUDIO_CONTROL */
        {
            InterfaceHeaderDescriptor = (PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (InterfaceHeaderDescriptor != NULL)
            {
                CommonDescriptor = USBD_ParseDescriptors(InterfaceHeaderDescriptor, InterfaceHeaderDescriptor->wTotalLength, (PVOID)((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->bLength), USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
                while (CommonDescriptor)
                {
                    InputTerminalDescriptor = (PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR)CommonDescriptor;
                    if (InputTerminalDescriptor->bDescriptorSubtype == 0x02 /* INPUT TERMINAL*/ || InputTerminalDescriptor->bDescriptorSubtype == 0x03 /* OUTPUT_TERMINAL*/)
                    {
                        if (InputTerminalDescriptor->wTerminalType != USB_AUDIO_STREAMING_TERMINAL_TYPE)
                        {
                            if (TerminalCount == Index)
                            {
                                return InputTerminalDescriptor;
                            }
                            TerminalCount++;
                        }
                    }
                    CommonDescriptor = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDescriptor + CommonDescriptor->bLength);
                    if ((ULONG_PTR)CommonDescriptor >= ((ULONG_PTR)InterfaceHeaderDescriptor + InterfaceHeaderDescriptor->wTotalLength))
                        break;
                }
            }
        }
    }
    return NULL;
}

VOID
UsbAudioGetDataRanges(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN UCHAR bTerminalID,
    OUT PKSDATARANGE** OutDataRanges,
    OUT PULONG OutDataRangesCount)
{
    PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR StreamingInterfaceDescriptor;
    PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR StreamingFormatDescriptor;
    PUSB_INTERFACE_DESCRIPTOR Descriptor;
    PKSDATARANGE_AUDIO DataRangeAudio;
    PKSDATARANGE *DataRangeAudioArray;

    ULONG NumFrequency, DataRangeCount, DataRangeIndex, Index;

    /* count all data ranges */
    DataRangeCount = 0;
    for (Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x02) /* AUDIO_STREAMING */
        {
            StreamingInterfaceDescriptor = (PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (StreamingInterfaceDescriptor != NULL)
            {
                ASSERT(StreamingInterfaceDescriptor->bDescriptorSubtype == 0x01);
                ASSERT(StreamingInterfaceDescriptor->wFormatTag == WAVE_FORMAT_PCM);
                if (StreamingInterfaceDescriptor->bTerminalLink == bTerminalID)
                {
                    DataRangeCount++;
                    DPRINT1("StreamingInterfaceDescriptor %p TerminalID %x\n", StreamingInterfaceDescriptor, bTerminalID);
                }
            }
            Descriptor = (PUSB_INTERFACE_DESCRIPTOR)StreamingInterfaceDescriptor;
        }
    }

    if (!DataRangeCount)
    {
        *OutDataRanges = NULL;
        *OutDataRangesCount = 0;
        return;
    }

    DataRangeAudioArray = AllocFunction(sizeof(PVOID) * DataRangeCount);
    if (DataRangeAudioArray == NULL)
    {
        /* no memory */
        *OutDataRanges = NULL;
        *OutDataRangesCount = 0;
        return;
    }

    DataRangeIndex = 0;
    for (Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1);
    Descriptor != NULL;
        Descriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)Descriptor + Descriptor->bLength), -1, -1, USB_DEVICE_CLASS_AUDIO, -1, -1))
    {
        if (Descriptor->bInterfaceSubClass == 0x02) /* AUDIO_STREAMING */
        {
            StreamingInterfaceDescriptor = (PUSB_AUDIO_STREAMING_INTERFACE_DESCRIPTOR)USBD_ParseDescriptors(ConfigurationDescriptor, ConfigurationDescriptor->wTotalLength, Descriptor, USB_AUDIO_CONTROL_TERMINAL_DESCRIPTOR_TYPE);
            if (StreamingInterfaceDescriptor != NULL)
            {
                ASSERT(StreamingInterfaceDescriptor->bDescriptorSubtype == 0x01);
                ASSERT(StreamingInterfaceDescriptor->wFormatTag == WAVE_FORMAT_PCM);
                if (StreamingInterfaceDescriptor->bTerminalLink == bTerminalID)
                {
                    StreamingFormatDescriptor = (PUSB_AUDIO_STREAMING_FORMAT_TYPE_DESCRIPTOR)((ULONG_PTR)StreamingInterfaceDescriptor + StreamingInterfaceDescriptor->bLength);
                    ASSERT(StreamingFormatDescriptor->bDescriptorType == 0x24);
                    ASSERT(StreamingFormatDescriptor->bDescriptorSubtype == 0x02);
                    ASSERT(StreamingFormatDescriptor->bFormatType == 0x01);

                    DataRangeAudio = AllocFunction(sizeof(KSDATARANGE_AUDIO));
                    if (DataRangeAudio == NULL)
                    {
                        /* no memory*/
                        return;
                    }

                    DataRangeAudio->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
                    DataRangeAudio->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
                    DataRangeAudio->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                    DataRangeAudio->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
                    DataRangeAudio->MaximumChannels = StreamingFormatDescriptor->bNrChannels;
                    /* WAVEFORMATEX wBitsPerSample is the container size, which is the subframe size.
                     * bBitResolution only gives the number of valid bits in it. */
                    DataRangeAudio->MinimumBitsPerSample = StreamingFormatDescriptor->bSubframeSize * 8;
                    DataRangeAudio->MaximumBitsPerSample = StreamingFormatDescriptor->bSubframeSize * 8;
                    /* bSamFreqType 0 means continuous sampling frequency, tSamFreq then holds
                     * the lower and the upper bound (USB Audio Data Formats 1.0, 2.2.5) */
                    NumFrequency = StreamingFormatDescriptor->bSamFreqType;
                    if (NumFrequency == 0)
                        NumFrequency = 2;

                    DataRangeAudio->MinimumSampleFrequency = MAXULONG;
                    DataRangeAudio->MaximumSampleFrequency = 0;
                    for (Index = 0; Index < NumFrequency; Index++)
                    {
                        DataRangeAudio->MinimumSampleFrequency = min(StreamingFormatDescriptor->tSamFreq[Index * 3] | StreamingFormatDescriptor->tSamFreq[(Index * 3) + 1] << 8 | StreamingFormatDescriptor->tSamFreq[(Index * 3) + 2] << 16, DataRangeAudio->MinimumSampleFrequency);
                        DataRangeAudio->MaximumSampleFrequency = max(StreamingFormatDescriptor->tSamFreq[Index * 3] | StreamingFormatDescriptor->tSamFreq[(Index * 3) + 1] << 8 | StreamingFormatDescriptor->tSamFreq[(Index * 3) + 2] << 16, DataRangeAudio->MaximumSampleFrequency);
                    }
                    DataRangeAudioArray[DataRangeIndex] = (PKSDATARANGE)DataRangeAudio;
                    DataRangeIndex++;
                }
            }
            Descriptor = (PUSB_INTERFACE_DESCRIPTOR)StreamingInterfaceDescriptor;
        }
    }

    *OutDataRanges = DataRangeAudioArray;
    *OutDataRangesCount = DataRangeCount;
}


NTSTATUS
USBAudioPinBuildDescriptors(
    PKSDEVICE Device,
    PKSPIN_DESCRIPTOR_EX *PinDescriptors,
    PULONG PinDescriptorsCount,
    PULONG PinDescriptorSize)
{
    PDEVICE_EXTENSION DeviceExtension;
    PKSPIN_DESCRIPTOR_EX Pins;
    ULONG TotalTerminalDescriptorCount = 0;
    ULONG NonStreamingTerminalDescriptorCount = 0;
    ULONG Index = 0;
    PUSB_AUDIO_CONTROL_INPUT_TERMINAL_DESCRIPTOR TerminalDescriptor = NULL;

    /* get device extension */
    DeviceExtension = Device->Context;

    CountTerminalUnits(DeviceExtension->ConfigurationDescriptor, &NonStreamingTerminalDescriptorCount, &TotalTerminalDescriptorCount);
    DPRINT("TotalTerminalDescriptorCount %lu NonStreamingTerminalDescriptorCount %lu\n", TotalTerminalDescriptorCount, NonStreamingTerminalDescriptorCount);

    /* allocate pins */
    Pins = AllocFunction(sizeof(KSPIN_DESCRIPTOR_EX) * TotalTerminalDescriptorCount);
    if (!Pins)
    {
        /* no memory*/
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Index = 0; Index < TotalTerminalDescriptorCount; Index++)
    {
        if (Index < (TotalTerminalDescriptorCount - NonStreamingTerminalDescriptorCount))
        {
            /* irp sink pins*/
            TerminalDescriptor = UsbAudioGetStreamingTerminalDescriptorByIndex(DeviceExtension->ConfigurationDescriptor, Index);
            ASSERT(TerminalDescriptor != NULL);

            Pins[Index].Dispatch = &UsbAudioPinDispatch;
            Pins[Index].PinDescriptor.InterfacesCount = 1;
            Pins[Index].PinDescriptor.Interfaces = &StandardPinInterface;
            Pins[Index].PinDescriptor.MediumsCount = 1;
            Pins[Index].PinDescriptor.Mediums = &StandardPinMedium;
            Pins[Index].PinDescriptor.Category = UsbAudioGetPinCategoryFromTerminalDescriptor(TerminalDescriptor);
            UsbAudioGetDataRanges(DeviceExtension->ConfigurationDescriptor, TerminalDescriptor->bTerminalID, (PKSDATARANGE**)&Pins[Index].PinDescriptor.DataRanges, &Pins[Index].PinDescriptor.DataRangesCount);

            if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_OUTPUT_TERMINAL)
            {
                Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_BOTH;
                Pins[Index].PinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;

                /* pin flags */
                Pins[Index].Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSFILTER_FLAG_CRITICAL_PROCESSING;
            }
            else if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_INPUT_TERMINAL)
            {
                Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
                Pins[Index].PinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;

                /* pin flags */
                Pins[Index].Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSPIN_FLAG_GENERATE_EOS_EVENTS | KSPIN_FLAG_INITIATE_PROCESSING_ON_EVERY_ARRIVAL;
            }

            /* data intersect handler */
            Pins[Index].IntersectHandler = UsbAudioPinDataIntersect;

            /* irp sinks / sources can be instantiated */
            Pins[Index].InstancesPossible = 1;
        }
        else
        {
            /* bridge pins */
            TerminalDescriptor = UsbAudioGetNonStreamingTerminalDescriptorByIndex(DeviceExtension->ConfigurationDescriptor, Index - (TotalTerminalDescriptorCount - NonStreamingTerminalDescriptorCount));
            Pins[Index].PinDescriptor.InterfacesCount = 1;
            Pins[Index].PinDescriptor.Interfaces = &StandardPinInterface;
            Pins[Index].PinDescriptor.MediumsCount = 1;
            Pins[Index].PinDescriptor.Mediums = &StandardPinMedium;
            Pins[Index].PinDescriptor.DataRanges = BridgePinAudioFormats;
            Pins[Index].PinDescriptor.DataRangesCount = 1;
            Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_BRIDGE;
            Pins[Index].PinDescriptor.Category = UsbAudioGetPinCategoryFromTerminalDescriptor(TerminalDescriptor);

            if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_INPUT_TERMINAL)
            {
                Pins[Index].PinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
            }
            else if (TerminalDescriptor->bDescriptorSubtype == USB_AUDIO_OUTPUT_TERMINAL)
            {
                Pins[Index].PinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
            }
        }

    }

    *PinDescriptors = Pins;
    *PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);
    *PinDescriptorsCount = TotalTerminalDescriptorCount;

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


NTSTATUS
USBAudioInitComponentId(
    PKSDEVICE Device,
    IN PKSCOMPONENTID ComponentId)
{
    PDEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;
    LPWSTR DescriptionBuffer;
    UNICODE_STRING GuidString;
    UNICODE_STRING Name;
    HANDLE hKey;
    GUID TempGuid;

    /* get device extension */
    DeviceExtension = Device->Context;

    /* init component id */
    ComponentId->Component = KSCOMPONENTID_USBAUDIO;
    ComponentId->Version = HIBYTE(DeviceExtension->DeviceDescriptor->bcdDevice);
    ComponentId->Revision = LOBYTE(DeviceExtension->DeviceDescriptor->bcdDevice);

    INIT_USBAUDIO_MID(&ComponentId->Manufacturer, DeviceExtension->DeviceDescriptor->idVendor);
    INIT_USBAUDIO_PID(&ComponentId->Product, DeviceExtension->DeviceDescriptor->idProduct);
    INIT_USBAUDIO_PRODUCT_NAME(&TempGuid, DeviceExtension->DeviceDescriptor->idVendor, DeviceExtension->DeviceDescriptor->idProduct, 0);

    if (DeviceExtension->DeviceDescriptor->iProduct)
    {
        Status = USBAudioGetStringDescriptor(DeviceExtension->LowerDevice, 100 * sizeof(WCHAR), DeviceExtension->DeviceDescriptor->iProduct, 0x0409 /* FIXME */, (PVOID*)&DescriptionBuffer);
        if (NT_SUCCESS(Status))
        {
            Status = RtlStringFromGUID(&TempGuid, &GuidString);
            if (NT_SUCCESS(Status))
            {
                Status = USBAudioRegCreateMediaCategoriesKey(&GuidString, &hKey);
                if (NT_SUCCESS(Status))
                {
                    RtlInitUnicodeString(&Name, L"Name");
                    ZwSetValueKey(hKey, &Name, 0, REG_SZ, DescriptionBuffer, (wcslen(DescriptionBuffer) + 1) * sizeof(WCHAR));
                    ZwClose(hKey);

                    INIT_USBAUDIO_PRODUCT_NAME(&ComponentId->Name, DeviceExtension->DeviceDescriptor->idVendor, DeviceExtension->DeviceDescriptor->idProduct, 0);
                }
                RtlFreeUnicodeString(&GuidString);
            }
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

    /* allocate descriptor */
    FilterDescriptor = AllocFunction(sizeof(KSFILTER_DESCRIPTOR));
    if (!FilterDescriptor)
    {
        /* no memory */
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init filter descriptor*/
    FilterDescriptor->Version = KSFILTER_DESCRIPTOR_VERSION;
    FilterDescriptor->Flags = 0;
    FilterDescriptor->ReferenceGuid = &KSNAME_Filter;
    FilterDescriptor->Dispatch = &USBAudioFilterDispatch;
    FilterDescriptor->CategoriesCount = 1;
    FilterDescriptor->Categories = &GUID_KSCATEGORY_AUDIO;

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
    Status = USBAudioPinBuildDescriptors(Device, (PKSPIN_DESCRIPTOR_EX *)&FilterDescriptor->PinDescriptors, &FilterDescriptor->PinDescriptorsCount, &FilterDescriptor->PinDescriptorSize);
    if (!NT_SUCCESS(Status))
    {
        /* failed*/
        FreeFunction(ComponentId);
        return Status;
    }

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
    DPRINT("KsCreateFilterFactory: %x\n", Status);

    return Status;
}


