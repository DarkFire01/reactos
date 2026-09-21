/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet queue interface between the two halves of the class
 *              extension
 */

#pragma once

#include <NetClientTypes.h>
#include <NetClientBuffer.h>
#include <net/ringcollection.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_CLIENT_QUEUE_CONFIG
{
    ULONG Size;
    ULONG NumberOfPackets;
    ULONG NumberOfFragments;
    NET_CLIENT_EXTENSION *Extensions;
    SIZE_T NumberOfExtensions;
    SIZE_T NumberOfDataBuffers;
    NET_CLIENT_BUFFER_POOL DataBufferPool;
    NET_CLIENT_QUEUE_TX_DEMUX_PROPERTY DemuxPeerAddressProperty;
    NET_CLIENT_QUEUE_TX_DEMUX_PROPERTY DemuxWmmProperty;
    NET_CLIENT_QUEUE_TX_DEMUX_PROPERTY Demux8021pProperty;
} NET_CLIENT_QUEUE_CONFIG;

FORCEINLINE
VOID
NET_CLIENT_QUEUE_CONFIG_INIT(
    _Out_ NET_CLIENT_QUEUE_CONFIG *Config,
    _In_ ULONG NumberOfPackets,
    _In_ ULONG NumberOfFragments)
{
    RtlZeroMemory(Config, sizeof(*Config));

    Config->Size = sizeof(*Config);
    Config->NumberOfPackets = NumberOfPackets;
    Config->NumberOfFragments = NumberOfFragments;
}

/* Implemented by the adapter half, called by the translator. */
typedef struct _NET_CLIENT_QUEUE_DISPATCH
{
    ULONG Size;

    VOID
    (*Start)(
        _In_ NET_CLIENT_QUEUE Queue);

    VOID
    (*Stop)(
        _In_ NET_CLIENT_QUEUE Queue);

    VOID
    (*Advance)(
        _In_ NET_CLIENT_QUEUE Queue);

    VOID
    (*Cancel)(
        _In_ NET_CLIENT_QUEUE Queue);

    VOID
    (*SetArmed)(
        _In_ NET_CLIENT_QUEUE Queue,
        _In_ BOOLEAN IsArmed);

    VOID
    (*GetExtension)(
        _In_ NET_CLIENT_QUEUE Queue,
        _In_ NET_CLIENT_EXTENSION const *ExtensionToQuery,
        _Out_ NET_EXTENSION *Extension);

    NET_RING_COLLECTION const *
    (*GetNetDatapathDescriptor)(
        _In_ NET_CLIENT_QUEUE Queue);
} NET_CLIENT_QUEUE_DISPATCH;

/* Implemented by the translator, called by the adapter half. */
typedef struct _NET_CLIENT_QUEUE_NOTIFY_DISPATCH
{
    ULONG Size;

    VOID
    (*Notify)(
        _In_ PVOID Queue);

    NET_CLIENT_QUEUE_TX_DEMUX_PROPERTY const *
    (*GetTxDemuxProperty)(
        _In_ NET_CLIENT_QUEUE Queue,
        _In_ NET_CLIENT_QUEUE_TX_DEMUX_TYPE Type);
} NET_CLIENT_QUEUE_NOTIFY_DISPATCH;

#ifdef __cplusplus
}
#endif
