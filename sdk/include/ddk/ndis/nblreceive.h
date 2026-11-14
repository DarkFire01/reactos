/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Flags on the NET_BUFFER_LIST receive and return paths
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* NdisMIndicateReceiveNetBufferLists */
#define NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL           0x00000001
#define NDIS_RECEIVE_FLAGS_RESOURCES                0x00000002
#define NDIS_RECEIVE_FLAGS_SINGLE_ETHER_TYPE        0x00000100
#define NDIS_RECEIVE_FLAGS_SINGLE_VLAN              0x00000200
#define NDIS_RECEIVE_FLAGS_PERFECT_FILTERED         0x00000400
#define NDIS_RECEIVE_FLAGS_SINGLE_QUEUE             0x00000800
#define NDIS_RECEIVE_FLAGS_SHARED_MEMORY_INFO_VALID 0x00001000
#define NDIS_RECEIVE_FLAGS_MORE_NBLS                0x00002000
#define NDIS_RECEIVE_FLAGS_SWITCH_DESTINATION_GROUP 0x00004000
#define NDIS_RECEIVE_FLAGS_SWITCH_SINGLE_SOURCE     0x00008000

/* MiniportReturnNetBufferLists */
#define NDIS_RETURN_FLAGS_DISPATCH_LEVEL            0x00000001
#define NDIS_RETURN_FLAGS_SINGLE_QUEUE              0x00000002
#define NDIS_RETURN_FLAGS_SWITCH_SINGLE_SOURCE      0x00000004

#ifdef __cplusplus
}
#endif
