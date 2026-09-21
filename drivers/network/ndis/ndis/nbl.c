/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NET_BUFFER and NET_BUFFER_LIST pools and data path
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/*
 * Both structures are part of the driver ABI, so their size is fixed. The x86
 * numbers are what ndis.sys itself reserves per object when a pool is created
 * with and without an inline NET_BUFFER.
 */
#ifdef _X86_
C_ASSERT(sizeof(NET_BUFFER_LIST) == 176);
C_ASSERT(sizeof(NET_BUFFER) == 96);
#endif

/* A 64 bit miniport sees 27 info slots; ndis.sys owns two more behind them. */
#if defined(_AMD64_) || defined(_ARM64_)
C_ASSERT(FIELD_OFFSET(NET_BUFFER_LIST, NetBufferListInfo) == 144);
C_ASSERT(MaxNetBufferListInfo == 29);
C_ASSERT(TcpRecvSegCoalesceInfo == 22);
#endif

C_ASSERT(FIELD_OFFSET(NET_BUFFER_LIST, Context) == sizeof(PVOID) * 2);
C_ASSERT(RTL_FIELD_SIZE(NET_BUFFER_LIST, NetBufferListInfo) ==
         sizeof(PVOID) * MaxNetBufferListInfo);

#define NDIS_TAG_NB_POOL    'PBNn'
#define NDIS_TAG_NBL_POOL   'PLNn'
#define NDIS_TAG_NBL_CTX    'CLNn'

/*
 * One pool serves both NET_BUFFERs and NET_BUFFER_LISTs. BlockSize covers the
 * fixed object plus whatever the pool parameters asked to be carved out behind
 * it, so a single allocation hands back a fully formed object.
 */
typedef struct _NDIS_NBL_POOL
{
    ULONG Signature;
    ULONG PoolTag;
    NDIS_HANDLE OwnerHandle;
    USHORT BlockSize;
    USHORT ContextSize;
    ULONG DataSize;
    ULONG MdlSize;
    UCHAR ProtocolId;
    BOOLEAN AllocateNetBuffer;
    KSPIN_LOCK Lock;
    LIST_ENTRY FreeList;
    LONG OutstandingCount;
} NDIS_NBL_POOL, *PNDIS_NBL_POOL;

#define NB_POOL_SIGNATURE   'lPBN'
#define NBL_POOL_SIGNATURE  'lPLN'

/*
 * Every block starts with this so a freed object can find its way back onto
 * the pool free list without the caller telling us which pool it came from.
 */
typedef struct _NDIS_POOL_BLOCK
{
    LIST_ENTRY Link;
    PNDIS_NBL_POOL Pool;
} NDIS_POOL_BLOCK, *PNDIS_POOL_BLOCK;

#define NDIS_POOL_BLOCK_FROM_OBJECT(Object) \
    ((PNDIS_POOL_BLOCK)((PUCHAR)(Object) - NDIS_POOL_BLOCK_OVERHEAD))

#define NDIS_OBJECT_FROM_POOL_BLOCK(Block) \
    ((PVOID)((PUCHAR)(Block) + NDIS_POOL_BLOCK_OVERHEAD))

#define NDIS_POOL_BLOCK_OVERHEAD \
    ALIGN_UP_BY(sizeof(NDIS_POOL_BLOCK), MEMORY_ALLOCATION_ALIGNMENT)

static
PNDIS_NBL_POOL
NTAPI
NdispCreateNblPool(
    _In_ ULONG Signature,
    _In_ ULONG PoolTag,
    _In_ USHORT BlockSize)
{
    PNDIS_NBL_POOL Pool;

    Pool = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Pool), PoolTag);
    if (Pool == NULL)
        return NULL;

    RtlZeroMemory(Pool, sizeof(*Pool));
    Pool->Signature = Signature;
    Pool->PoolTag = PoolTag;
    Pool->BlockSize = BlockSize;
    KeInitializeSpinLock(&Pool->Lock);
    InitializeListHead(&Pool->FreeList);

    return Pool;
}

static
PVOID
NTAPI
NdispAllocateFromPool(
    _In_ PNDIS_NBL_POOL Pool)
{
    PNDIS_POOL_BLOCK Block;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Pool->Lock, &OldIrql);
    Entry = RemoveHeadList(&Pool->FreeList);
    if (Entry == &Pool->FreeList)
        Entry = NULL;
    KeReleaseSpinLock(&Pool->Lock, OldIrql);

    if (Entry != NULL)
    {
        Block = CONTAINING_RECORD(Entry, NDIS_POOL_BLOCK, Link);
    }
    else
    {
        Block = ExAllocatePoolWithTag(NonPagedPool,
                                      NDIS_POOL_BLOCK_OVERHEAD + Pool->BlockSize,
                                      Pool->PoolTag);
        if (Block == NULL)
            return NULL;

        Block->Pool = Pool;
    }

    RtlZeroMemory(NDIS_OBJECT_FROM_POOL_BLOCK(Block), Pool->BlockSize);
    InterlockedIncrement(&Pool->OutstandingCount);

    return NDIS_OBJECT_FROM_POOL_BLOCK(Block);
}

static
VOID
NTAPI
NdispReturnToPool(
    _In_ PVOID Object)
{
    PNDIS_POOL_BLOCK Block = NDIS_POOL_BLOCK_FROM_OBJECT(Object);
    PNDIS_NBL_POOL Pool = Block->Pool;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Pool->Lock, &OldIrql);
    InsertHeadList(&Pool->FreeList, &Block->Link);
    KeReleaseSpinLock(&Pool->Lock, OldIrql);

    InterlockedDecrement(&Pool->OutstandingCount);
}

static
VOID
NTAPI
NdispDestroyNblPool(
    _In_ PNDIS_NBL_POOL Pool)
{
    PLIST_ENTRY Entry;

    /*
     * Blocks still in the caller's hands are a driver bug. Only what is on the
     * free list can be released here.
     */
    ASSERT(Pool->OutstandingCount == 0);

    while (!IsListEmpty(&Pool->FreeList))
    {
        Entry = RemoveHeadList(&Pool->FreeList);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, NDIS_POOL_BLOCK, Link), Pool->PoolTag);
    }

    ExFreePoolWithTag(Pool, Pool->PoolTag);
}

/*
 * The NET_BUFFER carved out of an NBL block sits directly behind it, and the
 * data area and its MDL sit behind that. Keeping the order fixed lets every
 * accessor here work from the object pointer alone.
 */
static
PNET_BUFFER
NTAPI
NdispNetBufferFromNblBlock(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ PNDIS_NBL_POOL Pool)
{
    SIZE_T Offset = ALIGN_UP_BY(sizeof(NET_BUFFER_LIST), MEMORY_ALLOCATION_ALIGNMENT);

    if (Pool->ContextSize != 0)
        Offset += ALIGN_UP_BY(sizeof(NET_BUFFER_LIST_CONTEXT) + Pool->ContextSize, 4);

    return (PNET_BUFFER)((PUCHAR)NetBufferList + Offset);
}

static
BOOLEAN
NTAPI
NdispValidatePoolHeader(
    _In_ PNDIS_OBJECT_HEADER Header,
    _In_ USHORT RequiredSize)
{
    if (Header->Type != NDIS_OBJECT_TYPE_DEFAULT)
        return FALSE;

    if (Header->Revision == 0)
        return FALSE;

    return Header->Size >= RequiredSize;
}

/*
 * Attach a data area and an MDL describing it to a freshly handed out
 * NET_BUFFER. Used when the pool was created with a non-zero DataSize.
 */
static
BOOLEAN
NTAPI
NdispBuildNetBufferData(
    _In_ PNET_BUFFER NetBuffer,
    _In_ PNDIS_NBL_POOL Pool,
    _In_ PVOID MdlStorage)
{
    PVOID Data = (PUCHAR)MdlStorage + Pool->MdlSize;
    PMDL Mdl = (PMDL)MdlStorage;

    MmInitializeMdl(Mdl, Data, Pool->DataSize);
    MmBuildMdlForNonPagedPool(Mdl);

    NET_BUFFER_FIRST_MDL(NetBuffer) = Mdl;
    NET_BUFFER_CURRENT_MDL(NetBuffer) = Mdl;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_LENGTH(NetBuffer) = Pool->DataSize;

    return TRUE;
}

_Use_decl_annotations_
NDIS_HANDLE
NTAPI
NdisAllocateNetBufferPool(
    NDIS_HANDLE NdisHandle,
    PNET_BUFFER_POOL_PARAMETERS Parameters)
{
    PNDIS_NBL_POOL Pool;
    SIZE_T BlockSize;

    if (!NdispValidatePoolHeader(&Parameters->Header,
                                 NDIS_SIZEOF_NET_BUFFER_POOL_PARAMETERS_REVISION_1))
    {
        return NULL;
    }

    BlockSize = ALIGN_UP_BY(sizeof(NET_BUFFER), MEMORY_ALLOCATION_ALIGNMENT);

    if (Parameters->DataSize != 0)
    {
        BlockSize += ALIGN_UP_BY(MmSizeOfMdl((PVOID)(PAGE_SIZE - 1), Parameters->DataSize), 4);
        BlockSize += Parameters->DataSize;
    }

    if (BlockSize > MAXUSHORT)
        return NULL;

    Pool = NdispCreateNblPool(NB_POOL_SIGNATURE,
                                 Parameters->PoolTag,
                                 (USHORT)BlockSize);
    if (Pool == NULL)
        return NULL;

    Pool->OwnerHandle = NdisHandle;
    Pool->DataSize = Parameters->DataSize;
    if (Parameters->DataSize != 0)
        Pool->MdlSize = ALIGN_UP_BY(MmSizeOfMdl((PVOID)(PAGE_SIZE - 1), Parameters->DataSize), 4);

    return (NDIS_HANDLE)Pool;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFreeNetBufferPool(
    NDIS_HANDLE PoolHandle)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)PoolHandle;

    ASSERT(Pool->Signature == NB_POOL_SIGNATURE);
    NdispDestroyNblPool(Pool);
}

_Use_decl_annotations_
PNET_BUFFER
NTAPI
NdisAllocateNetBuffer(
    NDIS_HANDLE PoolHandle,
    PMDL MdlChain,
    ULONG DataOffset,
    SIZE_T DataLength)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)PoolHandle;
    PNET_BUFFER NetBuffer;

    ASSERT(Pool->Signature == NB_POOL_SIGNATURE);

    NetBuffer = NdispAllocateFromPool(Pool);
    if (NetBuffer == NULL)
        return NULL;

    NetBuffer->NdisPoolHandle = PoolHandle;
    NET_BUFFER_FIRST_MDL(NetBuffer) = MdlChain;
    NET_BUFFER_CURRENT_MDL(NetBuffer) = MdlChain;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_OFFSET(NetBuffer) = DataOffset;
    NetBuffer->stDataLength = DataLength;

    if (MdlChain != NULL && DataOffset != 0)
        NdisAdjustNetBufferCurrentMdl(NetBuffer);

    return NetBuffer;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFreeNetBuffer(
    PNET_BUFFER NetBuffer)
{
    NdispReturnToPool(NetBuffer);
}

_Use_decl_annotations_
PNET_BUFFER
NTAPI
NdisAllocateNetBufferMdlAndData(
    NDIS_HANDLE PoolHandle)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)PoolHandle;
    PNET_BUFFER NetBuffer;
    PVOID MdlStorage;

    ASSERT(Pool->Signature == NB_POOL_SIGNATURE);

    if (Pool->DataSize == 0)
        return NULL;

    NetBuffer = NdispAllocateFromPool(Pool);
    if (NetBuffer == NULL)
        return NULL;

    NetBuffer->NdisPoolHandle = PoolHandle;
    MdlStorage = (PUCHAR)NetBuffer + ALIGN_UP_BY(sizeof(NET_BUFFER), MEMORY_ALLOCATION_ALIGNMENT);
    NdispBuildNetBufferData(NetBuffer, Pool, MdlStorage);

    return NetBuffer;
}

_Use_decl_annotations_
NDIS_HANDLE
NTAPI
NdisAllocateNetBufferListPool(
    NDIS_HANDLE NdisHandle,
    PNET_BUFFER_LIST_POOL_PARAMETERS Parameters)
{
    PNDIS_NBL_POOL Pool;
    SIZE_T BlockSize;
    ULONG MdlSize = 0;

    if (!NdispValidatePoolHeader(&Parameters->Header,
                                 NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1))
    {
        return NULL;
    }

    /* A context has to keep the trailing NET_BUFFER naturally aligned. */
    if ((Parameters->ContextSize & 3) != 0)
        return NULL;

    /* Data can only be carved out when there is a NET_BUFFER to hang it off. */
    if (Parameters->DataSize != 0 && !Parameters->fAllocateNetBuffer)
        return NULL;

    BlockSize = ALIGN_UP_BY(sizeof(NET_BUFFER_LIST), MEMORY_ALLOCATION_ALIGNMENT);

    if (Parameters->ContextSize != 0)
        BlockSize += ALIGN_UP_BY(sizeof(NET_BUFFER_LIST_CONTEXT) + Parameters->ContextSize, 4);

    if (Parameters->fAllocateNetBuffer)
        BlockSize += ALIGN_UP_BY(sizeof(NET_BUFFER), MEMORY_ALLOCATION_ALIGNMENT);

    if (Parameters->DataSize != 0)
    {
        MdlSize = ALIGN_UP_BY(MmSizeOfMdl((PVOID)(PAGE_SIZE - 1), Parameters->DataSize), 4);
        BlockSize += MdlSize + Parameters->DataSize;
    }

    if (BlockSize > MAXUSHORT)
        return NULL;

    Pool = NdispCreateNblPool(NBL_POOL_SIGNATURE,
                                 Parameters->PoolTag,
                                 (USHORT)BlockSize);
    if (Pool == NULL)
        return NULL;

    Pool->OwnerHandle = NdisHandle;
    Pool->ProtocolId = Parameters->ProtocolId;
    Pool->AllocateNetBuffer = Parameters->fAllocateNetBuffer;
    Pool->ContextSize = Parameters->ContextSize;
    Pool->DataSize = Parameters->DataSize;
    Pool->MdlSize = MdlSize;

    return (NDIS_HANDLE)Pool;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFreeNetBufferListPool(
    NDIS_HANDLE PoolHandle)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)PoolHandle;

    ASSERT(Pool->Signature == NBL_POOL_SIGNATURE);
    NdispDestroyNblPool(Pool);
}

/*
 * Lay out the context block the pool reserved. ContextBackFill leaves room in
 * front of the caller's data so a lower layer can claim its own slice later
 * without a second allocation.
 */
static
VOID
NTAPI
NdispInitializeContext(
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ PVOID Storage,
    _In_ USHORT ContextSize,
    _In_ USHORT ContextBackFill)
{
    PNET_BUFFER_LIST_CONTEXT Context = (PNET_BUFFER_LIST_CONTEXT)Storage;

    Context->Next = NetBufferList->Context;
    Context->Size = ContextSize + ContextBackFill;
    Context->Offset = ContextBackFill;
    NetBufferList->Context = Context;
}

_Use_decl_annotations_
PNET_BUFFER_LIST
NTAPI
NdisAllocateNetBufferList(
    NDIS_HANDLE PoolHandle,
    USHORT ContextSize,
    USHORT ContextBackFill)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)PoolHandle;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;

    ASSERT(Pool->Signature == NBL_POOL_SIGNATURE);

    if (ALIGN_UP_BY(ContextSize + ContextBackFill, 4) > Pool->ContextSize)
        return NULL;

    NetBufferList = NdispAllocateFromPool(Pool);
    if (NetBufferList == NULL)
        return NULL;

    NetBufferList->NdisPoolHandle = PoolHandle;

    if (Pool->ContextSize != 0)
    {
        NdispInitializeContext(NetBufferList,
                               (PUCHAR)NetBufferList +
                                   ALIGN_UP_BY(sizeof(NET_BUFFER_LIST), MEMORY_ALLOCATION_ALIGNMENT),
                               ContextSize,
                               ContextBackFill);
    }

    if (Pool->AllocateNetBuffer)
    {
        NetBuffer = NdispNetBufferFromNblBlock(NetBufferList, Pool);
        NetBuffer->NdisPoolHandle = PoolHandle;
        NET_BUFFER_LIST_FIRST_NB(NetBufferList) = NetBuffer;

        if (Pool->DataSize != 0)
        {
            NdispBuildNetBufferData(NetBuffer,
                                    Pool,
                                    (PUCHAR)NetBuffer +
                                        ALIGN_UP_BY(sizeof(NET_BUFFER), MEMORY_ALLOCATION_ALIGNMENT));
        }
    }

    return NetBufferList;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFreeNetBufferList(
    PNET_BUFFER_LIST NetBufferList)
{
    NdispReturnToPool(NetBufferList);
}

_Use_decl_annotations_
PNET_BUFFER_LIST
NTAPI
NdisAllocateNetBufferAndNetBufferList(
    NDIS_HANDLE PoolHandle,
    USHORT ContextSize,
    USHORT ContextBackFill,
    PMDL MdlChain,
    ULONG DataOffset,
    SIZE_T DataLength)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)PoolHandle;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;

    ASSERT(Pool->Signature == NBL_POOL_SIGNATURE);

    if (!Pool->AllocateNetBuffer)
        return NULL;

    NetBufferList = NdisAllocateNetBufferList(PoolHandle, ContextSize, ContextBackFill);
    if (NetBufferList == NULL)
        return NULL;

    NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
    NET_BUFFER_FIRST_MDL(NetBuffer) = MdlChain;
    NET_BUFFER_CURRENT_MDL(NetBuffer) = MdlChain;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_OFFSET(NetBuffer) = DataOffset;
    NetBuffer->stDataLength = DataLength;

    if (MdlChain != NULL && DataOffset != 0)
        NdisAdjustNetBufferCurrentMdl(NetBuffer);

    return NetBufferList;
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisAllocateNetBufferListContext(
    PNET_BUFFER_LIST NetBufferList,
    USHORT ContextSize,
    USHORT ContextBackFill,
    ULONG PoolTag)
{
    PNET_BUFFER_LIST_CONTEXT Context;
    USHORT Total = ContextSize + ContextBackFill;

    /*
     * A context already laid out by the pool can absorb the request if it is
     * big enough, which keeps the common case allocation free.
     */
    if (NetBufferList->Context != NULL &&
        NetBufferList->Context->Offset >= Total)
    {
        NetBufferList->Context->Offset -= Total;
        return NDIS_STATUS_SUCCESS;
    }

    Context = ExAllocatePoolWithTag(NonPagedPool,
                                    sizeof(*Context) + ALIGN_UP_BY(Total, MEMORY_ALLOCATION_ALIGNMENT),
                                    PoolTag);
    if (Context == NULL)
        return NDIS_STATUS_RESOURCES;

    NdispInitializeContext(NetBufferList, Context, ContextSize, ContextBackFill);

    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFreeNetBufferListContext(
    PNET_BUFFER_LIST NetBufferList,
    USHORT ContextSize)
{
    PNET_BUFFER_LIST_CONTEXT Context = NetBufferList->Context;

    if (Context == NULL)
        return;

    Context->Offset += ContextSize;

    if (Context->Offset < Context->Size)
        return;

    /* The whole block is unused again, so hand it back to whoever owns it. */
    NetBufferList->Context = Context->Next;
}

_Use_decl_annotations_
NDIS_HANDLE
NTAPI
NdisGetPoolFromNetBuffer(
    PNET_BUFFER NetBuffer)
{
    return NetBuffer->NdisPoolHandle;
}

_Use_decl_annotations_
NDIS_HANDLE
NTAPI
NdisGetPoolFromNetBufferList(
    PNET_BUFFER_LIST NetBufferList)
{
    return NetBufferList->NdisPoolHandle;
}

_Use_decl_annotations_
UCHAR
NTAPI
NdisGetNetBufferListProtocolId(
    PNET_BUFFER_LIST NetBufferList)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)NetBufferList->NdisPoolHandle;

    return Pool->ProtocolId;
}

_Use_decl_annotations_
VOID
NTAPI
NdisAdjustNetBufferCurrentMdl(
    PNET_BUFFER NetBuffer)
{
    PMDL Mdl = NET_BUFFER_FIRST_MDL(NetBuffer);
    ULONG Offset = NET_BUFFER_DATA_OFFSET(NetBuffer);
    ULONG MdlLength;

    while (Mdl != NULL)
    {
        MdlLength = MmGetMdlByteCount(Mdl);
        if (Offset < MdlLength)
            break;

        Offset -= MdlLength;
        Mdl = Mdl->Next;
    }

    NET_BUFFER_CURRENT_MDL(NetBuffer) = Mdl;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = Offset;
}

_Use_decl_annotations_
PVOID
NTAPI
NdisGetDataBuffer(
    PNET_BUFFER NetBuffer,
    ULONG BytesNeeded,
    PVOID Storage,
    ULONG AlignMultiple,
    ULONG AlignOffset)
{
    PMDL Mdl = NET_BUFFER_CURRENT_MDL(NetBuffer);
    ULONG Offset = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer);
    ULONG Copied = 0;
    PUCHAR Base;
    PUCHAR Out;
    ULONG Available;

    if (BytesNeeded == 0 || BytesNeeded > NET_BUFFER_DATA_LENGTH(NetBuffer))
        return NULL;

    if (Mdl == NULL)
        return NULL;

    Base = MmGetSystemAddressForMdlSafe(Mdl, LowPagePriority);
    if (Base == NULL)
        return NULL;

    /*
     * A run that is already contiguous in the first MDL can be handed back in
     * place, which is the whole point of the call. Alignment still has to hold
     * or the caller would fault on its own header cast.
     */
    Available = MmGetMdlByteCount(Mdl) - Offset;
    if (Available >= BytesNeeded)
    {
        if (AlignMultiple <= 1)
            return Base + Offset;

        if ((((ULONG_PTR)(Base + Offset)) & (AlignMultiple - 1)) == AlignOffset)
            return Base + Offset;
    }

    if (Storage == NULL)
        return NULL;

    Out = Storage;
    while (Copied < BytesNeeded && Mdl != NULL)
    {
        Base = MmGetSystemAddressForMdlSafe(Mdl, LowPagePriority);
        if (Base == NULL)
            return NULL;

        Available = MmGetMdlByteCount(Mdl) - Offset;
        if (Available > BytesNeeded - Copied)
            Available = BytesNeeded - Copied;

        RtlCopyMemory(Out + Copied, Base + Offset, Available);
        Copied += Available;
        Offset = 0;
        Mdl = Mdl->Next;
    }

    if (Copied < BytesNeeded)
        return NULL;

    return Storage;
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisRetreatNetBufferDataStart(
    PNET_BUFFER NetBuffer,
    ULONG DataOffsetDelta,
    ULONG DataBackFill,
    NET_BUFFER_ALLOCATE_MDL_HANDLER AllocateMdlHandler)
{
    PMDL Mdl;

    /* Room already reserved in front of the data needs no new MDL. */
    if (NET_BUFFER_DATA_OFFSET(NetBuffer) >= DataOffsetDelta)
    {
        NET_BUFFER_DATA_OFFSET(NetBuffer) -= DataOffsetDelta;
        NET_BUFFER_DATA_LENGTH(NetBuffer) += DataOffsetDelta;
        NdisAdjustNetBufferCurrentMdl(NetBuffer);
        return NDIS_STATUS_SUCCESS;
    }

    if (AllocateMdlHandler == NULL)
        return NDIS_STATUS_RESOURCES;

    Mdl = AllocateMdlHandler(DataOffsetDelta, DataBackFill);
    if (Mdl == NULL)
        return NDIS_STATUS_RESOURCES;

    Mdl->Next = NET_BUFFER_FIRST_MDL(NetBuffer);
    NET_BUFFER_FIRST_MDL(NetBuffer) = Mdl;
    NET_BUFFER_CURRENT_MDL(NetBuffer) = Mdl;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = MmGetMdlByteCount(Mdl) - DataOffsetDelta;
    NET_BUFFER_DATA_OFFSET(NetBuffer) = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer);
    NET_BUFFER_DATA_LENGTH(NetBuffer) += DataOffsetDelta;

    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
NdisAdvanceNetBufferDataStart(
    PNET_BUFFER NetBuffer,
    ULONG DataOffsetDelta,
    BOOLEAN FreeMdl,
    NET_BUFFER_FREE_MDL_HANDLER FreeMdlHandler)
{
    PMDL Mdl;
    PMDL Next;

    ASSERT(NET_BUFFER_DATA_LENGTH(NetBuffer) >= DataOffsetDelta);

    NET_BUFFER_DATA_OFFSET(NetBuffer) += DataOffsetDelta;
    NET_BUFFER_DATA_LENGTH(NetBuffer) -= DataOffsetDelta;
    NdisAdjustNetBufferCurrentMdl(NetBuffer);

    if (!FreeMdl || FreeMdlHandler == NULL)
        return;

    /* Drop the MDLs the new data start has moved past. */
    Mdl = NET_BUFFER_FIRST_MDL(NetBuffer);
    while (Mdl != NULL && Mdl != NET_BUFFER_CURRENT_MDL(NetBuffer))
    {
        Next = Mdl->Next;
        NET_BUFFER_DATA_OFFSET(NetBuffer) -= MmGetMdlByteCount(Mdl);
        FreeMdlHandler(Mdl);
        Mdl = Next;
    }

    NET_BUFFER_FIRST_MDL(NetBuffer) = Mdl;
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisRetreatNetBufferListDataStart(
    PNET_BUFFER_LIST NetBufferList,
    ULONG DataOffsetDelta,
    ULONG DataBackFill,
    NET_BUFFER_ALLOCATE_MDL_HANDLER AllocateMdlHandler,
    NET_BUFFER_FREE_MDL_HANDLER FreeMdlHandler)
{
    PNET_BUFFER NetBuffer;
    PNET_BUFFER Failed;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;

    for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
         NetBuffer != NULL;
         NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
    {
        Status = NdisRetreatNetBufferDataStart(NetBuffer,
                                               DataOffsetDelta,
                                               DataBackFill,
                                               AllocateMdlHandler);
        if (Status != NDIS_STATUS_SUCCESS)
            break;
    }

    if (Status == NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_SUCCESS;

    /* Roll the ones that did succeed back, so the list stays consistent. */
    Failed = NetBuffer;
    for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
         NetBuffer != Failed;
         NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
    {
        NdisAdvanceNetBufferDataStart(NetBuffer, DataOffsetDelta, TRUE, FreeMdlHandler);
    }

    return Status;
}

_Use_decl_annotations_
VOID
NTAPI
NdisAdvanceNetBufferListDataStart(
    PNET_BUFFER_LIST NetBufferList,
    ULONG DataOffsetDelta,
    BOOLEAN FreeMdl,
    NET_BUFFER_FREE_MDL_HANDLER FreeMdlHandler)
{
    PNET_BUFFER NetBuffer;

    for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
         NetBuffer != NULL;
         NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
    {
        NdisAdvanceNetBufferDataStart(NetBuffer, DataOffsetDelta, FreeMdl, FreeMdlHandler);
    }
}

_Use_decl_annotations_
ULONG
NTAPI
NdisQueryNetBufferPhysicalCount(
    PNET_BUFFER NetBuffer)
{
    PMDL Mdl = NET_BUFFER_CURRENT_MDL(NetBuffer);
    ULONG Offset = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer);
    ULONG Remaining = NET_BUFFER_DATA_LENGTH(NetBuffer);
    ULONG Count = 0;
    ULONG Length;

    while (Mdl != NULL && Remaining != 0)
    {
        Length = MmGetMdlByteCount(Mdl) - Offset;
        if (Length > Remaining)
            Length = Remaining;

        Count += ADDRESS_AND_SIZE_TO_SPAN_PAGES((PUCHAR)MmGetMdlVirtualAddress(Mdl) + Offset,
                                                Length);
        Remaining -= Length;
        Offset = 0;
        Mdl = Mdl->Next;
    }

    return Count;
}

/*
 * Walk a NET_BUFFER's MDL chain to the byte at Offset past its data start.
 */
static
PMDL
NTAPI
NdispSeekNetBuffer(
    _In_ PNET_BUFFER NetBuffer,
    _In_ ULONG Offset,
    _Out_ PULONG MdlOffset)
{
    PMDL Mdl = NET_BUFFER_CURRENT_MDL(NetBuffer);
    ULONG Skip = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) + Offset;
    ULONG Length;

    while (Mdl != NULL)
    {
        Length = MmGetMdlByteCount(Mdl);
        if (Skip < Length)
            break;

        Skip -= Length;
        Mdl = Mdl->Next;
    }

    *MdlOffset = Skip;

    return Mdl;
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisCopyFromNetBufferToNetBuffer(
    PNET_BUFFER Destination,
    ULONG DestinationOffset,
    ULONG BytesToCopy,
    PNET_BUFFER Source,
    ULONG SourceOffset,
    PULONG BytesCopied)
{
    PMDL SourceMdl;
    PMDL DestMdl;
    ULONG SourceMdlOffset;
    ULONG DestMdlOffset;
    PUCHAR SourceBase;
    PUCHAR DestBase;
    ULONG Chunk;
    ULONG Done = 0;

    *BytesCopied = 0;

    if (BytesToCopy == 0)
        return NDIS_STATUS_SUCCESS;

    if (SourceOffset + BytesToCopy > NET_BUFFER_DATA_LENGTH(Source))
        return NDIS_STATUS_INVALID_LENGTH;

    if (DestinationOffset + BytesToCopy > NET_BUFFER_DATA_LENGTH(Destination))
        return NDIS_STATUS_INVALID_LENGTH;

    SourceMdl = NdispSeekNetBuffer(Source, SourceOffset, &SourceMdlOffset);
    DestMdl = NdispSeekNetBuffer(Destination, DestinationOffset, &DestMdlOffset);

    while (Done < BytesToCopy)
    {
        if (SourceMdl == NULL || DestMdl == NULL)
            return NDIS_STATUS_FAILURE;

        SourceBase = MmGetSystemAddressForMdlSafe(SourceMdl, LowPagePriority);
        DestBase = MmGetSystemAddressForMdlSafe(DestMdl, LowPagePriority);
        if (SourceBase == NULL || DestBase == NULL)
            return NDIS_STATUS_RESOURCES;

        Chunk = min(MmGetMdlByteCount(SourceMdl) - SourceMdlOffset,
                    MmGetMdlByteCount(DestMdl) - DestMdlOffset);
        if (Chunk > BytesToCopy - Done)
            Chunk = BytesToCopy - Done;

        RtlCopyMemory(DestBase + DestMdlOffset, SourceBase + SourceMdlOffset, Chunk);

        Done += Chunk;
        SourceMdlOffset += Chunk;
        DestMdlOffset += Chunk;

        if (SourceMdlOffset == MmGetMdlByteCount(SourceMdl))
        {
            SourceMdl = SourceMdl->Next;
            SourceMdlOffset = 0;
        }

        if (DestMdlOffset == MmGetMdlByteCount(DestMdl))
        {
            DestMdl = DestMdl->Next;
            DestMdlOffset = 0;
        }
    }

    *BytesCopied = Done;

    return NDIS_STATUS_SUCCESS;
}

/*
 * Release a chain of MDLs built here with IoAllocateMdl.
 */
static
VOID
NTAPI
NdispFreeMdlChain(
    _In_opt_ PMDL Mdl)
{
    PMDL Next;

    while (Mdl != NULL)
    {
        Next = Mdl->Next;
        IoFreeMdl(Mdl);
        Mdl = Next;
    }
}

/*
 * Describe Length bytes of the source chain, starting Offset bytes into it,
 * with partial MDLs of our own. The pages stay shared, the descriptors do not.
 */
static
PMDL
NTAPI
NdispCloneMdlChain(
    _In_opt_ PMDL SourceMdl,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PMDL Head = NULL;
    PMDL Tail = NULL;
    PMDL Mdl;
    PVOID Address;
    ULONG Chunk;

    while (SourceMdl != NULL && Offset >= MmGetMdlByteCount(SourceMdl))
    {
        Offset -= MmGetMdlByteCount(SourceMdl);
        SourceMdl = SourceMdl->Next;
    }

    while (SourceMdl != NULL && Length != 0)
    {
        Chunk = MmGetMdlByteCount(SourceMdl) - Offset;
        if (Chunk > Length)
            Chunk = Length;

        Address = (PUCHAR)MmGetMdlVirtualAddress(SourceMdl) + Offset;

        Mdl = IoAllocateMdl(Address, Chunk, FALSE, FALSE, NULL);
        if (Mdl == NULL)
        {
            NdispFreeMdlChain(Head);
            return NULL;
        }

        IoBuildPartialMdl(SourceMdl, Mdl, Address, Chunk);
        Mdl->Next = NULL;

        if (Tail == NULL)
            Head = Mdl;
        else
            Tail->Next = Mdl;

        Tail = Mdl;
        Length -= Chunk;
        Offset = 0;
        SourceMdl = SourceMdl->Next;
    }

    return Head;
}

_Use_decl_annotations_
PNET_BUFFER_LIST
NTAPI
NdisAllocateCloneNetBufferList(
    PNET_BUFFER_LIST OriginalNetBufferList,
    NDIS_HANDLE NetBufferListPoolHandle,
    NDIS_HANDLE NetBufferPoolHandle,
    ULONG AllocateCloneFlags)
{
    PNDIS_NBL_POOL Pool;
    PNET_BUFFER_LIST Clone;
    PNET_BUFFER SourceNb;
    PNET_BUFFER CloneNb;
    PNET_BUFFER *Link;
    PMDL MdlChain;

    /* The internal pools ndis.sys falls back on for a null handle are not here. */
    if (NetBufferListPoolHandle == NULL || NetBufferPoolHandle == NULL)
        return NULL;

    Pool = (PNDIS_NBL_POOL)NetBufferListPoolHandle;
    ASSERT(Pool->Signature == NBL_POOL_SIGNATURE);

    Clone = NdisAllocateNetBufferList(NetBufferListPoolHandle, 0, 0);
    if (Clone == NULL)
        return NULL;

    /*
     * A pool carrying an inline NET_BUFFER has already handed one over, so the
     * first source NET_BUFFER reuses it and only the rest are allocated.
     */
    CloneNb = NET_BUFFER_LIST_FIRST_NB(Clone);
    if (CloneNb == NULL)
    {
        CloneNb = NdisAllocateNetBuffer(NetBufferPoolHandle, NULL, 0, 0);
        if (CloneNb == NULL)
        {
            NdisFreeCloneNetBufferList(Clone, AllocateCloneFlags);
            return NULL;
        }

        NET_BUFFER_LIST_FIRST_NB(Clone) = CloneNb;
    }

    Link = &NET_BUFFER_LIST_FIRST_NB(Clone);

    for (SourceNb = NET_BUFFER_LIST_FIRST_NB(OriginalNetBufferList);
         SourceNb != NULL;
         SourceNb = NET_BUFFER_NEXT_NB(SourceNb))
    {
        if (CloneNb == NULL)
        {
            CloneNb = NdisAllocateNetBuffer(NetBufferPoolHandle, NULL, 0, 0);
            if (CloneNb == NULL)
            {
                NdisFreeCloneNetBufferList(Clone, AllocateCloneFlags);
                return NULL;
            }

            *Link = CloneNb;
        }

        if ((AllocateCloneFlags & NDIS_CLONE_FLAGS_USE_ORIGINAL_MDLS) != 0)
        {
            NET_BUFFER_FIRST_MDL(CloneNb) = NET_BUFFER_FIRST_MDL(SourceNb);
            NET_BUFFER_CURRENT_MDL(CloneNb) = NET_BUFFER_CURRENT_MDL(SourceNb);
            NET_BUFFER_CURRENT_MDL_OFFSET(CloneNb) = NET_BUFFER_CURRENT_MDL_OFFSET(SourceNb);
            NET_BUFFER_DATA_OFFSET(CloneNb) = NET_BUFFER_DATA_OFFSET(SourceNb);
        }
        else
        {
            MdlChain = NdispCloneMdlChain(NET_BUFFER_FIRST_MDL(SourceNb),
                                          NET_BUFFER_DATA_OFFSET(SourceNb),
                                          NET_BUFFER_DATA_LENGTH(SourceNb));
            if (MdlChain == NULL && NET_BUFFER_DATA_LENGTH(SourceNb) != 0)
            {
                NdisFreeCloneNetBufferList(Clone, AllocateCloneFlags);
                return NULL;
            }

            /* The partial MDLs start at the data, so the offset resets. */
            NET_BUFFER_FIRST_MDL(CloneNb) = MdlChain;
            NET_BUFFER_CURRENT_MDL(CloneNb) = MdlChain;
            NET_BUFFER_CURRENT_MDL_OFFSET(CloneNb) = 0;
            NET_BUFFER_DATA_OFFSET(CloneNb) = 0;
        }

        CloneNb->stDataLength = SourceNb->stDataLength;

        Link = &NET_BUFFER_NEXT_NB(CloneNb);
        CloneNb = NULL;
    }

    Clone->SourceHandle = OriginalNetBufferList->SourceHandle;
    NET_BUFFER_LIST_INFO(Clone, NblOriginalInterfaceIfIndex) =
        NET_BUFFER_LIST_INFO(OriginalNetBufferList, NblOriginalInterfaceIfIndex);

    return Clone;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFreeCloneNetBufferList(
    PNET_BUFFER_LIST CloneNetBufferList,
    ULONG FreeCloneFlags)
{
    PNDIS_NBL_POOL Pool = (PNDIS_NBL_POOL)CloneNetBufferList->NdisPoolHandle;
    PNET_BUFFER FirstNetBuffer;
    PNET_BUFFER NetBuffer;
    PNET_BUFFER Next;
    BOOLEAN OwnMdls;

    ASSERT(Pool->Signature == NBL_POOL_SIGNATURE);

    /* A clone over the original's MDLs never owned them. */
    OwnMdls = (FreeCloneFlags & NDIS_CLONE_FLAGS_USE_ORIGINAL_MDLS) == 0;

    NET_BUFFER_LIST_NEXT_NBL(CloneNetBufferList) = NULL;

    FirstNetBuffer = NET_BUFFER_LIST_FIRST_NB(CloneNetBufferList);

    for (NetBuffer = FirstNetBuffer; NetBuffer != NULL; NetBuffer = Next)
    {
        Next = NET_BUFFER_NEXT_NB(NetBuffer);

        if (OwnMdls)
            NdispFreeMdlChain(NET_BUFFER_FIRST_MDL(NetBuffer));

        /*
         * An inline NET_BUFFER is part of the NBL block and goes back with it
         * rather than on its own.
         */
        if (NetBuffer != FirstNetBuffer || !Pool->AllocateNetBuffer)
            NdisFreeNetBuffer(NetBuffer);
    }

    NdisFreeNetBufferList(CloneNetBufferList);
}

/*
 * The info slots that survive a copy, per direction. The cancel id and the
 * frame type slot are handled outside the tables because neither is a
 * straight copy.
 */
static const UCHAR NdispReceiveInfoSlots[] =
{
    TcpIpChecksumNetBufferListInfo,
    IPsecOffloadV1NetBufferListInfo,
    TcpLargeSendNetBufferListInfo,
    Ieee8021QNetBufferListInfo,
    MediaSpecificInformation,
    NetBufferListFrameType,
    NetBufferListHashValue,
    NetBufferListHashInfo,
    IPsecOffloadV2TunnelNetBufferListInfo,
    IPsecOffloadV2HeaderNetBufferListInfo,
    NetBufferListFilteringInfo,
    NblOriginalInterfaceIfIndex,
    TcpRecvSegCoalesceInfo,
    RscTcpTimestampDelta
};

static const UCHAR NdispSendInfoSlots[] =
{
    TcpIpChecksumNetBufferListInfo,
    IPsecOffloadV1NetBufferListInfo,
    TcpLargeSendNetBufferListInfo,
    ClassificationHandleNetBufferListInfo,
    Ieee8021QNetBufferListInfo,
    NetBufferListCancelId,
    MediaSpecificInformation,
    NetBufferListHashValue,
    IPsecOffloadV2TunnelNetBufferListInfo,
    IPsecOffloadV2HeaderNetBufferListInfo,
    NetBufferListFilteringInfo,
    TcpSendOffloadsSupplementalNetBufferListInfo
};

_Use_decl_annotations_
VOID
NTAPI
NdisCopyReceiveNetBufferListInfo(
    PNET_BUFFER_LIST DestNetBufferList,
    PNET_BUFFER_LIST SrcNetBufferList)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(NdispReceiveInfoSlots); Index++)
    {
        NET_BUFFER_LIST_INFO(DestNetBufferList, NdispReceiveInfoSlots[Index]) =
            NET_BUFFER_LIST_INFO(SrcNetBufferList, NdispReceiveInfoSlots[Index]);
    }

    /* A cancel id only means anything when the NBL came from one source. */
    if ((NET_BUFFER_LIST_NBL_FLAGS(SrcNetBufferList) & NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET) != 0)
    {
        NET_BUFFER_LIST_NBL_FLAGS(DestNetBufferList) |= NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET;
        NET_BUFFER_LIST_INFO(DestNetBufferList, NetBufferListCancelId) =
            NET_BUFFER_LIST_INFO(SrcNetBufferList, NetBufferListCancelId);
    }
}

_Use_decl_annotations_
VOID
NTAPI
NdisCopySendNetBufferListInfo(
    PNET_BUFFER_LIST DestNetBufferList,
    PNET_BUFFER_LIST SrcNetBufferList)
{
    ULONG_PTR FrameType;
    UCHAR ProtocolId;
    ULONG Index;

    ProtocolId = NdisGetNetBufferListProtocolId(SrcNetBufferList);

    for (Index = 0; Index < RTL_NUMBER_OF(NdispSendInfoSlots); Index++)
    {
        NET_BUFFER_LIST_INFO(DestNetBufferList, NdispSendInfoSlots[Index]) =
            NET_BUFFER_LIST_INFO(SrcNetBufferList, NdispSendInfoSlots[Index]);
    }

    /* The frame type slot takes the source's protocol id, not a copy. */
    FrameType = (ULONG_PTR)NET_BUFFER_LIST_INFO(DestNetBufferList, NetBufferListFrameType);
    FrameType = (FrameType & ~(ULONG_PTR)0xFF) | ProtocolId;
    NET_BUFFER_LIST_INFO(DestNetBufferList, NetBufferListFrameType) = (PVOID)FrameType;
}
