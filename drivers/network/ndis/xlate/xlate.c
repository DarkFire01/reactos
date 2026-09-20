/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 5 packet model to NET_BUFFER_LIST translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ndis.h>
#include "xlate.h"

/*
 * An NDIS_BUFFER is an MDL, so the packet's buffer chain becomes the
 * NET_BUFFER's MDL chain as is. The length is recomputed rather than taken
 * from Private.TotalLength, which callers are allowed to leave stale, and the
 * packet is corrected to match.
 */
VOID
NTAPI
NdisXlatePacketToNetBuffer(
    _In_ PNDIS_PACKET Packet,
    _Inout_ PNET_BUFFER NetBuffer)
{
    PMDL Mdl;
    UINT Length = 0;

    for (Mdl = Packet->Private.Head; Mdl != NULL; Mdl = Mdl->Next)
        Length += MmGetMdlByteCount(Mdl);

    NET_BUFFER_FIRST_MDL(NetBuffer) = Packet->Private.Head;
    NET_BUFFER_CURRENT_MDL(NetBuffer) = Packet->Private.Head;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_LENGTH(NetBuffer) = Length;

    Packet->Private.TotalLength = Length;
}

/*
 * Release a chain this file built. The MDLs belong to the packets, so they are
 * detached first rather than freed with the NET_BUFFER_LIST.
 */
VOID
NTAPI
NdisXlateFreeNetBufferLists(
    _In_opt_ PNET_BUFFER_LIST NetBufferLists)
{
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER_LIST Next;
    PNET_BUFFER NetBuffer;

    for (NetBufferList = NetBufferLists; NetBufferList != NULL; NetBufferList = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);

        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
             NetBuffer != NULL;
             NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
        {
            NET_BUFFER_FIRST_MDL(NetBuffer) = NULL;
            NET_BUFFER_CURRENT_MDL(NetBuffer) = NULL;
        }

        NdisFreeNetBufferList(NetBufferList);
    }
}

/*
 * Translate packets from the cursor onwards into one NET_BUFFER_LIST chain.
 *
 * A chain only covers packets that agree on NDIS_FLAGS_DONT_LOOPBACK, because
 * the flag applies to a send as a whole rather than per packet. Hitting a
 * packet that disagrees ends the run and leaves the cursor on it.
 *
 * Returns TRUE when packets remain, so the caller knows to come back.
 */
BOOLEAN
NTAPI
NdisXlatePacketArray(
    _Inout_ PNDIS_SEND_XLATE Xlate)
{
    PNET_BUFFER_LIST Head = NULL;
    PNET_BUFFER_LIST Tail = NULL;
    PNET_BUFFER_LIST NetBufferList;
    PNDIS_PACKET Packet;
    BOOLEAN DontLoopback = FALSE;
    BOOLEAN First = TRUE;
    UINT Count = 0;

    Xlate->NetBufferLists = NULL;
    Xlate->NetBufferListCount = 0;

    while (Xlate->Translated < Xlate->PacketCount)
    {
        Packet = Xlate->Packets[Xlate->Translated];

        if (First)
        {
            DontLoopback = (Packet->Private.Flags & NDIS_FLAGS_DONT_LOOPBACK) != 0;
            First = FALSE;
        }
        else if (DontLoopback != ((Packet->Private.Flags & NDIS_FLAGS_DONT_LOOPBACK) != 0))
        {
            break;
        }

        NetBufferList = NdisAllocateNetBufferAndNetBufferList(Xlate->NblPool, 0, 0, NULL, 0, 0);
        if (NetBufferList == NULL)
        {
            /* Nothing partial goes out: drop the run and let the caller retry. */
            NdisXlateFreeNetBufferLists(Head);
            Xlate->NetBufferLists = NULL;
            Xlate->NetBufferListCount = 0;
            return Xlate->Translated < Xlate->PacketCount;
        }

        NdisXlatePacketToNetBuffer(Packet, NET_BUFFER_LIST_FIRST_NB(NetBufferList));

        NetBufferList->SourceHandle = Xlate->Owner;
        NetBufferList->NdisReserved[0] = Packet;

        NET_BUFFER_LIST_INFO(NetBufferList, NetBufferListFrameType) =
            (PVOID)(ULONG_PTR)(Packet->Private.Flags & NDIS_FLAGS_PROTOCOL_ID_MASK);

        if (Tail == NULL)
            Head = NetBufferList;
        else
            NET_BUFFER_LIST_NEXT_NBL(Tail) = NetBufferList;

        Tail = NetBufferList;
        Count++;
        Xlate->Translated++;
    }

    Xlate->NetBufferLists = Head;
    Xlate->NetBufferListCount = Count;
    Xlate->DontLoopback = DontLoopback;

    return Xlate->Translated < Xlate->PacketCount;
}
