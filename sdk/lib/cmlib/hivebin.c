/*
 * PROJECT:   Registry manipulation library
 * LICENSE:   GPL - See COPYING in the top level directory
 * COPYRIGHT: Copyright 2005 Filip Navara <navaraf@reactos.org>
 *            Copyright 2005 Hartmut Birr
 *            Copyright 2001 - 2005 Eric Kohl
 */

#include "cmlib.h"

/* Smallest block list, a list holds a power of two entries from here up */
#define HV_BLOCK_LIST_MIN_ENTRIES   16

/*
 * Sits in front of a block list. Cells are looked up without the hive lock,
 * so a list that is replaced stays allocated, linked here, until the hive goes.
 */
typedef struct _HV_BLOCK_LIST_HEADER
{
    PHMAP_ENTRY Retired;
    ULONG_PTR Reserved;
} HV_BLOCK_LIST_HEADER, *PHV_BLOCK_LIST_HEADER;

static
ULONG
HvpBlockListCapacity(
    _In_ ULONG Length)
{
    ULONG Capacity = HV_BLOCK_LIST_MIN_ENTRIES;

    while (Capacity < Length)
        Capacity <<= 1;

    return Capacity;
}

/**
 * @brief
 * Allocates a block list with room for at least Length blocks.
 *
 * @param[in] Hive
 * The hive that owns the list.
 *
 * @param[in] Length
 * The number of blocks the list must hold.
 *
 * @param[in] Paged
 * TRUE for paged pool.
 *
 * @param[in] Retired
 * The list this one replaces, freed along with it, or NULL.
 *
 * @return
 * The list, or NULL when out of memory.
 */
PHMAP_ENTRY
CMAPI
HvpAllocateBlockList(
    _In_ PHHIVE Hive,
    _In_ ULONG Length,
    _In_ BOOLEAN Paged,
    _In_opt_ PHMAP_ENTRY Retired)
{
    PHV_BLOCK_LIST_HEADER Header;

    Header = Hive->Allocate(sizeof(*Header) + HvpBlockListCapacity(Length) * sizeof(HMAP_ENTRY),
                            Paged,
                            TAG_CM);
    if (!Header)
        return NULL;

    Header->Retired = Retired;
    Header->Reserved = 0;
    return (PHMAP_ENTRY)(Header + 1);
}

/**
 * @brief
 * Frees a block list and every list it replaced.
 *
 * @param[in] Hive
 * The hive that owns the list.
 *
 * @param[in] BlockList
 * The list, or NULL.
 */
VOID
CMAPI
HvpFreeBlockList(
    _In_ PHHIVE Hive,
    _In_opt_ PHMAP_ENTRY BlockList)
{
    PHV_BLOCK_LIST_HEADER Header;

    while (BlockList)
    {
        Header = (PHV_BLOCK_LIST_HEADER)BlockList - 1;
        BlockList = Header->Retired;
        Hive->Free(Header, 0);
    }
}

PHBIN CMAPI
HvpAddBin(
    PHHIVE RegistryHive,
    ULONG Size,
    HSTORAGE_TYPE Storage)
{
    PHMAP_ENTRY BlockList;
    PHBIN Bin;
    ULONG BinSize;
    ULONG i;
    ULONG BitmapSize;
    ULONG BlockCount;
    ULONG OldBlockListSize;
    PHCELL Block;

    BinSize = ROUND_UP(Size + sizeof(HBIN), HBLOCK_SIZE);
    BlockCount = BinSize / HBLOCK_SIZE;

    Bin = RegistryHive->Allocate(BinSize, TRUE, TAG_CM);
    if (Bin == NULL)
        return NULL;
    RtlZeroMemory(Bin, BinSize);

    Bin->Signature = HV_HBIN_SIGNATURE;
    Bin->FileOffset = RegistryHive->Storage[Storage].Length *
                      HBLOCK_SIZE;
    Bin->Size = BinSize;

    /* Only move the block list when it has no room left */
    OldBlockListSize = RegistryHive->Storage[Storage].Length;
    BlockList = RegistryHive->Storage[Storage].BlockList;
    if ((BlockList == NULL) ||
        (HvpBlockListCapacity(OldBlockListSize + BlockCount) > HvpBlockListCapacity(OldBlockListSize)))
    {
        BlockList = HvpAllocateBlockList(RegistryHive,
                                         OldBlockListSize + BlockCount,
                                         TRUE,
                                         RegistryHive->Storage[Storage].BlockList);
        if (BlockList == NULL)
        {
            RegistryHive->Free(Bin, 0);
            return NULL;
        }

        if (OldBlockListSize > 0)
        {
            RtlCopyMemory(BlockList, RegistryHive->Storage[Storage].BlockList,
                          OldBlockListSize * sizeof(HMAP_ENTRY));
        }
    }

    for (i = 0; i < BlockCount; i++)
    {
        BlockList[OldBlockListSize + i].BlockAddress = ((ULONG_PTR)Bin + (i * HBLOCK_SIZE));
        BlockList[OldBlockListSize + i].BinAddress = (ULONG_PTR)Bin;
    }

    RegistryHive->Storage[Storage].BlockList = BlockList;
    RegistryHive->Storage[Storage].Length += BlockCount;

    /* Initialize a free block in this heap. */
    Block = (PHCELL)(Bin + 1);
    Block->Size = (LONG)(BinSize - sizeof(HBIN));

    if (Storage == Stable)
    {
        /* Calculate bitmap size in bytes (always a multiple of 32 bits). */
        BitmapSize = ROUND_UP(RegistryHive->Storage[Stable].Length,
                              sizeof(ULONG) * 8) / 8;

        /* Grow bitmap if necessary. */
        if (BitmapSize > RegistryHive->DirtyVector.SizeOfBitMap / 8)
        {
            PULONG BitmapBuffer;

            BitmapBuffer = RegistryHive->Allocate(BitmapSize, TRUE, TAG_CM);
            RtlZeroMemory(BitmapBuffer, BitmapSize);
            if (RegistryHive->DirtyVector.SizeOfBitMap > 0)
            {
                ASSERT(RegistryHive->DirtyVector.Buffer);
                RtlCopyMemory(BitmapBuffer,
                              RegistryHive->DirtyVector.Buffer,
                              RegistryHive->DirtyVector.SizeOfBitMap / 8);
                RegistryHive->Free(RegistryHive->DirtyVector.Buffer, 0);
            }
            RtlInitializeBitMap(&RegistryHive->DirtyVector, BitmapBuffer,
                                BitmapSize * 8);
        }

        /* Mark new bin dirty. */
        RtlSetBits(&RegistryHive->DirtyVector,
                   Bin->FileOffset / HBLOCK_SIZE,
                   BlockCount);

        /* Update size in the base block */
        RegistryHive->BaseBlock->Length += BinSize;
    }

    return Bin;
}
