/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Service discovery client and record printer
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "bthprobe.h"

#define SDP_ERROR_RESPONSE                  0x01
#define SDP_SEARCH_ATTRIBUTE_REQUEST        0x06
#define SDP_SEARCH_ATTRIBUTE_RESPONSE       0x07

#define SDP_PRIMARY_LANGUAGE_BASE           0x0100
#define SDP_ATTRIB_ADDITIONAL_PROTOCOLS     0x000D
#define SDP_MAX_COLLECTED                   (64 * 1024)
#define SDP_MAX_CONTINUATION                16
#define SDP_MAX_DEPTH                       8

typedef struct _SDP_CLIENT
{
    BOOLEAN Ready;
    ULONG Length;
    UCHAR Pdu[L2CAP_LOCAL_MTU];
} SDP_CLIENT, *PSDP_CLIENT;

typedef struct _SDP_ELEMENT
{
    UCHAR Type;
    ULONG Length;
    ULONG Total;
    const UCHAR *Data;
} SDP_ELEMENT, *PSDP_ELEMENT;

/* The Bluetooth base UUID, less its first four bytes */
static const UCHAR SdpBaseUuidTail[12] =
{
    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB
};

/* SDP fields are big endian, unlike HCI and L2CAP */
static
USHORT
SdpRead16(
    _In_reads_(2) const UCHAR *Data)
{
    return (USHORT)((Data[0] << 8) | Data[1]);
}

static
ULONG
SdpRead32(
    _In_reads_(4) const UCHAR *Data)
{
    return ((ULONG)Data[0] << 24) | ((ULONG)Data[1] << 16) | ((ULONG)Data[2] << 8) | Data[3];
}

static
BOOLEAN
SdpParseElement(
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length,
    _Out_ PSDP_ELEMENT Element)
{
    ULONG Header;
    ULONG Size;

    if (Length < 1)
        return FALSE;

    Element->Type = Data[0] >> 3;
    Header = 1;

    switch (Data[0] & 0x07)
    {
        case 0:
            Size = (Element->Type == SDP_TYPE_NIL) ? 0 : 1;
            break;
        case 1:
            Size = 2;
            break;
        case 2:
            Size = 4;
            break;
        case 3:
            Size = 8;
            break;
        case 4:
            Size = 16;
            break;
        case 5:
            if (Length < 2)
                return FALSE;
            Size = Data[1];
            Header = 2;
            break;
        case 6:
            if (Length < 3)
                return FALSE;
            Size = SdpRead16(&Data[1]);
            Header = 3;
            break;
        default:
            if (Length < 5)
                return FALSE;
            Size = SdpRead32(&Data[1]);
            Header = 5;
            break;
    }

    if (Size > Length - Header)
        return FALSE;

    Element->Data = Data + Header;
    Element->Length = Size;
    Element->Total = Header + Size;

    return TRUE;
}

/* Short form of a UUID, or zero when it is a full 128 bit vendor UUID */
static
ULONG
SdpShortUuid(
    _In_ const SDP_ELEMENT *Element)
{
    if (Element->Type != SDP_TYPE_UUID)
        return 0;

    if (Element->Length == 2)
        return SdpRead16(Element->Data);

    if (Element->Length == 4)
        return SdpRead32(Element->Data);

    if (Element->Length == 16 && memcmp(&Element->Data[4], SdpBaseUuidTail, sizeof(SdpBaseUuidTail)) == 0)
        return SdpRead32(Element->Data);

    return 0;
}

static
const CHAR *
SdpUuidName(
    _In_ ULONG Uuid)
{
    switch (Uuid)
    {
        case SDP_PROTOCOL_UUID16:
            return "SDP";
        case RFCOMM_PROTOCOL_UUID16:
            return "RFCOMM";
        case OBEX_PROTOCOL_UUID16:
            return "OBEX";
        case BNEP_PROTOCOL_UUID16:
            return "BNEP";
        case HID_PROTOCOL_UUID16:
            return "HIDP";
        case AVCTP_PROTOCOL_UUID16:
            return "AVCTP";
        case AVDTP_PROTOCOL_UUID16:
            return "AVDTP";
        case L2CAP_PROTOCOL_UUID16:
            return "L2CAP";
        case ServiceDiscoveryServerServiceClassID_UUID16:
            return "Service Discovery Server";
        case PublicBrowseGroupServiceClassID_UUID16:
            return "Public Browse Group";
        case SerialPortServiceClassID_UUID16:
            return "Serial Port";
        case DialupNetworkingServiceClassID_UUID16:
            return "Dial-up Networking";
        case OBEXObjectPushServiceClassID_UUID16:
            return "Object Push";
        case OBEXFileTransferServiceClassID_UUID16:
            return "File Transfer";
        case HeadsetServiceClassID_UUID16:
            return "Headset";
        case AudioSourceServiceClassID_UUID16:
            return "Audio Source";
        case AudioSinkSourceServiceClassID_UUID16:
            return "Audio Sink";
        case AVRemoteControlTargetServiceClassID_UUID16:
            return "A/V Remote Control Target";
        case AdvancedAudioDistributionServiceClassID_UUID16:
            return "Advanced Audio Distribution";
        case AVRemoteControlServiceClassID_UUID16:
            return "A/V Remote Control";
        case HeadsetAudioGatewayServiceClassID_UUID16:
            return "Headset Audio Gateway";
        case PANUServiceClassID_UUID16:
            return "PAN User";
        case NAPServiceClassID_UUID16:
            return "Network Access Point";
        case GNServiceClassID_UUID16:
            return "Group Ad-hoc Network";
        case HandsfreeServiceClassID_UUID16:
            return "Hands-Free";
        case HandsfreeAudioGatewayServiceClassID_UUID16:
            return "Hands-Free Audio Gateway";
        case HumanInterfaceDeviceServiceClassID_UUID16:
            return "Human Interface Device";
        case PnPInformationServiceClassID_UUID16:
            return "PnP Information";
        case GenericNetworkingServiceClassID_UUID16:
            return "Generic Networking";
        case GenericFileTransferServiceClassID_UUID16:
            return "Generic File Transfer";
        case GenericAudioServiceClassID_UUID16:
            return "Generic Audio";
        case GenericTelephonyServiceClassID_UUID16:
            return "Generic Telephony";

        /* Assigned after the SDK header */
        case 0x112F:
            return "Phonebook Access Server";
        case 0x1132:
            return "Message Access Server";
        case 0x1800:
            return "Generic Access";
        case 0x1801:
            return "Generic Attribute";
        default:
            return NULL;
    }
}

static
VOID
SdpPrintUuid(
    _In_ const SDP_ELEMENT *Element)
{
    const CHAR *Name;
    ULONG Uuid;
    ULONG Index;

    Uuid = SdpShortUuid(Element);
    if (Uuid != 0)
    {
        Name = SdpUuidName(Uuid);
        if (Name != NULL)
            printf("%s", Name);
        else
            printf("UUID 0x%04lX", Uuid);

        return;
    }

    printf("UUID ");
    for (Index = 0; Index < Element->Length; Index++)
    {
        printf("%02X", Element->Data[Index]);
        if (Index == 3 || Index == 5 || Index == 7 || Index == 9)
            printf("-");
    }
}

static
VOID
SdpPrintElement(
    _In_ const SDP_ELEMENT *Element,
    _In_ ULONG Depth)
{
    SDP_ELEMENT Child;
    ULONG Offset;
    ULONG Index;

    switch (Element->Type)
    {
        case SDP_TYPE_NIL:
            printf("nil");
            break;

        case SDP_TYPE_UINT:
        case SDP_TYPE_INT:
            if (Element->Length == 1)
                printf("0x%02X", Element->Data[0]);
            else if (Element->Length == 2)
                printf("0x%04X", SdpRead16(Element->Data));
            else if (Element->Length == 4)
                printf("0x%08lX", SdpRead32(Element->Data));
            else
            {
                printf("0x");
                for (Index = 0; Index < Element->Length; Index++)
                    printf("%02X", Element->Data[Index]);
            }
            break;

        case SDP_TYPE_UUID:
            SdpPrintUuid(Element);
            break;

        case SDP_TYPE_STRING:
        case SDP_TYPE_URL:
            printf("\"%.*s\"", (int)Element->Length, (const CHAR *)Element->Data);
            break;

        case SDP_TYPE_BOOLEAN:
            printf("%s", (Element->Length != 0 && Element->Data[0] != 0) ? "yes" : "no");
            break;

        case SDP_TYPE_SEQUENCE:
        case SDP_TYPE_ALTERNATIVE:
            if (Depth >= SDP_MAX_DEPTH)
            {
                printf("(...)");
                break;
            }

            printf("(");
            for (Offset = 0; Offset < Element->Length; Offset += Child.Total)
            {
                if (!SdpParseElement(Element->Data + Offset, Element->Length - Offset, &Child))
                    break;

                if (Offset != 0)
                    printf(", ");

                SdpPrintElement(&Child, Depth + 1);
            }
            printf(")");
            break;

        default:
            printf("type %u, %lu bytes", Element->Type, Element->Length);
            break;
    }
}

static
VOID
SdpPrintUuidList(
    _In_ const SDP_ELEMENT *List)
{
    SDP_ELEMENT Uuid;
    ULONG Offset;

    if (List->Type != SDP_TYPE_SEQUENCE)
    {
        SdpPrintElement(List, 0);
        return;
    }

    for (Offset = 0; Offset < List->Length; Offset += Uuid.Total)
    {
        if (!SdpParseElement(List->Data + Offset, List->Length - Offset, &Uuid))
            break;

        if (Offset != 0)
            printf(", ");

        SdpPrintElement(&Uuid, 0);
    }
}

/* Each layer is a sequence of a protocol UUID and its parameters, like L2CAP PSM 0x0011 > HIDP */
static
VOID
SdpPrintProtocols(
    _In_ const SDP_ELEMENT *List)
{
    SDP_ELEMENT Layer;
    SDP_ELEMENT Protocol;
    SDP_ELEMENT Parameter;
    ULONG LayerOffset;
    ULONG Offset;
    ULONG Uuid;

    if (List->Type != SDP_TYPE_SEQUENCE)
    {
        SdpPrintElement(List, 0);
        return;
    }

    for (LayerOffset = 0; LayerOffset < List->Length; LayerOffset += Layer.Total)
    {
        if (!SdpParseElement(List->Data + LayerOffset, List->Length - LayerOffset, &Layer) ||
            Layer.Type != SDP_TYPE_SEQUENCE ||
            !SdpParseElement(Layer.Data, Layer.Length, &Protocol))
        {
            break;
        }

        if (LayerOffset != 0)
            printf(" > ");

        SdpPrintElement(&Protocol, 0);
        Uuid = SdpShortUuid(&Protocol);

        for (Offset = Protocol.Total; Offset < Layer.Length; Offset += Parameter.Total)
        {
            if (!SdpParseElement(Layer.Data + Offset, Layer.Length - Offset, &Parameter))
                break;

            if (Uuid == L2CAP_PROTOCOL_UUID16 && Parameter.Type == SDP_TYPE_UINT && Parameter.Length == 2)
                printf(" PSM 0x%04X", SdpRead16(Parameter.Data));
            else if (Uuid == RFCOMM_PROTOCOL_UUID16 && Parameter.Type == SDP_TYPE_UINT && Parameter.Length == 1)
                printf(" channel %u", Parameter.Data[0]);
            else
            {
                printf(" ");
                SdpPrintElement(&Parameter, 0);
            }
        }
    }
}

static
VOID
SdpPrintProfiles(
    _In_ const SDP_ELEMENT *List)
{
    SDP_ELEMENT Profile;
    SDP_ELEMENT Uuid;
    SDP_ELEMENT Version;
    ULONG Offset;
    USHORT Value;

    if (List->Type != SDP_TYPE_SEQUENCE)
    {
        SdpPrintElement(List, 0);
        return;
    }

    for (Offset = 0; Offset < List->Length; Offset += Profile.Total)
    {
        if (!SdpParseElement(List->Data + Offset, List->Length - Offset, &Profile) ||
            Profile.Type != SDP_TYPE_SEQUENCE ||
            !SdpParseElement(Profile.Data, Profile.Length, &Uuid))
        {
            break;
        }

        if (Offset != 0)
            printf(", ");

        SdpPrintElement(&Uuid, 0);

        if (SdpParseElement(Profile.Data + Uuid.Total, Profile.Length - Uuid.Total, &Version) &&
            Version.Type == SDP_TYPE_UINT &&
            Version.Length == 2)
        {
            /* Profile versions are major.minor packed into one 16 bit value */
            Value = SdpRead16(Version.Data);
            printf(" %u.%u", Value >> 8, Value & 0xFF);
        }
    }
}

static
VOID
SdpPrintHidDescriptors(
    _In_ const SDP_ELEMENT *List)
{
    SDP_ELEMENT Entry;
    SDP_ELEMENT Type;
    SDP_ELEMENT Descriptor;
    ULONG Offset;
    ULONG Index;

    if (List->Type != SDP_TYPE_SEQUENCE)
        return;

    for (Offset = 0; Offset < List->Length; Offset += Entry.Total)
    {
        if (!SdpParseElement(List->Data + Offset, List->Length - Offset, &Entry) ||
            Entry.Type != SDP_TYPE_SEQUENCE ||
            !SdpParseElement(Entry.Data, Entry.Length, &Type) ||
            !SdpParseElement(Entry.Data + Type.Total, Entry.Length - Type.Total, &Descriptor))
        {
            break;
        }

        /* Type 0x22 is a report descriptor, carried as a byte string */
        printf("    HID descriptor   type 0x%02X, %lu bytes\n",
               (Type.Length != 0) ? Type.Data[0] : 0,
               Descriptor.Length);

        for (Index = 0; Index < Descriptor.Length; Index++)
        {
            if (Index % 16 == 0)
                printf("      ");

            printf("%02X ", Descriptor.Data[Index]);

            if (Index % 16 == 15 || Index + 1 == Descriptor.Length)
                printf("\n");
        }
    }
}

static
const CHAR *
SdpHidSubclassName(
    _In_ UCHAR Subclass)
{
    switch (Subclass & 0xC0)
    {
        case 0x40:
            return "keyboard";
        case 0x80:
            return "pointing device";
        case 0xC0:
            return "keyboard and pointing device";
        default:
            return "neither keyboard nor pointing device";
    }
}

static
VOID
SdpPrintAttribute(
    _In_ USHORT Attribute,
    _In_ const SDP_ELEMENT *Value,
    _In_ BOOLEAN Verbose)
{
    switch (Attribute)
    {
        case SDP_ATTRIB_RECORD_HANDLE:
            if (Value->Type == SDP_TYPE_UINT && Value->Length == 4)
                printf("  Record 0x%08lX\n", SdpRead32(Value->Data));
            return;

        case SDP_ATTRIB_CLASS_ID_LIST:
            printf("    Service          ");
            SdpPrintUuidList(Value);
            break;

        case SDP_ATTRIB_PROTOCOL_DESCRIPTOR_LIST:
            printf("    Protocols        ");
            SdpPrintProtocols(Value);
            break;

        case SDP_ATTRIB_ADDITIONAL_PROTOCOLS:
            /* A sequence of protocol lists, HID keeps its interrupt channel here */
            printf("    More protocols   ");
            if (Value->Type == SDP_TYPE_SEQUENCE)
            {
                SDP_ELEMENT Inner;

                if (SdpParseElement(Value->Data, Value->Length, &Inner))
                    SdpPrintProtocols(&Inner);
            }
            break;

        case SDP_ATTRIB_PROFILE_DESCRIPTOR_LIST:
            printf("    Profiles         ");
            SdpPrintProfiles(Value);
            break;

        case SDP_PRIMARY_LANGUAGE_BASE + STRING_NAME_OFFSET:
            printf("    Name             ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_PRIMARY_LANGUAGE_BASE + STRING_DESCRIPTION_OFFSET:
            printf("    Description      ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_PRIMARY_LANGUAGE_BASE + STRING_PROVIDER_NAME_OFFSET:
            printf("    Provider         ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_ATTRIB_HID_DEVICE_SUBCLASS:
            if (Value->Type != SDP_TYPE_UINT || Value->Length != 1)
                return;
            printf("    HID subclass     0x%02X, %s", Value->Data[0], SdpHidSubclassName(Value->Data[0]));
            break;

        case SDP_ATTRIB_HID_COUNTRY_CODE:
            printf("    HID country      ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_ATTRIB_HID_VIRTUAL_CABLE:
            printf("    Virtual cable    ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_ATTRIB_HID_RECONNECT_INITIATE:
            printf("    Reconnects       ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_ATTRIB_HID_NORMALLY_CONNECTABLE:
            printf("    Connectable      ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_ATTRIB_HID_BOOT_DEVICE:
            printf("    Boot protocol    ");
            SdpPrintElement(Value, 0);
            break;

        case SDP_ATTRIB_HID_DESCRIPTOR_LIST:
            SdpPrintHidDescriptors(Value);
            return;

        default:
            if (!Verbose)
                return;

            printf("    Attribute 0x%04X ", Attribute);
            SdpPrintElement(Value, 0);
            break;
    }

    printf("\n");
}

static
ULONG
SdpPrintRecords(
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length,
    _In_ BOOLEAN Verbose)
{
    SDP_ELEMENT Lists;
    SDP_ELEMENT Record;
    SDP_ELEMENT Id;
    SDP_ELEMENT Value;
    ULONG RecordOffset;
    ULONG Offset;
    ULONG Count;

    Count = 0;

    if (!SdpParseElement(Data, Length, &Lists) || Lists.Type != SDP_TYPE_SEQUENCE)
    {
        printf("  The device sent a malformed attribute list\n");
        return 0;
    }

    for (RecordOffset = 0; RecordOffset < Lists.Length; RecordOffset += Record.Total)
    {
        if (!SdpParseElement(Lists.Data + RecordOffset, Lists.Length - RecordOffset, &Record) ||
            Record.Type != SDP_TYPE_SEQUENCE)
        {
            break;
        }

        Count++;

        /* Attribute ID and value pairs */
        for (Offset = 0; Offset < Record.Length; Offset += Id.Total + Value.Total)
        {
            if (!SdpParseElement(Record.Data + Offset, Record.Length - Offset, &Id) ||
                Id.Type != SDP_TYPE_UINT ||
                Id.Length != 2 ||
                !SdpParseElement(Record.Data + Offset + Id.Total, Record.Length - Offset - Id.Total, &Value))
            {
                printf("    malformed attribute\n");
                break;
            }

            SdpPrintAttribute(SdpRead16(Id.Data), &Value, Verbose);
        }

        printf("\n");
    }

    return Count;
}

static L2CAP_RECEIVE_ROUTINE SdpReceive;

static
VOID
SdpReceive(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PSDP_CLIENT Client;

    UNREFERENCED_PARAMETER(Radio);

    Client = (PSDP_CLIENT)Channel->Context;
    Length = min(Length, sizeof(Client->Pdu));

    CopyMemory(Client->Pdu, Data, Length);
    Client->Length = Length;
    Client->Ready = TRUE;
}

BOOLEAN
SdpBrowse(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link)
{
    UCHAR Continuation[SDP_MAX_CONTINUATION];
    UCHAR Request[64];
    PL2CAP_CHANNEL Channel;
    SDP_CLIENT Client;
    PUCHAR Collected;
    ULONG CollectedLength;
    ULONG ParamLength;
    ULONG ByteCount;
    ULONG Length;
    DWORD Deadline;
    USHORT Transaction;
    USHORT Cid;
    UCHAR ContinuationLength;
    BOOLEAN Result;

    printf("Browsing services\n");

    ZeroMemory(&Client, sizeof(Client));
    Channel = L2capConnect(Radio, Link, L2CAP_PSM_SDP, SdpReceive, &Client);
    if (Channel == NULL)
        return FALSE;

    Cid = Channel->LocalCid;
    Collected = malloc(SDP_MAX_COLLECTED);
    if (Collected == NULL)
    {
        L2capDisconnect(Radio, Channel);
        return FALSE;
    }

    CollectedLength = 0;
    ContinuationLength = 0;
    Transaction = 1;
    Result = FALSE;

    for (;;)
    {
        Length = 5;

        /* Every service speaks L2CAP, so searching for it lists them all */
        Request[Length++] = 0x35;
        Request[Length++] = 0x03;
        Request[Length++] = 0x19;
        Request[Length++] = (UCHAR)(L2CAP_PROTOCOL_UUID16 >> 8);
        Request[Length++] = (UCHAR)(L2CAP_PROTOCOL_UUID16 & 0xFF);

        /* Keep each response inside the MTU we offered */
        Request[Length++] = (UCHAR)((L2CAP_LOCAL_MTU - 24) >> 8);
        Request[Length++] = (UCHAR)((L2CAP_LOCAL_MTU - 24) & 0xFF);

        /* All attributes, 0x0000 through 0xFFFF */
        Request[Length++] = 0x35;
        Request[Length++] = 0x05;
        Request[Length++] = 0x0A;
        Request[Length++] = 0x00;
        Request[Length++] = 0x00;
        Request[Length++] = 0xFF;
        Request[Length++] = 0xFF;

        Request[Length++] = ContinuationLength;
        CopyMemory(&Request[Length], Continuation, ContinuationLength);
        Length += ContinuationLength;

        Request[0] = SDP_SEARCH_ATTRIBUTE_REQUEST;
        Request[1] = (UCHAR)(Transaction >> 8);
        Request[2] = (UCHAR)(Transaction & 0xFF);
        Request[3] = (UCHAR)((Length - 5) >> 8);
        Request[4] = (UCHAR)((Length - 5) & 0xFF);

        Client.Ready = FALSE;
        if (!L2capSend(Radio, Channel, Request, Length))
        {
            printf("  could not send the SDP request\n");
            break;
        }

        Deadline = GetTickCount() + BT_SHORT_TIMEOUT;
        while (!Client.Ready && Channel->InUse && Channel->LocalCid == Cid && !BtStopRequested && !BtExpired(Deadline))
        {
            if (HciPump(Radio, BtRemaining(Deadline)) == HciItemError)
                break;
        }

        if (!Client.Ready)
        {
            printf("  no SDP response\n");
            break;
        }

        if (Client.Length < 5 || SdpRead16(&Client.Pdu[1]) != Transaction)
        {
            printf("  malformed SDP response\n");
            break;
        }

        ParamLength = SdpRead16(&Client.Pdu[3]);
        if (5 + ParamLength > Client.Length)
        {
            printf("  truncated SDP response\n");
            break;
        }

        if (Client.Pdu[0] == SDP_ERROR_RESPONSE)
        {
            printf("  the device answered with SDP error 0x%04X\n", (ParamLength >= 2) ? SdpRead16(&Client.Pdu[5]) : 0);
            break;
        }

        if (Client.Pdu[0] != SDP_SEARCH_ATTRIBUTE_RESPONSE || ParamLength < 3)
        {
            printf("  unexpected SDP response 0x%02X\n", Client.Pdu[0]);
            break;
        }

        ByteCount = SdpRead16(&Client.Pdu[5]);
        if (2 + ByteCount + 1 > ParamLength || CollectedLength + ByteCount > SDP_MAX_COLLECTED)
        {
            printf("  SDP response does not add up\n");
            break;
        }

        CopyMemory(&Collected[CollectedLength], &Client.Pdu[7], ByteCount);
        CollectedLength += ByteCount;

        /* A non-empty continuation state means ask again for the rest */
        ContinuationLength = Client.Pdu[7 + ByteCount];
        if (ContinuationLength == 0)
        {
            Result = TRUE;
            break;
        }

        if (ContinuationLength > SDP_MAX_CONTINUATION || 2 + ByteCount + 1 + ContinuationLength > ParamLength)
        {
            printf("  bad SDP continuation state\n");
            break;
        }

        CopyMemory(Continuation, &Client.Pdu[8 + ByteCount], ContinuationLength);
        Transaction++;
    }

    if (Result)
        printf("Found %lu service records\n\n", SdpPrintRecords(Collected, CollectedLength, Radio->Verbose));

    free(Collected);

    if (Channel->InUse && Channel->LocalCid == Cid)
        L2capDisconnect(Radio, Channel);

    return Result;
}
