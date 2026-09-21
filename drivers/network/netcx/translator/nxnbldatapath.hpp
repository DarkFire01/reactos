/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The NET_BUFFER_LIST dispatcher every adapter owns
 *
 * Each direction is guarded by its own rundown so a handler can be swapped
 * while NDIS keeps calling in. Both start closed until a handler is set.
 */

#pragma once

#include <KRundown.h>

#include "NxNbl.hpp"

class NxNblDatapath :
    public INxNblDispatcher
{
public:

    struct DatapathCounters
    {
        TxPacketCounters Tx;
        RxPacketCounters Rx;
    };

    NxNblDatapath(
        void
    );

    void
    SetNdisHandle(
        _In_ NDIS_HANDLE NdisHandle
    )
    {
        m_ndisHandle = NdisHandle;
    }

    void
    CloseTxHandler(
        void
    ) override;

    void
    SetTxHandler(
        _In_opt_ INxNblTx * Tx
    ) override;

    void
    SetRxHandler(
        _In_opt_ INxNblRx * Rx
    ) override;

    void
    SendNetBufferLists(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG PortNumber,
        _In_ ULONG SendFlags
    ) override;

    void
    SendNetBufferListsComplete(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG NumberOfNbls,
        _In_ ULONG SendCompleteFlags
    ) override;

    void
    CountTxStatistics(
        _In_ TxPacketCounters const & Counters
    ) override;

    _Must_inspect_result_
    bool
    IndicateReceiveNetBufferLists(
        _In_ RxPacketCounters const & Counters,
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG PortNumber,
        _In_ ULONG NumberOfNbls,
        _In_ ULONG ReceiveFlags
    ) override;

    void
    ReturnNetBufferLists(
        _In_ NET_BUFFER_LIST * NblChain,
        _In_ ULONG ReceiveReturnFlags
    ) override;

    NTSTATUS
    QueryStatisticsInfo(
        _Inout_ NDIS_OID_REQUEST & Request
    ) override;

    NTSTATUS
    QueryRscStatisticsInfo(
        _Inout_ NDIS_OID_REQUEST & Request
    ) const override;

private:

    NDIS_HANDLE
        m_ndisHandle = nullptr;

    KRundown
        m_txRundown;

    KRundown
        m_rxRundown;

    LONG
        m_txClosed = FALSE;

    INxNblTx *
        m_tx = nullptr;

    INxNblRx *
        m_rx = nullptr;

    DatapathCounters
        m_counters = {};

public:

    DatapathCounters const &
        Counters = m_counters;
};
