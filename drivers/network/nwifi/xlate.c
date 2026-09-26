/*
 * PROJECT:     ReactOS Native WiFi filter
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     802.3 to native 802.11 framing and back
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nwifi.h"

/*
 * A native 802.11 miniport carries MSDUs with an 802.11 header, the protocols
 * above only speak Ethernet. Every frame is copied into a list of our own in
 * the other framing, so the lists we were given go back untouched.
 */

/* Frame control field, little-endian as it sits on the wire */
#define DOT11_FC_TYPE_MASK          0x000C
#define DOT11_FC_TYPE_DATA          0x0008
#define DOT11_FC_SUBTYPE_QOS        0x0080
#define DOT11_FC_TO_DS              0x0100
#define DOT11_FC_FROM_DS            0x0200

#define DOT11_QOS_CONTROL_LENGTH    2
#define DOT11_DATA_HEADER_LENGTH    24
#define DOT11_ETHERNET_LENGTH       14
#define DOT11_SNAP_LENGTH           8

#include <pshpack1.h>
typedef struct _DOT11_DATA_HEADER
{
    USHORT FrameControl;
    USHORT DurationId;
    UCHAR Address1[DOT11_ADDRESS_LENGTH];
    UCHAR Address2[DOT11_ADDRESS_LENGTH];
    UCHAR Address3[DOT11_ADDRESS_LENGTH];
    USHORT SequenceControl;
} DOT11_DATA_HEADER, *PDOT11_DATA_HEADER;

typedef struct _DOT11_ETHERNET_HEADER
{
    UCHAR Destination[DOT11_ADDRESS_LENGTH];
    UCHAR Source[DOT11_ADDRESS_LENGTH];
    USHORT Type;
} DOT11_ETHERNET_HEADER, *PDOT11_ETHERNET_HEADER;

/* AA-AA-03-00-00-00 followed by the EtherType */
typedef struct _DOT11_LLC_SNAP
{
    UCHAR Dsap;
    UCHAR Ssap;
    UCHAR Control;
    UCHAR Oui[3];
    USHORT Type;
} DOT11_LLC_SNAP, *PDOT11_LLC_SNAP;
#include <poppack.h>

C_ASSERT(sizeof(DOT11_DATA_HEADER) == DOT11_DATA_HEADER_LENGTH);
C_ASSERT(sizeof(DOT11_ETHERNET_HEADER) == DOT11_ETHERNET_LENGTH);
C_ASSERT(sizeof(DOT11_LLC_SNAP) == DOT11_SNAP_LENGTH);

/* The longest 802.11 data header: four addresses and QoS control */
#define DOT11_MAX_HEADER_LENGTH \
    (DOT11_DATA_HEADER_LENGTH + DOT11_ADDRESS_LENGTH + DOT11_QOS_CONTROL_LENGTH)

static
BOOLEAN
NwifiCopyFromNetBuffer(
    _In_ PNET_BUFFER NetBuffer,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PUCHAR Destination,
    _In_ ULONG Length)
{
    PMDL Mdl = NET_BUFFER_CURRENT_MDL(NetBuffer);
    ULONG Skip = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) + Offset;
    ULONG Chunk;
    PUCHAR Source;

    while (Mdl != NULL && Skip >= MmGetMdlByteCount(Mdl))
    {
        Skip -= MmGetMdlByteCount(Mdl);
        Mdl = Mdl->Next;
    }

    while (Length != 0)
    {
        if (Mdl == NULL)
            return FALSE;

        Source = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        if (Source == NULL)
            return FALSE;

        Chunk = min(Length, MmGetMdlByteCount(Mdl) - Skip);
        RtlCopyMemory(Destination, Source + Skip, Chunk);

        Destination += Chunk;
        Length -= Chunk;
        Skip = 0;
        Mdl = Mdl->Next;
    }

    return TRUE;
}

/* A list of ours with one NET_BUFFER holding Length bytes, not yet filled */
static
PNET_BUFFER_LIST
NwifiAllocateFrame(
    _In_ PNWIFI_MODULE Module,
    _In_ ULONG Length,
    _Out_ PUCHAR *Data)
{
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;

    if (Length > NWIFI_FRAME_SIZE)
        return NULL;

    NetBufferList = NdisAllocateNetBufferList(Module->NblPool, NWIFI_FRAME_CONTEXT_SIZE, 0);
    if (NetBufferList == NULL)
        return NULL;

    NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
    *Data = MmGetSystemAddressForMdlSafe(NET_BUFFER_FIRST_MDL(NetBuffer), NormalPagePriority);
    if (*Data == NULL)
    {
        NdisFreeNetBufferList(NetBufferList);
        return NULL;
    }

    NET_BUFFER_DATA_LENGTH(NetBuffer) = Length;
    NetBufferList->SourceHandle = Module->FilterHandle;
    return NetBufferList;
}

/**
 * @brief
 * Builds the 802.11 data frame for one Ethernet frame, addressed to the AP.
 *
 * @param[in] Module
 * The adapter's module, which has to be associated.
 *
 * @param[in] NetBuffer
 * The Ethernet frame.
 *
 * @return
 * A list of ours carrying the frame, or NULL when it cannot be built.
 */
/* The exemption the list gives a frame of this EtherType and destination */
static
USHORT
NwifiExemptionFor(
    _In_ PNWIFI_MODULE Module,
    _In_ const DOT11_ETHERNET_HEADER *Ethernet)
{
    USHORT PacketType = (Ethernet->Destination[0] & 0x01) ? DOT11_EXEMPT_MULTICAST : DOT11_EXEMPT_UNICAST;
    USHORT Action = DOT11_EXEMPT_NO_EXEMPTION;
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&Module->Lock, &OldIrql);
    for (i = 0; i < Module->ExemptionCount; i++)
    {
        if (Module->Exemptions[i].usEtherType == Ethernet->Type &&
            (Module->Exemptions[i].usExemptionPacketType & PacketType))
        {
            Action = Module->Exemptions[i].usExemptionActionType;
            break;
        }
    }
    KeReleaseSpinLock(&Module->Lock, OldIrql);

    return Action;
}

PNET_BUFFER_LIST
NTAPI
NwifiBuildNative(
    _In_ PNWIFI_MODULE Module,
    _In_ PNET_BUFFER NetBuffer)
{
    ULONG Length = NET_BUFFER_DATA_LENGTH(NetBuffer);
    DOT11_ETHERNET_HEADER Ethernet;
    PNET_BUFFER_LIST NetBufferList;
    PDOT11_EXTSTA_SEND_CONTEXT SendContext;
    PDOT11_DATA_HEADER Dot11;
    PDOT11_LLC_SNAP Snap;
    PUCHAR Data;
    ULONG Payload;

    if (Length < DOT11_ETHERNET_LENGTH ||
        !NwifiCopyFromNetBuffer(NetBuffer, 0, (PUCHAR)&Ethernet, sizeof(Ethernet)))
    {
        return NULL;
    }

    Payload = Length - DOT11_ETHERNET_LENGTH;
    NetBufferList = NwifiAllocateFrame(Module, DOT11_DATA_HEADER_LENGTH + DOT11_SNAP_LENGTH + Payload, &Data);
    if (NetBufferList == NULL)
        return NULL;

    /* A station sends everything to the AP, which forwards it to Address3 */
    Dot11 = (PDOT11_DATA_HEADER)Data;
    RtlZeroMemory(Dot11, sizeof(*Dot11));
    Dot11->FrameControl = DOT11_FC_TYPE_DATA | DOT11_FC_TO_DS;
    RtlCopyMemory(Dot11->Address1, Module->Bssid, DOT11_ADDRESS_LENGTH);
    RtlCopyMemory(Dot11->Address2, Ethernet.Source, DOT11_ADDRESS_LENGTH);
    RtlCopyMemory(Dot11->Address3, Ethernet.Destination, DOT11_ADDRESS_LENGTH);

    Snap = (PDOT11_LLC_SNAP)(Data + DOT11_DATA_HEADER_LENGTH);
    Snap->Dsap = 0xAA;
    Snap->Ssap = 0xAA;
    Snap->Control = 0x03;
    Snap->Oui[0] = 0;
    Snap->Oui[1] = 0;
    Snap->Oui[2] = 0;
    Snap->Type = Ethernet.Type;

    if (!NwifiCopyFromNetBuffer(NetBuffer,
                                DOT11_ETHERNET_LENGTH,
                                Data + DOT11_DATA_HEADER_LENGTH + DOT11_SNAP_LENGTH,
                                Payload))
    {
        NdisFreeNetBufferList(NetBufferList);
        return NULL;
    }

    /* The miniport learns from this whether the frame may go out unencrypted */
    SendContext = &NWIFI_FRAME_CONTEXT_OF(NetBufferList)->SendContext;
    RtlZeroMemory(SendContext, sizeof(*SendContext));
    SendContext->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    SendContext->Header.Revision = DOT11_EXTSTA_SEND_CONTEXT_REVISION_1;
    SendContext->Header.Size = sizeof(*SendContext);
    SendContext->usExemptionActionType = NwifiExemptionFor(Module, &Ethernet);
    NET_BUFFER_LIST_INFO(NetBufferList, MediaSpecificInformation) = SendContext;

    return NetBufferList;
}

/**
 * @brief
 * Builds the Ethernet frame for one received 802.11 data frame.
 *
 * @param[in] Module
 * The adapter's module.
 *
 * @param[in] NetBuffer
 * The 802.11 frame.
 *
 * @return
 * A list of ours carrying the frame, or NULL for anything that is not an
 * 802.11 data frame with a SNAP header.
 */
PNET_BUFFER_LIST
NTAPI
NwifiBuildEthernet(
    _In_ PNWIFI_MODULE Module,
    _In_ PNET_BUFFER NetBuffer)
{
    UCHAR Header[DOT11_MAX_HEADER_LENGTH + DOT11_SNAP_LENGTH];
    ULONG Length = NET_BUFFER_DATA_LENGTH(NetBuffer);
    PDOT11_ETHERNET_HEADER Ethernet;
    PNET_BUFFER_LIST NetBufferList;
    PDOT11_DATA_HEADER Dot11;
    PDOT11_LLC_SNAP Snap;
    PUCHAR Destination, Source;
    USHORT FrameControl;
    ULONG HeaderLength;
    ULONG Payload;
    PUCHAR Data;

    if (Length < DOT11_DATA_HEADER_LENGTH + DOT11_SNAP_LENGTH)
        return NULL;

    if (!NwifiCopyFromNetBuffer(NetBuffer, 0, Header, min(Length, sizeof(Header))))
        return NULL;

    Dot11 = (PDOT11_DATA_HEADER)Header;
    FrameControl = Dot11->FrameControl;
    if ((FrameControl & DOT11_FC_TYPE_MASK) != DOT11_FC_TYPE_DATA)
        return NULL;

    HeaderLength = DOT11_DATA_HEADER_LENGTH;
    if ((FrameControl & (DOT11_FC_TO_DS | DOT11_FC_FROM_DS)) == (DOT11_FC_TO_DS | DOT11_FC_FROM_DS))
        HeaderLength += DOT11_ADDRESS_LENGTH;
    if (FrameControl & DOT11_FC_SUBTYPE_QOS)
        HeaderLength += DOT11_QOS_CONTROL_LENGTH;

    if (Length < HeaderLength + DOT11_SNAP_LENGTH)
        return NULL;

    Snap = (PDOT11_LLC_SNAP)(Header + HeaderLength);
    if (Snap->Dsap != 0xAA || Snap->Ssap != 0xAA || Snap->Control != 0x03)
        return NULL;

    /* Which addresses carry the endpoints depends on the way across the DS */
    switch (FrameControl & (DOT11_FC_TO_DS | DOT11_FC_FROM_DS))
    {
        case 0:
            Destination = Dot11->Address1;
            Source = Dot11->Address2;
            break;

        case DOT11_FC_FROM_DS:
            Destination = Dot11->Address1;
            Source = Dot11->Address3;
            break;

        case DOT11_FC_TO_DS:
            Destination = Dot11->Address3;
            Source = Dot11->Address2;
            break;

        default:
            Destination = Dot11->Address3;
            Source = Header + DOT11_DATA_HEADER_LENGTH;
            break;
    }

    Payload = Length - HeaderLength - DOT11_SNAP_LENGTH;
    NetBufferList = NwifiAllocateFrame(Module, DOT11_ETHERNET_LENGTH + Payload, &Data);
    if (NetBufferList == NULL)
        return NULL;

    Ethernet = (PDOT11_ETHERNET_HEADER)Data;
    RtlCopyMemory(Ethernet->Destination, Destination, DOT11_ADDRESS_LENGTH);
    RtlCopyMemory(Ethernet->Source, Source, DOT11_ADDRESS_LENGTH);
    Ethernet->Type = Snap->Type;

    if (!NwifiCopyFromNetBuffer(NetBuffer,
                                HeaderLength + DOT11_SNAP_LENGTH,
                                Data + DOT11_ETHERNET_LENGTH,
                                Payload))
    {
        NdisFreeNetBufferList(NetBufferList);
        return NULL;
    }

    return NetBufferList;
}

/**
 * @brief
 * Follows the dot11 association indications, so sends know the BSSID and go
 * nowhere without one.
 */
/**
 * @brief
 * Keeps the privacy exemption list the WLAN service set, so sends can be
 * marked with it.
 */
VOID
NTAPI
NwifiTrackExemptions(
    _In_ PNWIFI_MODULE Module,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    PDOT11_PRIVACY_EXEMPTION_LIST List = Buffer;
    ULONG Count;
    KIRQL OldIrql;

    if (Length < FIELD_OFFSET(DOT11_PRIVACY_EXEMPTION_LIST, PrivacyExemptionEntries))
        return;

    Count = min(List->uNumOfEntries, NWIFI_EXEMPTIONS_MAX);
    if (Count > (Length - FIELD_OFFSET(DOT11_PRIVACY_EXEMPTION_LIST, PrivacyExemptionEntries)) /
                sizeof(DOT11_PRIVACY_EXEMPTION))
    {
        return;
    }

    KeAcquireSpinLock(&Module->Lock, &OldIrql);
    RtlCopyMemory(Module->Exemptions, List->PrivacyExemptionEntries, Count * sizeof(DOT11_PRIVACY_EXEMPTION));
    Module->ExemptionCount = Count;
    KeReleaseSpinLock(&Module->Lock, OldIrql);
}

VOID
NTAPI
NwifiTrackStatus(
    _In_ PNWIFI_MODULE Module,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    PDOT11_ASSOCIATION_COMPLETION_PARAMETERS Completion;
    KIRQL OldIrql;

    switch (StatusIndication->StatusCode)
    {
        case NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION:
            Completion = StatusIndication->StatusBuffer;
            if (Completion == NULL ||
                StatusIndication->StatusBufferSize < RTL_SIZEOF_THROUGH_FIELD(DOT11_ASSOCIATION_COMPLETION_PARAMETERS, uStatus) ||
                Completion->uStatus != DOT11_ASSOC_STATUS_SUCCESS)
            {
                break;
            }

            KeAcquireSpinLock(&Module->Lock, &OldIrql);
            RtlCopyMemory(Module->Bssid, Completion->MacAddr, DOT11_ADDRESS_LENGTH);
            Module->Associated = TRUE;
            KeReleaseSpinLock(&Module->Lock, OldIrql);
            break;

        case NDIS_STATUS_DOT11_DISASSOCIATION:
            KeAcquireSpinLock(&Module->Lock, &OldIrql);
            Module->Associated = FALSE;
            RtlZeroMemory(Module->Bssid, sizeof(Module->Bssid));
            KeReleaseSpinLock(&Module->Lock, OldIrql);
            break;

        default:
            break;
    }
}
