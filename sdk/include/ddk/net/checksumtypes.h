/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Checksum offload packet extension types
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_PACKET_TX_CHECKSUM_ACTION
{
    NetPacketTxChecksumActionPassthrough = 0,
    NetPacketTxChecksumActionRequired = 2
} NET_PACKET_TX_CHECKSUM_ACTION;

typedef enum _NET_PACKET_RX_CHECKSUM_EVALUATION
{
    NetPacketRxChecksumEvaluationNotChecked = 0,
    NetPacketRxChecksumEvaluationValid = 1,
    NetPacketRxChecksumEvaluationInvalid = 2
} NET_PACKET_RX_CHECKSUM_EVALUATION;

typedef struct _NET_PACKET_CHECKSUM
{
    UINT8 Layer2 : 2;
    UINT8 Layer3 : 2;
    UINT8 Layer4 : 2;
    UINT8 Reserved : 2;
} NET_PACKET_CHECKSUM;

C_ASSERT(sizeof(NET_PACKET_CHECKSUM) == 1);

#define NET_PACKET_EXTENSION_CHECKSUM_NAME          L"ms_packet_checksum"
#define NET_PACKET_EXTENSION_CHECKSUM_VERSION_1     1U

#ifdef __cplusplus
}
#endif
