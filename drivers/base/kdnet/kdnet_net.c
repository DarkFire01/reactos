

#include "kdnet_private.h"
#include "kdnet_net.h"

DEBUG_NET_DATA       KdNetDataStorage = {0};
DEBUG_NET_PARAMETERS KdNetParameters = {0};
PDEBUG_NET_DATA      KdNetData = NULL;

/* ----------------------------------------------------------- tiny utils ---*/

static __forceinline USHORT KdNetSwap16(USHORT v)
{
    return (USHORT)((v >> 8) | (v << 8));
}

ULONG
KdpComputeChecksum(const UCHAR *Buffer, ULONG Length)
{
    ULONG sum = 0;
    while (Length--)
        sum += *Buffer++;
    return sum;
}

/* ------------------------------------------------------- NIC accessors -----*/

NTSTATUS
KdNetGetTxPacket(PDEBUG_NET_DATA Adapter, PULONG Handle)
{
    if (!Adapter)
        return STATUS_INVALID_PARAMETER;
    if (!KdNetExtensibilityExports || !KdNetExtensibilityExports->KdGetTxPacket)
        return STATUS_NOT_SUPPORTED;
    return KdNetExtensibilityExports->KdGetTxPacket(Adapter->KdNet.Hardware, Handle);
}

NTSTATUS
KdNetSendTxPacket(PDEBUG_NET_DATA Adapter, ULONG Handle, ULONG Length)
{
    if (!Adapter)
        return STATUS_INVALID_PARAMETER;
    if (!KdNetExtensibilityExports || !KdNetExtensibilityExports->KdSendTxPacket)
        return STATUS_NOT_SUPPORTED;
    return KdNetExtensibilityExports->KdSendTxPacket(Adapter->KdNet.Hardware, Handle, Length);
}

NTSTATUS
KdNetGetRxPacket(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet, PULONG Length)
{
    if (!Adapter)
        return STATUS_INVALID_PARAMETER;
    if (!KdNetExtensibilityExports || !KdNetExtensibilityExports->KdGetRxPacket)
        return STATUS_NOT_SUPPORTED;
    return KdNetExtensibilityExports->KdGetRxPacket(Adapter->KdNet.Hardware, Handle, Packet, Length);
}

VOID
KdNetReleaseRxPacket(PDEBUG_NET_DATA Adapter, ULONG Handle)
{
    if (Adapter && KdNetExtensibilityExports && KdNetExtensibilityExports->KdReleaseRxPacket)
        KdNetExtensibilityExports->KdReleaseRxPacket(Adapter->KdNet.Hardware, Handle);
}

PETHERNET_PACKET
KdNetGetPacketAddress(PDEBUG_NET_DATA Adapter, ULONG Handle)
{
    if (!Adapter || !KdNetExtensibilityExports || !KdNetExtensibilityExports->KdGetPacketAddress)
        return NULL;
    return (PETHERNET_PACKET)KdNetExtensibilityExports->KdGetPacketAddress(Adapter->KdNet.Hardware, Handle);
}

ULONG
KdNetGetPacketLength(PDEBUG_NET_DATA Adapter, ULONG Handle)
{
    if (!Adapter || !KdNetExtensibilityExports || !KdNetExtensibilityExports->KdGetPacketLength)
        return 0;
    return KdNetExtensibilityExports->KdGetPacketLength(Adapter->KdNet.Hardware, Handle);
}

PUCHAR
KdNetGetPacketKdData(PDEBUG_NET_DATA Adapter, ULONG Handle)
{
    PETHERNET_PACKET p = KdNetGetPacketAddress(Adapter, Handle);
    if (!p)
        return NULL;
    if (Adapter->Parameters && Adapter->Parameters->EncryptedLink)
        return &p->Data[42];
    return &p->Data[28];
}

/* ------------------------------------------------------------ SwapPacket ---*/

VOID
KdNetSwapPacket(PETHERNET_PACKET Packet, BOOLEAN ToNetwork)
{
    PUCHAR Data = Packet->Data;
    USHORT etherHost;

    etherHost = Packet->Header.EtherType;
    Packet->Header.EtherType = KdNetSwap16(Packet->Header.EtherType);
    if (!ToNetwork)
        etherHost = Packet->Header.EtherType;   /* from-network: use swapped (host) */

    if (etherHost == ETHERTYPE_IPV4)
    {
        /* IP total length, fragment word, source/dest IP. */
        *(PUSHORT)&Data[2] = KdNetSwap16(*(PUSHORT)&Data[2]);
        *(PUSHORT)&Data[6] = KdNetSwap16(*(PUSHORT)&Data[6]);
        *(PULONG)&Data[12] = _byteswap_ulong(*(PULONG)&Data[12]);
        *(PULONG)&Data[16] = _byteswap_ulong(*(PULONG)&Data[16]);

        if (ToNetwork && *(PUSHORT)&Data[10] == 0)
        {
            /* IP header checksum over the 20-byte header (one's complement). */
            ULONG sum = 0;
            ULONG i;
            for (i = 0; i < 20; i += 2)
                sum += *(PUSHORT)&Data[i];
            sum = (sum & 0xFFFF) + (sum >> 16);
            sum = (sum & 0xFFFF) + (sum >> 16);
            *(PUSHORT)&Data[10] = (USHORT)~sum;
        }

        if (Data[9] == IP_PROTOCOL_UDP)
        {
            *(PUSHORT)&Data[20] = KdNetSwap16(*(PUSHORT)&Data[20]);  /* src port  */
            *(PUSHORT)&Data[22] = KdNetSwap16(*(PUSHORT)&Data[22]);  /* dst port  */
            *(PUSHORT)&Data[24] = KdNetSwap16(*(PUSHORT)&Data[24]);  /* udp length*/
        }
    }
    else if (etherHost == ETHERTYPE_ARP || etherHost == 32821 /* RARP */)
    {
        BOOLEAN ethHw = (Data[4] == 6);
        *(PUSHORT)&Data[0] = KdNetSwap16(*(PUSHORT)&Data[0]);  /* hardware type  */
        *(PUSHORT)&Data[2] = KdNetSwap16(*(PUSHORT)&Data[2]);  /* protocol type  */
        *(PUSHORT)&Data[6] = KdNetSwap16(*(PUSHORT)&Data[6]);  /* opcode         */
        if (ethHw && Data[5] == 4)
        {
            *(PULONG)&Data[14] = _byteswap_ulong(*(PULONG)&Data[14]);  /* sender IP */
            *(PULONG)&Data[24] = _byteswap_ulong(*(PULONG)&Data[24]);  /* target IP */
        }
    }
}

/* ----------------------------------------------------------- TX framing ----*/

NTSTATUS
KdNetSendEthernetPacket(PDEBUG_NET_DATA Adapter, ULONG Handle, ULONG Length,
                        PETHERNET_ADDRESS Source, PETHERNET_ADDRESS Destination, USHORT EtherType)
{
    PETHERNET_PACKET p;

    UNREFERENCED_PARAMETER(EtherType);
    if (!Adapter || !Destination || !Source)
        return STATUS_INVALID_PARAMETER;

    p = KdNetGetPacketAddress(Adapter, Handle);
    if (!p)
        return STATUS_INVALID_PARAMETER;

    p->Header.Destination = *Destination;
    p->Header.Source = *Source;
    p->Header.EtherType = ETHERTYPE_IPV4;
    KdNetSwapPacket(p, TRUE);
    return KdNetSendTxPacket(Adapter, Handle, Length + 14);
}

NTSTATUS
KdNetSendIPPacket(PDEBUG_NET_DATA Adapter, ULONG Handle,
                  PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                  ULONG Length, ULONG SourceIP, ULONG DestinationIP,
                  UCHAR Protocol, UCHAR TypeOfService, UCHAR TimeToLive)
{
    PETHERNET_PACKET p;

    UNREFERENCED_PARAMETER(Protocol);
    UNREFERENCED_PARAMETER(TypeOfService);
    if (Length > 0xFFE3)
        return STATUS_INVALID_PARAMETER;

    p = KdNetGetPacketAddress(Adapter, Handle);
    if (!p)
        return STATUS_INVALID_PARAMETER;

    p->Data[0] = 0x45;                              /* Version 4, IHL 5 */
    p->Data[1] = 0;                                 /* ServiceType */
    *(PUSHORT)&p->Data[2] = (USHORT)(Length + 20);  /* total length */
    *(PULONG)&p->Data[4] = 0x40000000;              /* ID=0, DF flag */
    p->Data[8] = TimeToLive;
    p->Data[9] = IP_PROTOCOL_UDP;
    *(PUSHORT)&p->Data[10] = 0;                      /* checksum (computed in swap) */
    *(PULONG)&p->Data[12] = SourceIP;
    *(PULONG)&p->Data[16] = DestinationIP;

    return KdNetSendEthernetPacket(Adapter, Handle, Length + 20, SourceAddress, DestinationAddress, ETHERTYPE_IPV4);
}

NTSTATUS
KdNetSendUDPPacketEx(PDEBUG_NET_DATA Adapter, ULONG Handle,
                     PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                     ULONG SourceIP, ULONG DestinationIP, UCHAR TypeOfService, UCHAR TimeToLive,
                     ULONG Length, USHORT SourcePort, USHORT DestinationPort)
{
    PETHERNET_PACKET p;

    if (Length > 0xFFE3)
        return STATUS_INVALID_PARAMETER;

    p = KdNetGetPacketAddress(Adapter, Handle);
    if (!p)
        return STATUS_INVALID_PARAMETER;

    /* UDP header at Data[20..27]. */
    *(PUSHORT)&p->Data[20] = SourcePort;
    *(PUSHORT)&p->Data[22] = DestinationPort;
    *(PUSHORT)&p->Data[24] = (USHORT)(Length + 8);   /* UDP length */
    *(PUSHORT)&p->Data[26] = 0;                       /* UDP checksum (unused) */

    return KdNetSendIPPacket(Adapter, Handle, SourceAddress, DestinationAddress,
                             Length + 8, SourceIP, DestinationIP,
                             IP_PROTOCOL_UDP, TypeOfService, TimeToLive);
}

/* -------------------------------------------------------- KD packet send ---*/

NTSTATUS
KdNetSendKdPacket(PDEBUG_NET_DATA Adapter, ULONG Handle, ULONG Length,
                  USHORT SourcePort, USHORT DestinationPort)
{
    PDEBUG_NET_PARAMETERS prm = Adapter->Parameters;

    if (prm->EncryptedLink)
    {
        PKD_NET_HEADER_V2 kdPacket;
        PETHERNET_PACKET p;
        ULONGLONG seq;

        if (!prm->Connected)
        {
            /* No data channel yet — the offer handshake must complete first. */
            return STATUS_DEVICE_NOT_READY;
        }

        p = KdNetGetPacketAddress(Adapter, Handle);
        if (!p)
            return STATUS_INVALID_PARAMETER;

        kdPacket = (PKD_NET_HEADER_V2)&p->Data[28];

        /* Monotonic target sequence number. */
        seq = (ULONGLONG)InterlockedIncrement64(&prm->TargetSequenceNumber);

        KdNetEncryptKdPacket(kdPacket, &Length,
                             &Adapter->Crypto.DebugSessionKey,
                             &Adapter->Crypto.HMacKey,
                             seq, 0);
    }

    return KdNetSendUDPPacketEx(Adapter, Handle,
                               &Adapter->TargetMac, &Adapter->Parameters->HostMac,
                               Adapter->TargetIP, Adapter->Parameters->HostIP,
                               0, 0x10, Length, SourcePort, DestinationPort);
}

/* ------------------------------------------------------------- ARP / RX ----*/

NTSTATUS
KdNetHandleArp(PDEBUG_NET_DATA Adapter, PETHERNET_PACKET Packet)
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    PUCHAR d = Packet->Data;

    if (!Adapter->TargetIP)
        return status;

    KdNetSwapPacket(Packet, FALSE);

    if (*(PUSHORT)&d[0] == 1 &&            /* hardware type ethernet */
        *(PUSHORT)&d[2] == 0x0800 &&        /* protocol IPv4 */
        d[4] == 6 && d[5] == 4 &&           /* addr/proto lengths */
        *(PUSHORT)&d[6] == 1 &&             /* opcode = request */
        *(PULONG)&d[24] == Adapter->TargetIP)
    {
        ULONG senderMacLo = *(PULONG)&d[8];
        USHORT senderMacHi = *(PUSHORT)&d[12];
        ULONG senderIp = *(PULONG)&d[14];
        ULONG handle;
        PETHERNET_PACKET reply;

        status = KdNetGetTxPacket(Adapter, &handle);
        if (!NT_SUCCESS(status))
            return status;

        reply = KdNetGetPacketAddress(Adapter, handle);
        if (!reply)
            return STATUS_INVALID_PARAMETER;

        /* Destination = requester (or broadcast for link-local). */
        *(PULONG)&reply->Header.Destination.Address[0] = senderMacLo;
        *(PUSHORT)&reply->Header.Destination.Address[4] = senderMacHi;
        if ((Adapter->TargetIP & 0xFFFF0000) == KDNET_LINKLOCAL_NET)
        {
            *(PULONG)&reply->Header.Destination.Address[0] = 0xFFFFFFFF;
            *(PUSHORT)&reply->Header.Destination.Address[4] = 0xFFFF;
        }
        reply->Header.Source = Adapter->TargetMac;
        reply->Header.EtherType = ETHERTYPE_ARP;

        *(PUSHORT)&reply->Data[0] = 1;          /* hw type ethernet */
        /* Host order (KdNetSwapPacket byte-swaps the 16-bit fields on TX):
         * proto type host 0x0800 -> wire 0x0800; hwlen 6, protolen 4 are bytes.
         * NB: do NOT use 0x04060008 here -- that puts host 0x0008 at Data[2],
         * which swaps to wire 0x0008 and is silently dropped by real ARP stacks
         * (Linux/Windows require ar_pro == 0x0800); only QEMU's SLIRP tolerates it. */
        *(PULONG)&reply->Data[2] = 0x04060800;  /* proto IPv4, hwlen 6, protolen 4 */
        *(PUSHORT)&reply->Data[6] = 2;          /* opcode = reply */
        *(PULONG)&reply->Data[8] = *(PULONG)Adapter->TargetMac.Address;
        *(PUSHORT)&reply->Data[12] = *(PUSHORT)&Adapter->TargetMac.Address[4];
        *(PULONG)&reply->Data[14] = Adapter->TargetIP;
        *(PULONG)&reply->Data[18] = senderMacLo;
        *(PUSHORT)&reply->Data[22] = senderMacHi;
        *(PULONG)&reply->Data[24] = senderIp;

        KdNetSwapPacket(reply, TRUE);
        status = KdNetSendTxPacket(Adapter, handle, 0x2A);
    }
    else
    {
        KdNetSwapPacket(Packet, TRUE);   /* not for us: restore network order */
    }

    return status;
}

/* Best-effort handler for packets that don't match the awaited UDP flow.
 * DHCP and control-channel handling will be layered in here; for now anything
 * unhandled is simply dropped by the caller (which releases the RX packet). */
static NTSTATUS
KdNetProcessUnhandledPackets(PDEBUG_NET_DATA Adapter, ULONG PacketHandle)
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(PacketHandle);
    return STATUS_NOT_SUPPORTED;
}

static ULONGLONG
KdNetReadCycleCounter(VOID)
{
    return KdNetReadTimeStampCounter();
}

static NTSTATUS
KdNetWaitForRxPacket(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet, PULONG Length, PULONG Timeout)
{
    ULONGLONG start = KdNetReadCycleCounter();
    ULONGLONG tscPerUs = KdNetGetTicksPerMicrosecond();   /* calibrated; *Timeout is in us */
    ULONGLONG cyclesTimeout;
    NTSTATUS status;

    if (tscPerUs == 0)
        tscPerUs = 1;

    for (;;)
    {
        status = KdNetGetRxPacket(Adapter, Handle, Packet, Length);
        if (NT_SUCCESS(status))
            break;
        if (*Timeout == 0)
            break;
        if (*Timeout == 0xFFFFFFFF)
        {
            YieldProcessor();
            continue;
        }
        cyclesTimeout = (ULONGLONG)*Timeout * tscPerUs;
        if (KdNetReadCycleCounter() - start < cyclesTimeout)
            YieldProcessor();
        else
            *Timeout = 0;
    }

    if (*Timeout != 0 && *Timeout != 0xFFFFFFFF)
    {
        ULONGLONG elapsedUs = (KdNetReadCycleCounter() - start) / tscPerUs;
        if (elapsedUs < *Timeout)
            *Timeout -= (ULONG)elapsedUs;
        else
            *Timeout = 0;
    }

    return status;
}

static NTSTATUS
KdNetWaitForSpecificRxPacket(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet, PULONG Length, PULONG Timeout,
                             PETHERNET_ADDRESS Source, PETHERNET_ADDRESS Destination, PUSHORT Ethertype)
{
    NTSTATUS status;

    for (status = KdNetWaitForRxPacket(Adapter, Handle, Packet, Length, Timeout);
         NT_SUCCESS(status);
         status = KdNetWaitForRxPacket(Adapter, Handle, Packet, Length, Timeout))
    {
        PUCHAR f = (PUCHAR)*Packet;
        USHORT etherHost = KdNetSwap16(*(PUSHORT)&f[12]);

        BOOLEAN dstOk = (!Destination ||
                         (*(PULONG)Destination->Address == *(PULONG)&f[0] &&
                          *(PUSHORT)&Destination->Address[4] == *(PUSHORT)&f[4]));
        BOOLEAN srcOk = (!Source ||
                         (*(PULONG)Source->Address == *(PULONG)&f[6] &&
                          *(PUSHORT)&Source->Address[4] == *(PUSHORT)&f[10]));
        BOOLEAN etOk = (!Ethertype || *Ethertype == etherHost);

        if (dstOk && srcOk && etOk)
        {
            *Packet = f + 14;
            *Length -= 14;
            return status;
        }

        if (etherHost == ETHERTYPE_ARP)
        {
            if (KdNetHandleArp(Adapter, (PETHERNET_PACKET)f) != STATUS_NOT_SUPPORTED)
                goto release;
        }
        KdNetProcessUnhandledPackets(Adapter, *Handle);
release:
        KdNetReleaseRxPacket(Adapter, *Handle);
    }
    return status;
}

static NTSTATUS
KdNetWaitForSpecificRxIpPacket(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet, PULONG Length, PULONG Timeout,
                               PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                               ULONG SourceIP, ULONG DestinationIP)
{
    USHORT etherType;
    NTSTATUS status;

    SourceIP = _byteswap_ulong(SourceIP);
    DestinationIP = _byteswap_ulong(DestinationIP);

    for (;;)
    {
        PUCHAR ip;
        ULONG ipLen;
        USHORT totalLen, headerLen, payload;

        etherType = ETHERTYPE_IPV4;
        status = KdNetWaitForSpecificRxPacket(Adapter, Handle, Packet, Length, Timeout,
                                              SourceAddress, DestinationAddress, &etherType);
        if (!NT_SUCCESS(status))
            return status;

        if (*Length < 0x14)
        {
            KdNetReleaseRxPacket(Adapter, *Handle);
            continue;
        }

        ip = (PUCHAR)*Packet;
        ipLen = *Length - 20;
        if ((SourceIP && SourceIP != *(PULONG)&ip[12]) ||
            (DestinationIP && DestinationIP != *(PULONG)&ip[16]) ||
            ip[9] != IP_PROTOCOL_UDP)
        {
            KdNetProcessUnhandledPackets(Adapter, *Handle);
            KdNetReleaseRxPacket(Adapter, *Handle);
            continue;
        }

        /* Skip IP header (20 bytes); clamp the reported UDP-bearing length. */
        *Packet = ip + 20;
        totalLen = KdNetSwap16(*(PUSHORT)&ip[2]);
        headerLen = (USHORT)(4 * (ip[0] & 0xF));
        if (totalLen < headerLen)
            totalLen = headerLen;
        payload = (USHORT)(totalLen - headerLen);
        if (payload > ipLen)
            payload = (USHORT)ipLen;
        *Length = payload;
        return status;
    }
}

NTSTATUS
KdNetWaitForSpecificRxUdpPacketEx(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet, PULONG Length, PULONG Timeout,
                                  PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                                  ULONG SourceIP, ULONG DestinationIP,
                                  PUSHORT SourcePort, PUSHORT DestinationPort)
{
    NTSTATUS status;

    for (;;)
    {
        PUCHAR udp;
        ULONG udpAvail;
        USHORT wantSrc, wantDst, udpLen, payload;

        status = KdNetWaitForSpecificRxIpPacket(Adapter, Handle, Packet, Length, Timeout,
                                                SourceAddress, DestinationAddress, SourceIP, DestinationIP);
        if (!NT_SUCCESS(status))
            return status;

        if (*Length < 8)
        {
            KdNetReleaseRxPacket(Adapter, *Handle);
            continue;
        }

        udp = (PUCHAR)*Packet;
        udpAvail = *Length - 8;
        wantDst = KdNetSwap16(*DestinationPort);
        wantSrc = KdNetSwap16(*SourcePort);

        if ((wantSrc && wantSrc != *(PUSHORT)&udp[0]) ||
            (wantDst && wantDst != *(PUSHORT)&udp[2]))
        {
            KdNetProcessUnhandledPackets(Adapter, *Handle);
            KdNetReleaseRxPacket(Adapter, *Handle);
            continue;
        }

        *Packet = udp + 8;
        udpLen = KdNetSwap16(*(PUSHORT)&udp[4]);
        if (udpLen < 8)
            udpLen = 8;
        payload = (USHORT)(udpLen - 8);
        if (payload > udpAvail)
            payload = (USHORT)udpAvail;
        *Length = payload;

        /* Report the received ports back to the caller (host order). */
        *SourcePort = KdNetSwap16(*(PUSHORT)&udp[0]);
        *DestinationPort = KdNetSwap16(*(PUSHORT)&udp[2]);
        return status;
    }
}

/* ----------------------------------------------------- ARP resolution -------*/

NTSTATUS
KdNetGetNodeMacAddress(PDEBUG_NET_DATA Adapter, ULONG SourceIP, ULONG NodeIP,
                       PETHERNET_ADDRESS NodeAddress, ULONG Retries)
{
    ULONG handle;
    NTSTATUS status;
    ULONG retries = Retries;
    ULONG rxSeen = 0;     /* ARP packets received (any sender) while waiting */
    ULONG sends = 0;      /* ARP requests we transmitted */

    status = KdNetGetTxPacket(Adapter, &handle);
    if (!NT_SUCCESS(status))
        return status;

    for (;;)
    {
        PETHERNET_PACKET p = KdNetGetPacketAddress(Adapter, handle);
        PUCHAR d;
        ULONG i;
        ULONG timeout;

        if (!p)
            return STATUS_INVALID_PARAMETER;

        d = p->Data;
        for (i = 0; i < 0x2A; i++)
            d[i] = 0;

        /* Broadcast ethernet header. */
        *(PULONG)&p->Header.Destination.Address[0] = 0xFFFFFFFF;
        *(PUSHORT)&p->Header.Destination.Address[4] = 0xFFFF;
        p->Header.Source = Adapter->TargetMac;
        p->Header.EtherType = ETHERTYPE_ARP;

        /* ARP request. */
        *(PUSHORT)&d[0] = 1;             /* hardware type ethernet */
        /* Host order (KdNetSwapPacket byte-swaps the 16-bit fields on TX):
         * proto type host 0x0800 -> wire 0x0800; hwlen 6, protolen 4 are bytes.
         * NB: do NOT use 0x04060008 here -- that puts host 0x0008 at d[2], which
         * swaps to wire 0x0008 and is silently dropped by real ARP stacks
         * (Linux/Windows require ar_pro == 0x0800); only QEMU's SLIRP tolerates it. */
        *(PULONG)&d[2] = 0x04060800;     /* proto IPv4, hwlen 6, protolen 4 */
        *(PUSHORT)&d[6] = 1;             /* opcode = request */
        *(PULONG)&d[8] = *(PULONG)Adapter->TargetMac.Address;
        *(PUSHORT)&d[12] = *(PUSHORT)&Adapter->TargetMac.Address[4];
        *(PULONG)&d[14] = SourceIP;
        *(PULONG)&d[24] = NodeIP;

        KdNetSwapPacket(p, TRUE);
        status = KdNetSendTxPacket(Adapter, handle, 0x2A);
        if (!NT_SUCCESS(status))
            break;
        sends++;

        /* QEMU's e1000 defers inbound packets for ~1s after RX is enabled
         * (flush_queue_timer); poll well past that so the queued reply is seen. */
        timeout = 2500000;
        for (;;)
        {
            PVOID rxPayload;
            ULONG rxHandle;
            ULONG rxLen = 0;
            USHORT et = ETHERTYPE_ARP;
            PETHERNET_PACKET eth;

            status = KdNetWaitForSpecificRxPacket(Adapter, &rxHandle, &rxPayload, &rxLen,
                                                  &timeout, NULL, NULL, &et);
            if (status == STATUS_IO_TIMEOUT)
            {
                if (retries)
                    break;   /* resend */
                goto Done;   /* out of retries: report via the summary below */
            }
            if (!NT_SUCCESS(status))
                goto Done;

            /* rxPayload points past the 14-byte ethernet header. */
            eth = (PETHERNET_PACKET)((PUCHAR)rxPayload - 14);
            KdNetSwapPacket(eth, FALSE);
            rxSeen++;

            if (*(PUSHORT)&eth->Data[0] == 1 &&
                *(PUSHORT)&eth->Data[2] == 0x0800 &&
                eth->Data[4] == 6 && eth->Data[5] == 4 &&
                *(PULONG)&eth->Data[14] == NodeIP)
            {
                *(PULONG)NodeAddress->Address = *(PULONG)&eth->Data[8];
                *(PUSHORT)&NodeAddress->Address[4] = *(PUSHORT)&eth->Data[12];
                KdNetReleaseRxPacket(Adapter, rxHandle);
                if (FrLdrDbgPrint)
                    FrLdrDbgPrint("kdnet: ARP who-has 0x%08lx resolved after %lu send(s), rxSeen=%lu\n",
                                  NodeIP, sends, rxSeen);
                return STATUS_SUCCESS;
            }
            KdNetReleaseRxPacket(Adapter, rxHandle);
        }

        --retries;
        status = KdNetGetTxPacket(Adapter, &handle);
        if (!NT_SUCCESS(status))
            break;
    }

Done:
    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: ARP who-has 0x%08lx FAILED status=0x%08lx sends=%lu rxSeen=%lu\n",
                      NodeIP, status, sends, rxSeen);
    return status;
}

/* ------------------------------------------------------------------ DHCP ---*/
/*
 * Minimal DHCP client (DISCOVER/OFFER/REQUEST/ACK) matching the wire layout of
 * the reference kdnet.dll. The target acquires an IPv4 address before the offer
 * handshake. Ports: client 68, server 67. All multi-byte option values that the
 * wire carries big-endian are written/read with _byteswap.
 */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

/* "Keep waiting" (malformed/irrelevant reply) and "NAK received" sentinels —
 * the numeric values match the reference so the retry logic behaves the same. */
#define KDNET_DHCP_INVALID ((NTSTATUS)0xC00000C3L)
#define KDNET_DHCP_NAK     ((NTSTATUS)0xC00000BDL)

/* Locate a DHCP option (TLV) in the options area. */
static NTSTATUS
KdNetGetDhcpOption(PUCHAR Options, ULONG Length, UCHAR Option,
                   PUCHAR *OptionAddress, PUCHAR OptionLength)
{
    ULONG i = 0;

    while (i < Length)
    {
        UCHAR code = Options[i];
        if (code == 0)            /* pad */
        {
            i++;
            continue;
        }
        if (code == 0xFF)         /* end */
            break;
        if (i + 1 >= Length)
            break;
        if (code == Option)
        {
            UCHAR len = Options[i + 1];
            if (Length - (i + 1) >= (ULONG)len + 1)
            {
                *OptionLength = len;
                *OptionAddress = &Options[i + 2];
                return STATUS_SUCCESS;
            }
            return STATUS_UNSUCCESSFUL;
        }
        i += (ULONG)Options[i + 1] + 2;
    }
    return STATUS_UNSUCCESSFUL;
}

/* Build and broadcast (or unicast, for renew) a DHCP message. DhcpState selects
 * the request variant: 3 = SELECTING (after an offer), 6 = RENEWING, 7 = REBINDING. */
static NTSTATUS
KdNetSendDhcpPacket(PDEBUG_NET_DATA Adapter, ULONG DhcpState, UCHAR MessageType)
{
    PDHCP_STATE st = &Adapter->Parameters->DhcpState;
    ETHERNET_ADDRESS destMac;
    PETHERNET_PACKET pkt;
    PUCHAR p;
    ULONG handle, i, o;
    ULONG destIP = 0xFFFFFFFF;
    ULONG srcIP = 0;
    NTSTATUS status;

    *(PULONG)destMac.Address = 0xFFFFFFFF;       /* broadcast by default */
    *(PUSHORT)&destMac.Address[4] = 0xFFFF;

    status = KdNetGetTxPacket(Adapter, &handle);
    if (!NT_SUCCESS(status))
        return status;

    pkt = KdNetGetPacketAddress(Adapter, handle);
    if (!pkt)
        return STATUS_INVALID_PARAMETER;

    /* DHCP/BOOTP message sits at the UDP payload (Data[28] for a cleartext IP+UDP). */
    p = &pkt->Data[28];
    for (i = 0; i < 0xEC; i++)        /* 236-byte fixed BOOTP header */
        p[i] = 0;

    p[0] = 1;   /* op    = BOOTREQUEST */
    p[1] = 1;   /* htype = ethernet    */
    p[2] = 6;   /* hlen  = 6           */
    p[3] = 0;   /* hops               */
    *(PULONG)&p[4]  = st->DhcpTransactionID;
    *(PUSHORT)&p[8] = (USHORT)st->DhcpSeconds;
    /* flags(@10)=0 => unicast reply requested; ci/yi/si/gi already zero */
    *(PULONG)&p[28]  = *(PULONG)Adapter->TargetMac.Address;       /* chaddr */
    *(PUSHORT)&p[32] = *(PUSHORT)&Adapter->TargetMac.Address[4];
    if (MessageType == DHCP_REQUEST && (DhcpState == 6 || DhcpState == 7))
        *(PULONG)&p[12] = _byteswap_ulong(Adapter->TargetIP);    /* ciaddr (renew) */

    /* Options. */
    *(PULONG)&p[236] = 0x63538263;    /* magic cookie 99.130.83.99 */
    p[240] = 53; p[241] = 1; p[242] = MessageType;               /* opt 53 msg type */
    p[243] = 61; p[244] = 7; p[245] = 1;                         /* opt 61 client id */
    for (i = 0; i < 6; i++)
        p[246 + i] = Adapter->TargetMac.Address[i];
    o = 16;   /* options consumed: 4 cookie + 3 (opt53) + 9 (opt61) */

    if (MessageType == DHCP_DISCOVER || MessageType == DHCP_REQUEST)
    {
        /* opt 51: requested IP-address lease time = infinite. */
        p[236 + o] = 51; p[236 + o + 1] = 4;
        *(PULONG)&p[236 + o + 2] = 0xFFFFFFFF;
        o += 6;

        if (MessageType == DHCP_REQUEST && DhcpState == 3)
        {
            /* opt 54: server identifier (network order, as received). */
            p[236 + o] = 54; p[236 + o + 1] = 4;
            *(PULONG)&p[236 + o + 2] = st->DhcpServer;
            o += 6;
        }

        /* opt 57: maximum DHCP message size = 1280. */
        p[236 + o] = 57; p[236 + o + 1] = 2;
        p[236 + o + 2] = 0x05; p[236 + o + 3] = 0x00;
        o += 4;

        /* opt 55: parameter request list = subnet mask (1), router (3). */
        p[236 + o] = 55; p[236 + o + 1] = 2;
        p[236 + o + 2] = 1; p[236 + o + 3] = 3;
        o += 4;

        if (MessageType == DHCP_REQUEST && DhcpState == 3)
        {
            /* opt 50: requested IP address (network order, from the offer). */
            p[236 + o] = 50; p[236 + o + 1] = 4;
            *(PULONG)&p[236 + o + 2] = st->DhcpIPAddress;
            o += 6;
        }
    }

    p[236 + o] = 0xFF;    /* end option */
    o += 1;

    /* Unicast a renew (state 6) straight to the leasing server; everything else
     * goes to the broadcast address/MAC set above. */
    if (MessageType == DHCP_REQUEST)
    {
        if (DhcpState == 6)
        {
            destIP = _byteswap_ulong(st->DhcpServer);
            destMac = st->DhcpServerMac;
            srcIP = Adapter->TargetIP;
        }
        else if (DhcpState == 7)
        {
            srcIP = Adapter->TargetIP;
        }
    }

    return KdNetSendUDPPacketEx(Adapter, handle, &Adapter->TargetMac, &destMac,
                               srcIP, destIP, 0, 0x30, 236 + o, 68, 67);
}

/* Validate a received DHCP reply and, for an ACK, latch the lease into DhcpState. */
static NTSTATUS
KdNetProcessDhcpPacket(PDEBUG_NET_DATA Adapter, PDHCP Dhcp, ULONG Length, UCHAR MessageType)
{
    PDHCP_STATE st = &Adapter->Parameters->DhcpState;
    PUCHAR opts, opt;
    UCHAR optLen = 0;
    ULONG optsLen;

    if (Length > 0x500 || Length < 0xF0)
        return KDNET_DHCP_INVALID;
    optsLen = Length - 240;

    if (Dhcp->Opcode != 2)
        return KDNET_DHCP_INVALID;
    if (Dhcp->TransactionID != st->DhcpTransactionID)
        return KDNET_DHCP_INVALID;
    if (Dhcp->HardwareAddressType != 1 || Dhcp->HardwareAddressLength != 6)
        return KDNET_DHCP_INVALID;
    if (*(PULONG)Dhcp->ClientHardwareAddress != *(PULONG)Adapter->TargetMac.Address ||
        *(PUSHORT)&Dhcp->ClientHardwareAddress[4] != *(PUSHORT)&Adapter->TargetMac.Address[4])
        return KDNET_DHCP_INVALID;
    if (Dhcp->Options[0] != 99 || Dhcp->Options[1] != 0x82 ||
        Dhcp->Options[2] != 83 || Dhcp->Options[3] != 99)
        return KDNET_DHCP_INVALID;   /* magic cookie */

    opts = &Dhcp->Options[4];

    /* Server identifier (54) is mandatory. The OFFER's server wins the lease. */
    if (!NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 54, &opt, &optLen)) || optLen != 4)
        return KDNET_DHCP_INVALID;
    if (MessageType == DHCP_OFFER)
        st->DhcpServer = *(PULONG)opt;
    if (st->DhcpServer != *(PULONG)opt)
        return KDNET_DHCP_INVALID;

    /* Message type (53) must match what we're waiting for. */
    if (NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 53, &opt, &optLen)) &&
        optLen == 1 && *opt == MessageType)
    {
        if (MessageType == DHCP_OFFER)
            st->DhcpIPAddress = Dhcp->YourIpAddress;

        if (MessageType == DHCP_ACK)
        {
            ULONG router;

            if (!NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 1, &opt, &optLen)) || optLen != 4)
                return KDNET_DHCP_INVALID;        /* subnet mask */
            st->DhcpSubnetMask = _byteswap_ulong(*(PULONG)opt);

            if (!NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 51, &opt, &optLen)) || optLen != 4)
                return KDNET_DHCP_INVALID;        /* lease time */
            st->DhcpLeaseTime = _byteswap_ulong(*(PULONG)opt);

            if (!NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 58, &opt, &optLen)))
                st->DhcpRenewTime = st->DhcpLeaseTime >> 1;
            else if (optLen != 4)
                return KDNET_DHCP_INVALID;
            else
                st->DhcpRenewTime = _byteswap_ulong(*(PULONG)opt);

            if (!NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 59, &opt, &optLen)))
                st->DhcpRebindTime = (st->DhcpLeaseTime >> 1) +
                                     (st->DhcpLeaseTime >> 2) +
                                     (st->DhcpLeaseTime >> 3);
            else if (optLen != 4)
                return KDNET_DHCP_INVALID;
            else
                st->DhcpRebindTime = _byteswap_ulong(*(PULONG)opt);

            if (!NT_SUCCESS(KdNetGetDhcpOption(opts, optsLen, 3, &opt, &optLen)))
                router = Dhcp->RelayAgentIpAddress;
            else if (optLen & 3)
                return KDNET_DHCP_INVALID;        /* router list */
            else
                router = *(PULONG)opt;
            st->DhcpRouterIP = _byteswap_ulong(router);

            st->DhcpIPAddress = Dhcp->YourIpAddress;
            Adapter->TargetIP = _byteswap_ulong(Dhcp->YourIpAddress);
            st->DhcpTimer = 0;
            st->DhcpState = 5;
        }
        return STATUS_SUCCESS;
    }

    /* A NAK (msg type 6) means our requested address was rejected: restart. */
    if (MessageType == DHCP_ACK && optLen == 1 && *opt == DHCP_NAK)
    {
        st->DhcpTransactionID = 0;
        st->DhcpSeconds = 0;
        st->DhcpServer = 0;
        st->DhcpIPAddress = 0;
        return KDNET_DHCP_NAK;
    }

    return KDNET_DHCP_INVALID;
}

/* Wait for a DHCP reply of the given type, skipping unrelated/malformed ones. */
static NTSTATUS
KdNetWaitForDhcpPacket(PDEBUG_NET_DATA Adapter, PULONG Timeout, UCHAR MessageType)
{
    NTSTATUS status;

    do
    {
        ULONG handle, length;
        PVOID payload;
        USHORT srcPort = 67, dstPort = 68;

        status = KdNetWaitForSpecificRxUdpPacketEx(Adapter, &handle, &payload, &length, Timeout,
                                                   NULL, NULL, 0, 0, &srcPort, &dstPort);
        if (!NT_SUCCESS(status))
            break;

        status = KdNetProcessDhcpPacket(Adapter, (PDHCP)payload, length, MessageType);
        KdNetReleaseRxPacket(Adapter, handle);
    }
    while (status == KDNET_DHCP_INVALID);

    return status;
}

/* Acquire an IPv4 address via DHCP: DISCOVER -> OFFER -> REQUEST -> ACK, with
 * exponential backoff. On success Adapter->TargetIP holds the leased address and
 * the DHCP-server MAC is resolved for off-subnet routing. */
static NTSTATUS
KdNetGetTargetIPAddress(PDEBUG_NET_DATA Adapter)
{
    PDHCP_STATE st = &Adapter->Parameters->DhcpState;
    ULONG attempt = 0;
    ULONG backoff;
    NTSTATUS status;

Retry:
    ++attempt;
    st->DhcpTransactionID = (ULONG)(KdNetReadTimeStampCounter() >> 4);
    st->DhcpSeconds = 0;
    st->DhcpServer = 0;

    /* DISCOVER until an OFFER arrives. */
    for (backoff = 1; ; backoff *= 2)
    {
        ULONG timeout;

        status = KdNetSendDhcpPacket(Adapter, 0, DHCP_DISCOVER);
        if (!NT_SUCCESS(status))
            return status;

        timeout = backoff * 2500000;   /* > QEMU's ~1s RX defer on first poll */
        status = KdNetWaitForDhcpPacket(Adapter, &timeout, DHCP_OFFER);
        if (NT_SUCCESS(status))
            break;
        if (status != STATUS_IO_TIMEOUT || backoff >= 0x40)
            return status;
    }

    /* REQUEST (SELECTING) until an ACK arrives; a NAK restarts the whole thing. */
    for (backoff = 1; ; backoff *= 2)
    {
        ULONG timeout;

        status = KdNetSendDhcpPacket(Adapter, 3, DHCP_REQUEST);
        if (!NT_SUCCESS(status))
            return status;

        timeout = backoff * 2500000;
        status = KdNetWaitForDhcpPacket(Adapter, &timeout, DHCP_ACK);
        if (NT_SUCCESS(status))
            break;
        if (status == KDNET_DHCP_NAK && attempt < 3)
            goto Retry;
        if (status != STATUS_IO_TIMEOUT || backoff >= 8)
            return status;
    }

    /* Lease is bound. Resolve the next hop toward the server (router if it's on a
     * different subnet) so the server MAC is cached for renew. */
    {
        ULONG serverIP = _byteswap_ulong(st->DhcpServer);
        if (st->DhcpRouterIP && ((serverIP ^ Adapter->TargetIP) & st->DhcpSubnetMask) != 0)
            serverIP = st->DhcpRouterIP;
        status = KdNetGetNodeMacAddress(Adapter, Adapter->TargetIP, serverIP, &st->DhcpServerMac, 2);
    }

    return status;
}

NTSTATUS
KdNetInitializeNetwork(VOID)
{
    PDEBUG_NET_DATA a = KdNetData;
    NTSTATUS status;

    if (!a || !a->Parameters)
        return STATUS_INVALID_PARAMETER;

    /* Acquire an address via DHCP if enabled and none was statically configured. */
    if (a->Parameters->Dhcp && !a->TargetIP)
    {
        NTSTATUS ds = KdNetGetTargetIPAddress(a);
        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: DHCP TargetIP=0x%08lx status=0x%08lx\n",
                          a->TargetIP, ds);
        if (NT_SUCCESS(ds))
            a->Parameters->TargetIP = a->TargetIP;
    }

    /* Fall back to a static target IP if DHCP is off or didn't yield one (QEMU
     * user-net hands out 10.0.2.15 by default). */
    if (!a->TargetIP)
    {
        a->TargetIP = 0x0A00020F;   /* 10.0.2.15, host order */
        a->Parameters->TargetIP = a->TargetIP;
    }

    /* Resolve the next-hop MAC by ARP (this is the first TX). If the host debugger
     * is on a different /24 than the target, it is not directly reachable: ARP the
     * gateway instead (SLIRP's gateway is .2 of the target subnet) and route the
     * offers through it (dest MAC = gateway, dest IP = host). */
    if (a->Parameters->HostIP)
    {
        ULONG attempt;
        ULONG nextHop = a->Parameters->HostIP;

        if (((a->Parameters->HostIP ^ a->TargetIP) & 0xFFFFFF00) != 0)
        {
            /* Off-subnet: prefer the DHCP-supplied router (with its real subnet
             * mask) if we have one; otherwise assume SLIRP's .2 gateway. */
            PDHCP_STATE st = &a->Parameters->DhcpState;
            if (st->DhcpRouterIP &&
                ((a->Parameters->HostIP ^ a->TargetIP) & st->DhcpSubnetMask) != 0)
                nextHop = st->DhcpRouterIP;
            else
                nextHop = (a->TargetIP & 0xFFFFFF00) | 2;
        }

        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: ARP next-hop 0x%08lx (host 0x%08lx)\n",
                          nextHop, a->Parameters->HostIP);

        status = KdNetGetNodeMacAddress(a, a->TargetIP, nextHop,
                                        &a->Parameters->HostMac, 3);
        if (!NT_SUCCESS(status))
            return status;

        /* AssignedHost* mirror the host endpoint for the offer packet. */
        a->Parameters->AssignedHostIP = a->Parameters->HostIP;
        a->Parameters->AssignedHostPort = a->Parameters->HostPort;

        /* Connect handshake: send offers and wait for the host's connect
         * response, until the data channel is up. */
        for (attempt = 0; attempt < 24 && !a->Parameters->Connected; attempt++)
        {
            ULONG handle, rxHandle, rxLen, timeout;
            PVOID rxPkt;
            USHORT srcPort, dstPort;
            NTSTATUS rxStatus;

            if (NT_SUCCESS(KdNetGetTxPacket(a, &handle)))
                KdNetSendOfferPacket(a, handle, &a->Parameters->HostMac,
                                     a->Parameters->HostIP, a->Parameters->HostPort);

            /* Wait for a UDP reply from the host on our target port. */
            timeout = 1200000;   /* > QEMU 1s RX defer */
            srcPort = a->Parameters->HostPort;
            dstPort = a->Parameters->TargetPort;
            rxStatus = KdNetWaitForSpecificRxUdpPacketEx(a, &rxHandle, &rxPkt, &rxLen, &timeout,
                                                         &a->Parameters->HostMac, &a->TargetMac,
                                                         a->Parameters->HostIP, a->TargetIP,
                                                         &srcPort, &dstPort);
            if (NT_SUCCESS(rxStatus))
            {
                UCHAR *p = (UCHAR *)rxPkt;
                ULONG len = rxLen;
                BOOLEAN isControl = FALSE;
                if (NT_SUCCESS(KdNetDecryptKdPacket(&a->Crypto, &p, &len, &isControl)) && isControl)
                    KdNetProcessControlChannelPacket(a, p, len, 0);
                KdNetReleaseRxPacket(a, rxHandle);
            }
        }
    }

    return STATUS_SUCCESS;
}

/* --------------------------------------------------- connect handshake ------*/

ULONG     KdNetReconnectRunningTimeout = 0;
ULONGLONG KdNetReconnectTimestamp = 0;

NTSTATUS
KdNetSendPingPacket(PDEBUG_NET_DATA Adapter)
{
    ULONG handle;
    PUCHAR kd;
    NTSTATUS status;

    status = KdNetGetTxPacket(Adapter, &handle);
    if (!NT_SUCCESS(status))
        return status;

    kd = KdNetGetPacketKdData(Adapter, handle);
    if (!kd)
        return STATUS_INVALID_PARAMETER;

    *(PULONGLONG)kd = Adapter->Parameters->LastValidHostSequenceNumber;
    *(PULONG)(kd + 8) = Adapter->Parameters->HostIP;
    return KdNetSendKdPacket(Adapter, handle, 0xC,
                             Adapter->Parameters->TargetPort, Adapter->Parameters->HostPort);
}

NTSTATUS
KdNetSendOfferPacket(PDEBUG_NET_DATA Adapter, ULONG Handle,
                     PETHERNET_ADDRESS DestinationAddress, ULONG DestinationIP, USHORT DestinationPort)
{
    PDEBUG_NET_PARAMETERS prm = Adapter->Parameters;
    PETHERNET_PACKET pkt = KdNetGetPacketAddress(Adapter, Handle);
    PKD_NET_HEADER_V2 kdPacket;
    PUCHAR d;
    ULONG i;
    ULONG length;
    UCHAR flags;

    if (!pkt)
        return STATUS_INVALID_PARAMETER;

    kdPacket = (PKD_NET_HEADER_V2)&pkt->Data[28];
    d = KdNetGetPacketKdData(Adapter, Handle);
    if (!d)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < 0x158; i++)
        d[i] = 0;

    d[0] = 1;
    d[1] = 1;

    /* Bump the low 64 bits of TargetRandom (freshness); bytes 8..31 are stable. */
    (*(PULONGLONG)&prm->TargetRandom[0])++;
    RtlCopyMemory(d + 2, prm->TargetRandom, 0x20);

    *(PULONG)(d + 42) = _byteswap_ulong(0xFFFF);
    *(PULONG)(d + 46) = _byteswap_ulong(prm->TargetIP);
    *(PUSHORT)(d + 50) = _byteswap_ushort(prm->TargetPort);
    *(PULONG)(d + 60) = _byteswap_ulong(0xFFFF);
    *(PULONG)(d + 64) = _byteswap_ulong(prm->AssignedHostIP);
    *(PUSHORT)(d + 68) = _byteswap_ushort(prm->AssignedHostPort);
    *(PULONG)(d + 78) = _byteswap_ulong(0xFFFF);
    *(PULONG)(d + 82) = _byteswap_ulong(prm->Connected ? prm->HostIP : 0);
    if (prm->Connected)
    {
        *(PUSHORT)(d + 86) = _byteswap_ushort(prm->HostPort);
        RtlCopyMemory(d + 88, prm->HostConnectionInfo, 0x100);
    }

    length = 344;
    flags = 1;
    if (prm->OffersSendStatus)
        flags = 3;

    KdNetEncryptKdPacket(kdPacket, &length,
                         &Adapter->Crypto.TargetDebugKey, &Adapter->Crypto.HMacKey,
                         prm->OfferTimer, flags);

    return KdNetSendUDPPacketEx(Adapter, Handle, &Adapter->TargetMac, DestinationAddress,
                               Adapter->TargetIP, DestinationIP, 0, 0x10, length,
                               prm->TargetPort, DestinationPort);
}

static NTSTATUS
KdNetInitializeDataChannel(PDEBUG_NET_DATA Adapter, PUCHAR Packet, ULONG Length)
{
    KdNetCryptoSetSessionKey(&Adapter->Crypto, Adapter->Parameters->Key, Packet, Length,
                             Adapter->Parameters->SessionKey);
    Adapter->Parameters->TargetSequenceNumber = 0;
    Adapter->Parameters->LastValidHostSequenceNumber = 0;
    Adapter->Parameters->Connected = 1;
    return STATUS_SUCCESS;
}

NTSTATUS
KdNetProcessControlChannelPacket(PDEBUG_NET_DATA Adapter, PUCHAR Packet, ULONG Length, ULONGLONG SequenceNumber)
{
    PDEBUG_NET_PARAMETERS prm = Adapter->Parameters;

    if (Length == 0)
    {
        /* Host "poke": reply with an offer. */
        ULONG handle;
        if (NT_SUCCESS(KdNetGetTxPacket(Adapter, &handle)))
            return KdNetSendOfferPacket(Adapter, handle, &prm->HostMac, prm->HostIP, prm->HostPort);
        return STATUS_UNSUCCESSFUL;
    }

    /* Connect response: 322 bytes, Packet[0]==1, Packet[1]==2, echoes TargetRandom. */
    if (Length == 322 && Packet[0] == 1 && Packet[1] == 2)
    {
        ULONGLONG diff = prm->OfferTimer - SequenceNumber;
        ULONG j;

        if (diff == 0 || diff == 3)
        {
            for (j = 8; j < 0x20; j++)
            {
                if (Packet[j + 2] != prm->TargetRandom[j])
                    return STATUS_UNSUCCESSFUL;
            }
            /* Random matched -> establish the data channel. */
            KdNetInitializeDataChannel(Adapter, Packet, 0x142);
            KdNetSendPingPacket(Adapter);
            return STATUS_SUCCESS;
        }
    }

    return STATUS_UNSUCCESSFUL;
}

BOOLEAN
KdNetEnableHostReconnect(PDEBUG_NET_DATA Adapter, ULONG Timeout)
{
    ULONG handle;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    UNREFERENCED_PARAMETER(Timeout);

    Adapter->Parameters->OfferTimer += 3;

    if (Adapter->Parameters->Connected && Adapter->Parameters->LastValidHostSequenceNumber == 0)
        KdNetSendPingPacket(Adapter);

    if (NT_SUCCESS(KdNetGetTxPacket(Adapter, &handle)))
        status = KdNetSendOfferPacket(Adapter, handle, &Adapter->Parameters->HostMac,
                                      Adapter->Parameters->HostIP, Adapter->Parameters->HostPort);
    return NT_SUCCESS(status);
}
