/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     kdnet packet transfers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"
#include "kdnet_net.h"
static ULONG KdNetTxPacketId = 0;
static ULONG KdNetRxPacketId = 0x80000000;
static ULONG KdNetRetryCount = 3;

#define KDNET_READ_PACKET   0
#define KDNET_READ_TIMEOUT  1
#define KDNET_READ_ERROR    2

ULONG
NTAPI
KdpCalculateChecksum(PVOID Buffer, ULONG Length)
{
    return KdpComputeChecksum((const UCHAR *)Buffer, Length);
}

/* Build + send a control packet (ACK / RESEND / RESET) */
static VOID
KdNetSendControlPacket(USHORT PacketType, ULONG PacketId)
{
    PDEBUG_NET_DATA a = KdNetData;
    ULONG handle;
    PUCHAR kd;

    if (!a)
        return;
    if (!NT_SUCCESS(KdNetGetTxPacket(a, &handle)))
        return;

    kd = KdNetGetPacketKdData(a, handle);
    if (!kd)
        return;

    ((PKD_PACKET)kd)->PacketLeader = CONTROL_PACKET_LEADER;
    ((PKD_PACKET)kd)->PacketType = PacketType;
    ((PKD_PACKET)kd)->ByteCount = 0;
    ((PKD_PACKET)kd)->PacketId = PacketId;
    ((PKD_PACKET)kd)->Checksum = 0;

    KdNetSendKdPacket(a, handle, sizeof(KD_PACKET), a->Parameters->TargetPort, a->Parameters->HostPort);
}

static int
KdNetReadKdPacket(PKD_PACKET PacketHeader, PSTRING MessageHeader, PSTRING MessageData, PULONG Timeout)
{
    PDEBUG_NET_DATA a = KdNetData;

    if (!a)
        return KDNET_READ_ERROR;

    for (;;)
    {
        ULONG handle, len = 0, copy;
        PVOID rxPkt;
        USHORT srcPort = a->Parameters->HostPort;
        USHORT dstPort = a->Parameters->TargetPort;
        UCHAR *p;
        BOOLEAN isControl = FALSE;
        NTSTATUS status;

        status = KdNetWaitForSpecificRxUdpPacketEx(a, &handle, &rxPkt, &len, Timeout,
                                                   &a->Parameters->HostMac, &a->TargetMac,
                                                   a->Parameters->HostIP, a->TargetIP,
                                                   &srcPort, &dstPort);
        if (status == STATUS_IO_TIMEOUT)
            return KDNET_READ_TIMEOUT;
        if (!NT_SUCCESS(status))
            return KDNET_READ_ERROR;

        p = (UCHAR *)rxPkt;
        if (!NT_SUCCESS(KdNetDecryptKdPacket(&a->Crypto, &p, &len, &isControl)))
        {
            KdNetReleaseRxPacket(a, handle);
            continue;
        }

        if (isControl)
        {
            /* Control-channel traffic (offer poke / connect response): handle and
             * keep waiting for an actual KD data packet. */
            KdNetProcessControlChannelPacket(a, p, len, 0);
            KdNetReleaseRxPacket(a, handle);
            continue;
        }

        /* KD data packet: first 16 bytes are the KD_PACKET header. */
        copy = len < sizeof(KD_PACKET) ? len : sizeof(KD_PACKET);
        RtlCopyMemory(PacketHeader, p, copy);

        if (copy >= sizeof(KD_PACKET))
        {
            ULONG remaining;
            p += sizeof(KD_PACKET);
            len -= sizeof(KD_PACKET);
            remaining = len;

            /* Split the message body into MessageHeader then MessageData. */
            if (MessageHeader)
            {
                ULONG hlen = MessageHeader->MaximumLength;
                if (hlen > remaining)
                    hlen = remaining;
                RtlCopyMemory(MessageHeader->Buffer, p, hlen);
                MessageHeader->Length = (USHORT)hlen;
                p += hlen;
                remaining -= hlen;
            }
            if (MessageData)
            {
                ULONG dlen = MessageData->MaximumLength;
                if (dlen > remaining)
                    dlen = remaining;
                RtlCopyMemory(MessageData->Buffer, p, dlen);
                MessageData->Length = (USHORT)dlen;
            }
            KdNetReleaseRxPacket(a, handle);
            return KDNET_READ_PACKET;
        }

        KdNetReleaseRxPacket(a, handle);
        return KDNET_READ_ERROR;   /* runt */
    }
}

KDSTATUS
NTAPI
KdReceivePacket(
    IN ULONG PacketType,
    OUT PSTRING MessageHeader,
    OUT PSTRING MessageData,
    OUT PULONG DataLength,
    IN OUT PKD_CONTEXT Context)
{
    KD_PACKET PacketHeader;
    ULONG Timeout;
    int rc;

    (void)Context;

    if (!KdNetData || !KdNetParameters.DebuggerActive)
        return KdPacketNeedsResend;

    /* PACKET_TYPE_KD_POLL_BREAKIN (8) is a non-blocking break-in check. */
    Timeout = (PacketType != PACKET_TYPE_KD_POLL_BREAKIN) ? 0x7A120 : 0;

    for (;;)
    {
        RtlZeroMemory(&PacketHeader, sizeof(PacketHeader));
        rc = KdNetReadKdPacket(&PacketHeader, MessageHeader, MessageData, &Timeout);

        if (PacketType == PACKET_TYPE_KD_POLL_BREAKIN)
        {
            /* Non-blocking break-in poll. A host-initiated break (windbg's
             * Break/Pause) arrives as a short packet whose first decrypted byte
             * is the break-in byte (0x62) — NetReadKdPacket copies it into
             * PacketHeader and returns the short/error code, so check the byte
             * regardless of rc. On a real timeout PacketHeader is zeroed (so it
             * isn't 0x62) and we keep the target running. */
            if ((UCHAR)PacketHeader.PacketLeader == BREAKIN_PACKET_BYTE)
                return KdPacketReceived;
            return KdPacketTimedOut;
        }

        if (rc == KDNET_READ_TIMEOUT)
            return KdPacketTimedOut;
        if (rc == KDNET_READ_ERROR)
            return KdPacketNeedsResend;

        /* Control packets (ACK/RESET/RESEND). */
        if (PacketHeader.PacketLeader == CONTROL_PACKET_LEADER)
        {
            switch (PacketHeader.PacketType)
            {
                case PACKET_TYPE_KD_ACKNOWLEDGE:
                    if (PacketHeader.PacketId == KdNetTxPacketId &&
                        PacketType == PACKET_TYPE_KD_ACKNOWLEDGE)
                        return KdPacketReceived;
                    break;
                case PACKET_TYPE_KD_RESET:
                    KdNetRxPacketId = PacketHeader.PacketId;
                    KdNetSendControlPacket(PACKET_TYPE_KD_RESET, KdNetTxPacketId);
                    return KdPacketNeedsResend;
                case PACKET_TYPE_KD_RESEND:
                    return KdPacketNeedsResend;
                default:
                    break;
            }
            continue;
        }

        if (PacketHeader.PacketLeader != PACKET_LEADER)
            goto Resend;

        if (PacketType == PACKET_TYPE_KD_ACKNOWLEDGE)
        {
            if (PacketHeader.PacketId >= KdNetRxPacketId)
            {
                KdNetSendControlPacket(PACKET_TYPE_KD_RESEND, 0);
                return KdPacketReceived;
            }
            return KdPacketNeedsResend;
        }

        if (PacketType != PacketHeader.PacketType)
            goto Resend;

        /* Validate the checksum over the message we already copied out. */
        {
            ULONG hlen = MessageHeader ? MessageHeader->Length : 0;
            ULONG dlen = PacketHeader.ByteCount - hlen;
            ULONG sum = 0;

            if (PacketHeader.ByteCount > PACKET_MAX_SIZE)
                goto Resend;

            if (MessageHeader)
                sum += KdpComputeChecksum((const UCHAR *)MessageHeader->Buffer, hlen);
            if (MessageData)
                sum += KdpComputeChecksum((const UCHAR *)MessageData->Buffer, dlen);

            if (sum != PacketHeader.Checksum)
                goto Resend;

            if (MessageData)
                MessageData->Length = (USHORT)dlen;
            if (DataLength)
                *DataLength = dlen;

            KdNetSendControlPacket(PACKET_TYPE_KD_ACKNOWLEDGE, PacketHeader.PacketId);
            KdNetRxPacketId += 2;
            if (PacketHeader.PacketId + 2 > KdNetRxPacketId)
                KdNetRxPacketId = PacketHeader.PacketId + 2;
            return KdPacketReceived;
        }

    Resend:
        KdNetSendControlPacket(PACKET_TYPE_KD_RESEND, 0);
    }
}

VOID
NTAPI
KdSendPacket(
    IN ULONG PacketType,
    IN PSTRING MessageHeader,
    IN PSTRING MessageData,
    IN OUT PKD_CONTEXT Context)
{
    PDEBUG_NET_DATA a = KdNetData;
    ULONG retries;

    if (!a || !KdNetParameters.DebuggerActive)
        return;

    for (retries = KdNetRetryCount; ; )
    {
        ULONG handle;
        PUCHAR kd;
        PKD_PACKET hdr;
        ULONG dataLen = MessageData ? MessageData->Length : 0;
        ULONG sum;
        ULONG total;
        KDSTATUS ack;

        if (!NT_SUCCESS(KdNetGetTxPacket(a, &handle)))
            return;

        kd = KdNetGetPacketKdData(a, handle);
        if (!kd)
            return;

        sum = KdpComputeChecksum((const UCHAR *)MessageHeader->Buffer, MessageHeader->Length);
        if (MessageData)
            sum += KdpComputeChecksum((const UCHAR *)MessageData->Buffer, dataLen);

        hdr = (PKD_PACKET)kd;
        hdr->PacketLeader = PACKET_LEADER;
        hdr->PacketType = (USHORT)PacketType;
        hdr->ByteCount = (USHORT)(MessageHeader->Length + dataLen);
        hdr->PacketId = KdNetTxPacketId;
        hdr->Checksum = sum;

        RtlCopyMemory(kd + sizeof(KD_PACKET), MessageHeader->Buffer, MessageHeader->Length);
        total = sizeof(KD_PACKET) + MessageHeader->Length;
        if (MessageData)
        {
            RtlCopyMemory(kd + total, MessageData->Buffer, dataLen);
            total += dataLen;
        }

        KdNetSendKdPacket(a, handle, total, a->Parameters->TargetPort, a->Parameters->HostPort);

        /* Await the ACK. */
        ack = KdReceivePacket(PACKET_TYPE_KD_ACKNOWLEDGE, NULL, NULL, NULL, Context);
        if (ack == KdPacketReceived)
            break;
        if (ack == KdPacketTimedOut)
        {
            if (retries == 0)
                return;
            --retries;
        }
    }

    KdNetTxPacketId += 2;
}
