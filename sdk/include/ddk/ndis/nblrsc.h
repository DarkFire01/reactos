/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Receive segment coalescing information on a NET_BUFFER_LIST
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef union _NDIS_RSC_NBL_INFO
{
    struct
    {
        USHORT CoalescedSegCount;
        USHORT DupAckCount;
    } Info;
    PVOID Value;
} NDIS_RSC_NBL_INFO, *PNDIS_RSC_NBL_INFO;

C_ASSERT(sizeof(NDIS_RSC_NBL_INFO) == sizeof(PVOID));

#define NET_BUFFER_LIST_COALESCED_SEG_COUNT(_NBL)     (((PNDIS_RSC_NBL_INFO)&NET_BUFFER_LIST_INFO((_NBL), TcpRecvSegCoalesceInfo))->Info.CoalescedSegCount)

#define NET_BUFFER_LIST_DUP_ACK_COUNT(_NBL)     (((PNDIS_RSC_NBL_INFO)&NET_BUFFER_LIST_INFO((_NBL), TcpRecvSegCoalesceInfo))->Info.DupAckCount)

/* UDP coalescing shares its slot with the TCP receive byte count. */
typedef struct _NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO
{
    union
    {
        struct
        {
            USHORT SegCount;
            USHORT SegSize;
        } Receive;
        PVOID Value;
    } DUMMYUNIONNAME;
} NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO, *PNDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO;

C_ASSERT(sizeof(NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO) == sizeof(PVOID));

#define NET_BUFFER_LIST_UDP_COALESCED_SEG_COUNT(_NBL)     (((PNDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO)         &NET_BUFFER_LIST_INFO((_NBL), UdpRecvSegCoalesceOffloadInfo))->Receive.SegCount)

#ifdef __cplusplus
}
#endif
