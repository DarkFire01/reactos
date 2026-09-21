/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Receive segment coalescing packet extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_PACKET_RSC
{
    union
    {
        struct
        {
            UINT16 CoalescedSegmentCount;
            UINT16 DuplicateAckCount;
        } TCP;
        struct
        {
            UINT16 CoalescedSegmentCount;
            UINT16 CoalescedSegmentSize;
        } UDP;
    } DUMMYUNIONNAME;
} NET_PACKET_RSC;

C_ASSERT(sizeof(NET_PACKET_RSC) == 4);

typedef struct _NET_PACKET_RSC_TIMESTAMP
{
    union
    {
        struct
        {
            UINT32 RscTcpTimestampDelta;
        } TCP;
    } DUMMYUNIONNAME;
} NET_PACKET_RSC_TIMESTAMP;

C_ASSERT(sizeof(NET_PACKET_RSC_TIMESTAMP) == 4);

#define NET_PACKET_EXTENSION_RSC_NAME                       L"ms_packet_rsc"
#define NET_PACKET_EXTENSION_RSC_VERSION_1                  1U
#define NET_PACKET_EXTENSION_RSC_VERSION_2                  2U
#define NET_PACKET_EXTENSION_RSC_TIMESTAMP_NAME             L"ms_packet_rsc_timestamp"
#define NET_PACKET_EXTENSION_RSC_TIMESTAMP_VERSION_1        1U
#define NET_PACKET_EXTENSION_RSC_TIMESTAMP_VERSION_1_SIZE   sizeof(NET_PACKET_RSC_TIMESTAMP)

#ifdef __cplusplus
}
#endif
