/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER_LIST chain and queue helpers
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Last points at the Next field of the final entry, or at First when the
 * queue is empty, so appending never has to test for the empty case.
 */
typedef struct _NBL_QUEUE
{
    PNET_BUFFER_LIST First;
    PNET_BUFFER_LIST *Last;
} NBL_QUEUE, *PNBL_QUEUE;

typedef struct _NBL_COUNTED_QUEUE
{
    NBL_QUEUE Queue;
    SIZE_T NblCount;
} NBL_COUNTED_QUEUE, *PNBL_COUNTED_QUEUE;

FORCEINLINE
VOID
NdisInitializeNblQueue(
    _Out_ PNBL_QUEUE Queue)
{
    Queue->First = NULL;
    Queue->Last = &Queue->First;
}

FORCEINLINE
VOID
NdisInitializeNblCountedQueue(
    _Out_ PNBL_COUNTED_QUEUE Queue)
{
    NdisInitializeNblQueue(&Queue->Queue);
    Queue->NblCount = 0;
}

FORCEINLINE
BOOLEAN
NdisIsNblQueueEmpty(
    _In_ const NBL_QUEUE *Queue)
{
    return Queue->First == NULL;
}

FORCEINLINE
VOID
NdisAppendSingleNblToNblQueue(
    _Inout_ PNBL_QUEUE Queue,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    *Queue->Last = NetBufferList;
    NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;
    Queue->Last = &NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
}

/* Fast because the caller already knows where the chain ends. */
FORCEINLINE
VOID
NdisAppendNblChainToNblQueueFast(
    _Inout_ PNBL_QUEUE Queue,
    _In_ PNET_BUFFER_LIST FirstNetBufferList,
    _In_ PNET_BUFFER_LIST LastNetBufferList)
{
    *Queue->Last = FirstNetBufferList;
    NET_BUFFER_LIST_NEXT_NBL(LastNetBufferList) = NULL;
    Queue->Last = &NET_BUFFER_LIST_NEXT_NBL(LastNetBufferList);
}

FORCEINLINE
VOID
NdisAppendNblQueueToNblQueueFast(
    _Inout_ PNBL_QUEUE Queue,
    _Inout_ PNBL_QUEUE Source)
{
    if (!NdisIsNblQueueEmpty(Source))
    {
        *Queue->Last = Source->First;
        Queue->Last = Source->Last;
        NdisInitializeNblQueue(Source);
    }
}

FORCEINLINE
PNET_BUFFER_LIST
NdisPopAllFromNblQueue(
    _Inout_ PNBL_QUEUE Queue)
{
    PNET_BUFFER_LIST NetBufferList = Queue->First;

    NdisInitializeNblQueue(Queue);

    return NetBufferList;
}

FORCEINLINE
PNET_BUFFER_LIST
NdisLastNblInNblChain(
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    while (NET_BUFFER_LIST_NEXT_NBL(NetBufferList) != NULL)
        NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);

    return NetBufferList;
}

FORCEINLINE
PNET_BUFFER_LIST
NdisLastNblInNblChainWithCount(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _Out_ SIZE_T *Count)
{
    SIZE_T Seen = 1;

    while (NET_BUFFER_LIST_NEXT_NBL(NetBufferList) != NULL)
    {
        NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
        Seen++;
    }

    *Count = Seen;

    return NetBufferList;
}

FORCEINLINE
SIZE_T
NdisNumNblsInNblChain(
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    SIZE_T Count = 0;

    for (; NetBufferList != NULL; NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList))
        Count++;

    return Count;
}

FORCEINLINE
VOID
NdisSetStatusInNblChain(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ NDIS_STATUS Status)
{
    for (; NetBufferList != NULL; NetBufferList = NET_BUFFER_LIST_NEXT_NBL(NetBufferList))
        NET_BUFFER_LIST_STATUS(NetBufferList) = Status;
}

FORCEINLINE
VOID
NdisSetNblFlag(
    _Inout_ PNET_BUFFER_LIST NetBufferList,
    _In_ ULONG Flag)
{
    NET_BUFFER_LIST_NBL_FLAGS(NetBufferList) |= Flag;
}

FORCEINLINE
BOOLEAN
NdisTestNblFlag(
    _In_ const NET_BUFFER_LIST *NetBufferList,
    _In_ ULONG Flag)
{
    return (NET_BUFFER_LIST_NBL_FLAGS(NetBufferList) & Flag) == Flag;
}

#ifdef __cplusplus
}
#endif
