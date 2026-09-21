/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6.x scatter gather DMA for NET_BUFFERs
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_SG_DMA_DESCRIPTION                          0x83

/* NDIS_SG_DMA_DESCRIPTION::Flags */
#define NDIS_SG_DMA_64_BIT_ADDRESS                                   0x00000001
#if NDIS_SUPPORT_NDIS650
#define NDIS_SG_DMA_V3_HAL_API                                       0x00000002
#endif
#if NDIS_SUPPORT_NDIS685
#define NDIS_SG_DMA_HYBRID_DMA                                       0x00000004
#endif

#define NDIS_SG_DMA_DESCRIPTION_REVISION_1                           1
#if NDIS_SUPPORT_NDIS685
#define NDIS_SG_DMA_DESCRIPTION_REVISION_2                           2
#endif

/* NdisMAllocateNetBufferSGList Flags */
#define NDIS_SG_LIST_WRITE_TO_DEVICE                                 0x000000001

typedef VOID (NTAPI MINIPORT_PROCESS_SG_LIST)(
    _In_ PDEVICE_OBJECT pDO,
    _In_ PVOID Reserved,
    _In_ PSCATTER_GATHER_LIST pSGL,
    _In_ PVOID Context);
typedef MINIPORT_PROCESS_SG_LIST *MINIPORT_PROCESS_SG_LIST_HANDLER;

typedef VOID (NTAPI MINIPORT_ALLOCATE_SHARED_MEM_COMPLETE)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID VirtualAddress,
    _In_ PNDIS_PHYSICAL_ADDRESS PhysicalAddress,
    _In_ ULONG Length,
    _In_ PVOID Context);
typedef MINIPORT_ALLOCATE_SHARED_MEM_COMPLETE *MINIPORT_ALLOCATE_SHARED_MEM_COMPLETE_HANDLER;

typedef struct _NDIS_SG_DMA_DESCRIPTION
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    ULONG MaximumPhysicalMapping;
    MINIPORT_PROCESS_SG_LIST_HANDLER ProcessSGListHandler;
    MINIPORT_ALLOCATE_SHARED_MEM_COMPLETE_HANDLER SharedMemAllocateCompleteHandler;
    ULONG ScatterGatherListSize;
#if NDIS_SUPPORT_NDIS685
    DEVICE_OBJECT *PdoOverride;
#endif
} NDIS_SG_DMA_DESCRIPTION, *PNDIS_SG_DMA_DESCRIPTION;

#define NDIS_SIZEOF_SG_DMA_DESCRIPTION_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_SG_DMA_DESCRIPTION, ScatterGatherListSize)
#if NDIS_SUPPORT_NDIS685
#define NDIS_SIZEOF_SG_DMA_DESCRIPTION_REVISION_2 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_SG_DMA_DESCRIPTION, PdoOverride)
#endif

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisMRegisterScatterGatherDma(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _Inout_ PNDIS_SG_DMA_DESCRIPTION DmaDescription,
    _Out_ PNDIS_HANDLE NdisMiniportDmaHandle);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
NdisMDeregisterScatterGatherDma(
    _In_ NDIS_HANDLE NdisMiniportDmaHandle);

_Must_inspect_result_
_IRQL_requires_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisMAllocateNetBufferSGList(
    _In_ NDIS_HANDLE NdisMiniportDmaHandle,
    _In_ PNET_BUFFER NetBuffer,
    _In_ PVOID Context,
    _In_ ULONG Flags,
    _In_reads_bytes_opt_(ScatterGatherListBufferSize) PVOID ScatterGatherListBuffer,
    _In_ ULONG ScatterGatherListBufferSize);

_IRQL_requires_(DISPATCH_LEVEL)
VOID
NTAPI
NdisMFreeNetBufferSGList(
    _In_ NDIS_HANDLE NdisMiniportDmaHandle,
    _In_ PSCATTER_GATHER_LIST pSGL,
    _In_ PNET_BUFFER NetBuffer);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisMAllocateSharedMemoryAsyncEx(
    _In_ NDIS_HANDLE MiniportDmaHandle,
    _In_ ULONG Length,
    _In_ BOOLEAN Cached,
    _In_ PVOID Context);

#ifdef __cplusplus
}
#endif
