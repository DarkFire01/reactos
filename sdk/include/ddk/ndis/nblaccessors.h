/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER and NET_BUFFER_LIST field accessors
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NET_BUFFER_NEXT_NB(_NB)                 ((_NB)->Next)
#define NET_BUFFER_FIRST_MDL(_NB)               ((_NB)->MdlChain)
#define NET_BUFFER_DATA_LENGTH(_NB)             ((_NB)->DataLength)
#define NET_BUFFER_DATA_OFFSET(_NB)             ((_NB)->DataOffset)
#define NET_BUFFER_CURRENT_MDL(_NB)             ((_NB)->CurrentMdl)
#define NET_BUFFER_CURRENT_MDL_OFFSET(_NB)      ((_NB)->CurrentMdlOffset)
#define NET_BUFFER_PROTOCOL_RESERVED(_NB)       ((_NB)->ProtocolReserved)
#define NET_BUFFER_MINIPORT_RESERVED(_NB)       ((_NB)->MiniportReserved)
#define NET_BUFFER_CHECKSUM_BIAS(_NB)           ((_NB)->ChecksumBias)
#define NET_BUFFER_DATA_PHYSICAL_ADDRESS(_NB)   ((_NB)->DataPhysicalAddress)
#define NET_BUFFER_SHARED_MEMORY_INFO(_NB)      ((_NB)->SharedMemoryInfo)

#define NET_BUFFER_LIST_NEXT_NBL(_NBL)          ((_NBL)->Next)
#define NET_BUFFER_LIST_FIRST_NB(_NBL)          ((_NBL)->FirstNetBuffer)
#define NET_BUFFER_LIST_FLAGS(_NBL)             ((_NBL)->Flags)
#define NET_BUFFER_LIST_NBL_FLAGS(_NBL)         ((_NBL)->NblFlags)
#define NET_BUFFER_LIST_STATUS(_NBL)            ((_NBL)->Status)
#define NET_BUFFER_LIST_PROTOCOL_RESERVED(_NBL) ((_NBL)->ProtocolReserved)
#define NET_BUFFER_LIST_MINIPORT_RESERVED(_NBL) ((_NBL)->MiniportReserved)
#define NET_BUFFER_LIST_CONTEXT_DATA_START(_NBL) \
    ((PUCHAR)(((_NBL)->Context)->ContextData + (_NBL)->Context->Offset))
#define NET_BUFFER_LIST_CONTEXT_DATA_SIZE(_NBL) \
    ((_NBL)->Context->Size - (_NBL)->Context->Offset)

#define NET_BUFFER_LIST_INFO(_NBL, _Id)         ((_NBL)->NetBufferListInfo[(_Id)])

/*
 * The 8021Q tag, the hash value and the hash info are stored in the info slot
 * itself rather than behind a pointer.
 */
#define NET_BUFFER_LIST_RECEIVE_HASH_VALUE(_NBL) \
    PtrToUlong(NET_BUFFER_LIST_INFO(_NBL, NetBufferListHashValue))
#define NET_BUFFER_LIST_RECEIVE_HASH_TYPE(_NBL) \
    (PtrToUlong(NET_BUFFER_LIST_INFO(_NBL, NetBufferListHashInfo)) & 0x0000FFFF)
#define NET_BUFFER_LIST_RECEIVE_HASH_FUNCTION(_NBL) \
    ((PtrToUlong(NET_BUFFER_LIST_INFO(_NBL, NetBufferListHashInfo)) >> 16) & 0x0000FFFF)

#ifdef __cplusplus
}
#endif
