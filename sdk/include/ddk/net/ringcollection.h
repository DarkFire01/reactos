/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The set of rings that make up a queue
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct _NET_RING;
typedef struct _NET_RING NET_RING;

typedef enum _NET_RING_TYPE
{
    NetRingTypePacket,
    NetRingTypeFragment,
    NetRingTypeDataBuffer
} NET_RING_TYPE;

typedef struct _NET_RING_COLLECTION
{
    NET_RING *Rings[NetRingTypeDataBuffer + 1];
} NET_RING_COLLECTION;

FORCEINLINE
NET_RING *
NetRingCollectionGetPacketRing(
    _In_ const NET_RING_COLLECTION *Rings)
{
    return Rings->Rings[NetRingTypePacket];
}

FORCEINLINE
NET_RING *
NetRingCollectionGetFragmentRing(
    _In_ const NET_RING_COLLECTION *Rings)
{
    return Rings->Rings[NetRingTypeFragment];
}

FORCEINLINE
NET_RING *
NetRingCollectionGetFragmentReturnContextRing(
    _In_ const NET_RING_COLLECTION *Rings)
{
    return Rings->Rings[NetRingTypeDataBuffer];
}

/* Both indices move together, so they are updated as one interlocked write. */
typedef union _NET_RING_INDICES
{
    LONG64 AsLONG64;
    struct
    {
        UINT32 Packet;
        UINT32 Fragment;
    } DUMMYSTRUCTNAME;
} NET_RING_INDICES;

C_ASSERT(sizeof(NET_RING_INDICES) == sizeof(LONG64));

#ifdef __cplusplus
}
#endif
