/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER_LIST handlers and the counters they report
 *
 * The dispatcher sits between NDIS and whichever translator currently owns
 * the datapath. Transmit and receive handlers plug into it independently.
 */

#pragma once

#include <ndis.h>

struct PacketCounter
{
    ULONG64 Packets;
    ULONG64 Bytes;
};

struct PacketCounters
{
    PacketCounter Unicast;
    PacketCounter Multicast;
    PacketCounter Broadcast;
    ULONG64 Errors;
};

using TxPacketCounters = PacketCounters;

struct NblRscStatistics
{
    ULONG64 NblsCoalesced;
    ULONG64 NblsAborted;
    ULONG64 ScusGenerated;
    ULONG64 BytesCoalesced;
};

struct RxPacketCounters :
    public PacketCounters
{
    NblRscStatistics Rsc;
};

class INxNblTx
{
public:

    _IRQL_requires_max_(DISPATCH_LEVEL)
    virtual
    void
    SendNetBufferLists(
        _In_ NET_BUFFER_LIST * Nbl,
        _In_ ULONG PortNumber,
        _In_ ULONG NblCount,
        _In_ ULONG SendFlags
    ) = 0;
};

class INxNblRx
{
public:

    _IRQL_requires_max_(DISPATCH_LEVEL)
    virtual
    ULONG
    ReturnNetBufferLists(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG ReceiveCompleteFlags
    ) = 0;
};

class INxNblDispatcher
{
public:

    virtual
    void
    CloseTxHandler(
        void
    ) = 0;

    virtual
    void
    SetTxHandler(
        _In_opt_ INxNblTx * Tx
    ) = 0;

    virtual
    void
    SetRxHandler(
        _In_opt_ INxNblRx * Rx
    ) = 0;

    _IRQL_requires_max_(DISPATCH_LEVEL)
    virtual
    void
    SendNetBufferLists(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG PortNumber,
        _In_ ULONG SendFlags
    ) = 0;

    _IRQL_requires_max_(DISPATCH_LEVEL)
    virtual
    void
    SendNetBufferListsComplete(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG NumberOfNbls,
        _In_ ULONG SendCompleteFlags
    ) = 0;

    _IRQL_requires_max_(DISPATCH_LEVEL)
    virtual
    void
    CountTxStatistics(
        _In_ TxPacketCounters const & Counters
    ) = 0;

    /* FALSE means the receive path is closed and the chain was not taken. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Must_inspect_result_
    virtual
    bool
    IndicateReceiveNetBufferLists(
        _In_ RxPacketCounters const & Counters,
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG PortNumber,
        _In_ ULONG NumberOfNbls,
        _In_ ULONG ReceiveFlags
    ) = 0;

    _IRQL_requires_max_(DISPATCH_LEVEL)
    virtual
    void
    ReturnNetBufferLists(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG ReceiveReturnFlags
    ) = 0;

    virtual
    NTSTATUS
    QueryStatisticsInfo(
        _Inout_ NDIS_OID_REQUEST & Request
    ) = 0;

    virtual
    NTSTATUS
    QueryRscStatisticsInfo(
        _Inout_ NDIS_OID_REQUEST & Request
    ) = 0;
};
