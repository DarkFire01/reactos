/*
 * PROJECT:     ReactOS Qualcomm SMD driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared memory heap, partitions and hardware mutexes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "smd.h"


#define SMEM_ALIGN(Value, To)       (((Value) + (To) - 1) & ~((To) - 1))

/* Value each remote processor writes into a TCSR mutex it owns */
static const ULONG SmemMutexOwner[QCOM_HOST_COUNT] = { 1, 2, 3, 4, 5, 6, 7 };

static
PULONG
SmemMutexRegister(
    _In_ PSMD_DEVICE Device,
    _In_ ULONG Index)
{
    return (PULONG)(Device->MutexBase + Index * Device->MutexStride);
}

VOID
SmemMutexAcquire(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Index)
{
    PULONG Register = SmemMutexRegister(Device, Index);

    SmdRLockAcquire(&Device->MutexLock);

    if (Device->MutexDepth[Index]++ != 0)
        return;

    /* The block grants the lock only to the first writer, others read back the owner's value */
    while (READ_REGISTER_ULONG(Register) != SMEM_MUTEX_APPS_VALUE)
        WRITE_REGISTER_ULONG(Register, SMEM_MUTEX_APPS_VALUE);
}

VOID
SmemMutexRelease(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Index)
{
    PULONG Register = SmemMutexRegister(Device, Index);

    if (READ_REGISTER_ULONG(Register) != SMEM_MUTEX_APPS_VALUE)
        SMD_FATAL(0x10);

    if (--Device->MutexDepth[Index] == 0)
        WRITE_REGISTER_ULONG(Register, 0);

    SmdRLockRelease(&Device->MutexLock);
}

/* A processor that went down can no longer release what it held */
VOID
SmemMutexReleaseHost(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host)
{
    ULONG i;

    if (Host >= QCOM_HOST_COUNT || !Device->MutexBase)
        return;

    for (i = 0; i < SMEM_MUTEX_COUNT; i++)
    {
        if (READ_REGISTER_ULONG(SmemMutexRegister(Device, i)) == SmemMutexOwner[Host])
            WRITE_REGISTER_ULONG(SmemMutexRegister(Device, i), 0);
    }
}

static
PSMEM_HEAP_INFO
SmemHeapInfo(
    _In_ PSMD_DEVICE Device)
{
    return (PSMEM_HEAP_INFO)(Device->SmemBase + SMEM_OFFSET_HEAP_INFO);
}

static
PSMEM_TOC_ENTRY
SmemTocEntry(
    _In_ PSMD_DEVICE Device,
    _In_ ULONG Item)
{
    return (PSMEM_TOC_ENTRY)(Device->SmemBase + SMEM_OFFSET_TOC) + Item;
}

/* Resolves a heap TOC entry, items with region bits in Reserved live in the auxiliary region */
static
PVOID
SmemTocAddress(
    _In_ PSMD_DEVICE Device,
    _In_ PSMEM_TOC_ENTRY Entry)
{
    ULONG Offset = Entry->Offset;
    ULONG Size = Entry->Size;

    if (Entry->Reserved & ~3)
    {
        if (!Device->AuxBase || Offset > Device->AuxSize || Size > Device->AuxSize - Offset)
            return NULL;
        return Device->AuxBase + Offset;
    }

    if (Offset > Device->SmemSize || Size > Device->SmemSize - Offset)
        return NULL;

    return Device->SmemBase + Offset;
}

static
LONG
SmemGlobalLookup(
    _In_ PSMD_DEVICE Device,
    _In_ ULONG Item,
    _Out_ PULONG Size,
    _Out_ PVOID *Address)
{
    PSMEM_TOC_ENTRY Entry;

    *Size = 0;
    *Address = NULL;

    if (Item == 4)
    {
        *Size = 8;
        *Address = Device->SmemBase + SMEM_OFFSET_RESET_DETECT;
        return SMEM_OK;
    }

    if (Item < SMEM_FIRST_DYNAMIC_ITEM || Item >= SMEM_ITEM_COUNT)
        return SMEM_E_INVALID;

    Entry = SmemTocEntry(Device, Item);
    if (!Entry->Allocated)
        return SMEM_E_NOT_FOUND;

    *Address = SmemTocAddress(Device, Entry);
    if (!*Address)
        return SMEM_E_INVALID;

    *Size = Entry->Size;
    return SMEM_OK;
}

static
LONG
SmemGlobalAllocate(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Item,
    _Inout_ PULONG Size,
    _Out_ PVOID *Address)
{
    PSMEM_HEAP_INFO Heap = SmemHeapInfo(Device);
    PSMEM_TOC_ENTRY Entry;
    ULONG AllocSize = SMEM_ALIGN(*Size, 8);
    LONG Result;

    *Address = NULL;

    if (Item < SMEM_FIRST_DYNAMIC_ITEM || Item >= SMEM_ITEM_COUNT || AllocSize == 0)
        return SMEM_E_INVALID;

    Entry = SmemTocEntry(Device, Item);

    SmemMutexAcquire(Device, SMEM_MUTEX_HEAP);

    if (Entry->Allocated)
    {
        /* Someone else created it first, only an exact size match is a hit */
        if (Entry->Size != AllocSize)
        {
            *Size = Entry->Size;
            Result = SMEM_E_SIZE_MISMATCH;
        }
        else
        {
            *Address = SmemTocAddress(Device, Entry);
            Result = *Address ? SMEM_OK : SMEM_E_INVALID;
        }
    }
    else if (Heap->HeapRemaining < AllocSize)
    {
        Result = SMEM_E_NO_SPACE;
    }
    else
    {
        Entry->Offset = Heap->FreeOffset;
        Entry->Size = AllocSize;
        Entry->Reserved = 0;
        KeMemoryBarrier();
        Entry->Allocated = 1;

        Heap->FreeOffset += AllocSize;
        Heap->HeapRemaining -= AllocSize;
        KeMemoryBarrier();

        *Address = Device->SmemBase + Entry->Offset;
        Result = SMEM_OK;
    }

    SmemMutexRelease(Device, SMEM_MUTEX_HEAP);
    return Result;
}

static
ULONG
SmemCachedHeaderSize(
    _In_ PSMEM_PARTITION Partition)
{
    return SMEM_ALIGN((ULONG)sizeof(SMEM_PRIVATE_ENTRY), Partition->CacheLine);
}

static
VOID
SmemCheckEntry(
    _In_ PSMEM_PRIVATE_ENTRY Entry)
{
    if (Entry->Canary != SMEM_PRIVATE_CANARY)
        SMD_FATAL(0x20);
}

/* Walks both item lists of a partition looking for Item */
static
LONG
SmemPartitionLookup(
    _In_ PSMEM_PARTITION Partition,
    _In_ ULONG Item,
    _In_ BOOLEAN Cached,
    _Out_ PULONG Size,
    _Out_ PVOID *Address)
{
    PUCHAR Base = (PUCHAR)Partition->Header;
    PSMEM_PARTITION_HEADER Header = Partition->Header;
    PSMEM_PRIVATE_ENTRY Entry;
    PUCHAR Cursor, End;
    ULONG HeaderSize;

    if (!Cached)
    {
        Cursor = Base + sizeof(*Header);
        End = Base + Header->FreeUncached;
        while (Cursor < End)
        {
            Entry = (PSMEM_PRIVATE_ENTRY)Cursor;
            SmemCheckEntry(Entry);

            if (Entry->Item == Item)
            {
                *Address = Cursor + sizeof(*Entry) + Entry->PaddingHeader;
                *Size = Entry->Size - Entry->PaddingData;
                return SMEM_OK;
            }

            Cursor += sizeof(*Entry) + Entry->PaddingHeader + Entry->Size;
        }
    }
    else
    {
        HeaderSize = SmemCachedHeaderSize(Partition);
        Cursor = Base + Header->Size - HeaderSize;
        End = Base + Header->FreeCached;
        while (Cursor >= End && Cursor > Base)
        {
            Entry = (PSMEM_PRIVATE_ENTRY)Cursor;
            SmemCheckEntry(Entry);

            if (Entry->Item == Item)
            {
                *Address = Cursor - Entry->Size;
                *Size = Entry->Size - Entry->PaddingData;
                return SMEM_OK;
            }

            Cursor -= Entry->Size + HeaderSize;
        }
    }

    return SMEM_E_NOT_FOUND;
}

static
LONG
SmemPartitionAllocate(
    _Inout_ PSMD_DEVICE Device,
    _In_ PSMEM_PARTITION Partition,
    _In_ ULONG Item,
    _Inout_ PULONG Size,
    _In_ BOOLEAN Cached,
    _Out_ PVOID *Address)
{
    PSMEM_PARTITION_HEADER Header = Partition->Header;
    PUCHAR Base = (PUCHAR)Header;
    PSMEM_PRIVATE_ENTRY Entry;
    ULONG AllocSize = SMEM_ALIGN(*Size, 8);
    ULONG Existing, HeaderSize;
    PVOID Found;
    LONG Result;

    *Address = NULL;

    SmemMutexAcquire(Device, SMEM_MUTEX_HEAP);

    if (Header->FreeUncached > Header->FreeCached || Header->FreeCached > Header->Size)
        SMD_FATAL(0x21);

    Result = SmemPartitionLookup(Partition, Item, Cached, &Existing, &Found);
    if (Result == SMEM_OK)
    {
        if (Existing != *Size)
        {
            *Size = Existing;
            Result = SMEM_E_SIZE_MISMATCH;
        }
        else
        {
            *Address = Found;
        }
        goto Exit;
    }

    if (!Cached)
    {
        if (Header->FreeCached - Header->FreeUncached < sizeof(*Entry) + AllocSize)
        {
            Result = SMEM_E_NO_SPACE;
            goto Exit;
        }

        Entry = (PSMEM_PRIVATE_ENTRY)(Base + Header->FreeUncached);
        Entry->Canary = SMEM_PRIVATE_CANARY;
        Entry->Item = (USHORT)Item;
        Entry->Size = AllocSize;
        Entry->PaddingData = (USHORT)(AllocSize - *Size);
        Entry->PaddingHeader = 0;
        Entry->Reserved = 0;
        KeMemoryBarrier();

        Header->FreeUncached += sizeof(*Entry) + AllocSize;
        *Address = (PUCHAR)Entry + sizeof(*Entry);
    }
    else
    {
        HeaderSize = SmemCachedHeaderSize(Partition);
        AllocSize = SMEM_ALIGN(*Size, Partition->CacheLine);
        if (Header->FreeCached - Header->FreeUncached < HeaderSize + AllocSize)
        {
            Result = SMEM_E_NO_SPACE;
            goto Exit;
        }

        Entry = (PSMEM_PRIVATE_ENTRY)(Base + Header->FreeCached - HeaderSize);
        Entry->Canary = SMEM_PRIVATE_CANARY;
        Entry->Item = (USHORT)Item;
        Entry->Size = AllocSize;
        Entry->PaddingData = (USHORT)(AllocSize - *Size);
        Entry->PaddingHeader = 0;
        Entry->Reserved = 0;
        KeMemoryBarrier();

        Header->FreeCached -= HeaderSize + AllocSize;
        *Address = (PUCHAR)Entry - AllocSize;
    }

    KeMemoryBarrier();
    Result = SMEM_OK;

Exit:
    SmemMutexRelease(Device, SMEM_MUTEX_HEAP);
    return Result;
}

/**
 * @brief Finds or creates an SMEM item, in the partition shared with Host when there is one.
 *
 * @param[in,out] Size Requested size, receives the existing size on SMEM_E_SIZE_MISMATCH.
 * @param[in] Flags SMEM_FLAG_CACHED and SMEM_FLAG_NO_GLOBAL.
 */
LONG
SmemAllocate(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host,
    _In_ ULONG Item,
    _Inout_ PULONG Size,
    _In_ ULONG Flags,
    _Out_ PVOID *Address)
{
    *Address = NULL;

    if (!Device->SmemReady)
        return SMEM_E_INVALID;

    if ((Host >= QCOM_HOST_COUNT && Host != QCOM_HOST_GLOBAL) || Item >= SMEM_ITEM_COUNT || *Size == 0)
        return SMEM_E_INVALID;

    if (Host != QCOM_HOST_GLOBAL && Host != QCOM_HOST_APPS && Device->Partitions[Host].Header)
    {
        return SmemPartitionAllocate(Device,
                                     &Device->Partitions[Host],
                                     Item,
                                     Size,
                                     (Flags & SMEM_FLAG_CACHED) != 0,
                                     Address);
    }

    /* Without a partition for this host the item lives in the global heap */
    return SmemGlobalAllocate(Device, Item, Size, Address);
}

LONG
SmemLookup(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host,
    _In_ ULONG Item,
    _Out_ PULONG Size,
    _Out_ PVOID *Address)
{
    LONG Result;

    *Size = 0;
    *Address = NULL;

    if (!Device->SmemReady || Item >= SMEM_ITEM_COUNT)
        return SMEM_E_INVALID;

    if (Host < QCOM_HOST_COUNT && Host != QCOM_HOST_APPS && Device->Partitions[Host].Header)
    {
        SmemMutexAcquire(Device, SMEM_MUTEX_HEAP);
        Result = SmemPartitionLookup(&Device->Partitions[Host], Item, FALSE, Size, Address);
        if (Result == SMEM_E_NOT_FOUND)
            Result = SmemPartitionLookup(&Device->Partitions[Host], Item, TRUE, Size, Address);
        SmemMutexRelease(Device, SMEM_MUTEX_HEAP);

        if (Result != SMEM_E_NOT_FOUND)
            return Result;
    }

    return SmemGlobalLookup(Device, Item, Size, Address);
}

static
VOID
SmemLoadPartitions(
    _Inout_ PSMD_DEVICE Device)
{
    PSMEM_PTABLE Table;
    PSMEM_PTABLE_ENTRY Entry;
    PSMEM_PARTITION_HEADER Header;
    ULONG i, Remote;

    if (Device->SmemSize < 0x2000)
        return;

    Table = (PSMEM_PTABLE)(Device->SmemBase + Device->SmemSize - 0x1000);
    if (Table->Magic != SMEM_PTABLE_MAGIC)
        return;

    if (Table->Version != 1 || Table->NumEntries > SMEM_PTABLE_MAX_ENTRIES)
    {
        QC_LOG("Unsupported partition table v%lu with %lu entries", Table->Version, Table->NumEntries);
        return;
    }

    Entry = (PSMEM_PTABLE_ENTRY)(Table + 1);
    for (i = 0; i < Table->NumEntries; i++, Entry++)
    {
        if (Entry->Size == 0)
            continue;

        if (Entry->Host0 == QCOM_HOST_APPS)
            Remote = Entry->Host1;
        else if (Entry->Host1 == QCOM_HOST_APPS)
            Remote = Entry->Host0;
        else
            continue;

        if (Remote == QCOM_HOST_APPS || Remote >= QCOM_HOST_COUNT)
            continue;

        if ((Entry->Offset & 0xFFF) || Entry->Offset >= Device->SmemSize - 0x1000 ||
            Entry->Size > Device->SmemSize - Entry->Offset)
        {
            SMD_FATAL(0x22);
        }

        Header = (PSMEM_PARTITION_HEADER)(Device->SmemBase + Entry->Offset);
        if (Header->Magic != SMEM_PARTITION_MAGIC || Header->Size != Entry->Size ||
            (Header->Size & 0xFFF) || Header->Size == 0)
        {
            SMD_FATAL(0x23);
        }

        Device->Partitions[Remote].Header = Header;
        Device->Partitions[Remote].Size = Header->Size;
        Device->Partitions[Remote].CacheLine = Entry->CacheLine ? Entry->CacheLine : 32;
    }
}

/* Publishes our version in a version array item and checks every other processor agrees */
static
BOOLEAN
SmemVersionCheck(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Item,
    _In_ ULONG Version,
    _In_ ULONG Mask)
{
    PULONG Versions;
    PVOID Address;
    ULONG Size, i;

    if (Item != SMEM_ITEM_VERSION && (Item < 107 || Item > 131))
        return FALSE;

    if (Item == SMEM_ITEM_VERSION)
    {
        Address = Device->SmemBase + SMEM_OFFSET_VERSION;
        Size = 0x80;
    }
    else if (SmemGlobalLookup(Device, Item, &Size, &Address) != SMEM_OK)
    {
        Size = 0x80;
        if (SmemGlobalAllocate(Device, Item, &Size, &Address) != SMEM_OK)
            return FALSE;
    }

    if (Size < 0x80)
        return FALSE;

    Versions = Address;
    Versions[SMEM_VERSION_APPS_INDEX] |= Version & Mask;
    KeMemoryBarrier();

    for (i = 0; i < 32; i++)
    {
        if ((Versions[i] & Mask) && (Versions[i] & Mask) != (Version & Mask))
            return FALSE;
    }

    return TRUE;
}

NTSTATUS
SmemInitialize(
    _Inout_ PSMD_DEVICE Device)
{
    if (SmemHeapInfo(Device)->Initialized != 1)
    {
        QC_LOG("SMEM heap was not set up by the boot loader (initialized=%lu)",
               SmemHeapInfo(Device)->Initialized);
        return STATUS_DEVICE_NOT_READY;
    }

    Device->SmemReady = TRUE;

    if (!SmemVersionCheck(Device, SMEM_ITEM_VERSION, SMEM_APPS_VERSION, SMEM_VERSION_MASK))
    {
        QC_LOG("SMEM version mismatch");
        Device->SmemReady = FALSE;
        return STATUS_REVISION_MISMATCH;
    }

    SmemLoadPartitions(Device);
    QC_LOG("SMEM ready");
    return STATUS_SUCCESS;
}

PVOID
NTAPI
SmemIfAlloc(
    _In_ ULONG Item,
    _In_ ULONG Size)
{
    PVOID Address;

    if (!SmdGlobal || SmemAllocate(SmdGlobal, QCOM_HOST_GLOBAL, Item, &Size, 0, &Address) != SMEM_OK)
        return NULL;

    return Address;
}

ULONG
NTAPI
SmemIfVersionSet(
    _In_ ULONG Item,
    _In_ ULONG Version,
    _In_ ULONG Mask)
{
    if (!SmdGlobal || !SmdGlobal->SmemReady)
        return FALSE;

    return SmemVersionCheck(SmdGlobal, Item, Version, Mask);
}

PVOID
NTAPI
SmemIfGetAddr(
    _In_ ULONG Item,
    _Out_opt_ PULONG Size)
{
    PVOID Address;
    ULONG ItemSize;

    if (!SmdGlobal || SmemGlobalLookup(SmdGlobal, Item, &ItemSize, &Address) != SMEM_OK)
        return NULL;

    if (Size)
        *Size = ItemSize;
    return Address;
}

PVOID
NTAPI
SmemIfGetIpcRegister(
    _In_ ULONGLONG PhysicalAddress)
{
    ULONG i;

    if (!SmdGlobal)
        return NULL;

    for (i = 0; i < SmdGlobal->IpcRegCount; i++)
    {
        if ((ULONGLONG)SmdGlobal->IpcRegPhys[i].QuadPart == PhysicalAddress)
            return SmdGlobal->IpcRegVa[i];
    }

    return NULL;
}
