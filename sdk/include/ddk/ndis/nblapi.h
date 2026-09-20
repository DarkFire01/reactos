/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER and NET_BUFFER_LIST pool and data path API
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
NTAPI
NdisAllocateNetBufferPool(
    _In_opt_ NDIS_HANDLE NdisHandle,
    _In_ PNET_BUFFER_POOL_PARAMETERS Parameters);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeNetBufferPool(
    _In_ __drv_freesMem(mem) NDIS_HANDLE PoolHandle);

_IRQL_requires_max_(DISPATCH_LEVEL)
PNET_BUFFER
NTAPI
NdisAllocateNetBuffer(
    _In_ NDIS_HANDLE PoolHandle,
    _In_opt_ PMDL MdlChain,
    _In_ ULONG DataOffset,
    _In_ SIZE_T DataLength);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeNetBuffer(
    _In_ __drv_freesMem(mem) PNET_BUFFER NetBuffer);

_IRQL_requires_max_(DISPATCH_LEVEL)
PNET_BUFFER
NTAPI
NdisAllocateNetBufferMdlAndData(
    _In_ NDIS_HANDLE PoolHandle);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
NTAPI
NdisAllocateNetBufferListPool(
    _In_opt_ NDIS_HANDLE NdisHandle,
    _In_ PNET_BUFFER_LIST_POOL_PARAMETERS Parameters);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeNetBufferListPool(
    _In_ __drv_freesMem(mem) NDIS_HANDLE PoolHandle);

_IRQL_requires_max_(DISPATCH_LEVEL)
PNET_BUFFER_LIST
NTAPI
NdisAllocateNetBufferList(
    _In_ NDIS_HANDLE PoolHandle,
    _In_ USHORT ContextSize,
    _In_ USHORT ContextBackFill);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeNetBufferList(
    _In_ __drv_freesMem(mem) PNET_BUFFER_LIST NetBufferList);

_IRQL_requires_max_(DISPATCH_LEVEL)
PNET_BUFFER_LIST
NTAPI
NdisAllocateNetBufferAndNetBufferList(
    _In_ NDIS_HANDLE PoolHandle,
    _In_ USHORT ContextSize,
    _In_ USHORT ContextBackFill,
    _In_opt_ PMDL MdlChain,
    _In_ ULONG DataOffset,
    _In_ SIZE_T DataLength);

_IRQL_requires_max_(DISPATCH_LEVEL)
PNET_BUFFER_LIST
NTAPI
NdisAllocateCloneNetBufferList(
    _In_ PNET_BUFFER_LIST OriginalNetBufferList,
    _In_opt_ NDIS_HANDLE NetBufferListPoolHandle,
    _In_opt_ NDIS_HANDLE NetBufferPoolHandle,
    _In_ ULONG AllocateCloneFlags);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeCloneNetBufferList(
    _In_ __drv_freesMem(mem) PNET_BUFFER_LIST CloneNetBufferList,
    _In_ ULONG FreeCloneFlags);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisCopySendNetBufferListInfo(
    _In_ PNET_BUFFER_LIST DestNetBufferList,
    _In_ PNET_BUFFER_LIST SrcNetBufferList);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisCopyReceiveNetBufferListInfo(
    _In_ PNET_BUFFER_LIST DestNetBufferList,
    _In_ PNET_BUFFER_LIST SrcNetBufferList);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisAllocateNetBufferListContext(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ USHORT ContextSize,
    _In_ USHORT ContextBackFill,
    _In_ ULONG PoolTag);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeNetBufferListContext(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ USHORT ContextSize);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
NTAPI
NdisGetPoolFromNetBuffer(
    _In_ PNET_BUFFER NetBuffer);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
NTAPI
NdisGetPoolFromNetBufferList(
    _In_ PNET_BUFFER_LIST NetBufferList);

_IRQL_requires_max_(DISPATCH_LEVEL)
UCHAR
NTAPI
NdisGetNetBufferListProtocolId(
    _In_ PNET_BUFFER_LIST NetBufferList);

_IRQL_requires_max_(DISPATCH_LEVEL)
PVOID
NTAPI
NdisGetDataBuffer(
    _In_ PNET_BUFFER NetBuffer,
    _In_ ULONG BytesNeeded,
    _Out_writes_bytes_opt_(BytesNeeded) PVOID Storage,
    _In_ ULONG AlignMultiple,
    _In_ ULONG AlignOffset);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisRetreatNetBufferDataStart(
    _In_ PNET_BUFFER NetBuffer,
    _In_ ULONG DataOffsetDelta,
    _In_ ULONG DataBackFill,
    _In_opt_ NET_BUFFER_ALLOCATE_MDL_HANDLER AllocateMdlHandler);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisAdvanceNetBufferDataStart(
    _In_ PNET_BUFFER NetBuffer,
    _In_ ULONG DataOffsetDelta,
    _In_ BOOLEAN FreeMdl,
    _In_opt_ NET_BUFFER_FREE_MDL_HANDLER FreeMdlHandler);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisRetreatNetBufferListDataStart(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ ULONG DataOffsetDelta,
    _In_ ULONG DataBackFill,
    _In_opt_ NET_BUFFER_ALLOCATE_MDL_HANDLER AllocateMdlHandler,
    _In_opt_ NET_BUFFER_FREE_MDL_HANDLER FreeMdlHandler);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisAdvanceNetBufferListDataStart(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ ULONG DataOffsetDelta,
    _In_ BOOLEAN FreeMdl,
    _In_opt_ NET_BUFFER_FREE_MDL_HANDLER FreeMdlHandler);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisAdjustNetBufferCurrentMdl(
    _In_ PNET_BUFFER NetBuffer);

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG
NTAPI
NdisQueryNetBufferPhysicalCount(
    _In_ PNET_BUFFER NetBuffer);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisCopyFromNetBufferToNetBuffer(
    _In_ PNET_BUFFER Destination,
    _In_ ULONG DestinationOffset,
    _In_ ULONG BytesToCopy,
    _In_ PNET_BUFFER Source,
    _In_ ULONG SourceOffset,
    _Out_ PULONG BytesCopied);

#ifdef __cplusplus
}
#endif
