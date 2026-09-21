/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Receive hash carried in the NET_BUFFER_LIST info slots
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The hash value has a slot of its own. The hash info slot packs the function
 * into the low byte and the type into the two bytes above it.
 */
#define NET_BUFFER_LIST_GET_HASH_VALUE(_NBL)     PtrToUlong(NET_BUFFER_LIST_INFO((_NBL), NetBufferListHashValue))

#define NET_BUFFER_LIST_GET_HASH_TYPE(_NBL)     (PtrToUlong(NET_BUFFER_LIST_INFO((_NBL), NetBufferListHashInfo)) & NDIS_HASH_TYPE_MASK)

#define NET_BUFFER_LIST_GET_HASH_FUNCTION(_NBL)     (PtrToUlong(NET_BUFFER_LIST_INFO((_NBL), NetBufferListHashInfo)) & NDIS_HASH_FUNCTION_MASK)

#define NET_BUFFER_LIST_SET_HASH_VALUE(_NBL, _HashValue)     (NET_BUFFER_LIST_INFO((_NBL), NetBufferListHashValue) = UlongToPtr(_HashValue))

FORCEINLINE
VOID
NET_BUFFER_LIST_SET_HASH_TYPE(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ ULONG HashType)
{
    ULONG HashInfo = PtrToUlong(NET_BUFFER_LIST_INFO(NetBufferList, NetBufferListHashInfo));

    HashInfo = (HashInfo & ~NDIS_HASH_TYPE_MASK) | (HashType & NDIS_HASH_TYPE_MASK);
    NET_BUFFER_LIST_INFO(NetBufferList, NetBufferListHashInfo) = UlongToPtr(HashInfo);
}

FORCEINLINE
VOID
NET_BUFFER_LIST_SET_HASH_FUNCTION(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ ULONG HashFunction)
{
    ULONG HashInfo = PtrToUlong(NET_BUFFER_LIST_INFO(NetBufferList, NetBufferListHashInfo));

    HashInfo = (HashInfo & ~NDIS_HASH_FUNCTION_MASK) | (HashFunction & NDIS_HASH_FUNCTION_MASK);
    NET_BUFFER_LIST_INFO(NetBufferList, NetBufferListHashInfo) = UlongToPtr(HashInfo);
}

#ifdef __cplusplus
}
#endif
