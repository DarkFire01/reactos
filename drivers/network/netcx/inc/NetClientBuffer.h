/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Buffer pool interface between the class extension and its
 *              buffer manager
 *
 * The drop is an older revision than NetAdapterCx.pdb: its data header has a
 * single virtual address and its pool entry points count in SIZE_T and ULONG.
 * Both halves are compiled from the drop, so its shape is the one used here.
 */

#pragma once

#include <NetClientTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OS_ONLY_ALLOCATE is not in the 26100 binary, which has DRIVER_V2 at the
 * same value. The imported sources use the older name, so both are declared.
 */
typedef enum _NET_CLIENT_MEMORY_MANAGEMENT_MODE
{
    NET_CLIENT_MEMORY_MANAGEMENT_MODE_DRIVER = 0,
    NET_CLIENT_MEMORY_MANAGEMENT_MODE_OS_ALLOCATE_AND_ATTACH = 1,
    NET_CLIENT_MEMORY_MANAGEMENT_MODE_OS_ONLY_ALLOCATE = 2,
    NET_CLIENT_MEMORY_MANAGEMENT_MODE_DRIVER_V2 = 2
} NET_CLIENT_MEMORY_MANAGEMENT_MODE;

typedef enum _NET_CLIENT_MEMORY_MAPPING_REQUIREMENT
{
    NET_CLIENT_MEMORY_MAPPING_REQUIREMENT_NONE = 0,
    NET_CLIENT_MEMORY_MAPPING_REQUIREMENT_DMA_MAPPED = 1
} NET_CLIENT_MEMORY_MAPPING_REQUIREMENT;

typedef enum _NET_CLIENT_BUFFER_POOL_FLAGS
{
    NET_CLIENT_BUFFER_POOL_FLAGS_NONE = 0,
    NET_CLIENT_BUFFER_POOL_FLAGS_SERIALIZATION = 1
} NET_CLIENT_BUFFER_POOL_FLAGS;

typedef struct DECLSPEC_ALIGN(8) _NET_DATA_HEADER
{
    UINT64 LogicalAddress;
    PVOID VirtualAddress;
    PMDL Mdl;
} NET_DATA_HEADER;

typedef struct _NET_CLIENT_MEMORY_CONSTRAINTS
{
    NET_CLIENT_MEMORY_MAPPING_REQUIREMENT MappingRequirement;
    SIZE_T AlignmentRequirement;
    struct
    {
        PVOID DmaAdapter;
        PVOID PhysicalDeviceObject;
        PHYSICAL_ADDRESS MaximumPhysicalAddress;
        NET_CLIENT_TRI_STATE CacheEnabled;
        ULONG PreferredNode;
    } Dma;
} NET_CLIENT_MEMORY_CONSTRAINTS;

typedef struct _NET_CLIENT_BUFFER_POOL_CONFIG
{
    NET_CLIENT_MEMORY_CONSTRAINTS *MemoryConstraints;
    SIZE_T BufferCount;
    SIZE_T BufferSize;
    SIZE_T BufferAlignmentOffset;
    SIZE_T BufferAlignment;
    ULONG PreferredNode;
    NET_CLIENT_BUFFER_POOL_FLAGS Flag;
    struct _EPROCESS *Process;
} NET_CLIENT_BUFFER_POOL_CONFIG;

DECLARE_HANDLE(NET_CLIENT_BUFFER_POOL);

typedef
VOID
(*ENUMERATE_CALLBACK)(
    _In_ PVOID Context,
    _In_ SIZE_T Index,
    _In_ UINT64 LogicalAddress,
    _In_ PVOID VirtualAddress);

typedef struct _NET_CLIENT_BUFFER_POOL_DISPATCH
{
    ULONG Size;

    VOID
    (*NetClientDestroyBufferPool)(
        _In_ NET_CLIENT_BUFFER_POOL Pool);

    VOID
    (*NetClientEnumerateBuffers)(
        _In_ NET_CLIENT_BUFFER_POOL Pool,
        _In_ ENUMERATE_CALLBACK Callback,
        _In_ PVOID Context);

    NTSTATUS
    (*NetClientAllocateBuffer)(
        _In_ NET_CLIENT_BUFFER_POOL Pool,
        _Out_ SIZE_T *Index,
        _Out_ NET_DATA_HEADER *NetDataHeader,
        _Out_ SIZE_T *NetDataOffset);

    VOID
    (*NetClientFreeBuffers)(
        _In_ NET_CLIENT_BUFFER_POOL Pool,
        _Inout_updates_(NumBuffers) SIZE_T *Buffers,
        _In_ ULONG NumBuffers);
} NET_CLIENT_BUFFER_POOL_DISPATCH;

#ifdef __cplusplus
}
#endif
