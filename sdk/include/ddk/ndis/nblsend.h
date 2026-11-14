/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Flags on the NET_BUFFER_LIST send and send complete paths
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* NdisSendNetBufferLists and MiniportSendNetBufferLists */
#define NDIS_SEND_FLAGS_DISPATCH_LEVEL              0x00000001
#define NDIS_SEND_FLAGS_CHECK_FOR_LOOPBACK          0x00000002
#define NDIS_SEND_FLAGS_SINGLE_QUEUE                0x00000004
#define NDIS_SEND_FLAGS_SWITCH_DESTINATION_GROUP    0x00000010
#define NDIS_SEND_FLAGS_SWITCH_SINGLE_SOURCE        0x00000020

/* NdisMSendNetBufferListsComplete */
#define NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL         0x00000001
#define NDIS_SEND_COMPLETE_FLAGS_SINGLE_QUEUE           0x00000002
#define NDIS_SEND_COMPLETE_FLAGS_SWITCH_SINGLE_SOURCE   0x00000004

#ifdef __cplusplus
}
#endif
