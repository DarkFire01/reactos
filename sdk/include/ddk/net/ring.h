/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet ring buffer
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct _NET_PACKET;
typedef struct _NET_PACKET NET_PACKET;

struct _NET_FRAGMENT;
typedef struct _NET_FRAGMENT NET_FRAGMENT;

struct _NET_FRAGMENT_RETURN_CONTEXT;
typedef struct _NET_FRAGMENT_RETURN_CONTEXT NET_FRAGMENT_RETURN_CONTEXT;

/*
 * NumberOfElements is always a power of two, so ElementIndexMask clamps an
 * index with an and rather than a division.
 */
typedef struct DECLSPEC_CACHEALIGN _NET_RING
{
    UINT16 OSReserved1;
    UINT16 ElementStride;
    UINT32 NumberOfElements;
    UINT32 ElementIndexMask;
    UINT32 EndIndex;
    union
    {
        UINT32 OSReserved0;
        PVOID OSReserved2[4];
    } DUMMYUNIONNAME;
    UINT32 BeginIndex;
    UINT32 NextIndex;
    PVOID Scratch;
    DECLSPEC_CACHEALIGN
    _Field_size_(NumberOfElements * ElementStride)
    UCHAR Buffer[ANYSIZE_ARRAY];
} NET_RING;

C_ASSERT(FIELD_OFFSET(NET_RING, Buffer) == SYSTEM_CACHE_ALIGNMENT_SIZE);

FORCEINLINE
PVOID
NetRingGetElementAtIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index)
{
    return (PVOID)(Ring->Buffer + (SIZE_T)Index * Ring->ElementStride);
}

FORCEINLINE
UINT32
NetRingAdvanceIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index,
    _In_ INT32 Distance)
{
    return (Index + Distance) & Ring->ElementIndexMask;
}

FORCEINLINE
UINT32
NetRingIncrementIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index)
{
    return NetRingAdvanceIndex(Ring, Index, 1);
}

/* The range is half open, so [7, 7) is empty and [4, 1) wraps to 5 elements. */
FORCEINLINE
UINT32
NetRingGetRangeCount(
    _In_ const NET_RING *Ring,
    _In_ UINT32 StartIndex,
    _In_ UINT32 EndIndex)
{
    NT_ASSERT(StartIndex < Ring->NumberOfElements);
    NT_ASSERT(EndIndex < Ring->NumberOfElements);

    return (EndIndex - StartIndex) & Ring->ElementIndexMask;
}

FORCEINLINE
NET_PACKET *
NetRingGetPacketAtIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index)
{
    return (NET_PACKET *)NetRingGetElementAtIndex(Ring, Index);
}

FORCEINLINE
NET_FRAGMENT *
NetRingGetFragmentAtIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT *)NetRingGetElementAtIndex(Ring, Index);
}

FORCEINLINE
NET_FRAGMENT_RETURN_CONTEXT *
NetRingGetFragmentReturnContextAtIndex(
    _In_ const NET_RING *Ring,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_RETURN_CONTEXT *)NetRingGetElementAtIndex(Ring, Index);
}

#ifdef __cplusplus
}
#endif
