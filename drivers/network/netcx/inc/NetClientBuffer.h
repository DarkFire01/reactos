/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Buffer pool interface between the class extension and its
 *              buffer manager
 *
 * Layouts recovered from NetAdapterCx.pdb, see Reference/NetCx.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_CLIENT_TRI_STATE
{
    NET_CLIENT_TRI_STATE_FALSE = 0,
    NET_CLIENT_TRI_STATE_TRUE = 1,
    NET_CLIENT_TRI_STATE_DEFAULT = 2
} NET_CLIENT_TRI_STATE;

/*
 * OS_ONLY_ALLOCATE is not in the 26100 binary, which has DRIVER_V2 at the
 * same value. The imported sources are a different revision and use the older
 * name, so both are declared.
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
    PVOID KmVirtualAddress;
    PVOID UmVirtualAddress;
    MDL *Mdl;
    UINT32 Index;
} NET_DATA_HEADER;

typedef struct _NET_CLIENT_MEMORY_CONSTRAINTS
{
    NET_CLIENT_MEMORY_MAPPING_REQUIREMENT MappingRequirement;
    UINT64 AlignmentRequirement;
    struct
    {
        PVOID DmaAdapter;
        PVOID PhysicalDeviceObject;
        LARGE_INTEGER MaximumPhysicalAddress;
        NET_CLIENT_TRI_STATE CacheEnabled;
        UINT32 PreferredNode;
    } Dma;
} NET_CLIENT_MEMORY_CONSTRAINTS;

typedef struct _NET_CLIENT_BUFFER_POOL_CONFIG
{
    NET_CLIENT_MEMORY_CONSTRAINTS *MemoryConstraints;
    UINT64 BufferCount;
    UINT64 BufferSize;
    UINT64 BufferAlignmentOffset;
    UINT64 BufferAlignment;
    UINT32 PreferredNode;
    NET_CLIENT_BUFFER_POOL_FLAGS Flag;
    struct _EPROCESS *Process;
} NET_CLIENT_BUFFER_POOL_CONFIG;

DECLARE_HANDLE(NET_CLIENT_BUFFER_POOL);

typedef VOID (*NET_CLIENT_ENUMERATE_BUFFERS_CALLBACK)(
    _In_ PVOID Context,
    _In_ NET_DATA_HEADER *const DataHeader);

typedef struct _NET_CLIENT_BUFFER_POOL_DISPATCH
{
    UINT32 Size;

    VOID (*NetClientDestroyBufferPool)(
        _In_ NET_CLIENT_BUFFER_POOL BufferPool);

    VOID (*NetClientEnumerateBuffers)(
        _In_ NET_CLIENT_BUFFER_POOL BufferPool,
        _In_ NET_CLIENT_ENUMERATE_BUFFERS_CALLBACK Callback,
        _In_ PVOID Context);

    NTSTATUS (*NetClientAllocateBuffer)(
        _In_ NET_CLIENT_BUFFER_POOL BufferPool,
        _Out_ UINT64 *BufferIndex,
        _Out_ NET_DATA_HEADER *DataHeader,
        _Inout_ UINT64 *Count);

    VOID (*NetClientFreeBuffers)(
        _In_ NET_CLIENT_BUFFER_POOL BufferPool,
        _In_ UINT64 *BufferIndex,
        _In_ UINT32 Count);
} NET_CLIENT_BUFFER_POOL_DISPATCH;

#ifdef __cplusplus
}
#endif
