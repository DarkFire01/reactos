/*
 * PROJECT:     ReactOS Cellular Data Adapter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Emulated Ethernet over the raw IP of the modem, with local ARP and DHCP
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rmnet.h"

#define NDEBUG
#include <debug.h>

#define RMNET_IP_HEADER             20
#define RMNET_UDP_HEADER            8
#define RMNET_IP_PROTOCOL_UDP       17
#define RMNET_DHCP_SERVER_PORT      67
#define RMNET_DHCP_CLIENT_PORT      68
#define RMNET_DHCP_FIXED            236
#define RMNET_DHCP_COOKIE           0x63825363
#define RMNET_DHCP_LEASE_SECONDS    86400

#define RMNET_DHCP_OPT_PAD          0
#define RMNET_DHCP_OPT_SUBNET       1
#define RMNET_DHCP_OPT_ROUTER       3
#define RMNET_DHCP_OPT_DNS          6
#define RMNET_DHCP_OPT_MTU          26
#define RMNET_DHCP_OPT_LEASE        51
#define RMNET_DHCP_OPT_TYPE         53
#define RMNET_DHCP_OPT_SERVER       54
#define RMNET_DHCP_OPT_END          255

#define RMNET_DHCP_DISCOVER         1
#define RMNET_DHCP_OFFER            2
#define RMNET_DHCP_REQUEST          3
#define RMNET_DHCP_ACK              5
#define RMNET_DHCP_NAK              6

#define RMNET_ARP_REQUEST           1
#define RMNET_ARP_REPLY             2
#define RMNET_ARP_LENGTH            28

static
USHORT
RmNetGet16(
    _In_reads_bytes_(2) const UCHAR *Data)
{
    return (USHORT)((Data[0] << 8) | Data[1]);
}

static
VOID
RmNetPut16(
    _Out_writes_bytes_(2) PUCHAR Data,
    _In_ USHORT Value)
{
    Data[0] = (UCHAR)(Value >> 8);
    Data[1] = (UCHAR)Value;
}

static
VOID
RmNetPut32(
    _Out_writes_bytes_(4) PUCHAR Data,
    _In_ ULONG Value)
{
    Data[0] = (UCHAR)(Value >> 24);
    Data[1] = (UCHAR)(Value >> 16);
    Data[2] = (UCHAR)(Value >> 8);
    Data[3] = (UCHAR)Value;
}

static
USHORT
RmNetChecksum(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    ULONG Sum = 0;
    ULONG i;

    for (i = 0; i + 1 < Length; i += 2)
        Sum += RmNetGet16(Data + i);
    if (Length & 1)
        Sum += Data[Length - 1] << 8;
    while (Sum >> 16)
        Sum = (Sum & 0xFFFF) + (Sum >> 16);

    return (USHORT)~Sum;
}

/* Hands one complete frame to tcpip, from a copy the return handler frees */
static
VOID
RmNetIndicateFrame(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length)
{
    PNET_BUFFER_LIST List;
    PUCHAR Copy;
    PMDL Mdl;

    if (Adapter->Paused || Adapter->Halting)
        return;

    Copy = ExAllocatePoolWithTag(NonPagedPool, Length, RMNET_TAG);
    if (!Copy)
    {
        Adapter->DroppedPackets++;
        return;
    }

    NdisMoveMemory(Copy, Frame, Length);
    Mdl = NdisAllocateMdl(Adapter->MiniportHandle, Copy, Length);
    if (!Mdl)
    {
        ExFreePoolWithTag(Copy, RMNET_TAG);
        Adapter->DroppedPackets++;
        return;
    }

    List = NdisAllocateNetBufferAndNetBufferList(Adapter->FramePool, 0, 0, Mdl, 0, Length);
    if (!List)
    {
        NdisFreeMdl(Mdl);
        ExFreePoolWithTag(Copy, RMNET_TAG);
        Adapter->DroppedPackets++;
        return;
    }

    List->SourceHandle = Adapter->MiniportHandle;
    Adapter->ReceivedPackets++;
    NdisMIndicateReceiveNetBufferLists(Adapter->MiniportHandle,
                                       List,
                                       NDIS_DEFAULT_PORT_NUMBER,
                                       1,
                                       (KeGetCurrentIrql() == DISPATCH_LEVEL) ?
                                           NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL : 0);
}

/* A raw IP packet from the modem, framed as if the peer sent it */
VOID
NTAPI
RmNetIndicatePacket(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    UCHAR Frame[RMNET_FRAME_MAX];
    USHORT Type;

    if (!Length || Length > RMNET_MAX_MTU)
    {
        Adapter->DroppedPackets++;
        return;
    }

    switch (Packet[0] >> 4)
    {
        case 4: Type = RMNET_ETHERTYPE_IPV4; break;
        case 6: Type = RMNET_ETHERTYPE_IPV6; break;
        default:
            Adapter->DroppedPackets++;
            return;
    }

    NdisMoveMemory(Frame, Adapter->Address, RMNET_ETH_ADDRESS);
    NdisMoveMemory(Frame + RMNET_ETH_ADDRESS, Adapter->PeerAddress, RMNET_ETH_ADDRESS);
    RmNetPut16(Frame + 12, Type);
    NdisMoveMemory(Frame + RMNET_ETH_HEADER, Packet, Length);

    RmNetIndicateFrame(Adapter, Frame, RMNET_ETH_HEADER + Length);
}

/* Every address but ours sits behind the modem, so the peer answers for all of them */
static
VOID
RmNetAnswerArp(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Arp,
    _In_ ULONG Length)
{
    UCHAR Reply[RMNET_ETH_HEADER + RMNET_ARP_LENGTH];
    PUCHAR Body = Reply + RMNET_ETH_HEADER;
    const UCHAR *SenderMac, *SenderIp, *TargetIp;

    if (Length < RMNET_ARP_LENGTH ||
        RmNetGet16(Arp) != 1 || RmNetGet16(Arp + 2) != RMNET_ETHERTYPE_IPV4 ||
        Arp[4] != RMNET_ETH_ADDRESS || Arp[5] != 4 || RmNetGet16(Arp + 6) != RMNET_ARP_REQUEST)
    {
        return;
    }

    SenderMac = Arp + 8;
    SenderIp = Arp + 14;
    TargetIp = Arp + 24;

    /* Duplicate address probes and our own address stay unanswered */
    if (RtlCompareMemory(TargetIp, &Adapter->Link.Address, 4) == 4 ||
        RtlCompareMemory(TargetIp, SenderIp, 4) == 4 ||
        *(const ULONG UNALIGNED *)SenderIp == 0)
    {
        return;
    }

    NdisMoveMemory(Reply, SenderMac, RMNET_ETH_ADDRESS);
    NdisMoveMemory(Reply + RMNET_ETH_ADDRESS, Adapter->PeerAddress, RMNET_ETH_ADDRESS);
    RmNetPut16(Reply + 12, RMNET_ETHERTYPE_ARP);

    RmNetPut16(Body, 1);
    RmNetPut16(Body + 2, RMNET_ETHERTYPE_IPV4);
    Body[4] = RMNET_ETH_ADDRESS;
    Body[5] = 4;
    RmNetPut16(Body + 6, RMNET_ARP_REPLY);
    NdisMoveMemory(Body + 8, Adapter->PeerAddress, RMNET_ETH_ADDRESS);
    NdisMoveMemory(Body + 14, TargetIp, 4);
    NdisMoveMemory(Body + 18, SenderMac, RMNET_ETH_ADDRESS);
    NdisMoveMemory(Body + 24, SenderIp, 4);

    RmNetIndicateFrame(Adapter, Reply, sizeof(Reply));
}

static
PUCHAR
RmNetPutOption(
    _Inout_ PUCHAR Cursor,
    _In_ UCHAR Option,
    _In_reads_bytes_(Length) const VOID *Value,
    _In_ UCHAR Length)
{
    Cursor[0] = Option;
    Cursor[1] = Length;
    NdisMoveMemory(Cursor + 2, Value, Length);
    return Cursor + 2 + Length;
}

static
UCHAR
RmNetDhcpType(
    _In_reads_bytes_(Length) const UCHAR *Options,
    _In_ ULONG Length)
{
    ULONG i = 0;

    while (i < Length && Options[i] != RMNET_DHCP_OPT_END)
    {
        if (Options[i] == RMNET_DHCP_OPT_PAD)
        {
            i++;
            continue;
        }
        if (i + 1 >= Length || i + 2 + Options[i + 1] > Length)
            break;
        if (Options[i] == RMNET_DHCP_OPT_TYPE && Options[i + 1] >= 1)
            return Options[i + 2];
        i += 2 + Options[i + 1];
    }

    return 0;
}

/* Answers the DHCP client of tcpip with what the data session got from the network */
static
VOID
RmNetAnswerDhcp(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Request,
    _In_ ULONG Length)
{
    UCHAR Reply[RMNET_ETH_HEADER + RMNET_IP_HEADER + RMNET_UDP_HEADER + RMNET_DHCP_FIXED + 4 + 64];
    PUCHAR Ip = Reply + RMNET_ETH_HEADER;
    PUCHAR Udp = Ip + RMNET_IP_HEADER;
    PUCHAR Dhcp = Udp + RMNET_UDP_HEADER;
    PUCHAR Cursor;
    RMNET_LINK Link = Adapter->Link;
    UCHAR Type, Answer, Dns[8], DnsLength = 0;
    ULONG Lease, Total, UdpLength;
    USHORT Mtu;

    if (Length < RMNET_DHCP_FIXED + 4 || Request[0] != 1 ||
        Request[236] != 0x63 || Request[237] != 0x82 || Request[238] != 0x53 || Request[239] != 0x63)
    {
        return;
    }

    Type = RmNetDhcpType(Request + RMNET_DHCP_FIXED + 4, Length - RMNET_DHCP_FIXED - 4);
    if (Type == RMNET_DHCP_DISCOVER)
        Answer = RMNET_DHCP_OFFER;
    else if (Type == RMNET_DHCP_REQUEST)
        Answer = Adapter->Connected ? RMNET_DHCP_ACK : RMNET_DHCP_NAK;
    else
        return;

    /* Without a session there is nothing to offer */
    if (!Adapter->Connected && Answer == RMNET_DHCP_OFFER)
        return;

    NdisZeroMemory(Reply, sizeof(Reply));
    Dhcp[0] = 2;
    Dhcp[1] = 1;
    Dhcp[2] = RMNET_ETH_ADDRESS;
    NdisMoveMemory(Dhcp + 4, Request + 4, 4);
    NdisMoveMemory(Dhcp + 10, Request + 10, 2);
    if (Answer != RMNET_DHCP_NAK)
    {
        NdisMoveMemory(Dhcp + 16, &Link.Address, 4);
        NdisMoveMemory(Dhcp + 20, &Link.Gateway, 4);
    }
    NdisMoveMemory(Dhcp + 28, Request + 28, 16);
    RmNetPut32(Dhcp + 236, RMNET_DHCP_COOKIE);

    Cursor = Dhcp + RMNET_DHCP_FIXED + 4;
    Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_TYPE, &Answer, 1);
    Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_SERVER, &Link.Gateway, 4);
    if (Answer != RMNET_DHCP_NAK)
    {
        RmNetPut32((PUCHAR)&Lease, RMNET_DHCP_LEASE_SECONDS);
        Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_LEASE, &Lease, 4);
        Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_SUBNET, &Link.SubnetMask, 4);
        Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_ROUTER, &Link.Gateway, 4);
        if (Link.PrimaryDns)
        {
            NdisMoveMemory(Dns, &Link.PrimaryDns, 4);
            DnsLength = 4;
        }
        if (Link.SecondaryDns)
        {
            NdisMoveMemory(Dns + DnsLength, &Link.SecondaryDns, 4);
            DnsLength += 4;
        }
        if (DnsLength)
            Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_DNS, Dns, DnsLength);
        RmNetPut16((PUCHAR)&Mtu, (USHORT)Link.Mtu);
        Cursor = RmNetPutOption(Cursor, RMNET_DHCP_OPT_MTU, &Mtu, 2);
    }
    *Cursor++ = RMNET_DHCP_OPT_END;

    UdpLength = (ULONG)(Cursor - Udp);
    Total = RMNET_IP_HEADER + UdpLength;

    /* The client has no address yet, the answer goes out broadcast */
    NdisFillMemory(Reply, RMNET_ETH_ADDRESS, 0xFF);
    NdisMoveMemory(Reply + RMNET_ETH_ADDRESS, Adapter->PeerAddress, RMNET_ETH_ADDRESS);
    RmNetPut16(Reply + 12, RMNET_ETHERTYPE_IPV4);

    Ip[0] = 0x45;
    RmNetPut16(Ip + 2, (USHORT)Total);
    Ip[8] = 64;
    Ip[9] = RMNET_IP_PROTOCOL_UDP;
    NdisMoveMemory(Ip + 12, &Link.Gateway, 4);
    NdisFillMemory(Ip + 16, 4, 0xFF);
    RmNetPut16(Ip + 10, RmNetChecksum(Ip, RMNET_IP_HEADER));

    RmNetPut16(Udp, RMNET_DHCP_SERVER_PORT);
    RmNetPut16(Udp + 2, RMNET_DHCP_CLIENT_PORT);
    RmNetPut16(Udp + 4, (USHORT)UdpLength);

    DPRINT1("rmnet: DHCP %s for the data session\n", (Answer == RMNET_DHCP_OFFER) ? "offer" :
                                                     (Answer == RMNET_DHCP_ACK) ? "ack" : "nak");
    RmNetIndicateFrame(Adapter, Reply, RMNET_ETH_HEADER + Total);
}

static
BOOLEAN
RmNetIsDhcpRequest(
    _In_reads_bytes_(Length) const UCHAR *Ip,
    _In_ ULONG Length,
    _Out_ const UCHAR **Payload,
    _Out_ PULONG PayloadLength)
{
    ULONG HeaderLength;

    if (Length < RMNET_IP_HEADER || (Ip[0] >> 4) != 4 || Ip[9] != RMNET_IP_PROTOCOL_UDP)
        return FALSE;

    HeaderLength = (Ip[0] & 0x0F) * 4;
    if (HeaderLength < RMNET_IP_HEADER || Length < HeaderLength + RMNET_UDP_HEADER)
        return FALSE;

    if (RmNetGet16(Ip + HeaderLength + 2) != RMNET_DHCP_SERVER_PORT)
        return FALSE;

    *Payload = Ip + HeaderLength + RMNET_UDP_HEADER;
    *PayloadLength = Length - HeaderLength - RMNET_UDP_HEADER;
    return TRUE;
}

/* One frame from tcpip: ARP and DHCP are answered here, IP goes to the modem */
VOID
NTAPI
RmNetSendFrame(
    _In_ PRMNET_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length)
{
    const UCHAR *Body = Frame + RMNET_ETH_HEADER;
    ULONG BodyLength = Length - RMNET_ETH_HEADER;
    const UCHAR *Payload;
    ULONG PayloadLength;

    switch (RmNetGet16(Frame + 12))
    {
        case RMNET_ETHERTYPE_ARP:
            RmNetAnswerArp(Adapter, Body, BodyLength);
            return;

        case RMNET_ETHERTYPE_IPV4:
            if (RmNetIsDhcpRequest(Body, BodyLength, &Payload, &PayloadLength))
            {
                RmNetAnswerDhcp(Adapter, Payload, PayloadLength);
                return;
            }

            if (!Adapter->Connected || !RmNetIpaSend(Adapter, Body, BodyLength))
                Adapter->DroppedPackets++;
            else
                Adapter->SentPackets++;
            return;

        /* The session is IPv4 only for now */
        default:
            Adapter->DroppedPackets++;
            return;
    }
}

VOID
NTAPI
RmNetIndicateLinkState(
    _In_ PRMNET_ADAPTER Adapter)
{
    NDIS_STATUS_INDICATION Indication;
    NDIS_LINK_STATE State;

    NdisZeroMemory(&State, sizeof(State));
    State.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    State.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    State.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    State.MediaConnectState = Adapter->Connected ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    State.MediaDuplexState = MediaDuplexStateFull;
    State.XmitLinkSpeed = Adapter->Connected ? RMNET_LINK_SPEED : NDIS_LINK_SPEED_UNKNOWN;
    State.RcvLinkSpeed = State.XmitLinkSpeed;
    State.PauseFunctions = NdisPauseFunctionsUnsupported;

    NdisZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    Indication.SourceHandle = Adapter->MiniportHandle;
    Indication.StatusCode = NDIS_STATUS_LINK_STATE;
    Indication.StatusBuffer = &State;
    Indication.StatusBufferSize = sizeof(State);

    NdisMIndicateStatusEx(Adapter->MiniportHandle, &Indication);
}
