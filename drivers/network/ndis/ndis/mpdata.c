/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER_LIST send and receive through the miniport core
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/* Packets over NET_BUFFERs, shared by both NDIS 5 edges */

/**
 * @brief
 * Lays an NDIS_PACKET over the MDLs of a NET_BUFFER without copying.
 *
 * The first MDL is advanced past the bytes before the data and the last one
 * is cut short at the end of it. CoreFreeNetBufferPacket puts both back.
 *
 * @param[in] PacketPool
 * A pool with room for the packet state past the protocol reserved area.
 *
 * @param[in] NetBufferList
 * The list the NET_BUFFER belongs to, remembered in the packet.
 *
 * @param[in] NetBuffer
 * The data to describe.
 *
 * @return
 * The packet, or NULL when the pool is empty or the NET_BUFFER has no data.
 */
PNDIS_PACKET
NTAPI
CorePacketFromNetBuffer(
    _In_ NDIS_HANDLE PacketPool,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ PNET_BUFFER NetBuffer)
{
    PCORE_PACKET_STATE State;
    PNDIS_PACKET Packet;
    NDIS_STATUS Status;
    PMDL First;
    PMDL Last;
    ULONG Offset;
    ULONG Remaining;

    First = NET_BUFFER_CURRENT_MDL(NetBuffer);
    Offset = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer);
    Remaining = NET_BUFFER_DATA_LENGTH(NetBuffer);

    if (First == NULL || Remaining == 0)
        return NULL;

    NdisAllocatePacket(&Status, &Packet, PacketPool);
    if (Status != NDIS_STATUS_SUCCESS)
        return NULL;

    State = CORE_PACKET_STATE(Packet);
    State->NetBufferList = NetBufferList;
    State->NetBuffer = NetBuffer;
    State->FirstMdlOffset = Offset;

    First->ByteOffset += Offset;
    First->ByteCount -= Offset;
    if (First->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL))
        First->MappedSystemVa = (PUCHAR)First->MappedSystemVa + Offset;

    /* Find the MDL the data ends in */
    for (Last = First; Last->ByteCount < Remaining && Last->Next != NULL; Last = Last->Next)
        Remaining -= Last->ByteCount;

    State->LastMdl = Last;
    State->LastMdlNext = Last->Next;
    State->LastMdlByteCount = Last->ByteCount;

    Last->ByteCount = min(Last->ByteCount, Remaining);
    Last->Next = NULL;

    Packet->Private.Head = First;
    Packet->Private.Tail = Last;
    Packet->Private.ValidCounts = FALSE;

    return Packet;
}

/**
 * @brief
 * Puts the MDLs under a packet from CorePacketFromNetBuffer back the way they
 * were and frees the packet.
 *
 * @param[in] Packet
 * The packet to release.
 */
VOID
NTAPI
CoreFreeNetBufferPacket(
    _In_ PNDIS_PACKET Packet)
{
    PCORE_PACKET_STATE State = CORE_PACKET_STATE(Packet);
    PMDL First = Packet->Private.Head;

    State->LastMdl->ByteCount = State->LastMdlByteCount;
    State->LastMdl->Next = State->LastMdlNext;

    First->ByteOffset -= State->FirstMdlOffset;
    First->ByteCount += State->FirstMdlOffset;
    if (First->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL))
        First->MappedSystemVa = (PUCHAR)First->MappedSystemVa - State->FirstMdlOffset;

    Packet->Private.Head = NULL;
    Packet->Private.Tail = NULL;
    NdisFreePacket(Packet);
}

/* Send */

static
VOID
CoreCompleteSendsNow(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_STATUS Status)
{
    PNET_BUFFER_LIST NetBufferList;

    for (NetBufferList = NetBufferLists;
         NetBufferList != NULL;
         NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList))
    {
        NET_BUFFER_LIST_STATUS(NetBufferList) = Status;
    }

    Pro5SendComplete(Adapter, NetBufferLists);
}

/*
 * A frame addressed to the adapter itself is looped back here when the
 * miniport says it cannot do that. It never reaches the wire.
 */
static
BOOLEAN
CoreIsLoopbackFrame(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    PNET_BUFFER NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
    UCHAR Destination[ETH_LENGTH_OF_ADDRESS];
    PUCHAR Data;

    if (!(Adapter->Core.MacOptions & NDIS_MAC_OPTION_NO_LOOPBACK) ||
        Adapter->Core.MediaType != NdisMedium802_3 ||
        NetBuffer == NULL)
    {
        return FALSE;
    }

    Data = NdisGetDataBuffer(NetBuffer, sizeof(Destination), Destination, 1, 0);
    if (Data == NULL)
        return FALSE;

    return RtlEqualMemory(Data, Adapter->Address.Type.Medium802_3, ETH_LENGTH_OF_ADDRESS);
}

static
VOID
CoreLoopbackNetBufferList(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    PNET_BUFFER_LIST Unheld;
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* The sender still owns the data, so the protocols are told to copy it */
    Pro5IndicateReceive(Adapter,
                        NetBufferList,
                        NDIS_RECEIVE_FLAGS_RESOURCES | NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL,
                        &Unheld);

    NET_BUFFER_LIST_STATUS(NetBufferList) = NDIS_STATUS_SUCCESS;
    Pro5SendComplete(Adapter, NetBufferList);

    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * The one path every send takes on its way to a miniport.
 *
 * @param[in] Adapter
 * The adapter to send on.
 *
 * @param[in] NetBufferLists
 * The chain to send. Each list's SourceHandle says who to complete it to.
 *
 * @param[in] PortNumber
 * The NDIS port.
 *
 * @param[in] SendFlags
 * NDIS_SEND_FLAGS_*.
 */
VOID
NTAPI
CoreSendNetBufferLists(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER_LIST Next;
    PNET_BUFFER_LIST Head = NULL;
    PNET_BUFFER_LIST Tail = NULL;
    LONG Count = 0;
    KIRQL OldIrql;

    /* Frames for this adapter itself come out of the chain first */
    for (NetBufferList = NetBufferLists; NetBufferList != NULL; NetBufferList = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
        NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;

        if (CoreIsLoopbackFrame(Adapter, NetBufferList))
        {
            CoreLoopbackNetBufferList(Adapter, NetBufferList);
            continue;
        }

        if (Tail == NULL)
            Head = NetBufferList;
        else
            NET_BUFFER_LIST_NEXT_NBL(Tail) = NetBufferList;
        Tail = NetBufferList;
        Count++;
    }

    if (Head == NULL)
        return;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);

    if (Core->State != CoreMiniportRunning)
    {
        KeReleaseSpinLock(&Core->Lock, OldIrql);
        CoreCompleteSendsNow(Adapter, Head, NDIS_STATUS_PAUSED);
        return;
    }

    if (Core->OutstandingSends == 0)
        KeClearEvent(&Core->SendsDrained);
    Core->OutstandingSends += Count;

    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (KeGetCurrentIrql() == DISPATCH_LEVEL)
        SendFlags |= NDIS_SEND_FLAGS_DISPATCH_LEVEL;
    else
        SendFlags &= ~NDIS_SEND_FLAGS_DISPATCH_LEVEL;

    Core->Dispatch->SendNetBufferListsHandler(CORE_DISPATCH_CONTEXT(Adapter),
                                              Head,
                                              PortNumber,
                                              SendFlags);
}

/**
 * @brief
 * A miniport hands back NET_BUFFER_LISTs it finished sending.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter that sent them.
 *
 * @param[in] NetBufferList
 * The completed chain, each list carrying its status.
 *
 * @param[in] SendCompleteFlags
 * NDIS_SEND_COMPLETE_FLAGS_*.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMSendNetBufferListsComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    PNET_BUFFER_LIST NetBufferList,
    ULONG SendCompleteFlags)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PMINIPORT_CORE Core = &Adapter->Core;
    PNET_BUFFER_LIST Current;
    LONG Count = 0;
    KIRQL OldIrql;

    for (Current = NetBufferList; Current != NULL; Current = NET_BUFFER_LIST_NEXT_NBL(Current))
        Count++;

    if (!(SendCompleteFlags & NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL))
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    Pro5SendComplete(Adapter, NetBufferList);

    KeAcquireSpinLockAtDpcLevel(&Core->Lock);
    Core->OutstandingSends -= Count;
    if (Core->OutstandingSends <= 0)
    {
        Core->OutstandingSends = 0;
        KeSetEvent(&Core->SendsDrained, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLockFromDpcLevel(&Core->Lock);

    if (!(SendCompleteFlags & NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL))
        KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Waits until the miniport completed every send it was given.
 *
 * @param[in] Adapter
 * The adapter to drain.
 */
VOID
NTAPI
CoreWaitForSends(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    KeWaitForSingleObject(&Adapter->Core.SendsDrained, Executive, KernelMode, FALSE, NULL);
}

/* Receive */

static
VOID
CoreReturnToMiniport(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PNET_BUFFER_LIST NetBufferLists)
{
    ULONG ReturnFlags = 0;

    if (NetBufferLists == NULL)
        return;

    if (KeGetCurrentIrql() == DISPATCH_LEVEL)
        ReturnFlags |= NDIS_RETURN_FLAGS_DISPATCH_LEVEL;

    Adapter->Core.Dispatch->ReturnNetBufferListsHandler(CORE_DISPATCH_CONTEXT(Adapter),
                                                        NetBufferLists,
                                                        ReturnFlags);
}

/**
 * @brief
 * The one path every received frame takes up from a miniport.
 *
 * @param[in] MiniportAdapterHandle
 * The receiving adapter.
 *
 * @param[in] NetBufferList
 * The received chain.
 *
 * @param[in] PortNumber
 * The NDIS port.
 *
 * @param[in] NumberOfNetBufferLists
 * How many lists the chain holds.
 *
 * @param[in] ReceiveFlags
 * NDIS_RECEIVE_FLAGS_*. With NDIS_RECEIVE_FLAGS_RESOURCES the miniport keeps
 * the chain once this returns and nothing is handed back to it.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMIndicateReceiveNetBufferLists(
    NDIS_HANDLE MiniportAdapterHandle,
    PNET_BUFFER_LIST NetBufferList,
    NDIS_PORT_NUMBER PortNumber,
    ULONG NumberOfNetBufferLists,
    ULONG ReceiveFlags)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PNET_BUFFER_LIST Unheld = NULL;
    KIRQL OldIrql = DISPATCH_LEVEL;

    UNREFERENCED_PARAMETER(PortNumber);
    UNREFERENCED_PARAMETER(NumberOfNetBufferLists);

    if (!(ReceiveFlags & NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL))
    {
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        ReceiveFlags |= NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL;
    }

    if (Adapter->Core.State == CoreMiniportRunning)
        Pro5IndicateReceive(Adapter, NetBufferList, ReceiveFlags, &Unheld);
    else
        Unheld = NetBufferList;

    /* Under NDIS_RECEIVE_FLAGS_RESOURCES the lists never left the miniport */
    if (!(ReceiveFlags & NDIS_RECEIVE_FLAGS_RESOURCES))
        CoreReturnToMiniport(Adapter, Unheld);

    if (OldIrql != DISPATCH_LEVEL)
        KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Gives a received NET_BUFFER_LIST a protocol held on to back to the miniport.
 *
 * @param[in] Adapter
 * The adapter it was received on.
 *
 * @param[in] NetBufferList
 * The list, no longer in use above.
 */
VOID
NTAPI
CoreReturnNetBufferList(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    KIRQL OldIrql;

    NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    CoreReturnToMiniport(Adapter, NetBufferList);
    KeLowerIrql(OldIrql);
}
