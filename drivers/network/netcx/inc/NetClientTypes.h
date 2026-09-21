/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Types shared by both halves of the class extension
 *
 * The adapter half and the translator half are built into the same binary, so
 * this contract never crosses a module boundary. Layouts follow the drop where
 * it differs from NetAdapterCx.pdb, see Reference/NetCx.
 */

#pragma once

#include <net/extension.h>

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NET_CLIENT_ADAPTER);
DECLARE_HANDLE(NET_CLIENT_QUEUE);

typedef enum _NET_CLIENT_TRI_STATE
{
    NET_CLIENT_TRI_STATE_FALSE = 0,
    NET_CLIENT_TRI_STATE_TRUE = 1,
    NET_CLIENT_TRI_STATE_DEFAULT = 2
} NET_CLIENT_TRI_STATE;

typedef union _NET_CLIENT_EUI48_ADDRESS
{
    UINT8 Value[6];
} NET_CLIENT_EUI48_ADDRESS;

typedef struct DECLSPEC_ALIGN(8) _NET_CLIENT_EXTENSION
{
    ULONG Size;
    PCWSTR Name;
    ULONG Version;
    ULONG Alignment;
    SIZE_T ExtensionSize;
    NET_EXTENSION_TYPE Type;
} NET_CLIENT_EXTENSION;

typedef enum _NET_CLIENT_QUEUE_TX_DEMUX_TYPE
{
    TxDemuxType8021p = 1,
    TxDemuxTypeWmmInfo = 2,
    TxDemuxTypePeerAddress = 3
} NET_CLIENT_QUEUE_TX_DEMUX_TYPE;

typedef struct DECLSPEC_ALIGN(4) _NET_CLIENT_QUEUE_TX_DEMUX_PROPERTY
{
    NET_CLIENT_QUEUE_TX_DEMUX_TYPE Type;
    SIZE_T Value;
    union
    {
        UINT8 UserPriority;
        UINT8 WmmInfo;
        NET_CLIENT_EUI48_ADDRESS PeerAddress;
    } Property;
} NET_CLIENT_QUEUE_TX_DEMUX_PROPERTY;

/*
 * The adapter half hands its NET_ADAPTER_TX_DEMUX array over as this type, so
 * the values have to track NET_ADAPTER_TX_DEMUX_TYPE rather than the pdb.
 */
typedef enum _NET_CLIENT_ADAPTER_TX_DEMUX_TYPE
{
    NetClientAdapterTxDemuxType8021p = 1
} NET_CLIENT_ADAPTER_TX_DEMUX_TYPE;

#ifdef __cplusplus
}
#endif
