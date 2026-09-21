/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Network adapter object, preview surface
 *
 * Transmit demultiplexing lets a driver get one transmit queue per 802.1p
 * priority or per Wi-Fi peer instead of a single queue per processor.
 */

#pragma once

#include <netcx/netadapter.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_ADAPTER_TX_DEMUX_TYPE
{
    NetAdapterTxDemuxType8021p = 1
} NET_ADAPTER_TX_DEMUX_TYPE;

/* Range is how many distinct values of the demux key the driver wants queues for. */
typedef struct _NET_ADAPTER_TX_DEMUX
{
    ULONG Size;
    NET_ADAPTER_TX_DEMUX_TYPE Type;
    UINT8 Range;
} NET_ADAPTER_TX_DEMUX;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_TX_DEMUX_8021P_INIT(
    _Out_ NET_ADAPTER_TX_DEMUX *Demux,
    _In_ UINT8 Range)
{
    RtlZeroMemory(Demux, sizeof(*Demux));
    Demux->Size = sizeof(*Demux);
    Demux->Type = NetAdapterTxDemuxType8021p;
    Demux->Range = Range;
}

/* Pattern IDs reported in NET_ADAPTER_WAKE_REASON_PACKET for the fixed wake sources. */
typedef enum _NET_ADAPTER_WAKE_PATTERN_ID
{
    NetAdapterWakeFilterPatternId = 0xFFFC,
    NetAdapterWakeEapolPatternId = 0xFFFD,
    NetAdapterWakeMagicPatternId = 0xFFFE
} NET_ADAPTER_WAKE_PATTERN_ID;

typedef enum _NET_WAKE_REASON_TYPE
{
    NetWakeReasonTypeNone = 0,
    NetWakeReasonTypeBitmapPattern,
    NetWakeReasonTypeMagicPacket,
    NetWakeReasonTypeMediaChange,
    NetWakeReasonTypePacketFilterMatch,
    NetWakeReasonTypeEapolPacket,
    NetWakeReasonTypeNdis,
    NetWakeReasonTypeDevice
} NET_WAKE_REASON_TYPE;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERINITADDTXDEMUX)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_ CONST NET_ADAPTER_TX_DEMUX *Demux);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterInitAddTxDemux(
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_ CONST NET_ADAPTER_TX_DEMUX *Demux)
{
    ((PFN_NETADAPTERINITADDTXDEMUX)NetFunctions[NetAdapterInitAddTxDemuxTableIndex])(
        NetDriverGlobals, AdapterInit, Demux);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
CONST NET_ADAPTER_TX_DEMUX *
(NTAPI *PFN_NETADAPTERGETTXPEERADDRESSDEMUX)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
CONST NET_ADAPTER_TX_DEMUX *
NTAPI
NetAdapterGetTxPeerAddressDemux(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERGETTXPEERADDRESSDEMUX)NetFunctions[NetAdapterGetTxPeerAddressDemuxTableIndex])(
        NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWIFIDESTROYPEERADDRESSDATAPATH)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ SIZE_T Demux);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterWifiDestroyPeerAddressDatapath(
    _In_ NETADAPTER Adapter,
    _In_ SIZE_T Demux)
{
    ((PFN_NETADAPTERWIFIDESTROYPEERADDRESSDATAPATH)NetFunctions[NetAdapterWifiDestroyPeerAddressDatapathTableIndex])(
        NetDriverGlobals, Adapter, Demux);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
UINT8
(NTAPI *PFN_NETTXQUEUEGETDEMUX8021P)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
UINT8
NTAPI
NetTxQueueGetDemux8021p(
    _In_ NETPACKETQUEUE Queue)
{
    return ((PFN_NETTXQUEUEGETDEMUX8021P)NetFunctions[NetTxQueueGetDemux8021pTableIndex])(
        NetDriverGlobals, Queue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
UINT8
(NTAPI *PFN_NETTXQUEUEGETDEMUXWMMINFO)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
UINT8
NTAPI
NetTxQueueGetDemuxWmmInfo(
    _In_ NETPACKETQUEUE Queue)
{
    return ((PFN_NETTXQUEUEGETDEMUXWMMINFO)NetFunctions[NetTxQueueGetDemuxWmmInfoTableIndex])(
        NetDriverGlobals, Queue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NET_EUI48_ADDRESS
(NTAPI *PFN_NETTXQUEUEGETDEMUXPEERADDRESS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NET_EUI48_ADDRESS
NTAPI
NetTxQueueGetDemuxPeerAddress(
    _In_ NETPACKETQUEUE Queue)
{
    return ((PFN_NETTXQUEUEGETDEMUXPEERADDRESS)NetFunctions[NetTxQueueGetDemuxPeerAddressTableIndex])(
        NetDriverGlobals, Queue);
}

#ifdef __cplusplus
}
#endif
