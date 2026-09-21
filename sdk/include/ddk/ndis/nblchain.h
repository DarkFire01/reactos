/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER_LIST chain helpers
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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
