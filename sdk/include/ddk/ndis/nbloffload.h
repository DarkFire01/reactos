/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER_LIST offload information slots
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Each of these occupies one NetBufferListInfo slot, so Value aliases the
 * whole thing and is what the slot actually stores.
 */
typedef struct _NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO
{
    union
    {
        struct
        {
            ULONG IsIPv4 : 1;
            ULONG IsIPv6 : 1;
            ULONG TcpChecksum : 1;
            ULONG UdpChecksum : 1;
            ULONG IpHeaderChecksum : 1;
            ULONG Reserved : 11;
            ULONG TcpHeaderOffset : 10;
        } Transmit;
        struct
        {
            ULONG TcpChecksumFailed : 1;
            ULONG UdpChecksumFailed : 1;
            ULONG IpChecksumFailed : 1;
            ULONG TcpChecksumSucceeded : 1;
            ULONG UdpChecksumSucceeded : 1;
            ULONG IpChecksumSucceeded : 1;
            ULONG Loopback : 1;
            ULONG TcpChecksumValueInvalid : 1;
            ULONG IpChecksumValueInvalid : 1;
        } Receive;
        PVOID Value;
    } DUMMYUNIONNAME;
} NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO, *PNDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO;

C_ASSERT(sizeof(NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO) == sizeof(PVOID));

/* Type selects between the V1 and V2 views of the same slot. */
typedef struct _NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO
{
    union
    {
        struct
        {
            ULONG Unused : 30;
            ULONG Type : 1;
            ULONG Reserved2 : 1;
        } Transmit;
        struct
        {
            ULONG MSS : 20;
            ULONG TcpHeaderOffset : 10;
            ULONG Type : 1;
            ULONG Reserved2 : 1;
        } LsoV1Transmit;
        struct
        {
            ULONG TcpPayload : 30;
            ULONG Type : 1;
            ULONG Reserved2 : 1;
        } LsoV1TransmitComplete;
        struct
        {
            ULONG MSS : 20;
            ULONG TcpHeaderOffset : 10;
            ULONG Type : 1;
            ULONG IPVersion : 1;
        } LsoV2Transmit;
        struct
        {
            ULONG Reserved : 30;
            ULONG Type : 1;
            ULONG Reserved2 : 1;
        } LsoV2TransmitComplete;
        PVOID Value;
    } DUMMYUNIONNAME;
} NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO,
  *PNDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO;

C_ASSERT(sizeof(NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO) == sizeof(PVOID));

typedef struct _NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO
{
    union
    {
        struct
        {
            ULONG MSS : 20;
            ULONG UdpHeaderOffset : 10;
            ULONG Reserved : 1;
            ULONG IPVersion : 1;
        } Transmit;
        PVOID Value;
    } DUMMYUNIONNAME;
} NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO,
  *PNDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO;

C_ASSERT(sizeof(NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO) == sizeof(PVOID));

#define NDIS_TCP_LARGE_SEND_OFFLOAD_V1_TYPE     0
#define NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE     1

#define NDIS_TCP_LARGE_SEND_OFFLOAD_IPv4        0
#define NDIS_TCP_LARGE_SEND_OFFLOAD_IPv6        1

#ifdef __cplusplus
}
#endif
