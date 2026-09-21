/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     802.1Q packet extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_PACKET_TX_IEEE8021Q_ACTION_FLAGS
{
    NetPacketTxIeee8021qActionFlagPriorityRequired = 1,
    NetPacketTxIeee8021qActionFlagVlanRequired = 2
} NET_PACKET_TX_IEEE8021Q_ACTION_FLAGS;

typedef struct _NET_PACKET_IEEE8021Q
{
    UINT16 PriorityCodePoint : 3;
    UINT16 VlanIdentifier : 12;
    UINT8 TxTagging : 2;
} NET_PACKET_IEEE8021Q;

C_ASSERT(sizeof(NET_PACKET_IEEE8021Q) == 4);

#define NET_PACKET_EXTENSION_IEEE8021Q_NAME         L"ms_packet_ieee8021q"
#define NET_PACKET_EXTENSION_IEEE8021Q_VERSION_1    1U

#ifdef __cplusplus
}
#endif
