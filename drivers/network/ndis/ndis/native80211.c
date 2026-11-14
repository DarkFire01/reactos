/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Native 802.11 to Ethernet translation for NDIS 6 miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A native 802.11 miniport carries data as 802.11 MPDUs, but the protocols
 * above NDIS only speak Ethernet. Received frames are turned into 802.3 on
 * their way up and sends are turned back into 802.11 on their way down, the
 * same job a Native WiFi MAC driver does on Windows. The association the
 * sends are addressed to is tracked from the dot11 status indications.
 */

#include "ndissys.h"

#define NDEBUG
#include <debug.h>

/* Frame control field, little-endian as it sits on the wire */
#define DOT11_FC_TYPE_MASK        0x000C
#define DOT11_FC_TYPE_DATA        0x0008
#define DOT11_FC_SUBTYPE_QOS      0x0080
#define DOT11_FC_TO_DS            0x0100
#define DOT11_FC_FROM_DS          0x0200

#define DOT11_ADDRESS_LENGTH      6
#define DOT11_QOS_CONTROL_LENGTH  2

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

#define DOT11_DATA_HEADER_LENGTH    24
#define DOT11_ETHERNET_LENGTH       14
#define DOT11_SNAP_LENGTH            8

C_ASSERT(sizeof(DOT11_DATA_HEADER) == DOT11_DATA_HEADER_LENGTH);
C_ASSERT(sizeof(DOT11_ETHERNET_HEADER) == DOT11_ETHERNET_LENGTH);
C_ASSERT(sizeof(DOT11_LLC_SNAP) == DOT11_SNAP_LENGTH);

/**
 * @brief
 * Whether an adapter frames its data the native 802.11 way and so needs the
 * translation this file provides.
 */
BOOLEAN
NTAPI
NdisDot11Active(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    return Adapter->Core.MediaType == NdisMediumNative802_11;
}

/**
 * @brief
 * Turns one received 802.11 data frame into an 802.3 frame in place.
 *
 * @return
 * TRUE when the frame was rewritten, FALSE when it was left untouched because
 * it is not an 802.11 data frame carrying a SNAP header.
 */
static
BOOLEAN
NdisDot11ReceiveNetBuffer(
    _In_ PNET_BUFFER NetBuffer)
{
    UCHAR Storage[DOT11_DATA_HEADER_LENGTH + DOT11_QOS_CONTROL_LENGTH +
                  DOT11_ADDRESS_LENGTH + DOT11_SNAP_LENGTH];
    DOT11_ETHERNET_HEADER Ethernet;
    PDOT11_DATA_HEADER Dot11;
    PDOT11_LLC_SNAP Snap;
    PVOID Front;
    USHORT FrameControl;
    ULONG HeaderLength;
    ULONG Strip;

    Dot11 = NdisGetDataBuffer(NetBuffer, sizeof(Storage), Storage, 1, 0);
    if (Dot11 == NULL)
        return FALSE;

    FrameControl = Dot11->FrameControl;
    if ((FrameControl & DOT11_FC_TYPE_MASK) != DOT11_FC_TYPE_DATA)
        return FALSE;

    HeaderLength = DOT11_DATA_HEADER_LENGTH;
    if ((FrameControl & (DOT11_FC_TO_DS | DOT11_FC_FROM_DS)) ==
        (DOT11_FC_TO_DS | DOT11_FC_FROM_DS))
    {
        HeaderLength += DOT11_ADDRESS_LENGTH;
    }
    if (FrameControl & DOT11_FC_SUBTYPE_QOS)
        HeaderLength += DOT11_QOS_CONTROL_LENGTH;

    Snap = (PDOT11_LLC_SNAP)((PUCHAR)Dot11 + HeaderLength);
    if (Snap->Dsap != 0xAA || Snap->Ssap != 0xAA || Snap->Control != 0x03)
        return FALSE;

    /* Which address holds the source and the destination depends on the
       direction the frame took across the distribution system */
    if (FrameControl & DOT11_FC_FROM_DS)
    {
        RtlCopyMemory(Ethernet.Destination, Dot11->Address1, DOT11_ADDRESS_LENGTH);
        RtlCopyMemory(Ethernet.Source, Dot11->Address3, DOT11_ADDRESS_LENGTH);
    }
    else
    {
        RtlCopyMemory(Ethernet.Destination, Dot11->Address3, DOT11_ADDRESS_LENGTH);
        RtlCopyMemory(Ethernet.Source, Dot11->Address2, DOT11_ADDRESS_LENGTH);
    }
    Ethernet.Type = Snap->Type;

    /* Drop the header and SNAP but keep room for the Ethernet header, then
       lay it over the last bytes ahead of the payload */
    Strip = HeaderLength + DOT11_SNAP_LENGTH - DOT11_ETHERNET_LENGTH;
    NdisAdvanceNetBufferDataStart(NetBuffer, Strip, FALSE, NULL);

    Front = NdisGetDataBuffer(NetBuffer, DOT11_ETHERNET_LENGTH, NULL, 1, 0);
    if (Front == NULL)
    {
        NdisRetreatNetBufferDataStart(NetBuffer, Strip, 0, NULL);
        return FALSE;
    }

    RtlCopyMemory(Front, &Ethernet, DOT11_ETHERNET_LENGTH);
    return TRUE;
}

/**
 * @brief
 * Rewrites received 802.11 data frames as 802.3 before the protocols see them.
 */
VOID
NTAPI
NdisDot11ReceiveToEthernet(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists)
{
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;

    UNREFERENCED_PARAMETER(Adapter);

    for (NetBufferList = NetBufferLists;
         NetBufferList != NULL;
         NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList))
    {
        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
             NetBuffer != NULL;
             NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
        {
            NdisDot11ReceiveNetBuffer(NetBuffer);
        }
    }
}

/**
 * @brief
 * Turns one 802.3 frame into an 802.11 data frame addressed to the AP.
 */
static
BOOLEAN
NdisDot11SendNetBuffer(
    _In_ PMINIPORT_CORE Core,
    _In_ PNET_BUFFER NetBuffer)
{
    DOT11_ETHERNET_HEADER Ethernet;
    PDOT11_DATA_HEADER Dot11;
    PDOT11_LLC_SNAP Snap;
    PVOID Data;
    NDIS_STATUS Status;
    ULONG Grow;

    Data = NdisGetDataBuffer(NetBuffer, DOT11_ETHERNET_LENGTH, &Ethernet, 1, 0);
    if (Data == NULL)
        return FALSE;
    if (Data != &Ethernet)
        RtlCopyMemory(&Ethernet, Data, DOT11_ETHERNET_LENGTH);

    Grow = DOT11_DATA_HEADER_LENGTH + DOT11_SNAP_LENGTH - DOT11_ETHERNET_LENGTH;
    Status = NdisRetreatNetBufferDataStart(NetBuffer, Grow, 0, NULL);
    if (Status != NDIS_STATUS_SUCCESS)
        return FALSE;

    Dot11 = NdisGetDataBuffer(NetBuffer, DOT11_DATA_HEADER_LENGTH + DOT11_SNAP_LENGTH, NULL, 1, 0);
    if (Dot11 == NULL)
    {
        NdisAdvanceNetBufferDataStart(NetBuffer, Grow, FALSE, NULL);
        return FALSE;
    }

    RtlZeroMemory(Dot11, DOT11_DATA_HEADER_LENGTH);
    Dot11->FrameControl = DOT11_FC_TYPE_DATA | DOT11_FC_TO_DS;
    RtlCopyMemory(Dot11->Address1, Core->Dot11Bssid, DOT11_ADDRESS_LENGTH);
    RtlCopyMemory(Dot11->Address2, Ethernet.Source, DOT11_ADDRESS_LENGTH);
    RtlCopyMemory(Dot11->Address3, Ethernet.Destination, DOT11_ADDRESS_LENGTH);

    Snap = (PDOT11_LLC_SNAP)((PUCHAR)Dot11 + DOT11_DATA_HEADER_LENGTH);
    Snap->Dsap = 0xAA;
    Snap->Ssap = 0xAA;
    Snap->Control = 0x03;
    Snap->Oui[0] = 0;
    Snap->Oui[1] = 0;
    Snap->Oui[2] = 0;
    Snap->Type = Ethernet.Type;
    return TRUE;
}

/**
 * @brief
 * Rewrites 802.3 sends as 802.11 before the miniport is given them. Frames go
 * nowhere until the adapter has an association to address them to.
 */
VOID
NTAPI
NdisDot11SendToNative(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;

    if (!Core->Dot11Associated)
        return;

    for (NetBufferList = NetBufferLists;
         NetBufferList != NULL;
         NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList))
    {
        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
             NetBuffer != NULL;
             NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
        {
            NdisDot11SendNetBuffer(Core, NetBuffer);
        }
    }
}

/**
 * @brief
 * Follows the dot11 association indications so sends know the BSSID and the
 * link is only reported up once there is one.
 */
VOID
NTAPI
NdisDot11CaptureStatus(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_STATUS StatusCode,
    _In_reads_bytes_opt_(BufferLength) PVOID StatusBuffer,
    _In_ ULONG BufferLength)
{
    PMINIPORT_CORE Core = &Adapter->Core;

    switch (StatusCode)
    {
        case NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION:
            /* The AP address follows the object header of
               DOT11_ASSOCIATION_COMPLETION_PARAMETERS */
            if (StatusBuffer != NULL &&
                BufferLength >= sizeof(NDIS_OBJECT_HEADER) + DOT11_ADDRESS_LENGTH)
            {
                RtlCopyMemory(Core->Dot11Bssid,
                              (PUCHAR)StatusBuffer + sizeof(NDIS_OBJECT_HEADER),
                              DOT11_ADDRESS_LENGTH);
                Core->Dot11Associated = TRUE;
            }
            break;

        case NDIS_STATUS_DOT11_DISASSOCIATION:
            Core->Dot11Associated = FALSE;
            RtlZeroMemory(Core->Dot11Bssid, sizeof(Core->Dot11Bssid));
            break;

        default:
            break;
    }
}
