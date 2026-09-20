/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 5 packet model to NET_BUFFER_LIST translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * State for turning a run of NDIS_PACKETs into a NET_BUFFER_LIST chain.
 *
 * Translated is a cursor, not a count: a run stops early when the batch key
 * changes or an allocation fails, and the caller resumes from where it left
 * off. Everything below Translated has already been handed over.
 */
typedef struct _NDIS_SEND_XLATE
{
    NDIS_HANDLE NblPool;
    NDIS_HANDLE Owner;
    PPNDIS_PACKET Packets;
    UINT PacketCount;
    UINT Translated;
    PNET_BUFFER_LIST NetBufferLists;
    UINT NetBufferListCount;
    BOOLEAN DontLoopback;
} NDIS_SEND_XLATE, *PNDIS_SEND_XLATE;

/*
 * The originating NDIS_PACKET, parked on the NET_BUFFER_LIST so send
 * completion can find its way back to the packet the protocol owns.
 */
#define NDIS_XLATE_PACKET(_Nbl)     ((PNDIS_PACKET)((_Nbl)->NdisReserved[0]))

VOID
NTAPI
NdisXlatePacketToNetBuffer(
    _In_ PNDIS_PACKET Packet,
    _Inout_ PNET_BUFFER NetBuffer);

BOOLEAN
NTAPI
NdisXlatePacketArray(
    _Inout_ PNDIS_SEND_XLATE Xlate);

VOID
NTAPI
NdisXlateFreeNetBufferLists(
    _In_opt_ PNET_BUFFER_LIST NetBufferLists);

#ifdef __cplusplus
}
#endif
