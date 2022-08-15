/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Configuration Manager Library - Registry Validation
 * COPYRIGHT:       Copyright 2022 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "cmlib.h"
#define NDEBUG
#include <debug.h>

/* STRUCTURES ****************************************************************/

typedef struct _CMP_REGISTRY_STACK_WORK_STATE
{
    ULONG ChildCellIndex;
    HCELL_INDEX Parent;
    HCELL_INDEX Current;
    HCELL_INDEX Sibling;
} CMP_REGISTRY_STACK_WORK_STATE, *PCMP_REGISTRY_STACK_WORK_STATE;

/* DEFINES  ******************************************************************/

#define GET_HHIVE(CmHive) (&((CmHive)->Hive))
#define GET_HHIVE_ROOT_CELL(Hive) ((Hive)->BaseBlock->RootCell)
#define GET_HHIVE_BIN(Hive, StorageIndex, BlockIndex) ((PHBIN)Hive->Storage[StorageIndex].BlockList[BlockIndex].BinAddress)
#define GET_CELL_BIN(Bin) ((PHCELL)((PUCHAR)Bin + sizeof(HBIN)))

#define IS_CELL_VOLATILE(Cell) (HvGetCellType(Cell) == Volatile)

#if !defined(CMLIB_HOST) && !defined(_BLDR_)
extern PCMHIVE CmiVolatileHive;
#endif

#define CMP_PRIOR_STACK 1
#define CMP_REGISTRY_MAX_LEVELS_TREE_DEPTH 512

#define CMP_KEY_SIZE_THRESHOLD 0x45C
#define CMP_VOLATILE_LIST_UNINTIALIZED 0xBAADF00D

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Validates the lexicographical order between a child
 * and prior sibling of the said child.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] Child
 * A child subkey cell used for lexicographical order
 * validation checks.
 *
 * @param[in] Sibling
 * A subkey cell which is the prior sibling of the child.
 * This is used in conjuction with the child to perfrom
 * lexical order checks.
 *
 * @return
 * Returns TRUE if the order is legal, FALSE otherwise.
 */
BOOLEAN
CmpValidateLexicalOrder(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX Child,
    _In_ HCELL_INDEX Sibling)
{
    LONG Result;
    UNICODE_STRING ChildString, SiblingString;
    PCM_KEY_NODE ChildNode, SiblingNode;

    PAGED_CODE();

    /* Obtain the child node */
    ChildNode = (PCM_KEY_NODE)HvGetCell(Hive, Child);
    if (!ChildNode)
    {
        /* Couldn't get the child node, bail out */
        DPRINT1("CmpValidateLexicalOrder(): Failed to get the child node!\n");
        return FALSE;
    }

    /* Obtain the sibling node */
    SiblingNode = (PCM_KEY_NODE)HvGetCell(Hive, Sibling);
    if (!SiblingNode)
    {
        /* Couldn't get the sibling node, bail out */
        DPRINT1("CmpValidateLexicalOrder(): Failed to get the sibling node!\n");
        return FALSE;
    }

    /* CASE 1: Two distinct non-compressed Unicode names */
    if ((ChildNode->Flags & KEY_COMP_NAME) == 0 &&
        (SiblingNode->Flags & KEY_COMP_NAME) == 0)
    {
        Result = CmpCompareDistinctNames(&(SiblingNode->Name[0]),
                                         SiblingNode->NameLength,
                                         &(ChildNode->Name[0]),
                                         ChildNode->NameLength);
        if (Result >= 0)
        {
            DPRINT1("CmpValidateLexicalOrder(): The sibling node name is greater or equal to that of the child!\n");
            return FALSE;
        }
    }

    /* CASE 2: Both compressed Unicode names */
    if ((ChildNode->Flags & KEY_COMP_NAME) &&
        (SiblingNode->Flags & KEY_COMP_NAME))
    {
        Result = CmpCompareBothCompressedNames(SiblingNode->Name,
                                               SiblingNode->NameLength,
                                               ChildNode->Name,
                                               ChildNode->NameLength);
        if (Result >= 0)
        {
            DPRINT1("CmpValidateLexicalOrder(): The compressed sibling node name is greater or equal to that of the compressed child!\n");
            return FALSE;
        }
    }

    /* CASE 3: The child name is compressed but the sibling is not */
    if ((ChildNode->Flags & KEY_COMP_NAME) &&
        (SiblingNode->Flags & KEY_COMP_NAME) == 0)
    {
        SiblingString.Buffer = &(SiblingNode->Name[0]);
        SiblingString.Length = SiblingNode->NameLength;
        SiblingString.MaximumLength = SiblingNode->NameLength;
        Result = CmpCompareCompressedName(&SiblingString,
                                          ChildNode->Name,
                                          ChildNode->NameLength);
        if (Result >= 0)
        {
            DPRINT1("CmpValidateLexicalOrder(): The sibling node name is greater or equal to that of the compressed child!\n");
            return FALSE;
        }
    }

    /* CASE 4: The sibling name is compressed but the child is not */
    if ((SiblingNode->Flags & KEY_COMP_NAME) &&
        (ChildNode->Flags & KEY_COMP_NAME) == 0)
    {
        ChildString.Buffer = &(ChildNode->Name[0]);
        ChildString.Length = ChildNode->NameLength;
        ChildString.MaximumLength = ChildNode->NameLength;
        Result = CmpCompareCompressedName(&ChildString,
                                          SiblingNode->Name,
                                          SiblingNode->NameLength);
        if (Result <= 0)
        {
            DPRINT1("CmpValidateLexicalOrder(): The compressed sibling node name is lesser or equal to that of the child!\n");
            return FALSE;
        }
    }

    /*
     * One of the cases above has met the conditions
     * successfully, the lexicographical order is legal.
     */
    return TRUE;
}

/**
 * @brief
 * Validates the class of a given key cell.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current key cell that the class points to.
 *
 * @param[in] CellData
 * A pointer to cell data of the current key cell
 * that contains the class to be validated.
 *
 * @return
 * Returns CM_CHECK_REGISTRY_GOOD if the class is in good shape.
 * The same CM status code is returned if the class doesn't
 * but the class length says otherwise. CM_CHECK_REGISTRY_KEY_CLASS_UNALLOCATED
 * is returned if the class cell is not allocated.
 */
CM_CHECK_REGISTRY_STATUS
CmpValidateClass(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData)
{
    ULONG ClassLength;
    HCELL_INDEX ClassCell;

    PAGED_CODE();

    /* Sanity checks */
    ASSERT(CurrentCell != HCELL_NIL);
    ASSERT(CellData);

    /* Cache the class cell and validate it */
    ClassCell = CellData->u.KeyNode.Class;
    ClassLength = CellData->u.KeyNode.ClassLength;
    if (ClassLength > 0)
    {
        /* Do we have a class? */
        if (ClassCell == HCELL_NIL)
        {
            /*
             * Whoever killed this class did it by
             * leaving the length of the class untouched.
             * Or probably the class got lost. Whatever
             * the case, a non zero length is not a problem
             * for us here, unlike an unallocated class
             * cell in the code path below.
             */
            DPRINT1("CmpValidateClass(): The key node class is NIL but the class length is not 0, resetting it!\n");
            HvMarkCellDirty(Hive, CurrentCell, FALSE);
            CellData->u.KeyNode.ClassLength = 0;
            return CM_CHECK_REGISTRY_GOOD;
        }

        /* Is the class allocated? */
        if (!HvIsCellAllocated(Hive, ClassCell))
        {
            DPRINT1("CmpValidateClass(): The key class is not allocated!\n");
            return CM_CHECK_REGISTRY_KEY_CLASS_UNALLOCATED;
        }
    }

    /* We're good */
    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Validates each value in the list by count. A
 * value that is damaged gets removed from the list.
 * This routine performs self-healing process in
 * this case.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current key cell that the value list points to.
 *
 * @param[in] ListCount
 * The list count that describes the actual number of
 * values in the list.
 *
 * @param[in] ValueListData
 * A pointer to cell data of the current key cell
 * that contains the value list to be validated.
 *
 * @param[out] ValuesRemoved
 * When the function finishes doing its operations,
 * this parameter contains the amount of removed values
 * from the list. A value of 0 indicates no values have
 * been removed (which that would imply a self-healing
 * process of the value list has occurred).
 *
 * @return
 * Returns CM_CHECK_REGISTRY_GOOD if the value list is
 * sane. CM_CHECK_REGISTRY_VALUE_CELL_NIL is returned
 * if a certain value cell is HCELL_NIL at specific
 * count index. CM_CHECK_REGISTRY_VALUE_CELL_UNALLOCATED is
 * returned if a certain value cell is unallocated at specific
 * count index. CM_CHECK_REGISTRY_VALUE_CELL_DATA_NOT_FOUND is
 * returned if cell data could not be mapped from the value cell,
 * the value list is totally torn apart in this case.
 * CM_CHECK_REGISTRY_VALUE_CELL_SIZE_NOT_SANE is returned if the
 * value's size is bogus. CM_CHECK_REGISTRY_CORRUPT_VALUE_DATA
 * is returned if the data inside the value is HCELL_NIL.
 * CM_CHECK_REGISTRY_DATA_CELL_NOT_ALLOCATED is returned if the
 * data cell inside the value is not allocated.
 * CM_CHECK_REGISTRY_BAD_KEY_VALUE_SIGNATURE is returned if the
 * value's signature is not valid.
 */
CM_CHECK_REGISTRY_STATUS
CmpValidateValueListByCount(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _In_ ULONG ListCount,
    _In_ PCELL_DATA ValueListData,
    _Out_ PULONG ValuesRemoved)
{
    ULONG ValueDataSize;
    ULONG ListCountIndex;
    ULONG DataSize;
    HCELL_INDEX DataCell;
    HCELL_INDEX ValueCell;
    PCELL_DATA ValueData;
    ULONG ValueNameLength, TotalValueNameLength;

    PAGED_CODE();

    /* Sanity checks */
    ASSERT(ValueListData);
    ASSERT(ListCount != 0);

    /* Assume we haven't removed any value counts for now */
    *ValuesRemoved = 0;

    /*
     * Begin looping each value in the list and
     * validate it accordingly.
     */
    ListCountIndex = 0;
    while (ListCountIndex < ListCount)
    {
        /* Is this cell value NIL? */
        ValueCell = ValueListData->u.KeyList[ListCountIndex];
        if (ValueCell == HCELL_NIL)
        {
            /* Kill this value from the list */
            if (!CmpRepairValueListCount(Hive,
                                         CurrentCell,
                                         ListCountIndex,
                                         ValueListData))
            {
                DPRINT1("CmpValidateValueListByCount(): The value cell is NIL (at index %lu, list count %lu)!\n",
                        ListCountIndex, ListCount);
                return CM_CHECK_REGISTRY_VALUE_CELL_NIL;
            }

            /*
             * We have altered the list count of the cell,
             * now we must update the list count here as well.
             * Decrease the count and continue with the next value.
             * Inform the caller we have removed a value.
             */
            ListCount--;
            *ValuesRemoved++;
            DPRINT1("CmpValidateValueListByCount(): Damaged value removed, continuing with the next value...\n");
            continue;
        }

        /* Is the value cell allocated? */
        if (!HvIsCellAllocated(Hive, ValueCell))
        {
            /* Kill this value from the list */
            if (!CmpRepairValueListCount(Hive,
                                         CurrentCell,
                                         ListCountIndex,
                                         ValueListData))
            {
                DPRINT1("CmpValidateValueListByCount(): The value cell is not allocated (at index %lu, list count %lu!\n",
                        ListCountIndex, ListCount);
                return CM_CHECK_REGISTRY_VALUE_CELL_UNALLOCATED;
            }

            /*
             * We have altered the list count of the cell,
             * now we must update the list count here as well.
             * Decrease the count and continue with the next value.
             * Inform the caller we have removed a value.
             */
            ListCount--;
            *ValuesRemoved++;
            DPRINT1("CmpValidateValueListByCount(): Damaged value removed, continuing with the next value...\n");
            continue;
        }

        /* Obtain a cell data from this value */
        ValueData = (PCELL_DATA)HvGetCell(Hive, ValueCell);
        if (!ValueData)
        {
            /* The value list is toast if we can't get the data */
            DPRINT1("CmpValidateValueListByCount(): Cell data of the value cell not found (at index %lu, value count %lu)!\n",
                    ListCountIndex, ListCount);
            return CM_CHECK_REGISTRY_VALUE_CELL_DATA_NOT_FOUND;
        }

        /* Check that the value size is sane */
        ValueDataSize = HvGetCellSize(Hive, ValueData);
        ValueNameLength = ValueData->u.KeyValue.NameLength;
        TotalValueNameLength = ValueNameLength + FIELD_OFFSET(CM_KEY_VALUE, Name);
        if (TotalValueNameLength > ValueDataSize)
        {
            /* Kill this value from the list */
            if (!CmpRepairValueListCount(Hive,
                                         CurrentCell,
                                         ListCountIndex,
                                         ValueListData))
            {
                DPRINT1("CmpValidateValueListByCount(): The total size is bigger than the actual cell size (total size %lu, cell size %lu, at index %lu)\n",
                        TotalValueNameLength, ValueDataSize, ListCountIndex);
                return CM_CHECK_REGISTRY_VALUE_CELL_SIZE_NOT_SANE;
            }

            /*
             * We have altered the list count of the cell,
             * now we must update the list count here as well.
             * Decrease the count and continue with the next value.
             * Inform the caller we have removed a value.
             */
            ListCount--;
            *ValuesRemoved++;
            DPRINT1("CmpValidateValueListByCount(): Damaged value removed, continuing with the next value...\n");
            continue;
        }

        /*
         * The value cell has a sane size. The last thing
         * to validate is the actual data of the value cell.
         * That is, we want that the data itself and length
         * are consistent. Technically speaking, value keys
         * that are small are directly located in the value
         * cell and it's built-in, in other words, the data
         * is immediately present in the cell so we don't have
         * to bother validating them since they're alright on
         * their own. This can't be said the same about normal
         * values though.
         */
        DataCell = ValueData->u.KeyValue.Data;
        if (!CmpIsKeyValueSmall(&DataSize, ValueData->u.KeyValue.DataLength))
        {
            /*
             * Validate the actual data based on size.
             *
             * CASE DataSize == 0 && DataCell != HCELL_NIL:
             *
             * - The length is 0 but there's some data still. This
             * could indicate this registry hive had some data before
             * but its internal properties got corrupt and the data
             * is still here. We cannot trust it so ultimately the
             * value will be killed.
             *
             * CASE DataSize > 0:
             *
             * - This registry hive still has some data as the length
             * tells us but we want to be sure this kind of data is
             * not "cancerous", aka, is the cell allocated?
             *
             */
            if (DataSize == 0)
            {
                if (DataCell != HCELL_NIL)
                {
                    /* Kill this value from the list */
                    if (!CmpRepairValueListCount(Hive,
                                                 CurrentCell,
                                                 ListCountIndex,
                                                 ValueListData))
                    {
                        DPRINT1("CmpValidateValueListByCount(): The data is not NIL on a 0 length, data is corrupt!\n");
                        return CM_CHECK_REGISTRY_CORRUPT_VALUE_DATA;
                    }

                    /*
                     * We have altered the list count of the cell,
                     * now we must update the list count here as well.
                     * Decrease the count and continue with the next value.
                     * Inform the caller we have removed a value.
                     */
                    ListCount--;
                    *ValuesRemoved++;
                    DPRINT1("CmpValidateValueListByCount(): Damaged value removed, continuing with the next value...\n");
                    continue;
                }
            }
            else
            {
                if (!HvIsCellAllocated(Hive, DataCell))
                {
                    /* Kill this value from the list */
                    if (!CmpRepairValueListCount(Hive,
                                                 CurrentCell,
                                                 ListCountIndex,
                                                 ValueListData))
                    {
                        DPRINT1("CmpValidateValueListByCount(): The data is not NIL on a 0 length, data is corrupt!\n");
                        return CM_CHECK_REGISTRY_DATA_CELL_NOT_ALLOCATED;
                    }

                    /*
                     * We have altered the list count of the cell,
                     * now we must update the list count here as well.
                     * Decrease the count and continue with the next value.
                     * Inform the caller we have removed a value.
                     */
                    ListCount--;
                    *ValuesRemoved++;
                    DPRINT1("CmpValidateValueListByCount(): Damaged value removed, continuing with the next value...\n");
                    continue;
                }
            }

            /* FIXME: Big values not supported yet */
            ASSERT_VALUE_BIG(Hive, DataSize);
        }

        /* Is the signature valid? */
        if (ValueData->u.KeyValue.Signature != CM_KEY_VALUE_SIGNATURE)
        {
            /* Kill this value from the list */
            if (!CmpRepairValueListCount(Hive,
                                         CurrentCell,
                                         ListCountIndex,
                                         ValueListData))
            {
                DPRINT1("CmpValidateValueListByCount(): The key value signature is invalid!\n");
                return CM_CHECK_REGISTRY_BAD_KEY_VALUE_SIGNATURE;
            }

            /*
             * We have altered the list count of the cell,
             * now we must update the list count here as well.
             * Decrease the count and continue with the next value.
             * Inform the caller we have removed a value.
             */
            ListCount--;
            *ValuesRemoved++;
            DPRINT1("CmpValidateValueListByCount(): Damaged value removed, continuing with the next value...\n");
            continue;
        }

        /*
         * This value is in perfect shape, advance
         * to the next value.
         */
        ListCountIndex++;
    }

    /* We're good */
    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Validates the value list of a key. If the list
 * is damaged due to corruption, the whole list
 * is expunged. This function performs self-healing
 * procedures in this case.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current key cell that the value list points to.
 *
 * @param[in] CellData
 * The cell data of the current cell of which the value
 * list comes from.
 *
 * @return
 * Returns CM_CHECK_REGISTRY_GOOD if the value list is
 * sane. CM_CHECK_REGISTRY_VALUE_LIST_UNALLOCATED is returned
 * if the value list cell is not allocated. CM_CHECK_REGISTRY_VALUE_LIST_DATA_NOT_FOUND
 * is returned if cell data could not be mapped from the value
 * list cell. CM_CHECK_REGISTRY_VALUE_LIST_SIZE_NOT_SANE is returned
 * if the size of the value list is bogus. A failure CM status code
 * is returned otherwise.
 */
CM_CHECK_REGISTRY_STATUS
CmpValidateValueList(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData)
{
    CM_CHECK_REGISTRY_STATUS CmStatusCode;
    ULONG TotalValueLength, ValueSize;
    ULONG ValueListCount;
    ULONG ValuesRemoved;
    HCELL_INDEX ValueListCell;
    PCELL_DATA ValueListData;

    PAGED_CODE();

    /* Sanity checks */
    ASSERT(CurrentCell != HCELL_NIL);
    ASSERT(CellData);

    /* Cache the value list and validate it */
    ValueListCell = CellData->u.KeyNode.ValueList.List;
    ValueListCount = CellData->u.KeyNode.ValueList.Count;
    if (ValueListCount > 0)
    {
        /* Is the value list allocated? */
        if (!HvIsCellAllocated(Hive, ValueListCell))
        {
            DPRINT1("CmpValidateValueList(): The value list is not allocated!\n");
            return CM_CHECK_REGISTRY_VALUE_LIST_UNALLOCATED;
        }

        /*
         * The value list is allocated, obtain
         * a cell data from it so that we can
         * further examine its guts.
         */
        ValueListData = (PCELL_DATA)HvGetCell(Hive, ValueListCell);
        if (!ValueListData)
        {
            DPRINT1("CmpValidateValueList(): Could not get cell data from the value list!\n");
            return CM_CHECK_REGISTRY_VALUE_LIST_DATA_NOT_FOUND;
        }

        /*
         * Cache the value size and total length and
         * assert ourselves this is a sane value list
         * to begin with.
         */
        ValueSize = HvGetCellSize(Hive, ValueListData);
        TotalValueLength = ValueListCount * sizeof(HCELL_INDEX);
        if (TotalValueLength > ValueSize)
        {
            DPRINT1("CmpValidateValueList(): The value list is bigger than the cell (value list size %lu, cell size %lu)!\n",
                    TotalValueLength, ValueSize);
            return CM_CHECK_REGISTRY_VALUE_LIST_SIZE_NOT_SANE;
        }

        /*
         * The value list is sane, now we would
         * need to validate the actual list
         * by its count.
         */
        CmStatusCode = CmpValidateValueListByCount(Hive,
                                                   CurrentCell,
                                                   ValueListCount,
                                                   ValueListData,
                                                   &ValuesRemoved);
        if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
        {
            DPRINT1("CmpValidateValueList(): One of the values is corrupt and couldn't be repaired!\n");
            return CmStatusCode;
        }

        /* Log how much values have been removed */
        if (ValuesRemoved > 0)
            DPRINT1("CmpValidateValueList(): Values removed in the list -- %lu\n", ValuesRemoved);
    }

    /* We're good */
    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Validates the subkeys list of a key. If the list is
 * damaged from corruption, the function can either
 * salvage this list or purge the whole of it. The
 * function performs different validation steps for
 * different storage types.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current key cell that the subkeys list points to.
 *
 * @param[in] CellData
 * The cell data of the current cell of which the subkeys
 * list comes from.
 *
 * @param[out] DoRepair
 * A pointer to a boolean value set up by the function itself.
 * The function automatically sets this to FALSE indicating
 * that repairs can't be done onto the list itself. If the
 * list can be salvaged, then the function sets this to TRUE.
 * See Remarks for further information.
 *
 * @return
 * Returns CM_CHECK_REGISTRY_GOOD if the subkeys list is in
 * perfect shape. CM_CHECK_REGISTRY_STABLE_KEYS_ON_VOLATILE is
 * returned if the volatile storage has stable data which
 * that should not happen (this is only for the case of volatile
 * cells). CM_CHECK_REGISTRY_SUBKEYS_LIST_UNALLOCATED is returned
 * if the subkeys list cell is not allocated. CM_CHECK_REGISTRY_CORRUPT_SUBKEYS_INDEX
 * is returned if a key index could not be mapped from the subkeys
 * list cell. CM_CHECK_REGISTRY_BAD_SUBKEY_COUNT is returned if
 * the key index is a leaf and the subkeys count doesn't match up
 * with that of the leaf. CM_CHECK_REGISTRY_KEY_INDEX_CELL_UNALLOCATED is
 * returned if the key index cell at the specific index in the list of
 * the index is not allocated. CM_CHECK_REGISTRY_CORRUPT_LEAF_ON_ROOT is
 * returned if a leaf could not be mapped from an index.
 * CM_CHECK_REGISTRY_CORRUPT_LEAF_SIGNATURE is returned if the leaf has
 * an invalid signature. CM_CHECK_REGISTRY_CORRUPT_KEY_INDEX_SIGNATURE
 * is returned if the key index has an invalid signature, that is, it's
 * not a leaf nor a root.
 *
 * @remarks
 * Deep subkeys list healing can be done in specific cases where only
 * a subkey doesn't actually affect the key itself. The function will
 * mark the subkeys list as repairable by setting DoRepair parameter
 * to TRUE and the caller is responsible to heal the key by purging
 * the whole subkeys list. If the damage is so bad that there's
 * possibility the key itself is even damaged, no healing is done.
 */
CM_CHECK_REGISTRY_STATUS
CmpValidateSubKeyList(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData,
    _Out_ PBOOLEAN DoRepair)
{
    ULONG SubKeyCounts;
    HCELL_INDEX KeyIndexCell, SubKeysListCell;
    PCM_KEY_INDEX RootKeyIndex, LeafKeyIndex;
    ULONG RootIndex;
    ULONG TotalLeafCount;
    PAGED_CODE();

    /* Sanity checks */
    ASSERT(CurrentCell != HCELL_NIL);
    ASSERT(CellData);

    /* Initialize our stuff */
    RootKeyIndex = NULL;
    LeafKeyIndex = NULL;
    TotalLeafCount = 0;

    /*
     * Assume for now that the caller should not
     * do any kind of repairs on the subkeys list,
     * unless explicitly given the consent by us.
     */
    *DoRepair = FALSE;

    /*
     * For volatile keys they have data that can
     * fluctuate and change on the fly so there's
     * pretty much nothing that we can validate those.
     * But still, we would want that the volatile key
     * is not damaged by external factors, like e.g.,
     * having stable keys on a volatile space.
     */
    if (IS_CELL_VOLATILE(CurrentCell))
    {
        if (CellData->u.KeyNode.SubKeyCounts[Stable] != 0)
        {
            DPRINT1("CmpValidateSubKeyList(): The volatile key has stable subkeys!\n");
            return CM_CHECK_REGISTRY_STABLE_KEYS_ON_VOLATILE;
        }

        return CM_CHECK_REGISTRY_GOOD;
    }

    /*
     * This is not a volatile key, cache the subkeys list
     * and validate it.
     */
    SubKeysListCell = CellData->u.KeyNode.SubKeyLists[Stable];
    SubKeyCounts = CellData->u.KeyNode.SubKeyCounts[Stable];
    if (SubKeyCounts > 0)
    {
        /* Is the subkey list cell allocated? */
        if (!HvIsCellAllocated(Hive, SubKeysListCell))
        {
            DPRINT1("CmpValidateSubKeyList(): The subkeys list cell is not allocated!\n");
            *DoRepair = TRUE;
            return CM_CHECK_REGISTRY_SUBKEYS_LIST_UNALLOCATED;
        }

        /* Obtain a root index and validate it */
        RootKeyIndex = (PCM_KEY_INDEX)HvGetCell(Hive, SubKeysListCell);
        if (!RootKeyIndex)
        {
            DPRINT1("CmpValidateSubKeyList(): Could not get the root key index of the subkeys list cell!\n");
            return CM_CHECK_REGISTRY_CORRUPT_SUBKEYS_INDEX;
        }

        /*
         * For simple, fast and hashed leaves we would want
         * that the corresponding root index count matches with
         * that of the subkey counts itself. If this is not the
         * case we can isolate this problem and fix the count.
         */
        if (RootKeyIndex->Signature == CM_KEY_INDEX_LEAF ||
            RootKeyIndex->Signature == CM_KEY_FAST_LEAF ||
            RootKeyIndex->Signature == CM_KEY_HASH_LEAF)
        {
            if (SubKeyCounts != RootKeyIndex->Count)
            {
                /* Okay, fix the count */
                if (!CmpRepairSubKeyCounts(Hive,
                                           CurrentCell,
                                           RootKeyIndex->Count,
                                           CellData))
                {
                    DPRINT1("CmpValidateSubKeyList(): The subkeys list has invalid count (subkeys count %lu, root key index count %lu)!\n",
                            SubKeyCounts, RootKeyIndex->Count);
                    return CM_CHECK_REGISTRY_BAD_SUBKEY_COUNT;
                }
            }

            return CM_CHECK_REGISTRY_GOOD;
        }

        /*
         * The root index is not a leaf, check if the index
         * is an actual root then.
         */
        if (RootKeyIndex->Signature == CM_KEY_INDEX_ROOT)
        {
            /*
             * For the root we have to loop each leaf
             * from it and increase the total leaf count
             * in the root after we determined the validity
             * of a leaf. This way we can see if the subcount
             * matches with that of the subkeys list count.
             */
            for (RootIndex = 0; RootIndex < RootKeyIndex->Count; RootIndex++)
            {
                /*
                 * We must assert ourselves that the cell
                 * at this index is allocated.
                 */
                KeyIndexCell = RootKeyIndex->List[RootIndex];
                if (!HvIsCellAllocated(Hive, KeyIndexCell))
                {
                    DPRINT1("CmpValidateSubKeyList(): The key index cell is not allocated at index %lu!\n", RootIndex);
                    *DoRepair = TRUE;
                    return CM_CHECK_REGISTRY_KEY_INDEX_CELL_UNALLOCATED;
                }

                /* Obtain a leaf from the root */
                LeafKeyIndex = (PCM_KEY_INDEX)HvGetCell(Hive, KeyIndexCell);
                if (!LeafKeyIndex)
                {
                    /* This root is kaput */
                    DPRINT1("CmpValidateSubKeyList(): The root key index's signature is invalid!\n");
                    return CM_CHECK_REGISTRY_CORRUPT_LEAF_ON_ROOT;
                }

                /* Check that the leaf has valid signature */
                if (LeafKeyIndex->Signature != CM_KEY_INDEX_LEAF &&
                    LeafKeyIndex->Signature != CM_KEY_FAST_LEAF &&
                    LeafKeyIndex->Signature != CM_KEY_HASH_LEAF)
                {
                    DPRINT1("CmpValidateSubKeyList(): The leaf's signature is invalid!\n");
                    *DoRepair = TRUE;
                    return CM_CHECK_REGISTRY_CORRUPT_LEAF_SIGNATURE;
                }

                /* Add up the count of the leaf */
                TotalLeafCount += LeafKeyIndex->Count;
            }

            /*
             * We have built up the total leaf count,
             * we have to determine this count is exactly
             * the same as the subkeys list count. Otherwise
             * just fix it.
             */
            if (SubKeyCounts != TotalLeafCount)
            {
                if (!CmpRepairSubKeyCounts(Hive,
                                           CurrentCell,
                                           TotalLeafCount,
                                           CellData))
                {
                    DPRINT1("CmpValidateSubKeyList(): The subkeys list has invalid count (subkeys count %lu, total leaf count %lu)!\n",
                            SubKeyCounts, TotalLeafCount);
                    return CM_CHECK_REGISTRY_BAD_SUBKEY_COUNT;
                }
            }

            return CM_CHECK_REGISTRY_GOOD;
        }

        /*
         * None of the valid signatures match with that of
         * the root key index. By definition, the whole subkey
         * list is total toast.
         */
        DPRINT1("CmpValidateSubKeyList(): The root key index's signature is invalid!\n");
        *DoRepair = TRUE;
        return CM_CHECK_REGISTRY_CORRUPT_KEY_INDEX_SIGNATURE;
    }

    /* If we reach here then this key has no subkeys */
    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Purges the volatile storage of a registry
 * hive. This operation is done mainly during
 * the bootup of the system.
 *
 * @param[in] Hive
 * A pointer to a hive where volatiles are
 * to be purged.
 *
 * @param[in] CurrentCell
 * The current key cell that the volatile storage of
 * the hive points to.
 *
 * @param[in] CellData
 * The cell data of the current cell of which the volatile
 * subkeys storage comes from.
 *
 * @param[in] Flags
 * A bit mask flag that is used to influence how is the
 * purging operation to be done. See CmCheckRegistry documentation
 * below for more information.
 *
 * @return
 * Nothing.
 */
VOID
CmpPurgeVolatiles(
    _In_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData,
    _In_ ULONG Flags)
{
    PAGED_CODE();

    /* Sanity check */
    ASSERT(CellData);

    /* Did the caller ask to purge volatiles? */
    if ((Flags & CM_CHECK_REGISTRY_PURGE_VOLATILES) &&
        (CellData->u.KeyNode.SubKeyCounts[Volatile] != 0))
    {
        /*
         * OK, the caller wants them cleaned from this
         * hive. For XP Beta 1 or newer hives, we unintialize
         * the whole volatile subkeys list. For older hives,
         * we just do a cleanup.
         */
        HvMarkCellDirty(Hive, CurrentCell, FALSE);
        if ((Flags & CM_CHECK_REGISTRY_BOOTLOADER_PURGE_VOLATILES) &&
            (Hive->Version >= HSYS_WHISTLER_BETA1))
        {
            CellData->u.KeyNode.SubKeyLists[Volatile] = CMP_VOLATILE_LIST_UNINTIALIZED;
        }
        else
        {
            CellData->u.KeyNode.SubKeyLists[Volatile] = HCELL_NIL;
        }

        /* Clear the count */
        CellData->u.KeyNode.SubKeyCounts[Volatile] = 0;
    }
}

/**
 * @brief
 * Validates the key cell, ensuring that
 * the key in the registry is valid and not corrupted.
 *
 * @param[in] Hive
 * A pointer to a hive of the registry where the
 * key is to be validated.
 *
 * @param[in] SecurityDefaulted
 * If the caller sets this to TRUE, this indicates the
 * hive has its security property defaulted due to
 * heal recovery of the said security. If the caller
 * sets this to FALSE, the hive comes with its own
 * security details. This parameter is currently unused.
 *
 * @param[in] ParentCell
 * The parent key cell that comes before the current cell.
 * This parameter can be HCELL_NIL if the first cell is
 * the root cell which is the parent of its own.
 *
 * @param[in] CurrentCell
 * The current child key cell that is to be validated.
 *
 * @param[out] Flags
 * A bit mask flag that is used to influence how is the
 * purging operation of volatile keys in the volatile storage
 * to be done. See CmCheckRegistry documentation below for more
 * information.
 *
 * @return
 * Returns CM_CHECK_REGISTRY_GOOD if the key that has been validated
 * is valid and not corrupted. CM_CHECK_REGISTRY_KEY_CELL_NOT_ALLOCATED is
 * returned if the key cell is not allocated. CM_CHECK_REGISTRY_CELL_DATA_NOT_FOUND
 * is returned if cell data could not be mapped from the key cell.
 * CM_CHECK_REGISTRY_CELL_SIZE_NOT_SANE is returned if the key cell
 * has an abnormal size that is above the trehshold the validation checks
 * can permit. CM_CHECK_REGISTRY_KEY_NAME_LENGTH_ZERO is returned if the
 * name length of the key node is 0, meaning that the key has no name.
 * CM_CHECK_REGISTRY_KEY_TOO_BIG_THAN_CELL is returned if the key is too
 * big than the cell itself. CM_CHECK_REGISTRY_BAD_KEY_NODE_PARENT is
 * returned if the parent node of the key is incosistent and it couldn't
 * be fixed. CM_CHECK_REGISTRY_BAD_KEY_NODE_SIGNATURE is returned if
 * the signature of the key node is corrupt and it couldn't be fixed.
 * A failure CM status code is returned otherwise.
 */
CM_CHECK_REGISTRY_STATUS
CmpValidateKey(
    _In_ PHHIVE Hive,
    _In_ BOOLEAN SecurityDefaulted,
    _In_ HCELL_INDEX ParentCell,
    _In_ HCELL_INDEX CurrentCell,
    _In_ ULONG Flags)
{
    CM_CHECK_REGISTRY_STATUS CmStatusCode;
    PCELL_DATA CellData;
    ULONG CellSize;
    BOOLEAN DoSubkeysRepair;
    ULONG TotalKeyNameLength, NameLength;

    PAGED_CODE();

    /* The current key cell mustn't be NIL here! */
    ASSERT(CurrentCell != HCELL_NIL);

    /* TODO: To be removed once we support security caching in Cm */
    UNREFERENCED_PARAMETER(SecurityDefaulted);

    /*
     * We must ensure that the key cell is
     * allocated in the first place before
     * we go further.
     */
    if (!HvIsCellAllocated(Hive, CurrentCell))
    {
        DPRINT1("CmpValidateKey(): The key cell is not allocated!\n");
        return CM_CHECK_REGISTRY_KEY_CELL_NOT_ALLOCATED;
    }

    /* Obtain a cell data basket from it */
    CellData = (PCELL_DATA)HvGetCell(Hive, CurrentCell);
    if (!CellData)
    {
        DPRINT1("CmpValidateKey(): Could not get cell data from the cell!\n");
        return CM_CHECK_REGISTRY_CELL_DATA_NOT_FOUND;
    }

    /*
     * Get the size of this cell and validate its size.
     * That is, the key must not be above the plausible
     * size threshold. Otherwise we have gotten an
     * implausible cell size.
     */
    CellSize = HvGetCellSize(Hive, CellData);
    if (CellSize > CMP_KEY_SIZE_THRESHOLD)
    {
        DPRINT1("CmpValidateKey(): The cell size is above the threshold size (size %lu)!\n", CellSize);
        return CM_CHECK_REGISTRY_CELL_SIZE_NOT_SANE;
    }

    /*
     * The cell size is OK but we must ensure
     * the key is not bigger than the container
     * of the cell.
     */
    NameLength = CellData->u.KeyNode.NameLength;
    if (NameLength == 0)
    {
        /*
         * A key without a name length? This
         * has to be a phantom key then, bail
         * out.
         */
        DPRINT1("CmpValidateKey(): The key node name length is 0!\n");
        return CM_CHECK_REGISTRY_KEY_NAME_LENGTH_ZERO;
    }

    TotalKeyNameLength = NameLength + FIELD_OFFSET(CM_KEY_NODE, Name);
    if (TotalKeyNameLength > CellSize)
    {
        /* The key is indeed too big than the cell, bail out */
        DPRINT1("CmpValidateKey(): The key is too big than the cell (key size %lu, cell size %lu)!\n", TotalKeyNameLength, CellSize);
        return CM_CHECK_REGISTRY_KEY_TOO_BIG_THAN_CELL;
    }

    /*
     * The key size is OK. We're now comfortable this
     * key is not maimed so we can continue with
     * further registry checks. Now, check that
     * the parent of the key node points to the
     * actual provided parent.
     */
    if (ParentCell != HCELL_NIL &&
        ParentCell != CellData->u.KeyNode.Parent)
    {
        /*
         * The node is fractured since its parent
         * isn't consistent, but we can isolate
         * this problem and fix it.
         */
        if (!CmpRepairParentNode(Hive,
                                 CurrentCell,
                                 ParentCell,
                                 CellData))
        {
            /* We failed repairing the hive, bail out */
            DPRINT1("CmpValidateKey(): The parent key node doesn't point to the actual parent!\n");
            return CM_CHECK_REGISTRY_BAD_KEY_NODE_PARENT;
        }
    }

    /* Is the key node signature genuine? */
    if (CellData->u.KeyNode.Signature != CM_KEY_NODE_SIGNATURE)
    {
        /*
         * It's not, the node has received a small
         * bruise. Attempt to heal the hive.
         */
        if (!CmpRepairKeyNodeSignature(Hive,
                                       CurrentCell,
                                       CellData))
        {
            /* We failed repairing the hive, bail out */
            DPRINT1("CmpValidateKey(): The parent key node signature is not genuine!\n");
            return CM_CHECK_REGISTRY_BAD_KEY_NODE_SIGNATURE;
        }
    }

    /*
     * FIXME: Security cell checks have to be implemented here
     * once we properly and reliably implement security caching
     * in the kernel.
     */

    /* Validate the class */
    CmStatusCode = CmpValidateClass(Hive, CurrentCell, CellData);
    if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
    {
        /*
         * The class of this key is borked.
         * Attempt to heal the hive.
         */
        if (!CmpRepairClassOfNodeKey(Hive,
                                     CurrentCell,
                                     CellData))
        {
            /* We failed repairing the hive, bail out */
            DPRINT1("CmpValidateKey(): Failed to repair the hive, the cell class is not valid!\n");
            return CmStatusCode;
        }
    }

    /* Validate the value list */
    CmStatusCode = CmpValidateValueList(Hive, CurrentCell, CellData);
    if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
    {
        /*
         * It happens that a certain value in the list
         * is so bad like we couldn't map a cell data from it
         * or the list itself is toast. In such cases what we
         * can do here is to do a "value list sacrifice", aka
         * purge the whole list.
         */
        if (!CmpRepairValueList(Hive, CurrentCell))
        {
            /* We failed repairing the hive, bail out */
            DPRINT1("CmpValidateKey(): Failed to repair the hive, the value list is corrupt!\n");
            return CmStatusCode;
        }
    }

    /* Validate the subkeys list */
    CmStatusCode = CmpValidateSubKeyList(Hive, CurrentCell, CellData, &DoSubkeysRepair);
    if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
    {
        /*
         * The subkeys list is in trouble. Worse when the actual
         * subkey list is so severed this key is also kaput on itself.
         */
        if (!DoSubkeysRepair)
        {
            DPRINT1("CmpValidateKey(): The subkeys list is totally corrupt, can't repair!\n");
            return CmStatusCode;
        }

        /*
         * OK, there's still some salvation for this key.
         * Purge the whole subkeys list in order to fix it.
         */
        if (!CmpRepairSubKeyList(Hive,
                                 CurrentCell,
                                 CellData))
        {
            /* We failed repairing the hive, bail out */
            DPRINT1("CmpValidateKey(): Failed to repair the hive, the subkeys list is corrupt!\n");
            return CmStatusCode;
        }
    }

    /*
     * We're done, what's left is to check
     * if the caller wants the volatiles to be
     * purged. Generally purging is mainly done
     * upon system loading.
     */
    CmpPurgeVolatiles(Hive, CurrentCell, CellData, Flags);
    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Performs deep checking of the registry by walking
 * down the registry tree using a stack based pool.
 * This function is the guts of CmCheckRegistry.
 *
 * @param[in] Hive
 * A pointer to a hive of the registry where the
 * validation is to be performed.
 *
 * @param[in] Flags
 * Bit mask flag used for volatiles purging. Such
 * flags influence on how volatile purging is actually
 * done. See CmCheckRegistry documentation for more
 * information.
 *
 * @param[in] SecurityDefaulted
 * If the caller sets this to FALSE, the registry hive
 * uses its own unique security details. Otherwise
 * registry hive has the security details defaulted.
 *
 * @return
 * Returns CM_CHECK_REGISTRY_GOOD is returned if the function
 * has successfully performed deep registry checking and
 * the registry contents are valid. CM_CHECK_REGISTRY_ALLOCATE_MEM_STACK_FAIL
 * is returned if the function has failed to allocate the
 * stack work state buffer in memory which is necessary for
 * deep checking of the registry. CM_CHECK_REGISTRY_ROOT_CELL_NOT_FOUND
 * is returned if no root cell has been found of this hive.
 * CM_CHECK_REGISTRY_BAD_LEXICOGRAPHICAL_ORDER is returned if the lexical
 * order is not valid. CM_CHECK_REGISTRY_NODE_NOT_FOUND is returned if
 * the no key node could be mapped from the key. CM_CHECK_REGISTRY_SUBKEY_NOT_FOUND
 * is returned if no subkey child cell could be found. CM_CHECK_REGISTRY_TREE_TOO_MANY_LEVELS
 * is returned if we have reached the maximum stack limit which means the registry that
 * we have checked is too fat.
 */
CM_CHECK_REGISTRY_STATUS
CmpValidateRegistryInternal(
    _In_ PHHIVE Hive,
    _In_ ULONG Flags,
    _In_ BOOLEAN SecurityDefaulted)
{
    CM_CHECK_REGISTRY_STATUS CmStatusCode;
    PCMP_REGISTRY_STACK_WORK_STATE WorkState;
    HCELL_INDEX RootCell, ParentCell, CurrentCell;
    HCELL_INDEX ChildSubKeyCell;
    PCM_KEY_NODE KeyNode;
    ULONG WorkStateLength;
    LONG StackDepth;
    BOOLEAN AllChildrenChecked;

    PAGED_CODE();

    /* Sanity check */
    ASSERT(Hive);

    /*
     * Allocate some memory blocks for the stack
     * state structure. We'll be using it to walk
     * down the registry hive tree in a recursive
     * way without worrying that we explode the
     * kernel stack in the most gruesome and gross
     * ways.
     */
    WorkStateLength = CMP_REGISTRY_MAX_LEVELS_TREE_DEPTH * sizeof(CMP_REGISTRY_STACK_WORK_STATE);
    WorkState = CmpAllocate(WorkStateLength,
                            TRUE,
                            TAG_REGISTRY_STACK);
    if (!WorkState)
    {
        /* We're in despair if we can't get a buffer so bail out */
        DPRINT1("CmpValidateRegistryInternal(): Couldn't allocate memory for registry stack work state!\n");
        return CM_CHECK_REGISTRY_ALLOCATE_MEM_STACK_FAIL;
    }

    /* Obtain the root cell of the hive */
    RootCell = GET_HHIVE_ROOT_CELL(Hive);
    if (RootCell == HCELL_NIL)
    {
        /* Not getting the root cell is fatal, bail out */
        DPRINT1("CmpValidateRegistryInternal(): Couldn't get the root cell of the hive!\n");
        CmpFree(WorkState, WorkStateLength);
        return CM_CHECK_REGISTRY_ROOT_CELL_NOT_FOUND;
    }

RestartValidation:
    /*
     * Prepare the stack state and start from
     * the root cell. Ensure that the root cell
     * itself is OK before we go forward.
     */
    StackDepth = 0;
    WorkState[StackDepth].ChildCellIndex = 0;
    WorkState[StackDepth].Current = RootCell;
    WorkState[StackDepth].Parent = HCELL_NIL;
    WorkState[StackDepth].Sibling = HCELL_NIL;

    /*
     * As we start checking the root cell which
     * is the top element of a registry hive,
     * we'll be going to look for child keys
     * in the course of walking down the tree.
     */
    AllChildrenChecked = FALSE;

    while (StackDepth >= 0)
    {
        /* Cache the current and parent cells */
        CurrentCell = WorkState[StackDepth].Current;
        ParentCell = WorkState[StackDepth].Parent;

        /* Do we have still have children to validate? */
        if (!AllChildrenChecked)
        {
            /* Check that the key is OK */
            CmStatusCode = CmpValidateKey(Hive,
                                          SecurityDefaulted,
                                          ParentCell,
                                          CurrentCell,
                                          Flags);
            if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
            {
                /*
                 * The key cell is damaged. We have to pray and
                 * hope that this is not the root cell as any
                 * damage done to the root is catastrophically
                 * fatal.
                 */
                if (CurrentCell == RootCell)
                {
                    /*
                     * IT IS THE ROOT!!! There's no hope for
                     * this registry hive, it will go straight
                     * to hell.
                     */
                    DPRINT1("CmpValidateRegistryInternal(): THE ROOT CELL IS BROKEN!\n");
                    CmpFree(WorkState, WorkStateLength);
                    return CmStatusCode;
                }

                /*
                 * It is not the root, remove the faulting
                 * damaged cell from the parent so that we
                 * can heal the hive.
                 */
                if (!CmpRepairParentKey(Hive, CurrentCell, ParentCell))
                {
                    /*
                     * If we reach here then either self healing is
                     * not possible or we could not repair the parent
                     * list from damage. Bail out.
                     */
                    DPRINT1("CmpValidateRegistryInternal(): The key is corrupt (current cell %lu, parent cell %lu)!\n",
                            CurrentCell, ParentCell);
                    CmpFree(WorkState, WorkStateLength);
                    return CmStatusCode;
                }

                /*
                 * We have repaired the hive. Since the damaged
                 * cell has been removed, we must restart the
                 * loop and start over at the beginning.
                 */
                DPRINT1("CmpValidateRegistryInternal(): Hive repaired, restarting the validation loop...\n");
                goto RestartValidation;
            }

            /*
             * The key is in perfect shape. If we have advanced
             * the stack depth then check the lexicographical
             * order of the keys as well.
             */
            if (StackDepth > 0 &&
                CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
            {
                if (WorkState[StackDepth - CMP_PRIOR_STACK].Sibling != HCELL_NIL)
                {
                    if (!CmpValidateLexicalOrder(Hive,
                                                 CurrentCell,
                                                 WorkState[StackDepth - CMP_PRIOR_STACK].Sibling))
                    {
                        /*
                         * The lexicographical order is bad,
                         * attempt to heal the hive.
                         */
                        if (!CmpRepairParentKey(Hive, CurrentCell, ParentCell))
                        {
                            /*
                             * If we reach here then either self healing
                             * is not possible or we couldn't repair the
                             * parent list from damage. Bail out.
                             */
                            DPRINT1("CmpValidateRegistryInternal(): The lexicographical order is invalid (sibling %lu, current cell %lu)!\n",
                            CurrentCell, WorkState[StackDepth - CMP_PRIOR_STACK].Sibling);
                            CmpFree(WorkState, WorkStateLength);
                            return CM_CHECK_REGISTRY_BAD_LEXICOGRAPHICAL_ORDER;
                        }

                        /*
                         * We have repaired the hive. Since the damaged
                         * cell has been removed, we must restart the
                         * loop and start over at the beginning.
                         */
                        DPRINT1("CmpValidateRegistryInternal(): Hive repaired, restarting the validation loop...\n");
                        goto RestartValidation;
                    }
                }

                /* Assign the prior sibling for upcoming iteration */
                WorkState[StackDepth - CMP_PRIOR_STACK].Sibling = CurrentCell;
            }
        }

        /* Obtain a node for this key */
        KeyNode = (PCM_KEY_NODE)HvGetCell(Hive, CurrentCell);
        if (!KeyNode)
        {
            /* We couldn't be able to get the node, bail out */
            DPRINT1("CmpValidateRegistryInternal(): Couldn't get the node of key (current cell %lu)!\n", CurrentCell);
            CmpFree(WorkState, WorkStateLength);
            return CM_CHECK_REGISTRY_NODE_NOT_FOUND;
        }

        /*
         * If we have processed all the children from this
         * node then adjust the stack depth work state by
         * going back and restart the loop to lookup for
         * the rest of the tree. Acknowledge the code path
         * above that we checked all the children so that
         * we don't have to validate the same subkey again.
         */
        if (WorkState[StackDepth].ChildCellIndex < KeyNode->SubKeyCounts[Stable])
        {
            /*
             * We have children to process, obtain the
             * child subkey in question so that we can
             * cache it later for the next key validation.
             */
            ChildSubKeyCell = CmpFindSubKeyByNumber(Hive, KeyNode, WorkState[StackDepth].ChildCellIndex);
            if (ChildSubKeyCell == HCELL_NIL)
            {
                /*
                * If this cell couldn't be found despite the
                * number count of subkeys in this node then
                * the registry hive has received a major
                * laceration, so bail out.
                */
                DPRINT1("CmpValidateRegistryInternal(): Couldn't get the child subkey cell (at stack index %lu)!\n", StackDepth);
                CmpFree(WorkState, WorkStateLength);
                return CM_CHECK_REGISTRY_SUBKEY_NOT_FOUND;
            }

            /*
             * As we got the subkey advance the child index as
             * well as the stack depth work state for the next
             * key validation. However we must ensure since
             * we're advancing the stack depth that we don't
             * go over the maximum tree level depth. A registry
             * tree can be at maximum 512 levels deep.
             *
             * For more information see https://docs.microsoft.com/en-us/windows/win32/sysinfo/registry-element-size-limits.
             */
            WorkState[StackDepth].ChildCellIndex++;
            StackDepth++;
            if (StackDepth >= CMP_REGISTRY_MAX_LEVELS_TREE_DEPTH - 1)
            {
                /*
                 * This registry has so many levels it's
                 * so fat. We don't want to explode our
                 * kernel stack, so just simply bail out...
                 */
                DPRINT1("CmpValidateRegistryInternal(): The registry tree has so many levels!\n");
                CmpFree(WorkState, WorkStateLength);
                return CM_CHECK_REGISTRY_TREE_TOO_MANY_LEVELS;
            }

            /* Prepare the work state for the next key */
            WorkState[StackDepth].ChildCellIndex = 0;
            WorkState[StackDepth].Current = ChildSubKeyCell;
            WorkState[StackDepth].Parent = WorkState[StackDepth - CMP_PRIOR_STACK].Current;
            WorkState[StackDepth].Sibling = HCELL_NIL;

            /*
             * As we prepared the work state, acknowledge the
             * code path at the top of the loop that we need
             * to process and validate the next child subkey.
             */
            AllChildrenChecked = FALSE;
            continue;
        }

        AllChildrenChecked = TRUE;
        StackDepth--;
        continue;
    }

    CmpFree(WorkState, WorkStateLength);
    return CM_CHECK_REGISTRY_GOOD;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Validates a bin from a hive. It performs checks
 * against the cells from this bin, ensuring the
 * bin is not corrupt and that the cells are consistent
 * with each other.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] Bin
 * A pointer to a bin where its cells are to be
 * validated.
 *
 * @return
 * CM_CHECK_REGISTRY_GOOD is returned if the bin is
 * valid and not corrupt. CM_CHECK_REGISTRY_BIN_SIGNATURE_HEADER_CORRUPT
 * is returned if this bin has a corrupt signature. CM_CHECK_REGISTRY_BAD_FREE_CELL
 * is returned if the free cell has a bogus size. CM_CHECK_REGISTRY_BAD_ALLOC_CELL
 * is returned for the allocated cell has a bogus size.
 */
CM_CHECK_REGISTRY_STATUS
NTAPI
HvValidateBin(
    _In_ PHHIVE Hive,
    _In_ PHBIN Bin)
{
    PHCELL Cell, Basket;

    PAGED_CODE();

    /* Sanity checks */
    ASSERT(Bin);
    ASSERT(Hive);

    /* Ensure that this bin we got has valid signature header */
    if (Bin->Signature != HV_HBIN_SIGNATURE)
    {
        DPRINT1("HvValidateBin(): The bin's signature header is corrupt!\n");
        return CM_CHECK_REGISTRY_BIN_SIGNATURE_HEADER_CORRUPT;
    }

    /*
     * Walk over all the cells from this bin and
     * validate that they're consistent with the bin.
     * Namely we want that each cell from this bin doesn't
     * have a bogus size.
     */
    Basket = (PHCELL)((PUCHAR)Bin + Bin->Size);
    for (Cell = GET_CELL_BIN(Bin);
         Cell < Basket;
         Cell = (PHCELL)((PUCHAR)Cell + abs(Cell->Size)))
    {
        /* Does this cell have some free space? */
        if (IsFreeCell(Cell))
        {
            /*
             * This cell indeed is free, check that
             * the size of this cell is not bogus.
             */
            if (Cell->Size > Bin->Size ||
                Cell->Size == 0)
            {
                /*
                 * This cell has too much free space that
                 * exceeds the boundary of the bin size.
                 * Otherwise the cell doesn't have actual
                 * free space (aka Size == 0) which is a
                 * no go for a bin.
                 */
                DPRINT1("HvValidateBin(): The free cell exceeds the bin size or cell size equal to 0 (cell: %p, cell size %d, bin size %lu)\n",
                        Cell, Cell->Size, Bin->Size);
                return CM_CHECK_REGISTRY_BAD_FREE_CELL;
            }
        }
        else
        {
            /*
             * This cell is allocated, make sure that
             * the size of this cell is not bogus.
             */
            if (abs(Cell->Size) > Bin->Size)
            {
                /*
                 * This cell allocated too much space
                 * that exceeds the boundary of the
                 * bin size.
                 */
                DPRINT1("HvValidateBin(): The allocated cell exceeds the bin size (cell: %p, cell size %d, bin size %lu)\n",
                        Cell, abs(Cell->Size), Bin->Size);
                return CM_CHECK_REGISTRY_BAD_ALLOC_CELL;
            }
        }
    }

    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Validates a registry hive. This function ensures
 * that the storage of this hive has valid bins.
 *
 * @param[in] Hive
 * A pointer to a hive where validation on its bins
 * is to be performed.
 *
 * @return
 * CM_CHECK_REGISTRY_GOOD is returned if the hive
 * is valid. CM_CHECK_REGISTRY_HIVE_CORRUPT_SIGNATURE is
 * returned if the hive has a corrupted signature.
 * CM_CHECK_REGISTRY_BIN_SIZE_OR_OFFSET_CORRUPT is returned
 * if the captured bin has a bad size. A failure CM status
 * code is returned otherwise.
 */
CM_CHECK_REGISTRY_STATUS
NTAPI
HvValidateHive(
    _In_ PHHIVE Hive)
{
    CM_CHECK_REGISTRY_STATUS CmStatusCode;
    ULONG StorageIndex;
    ULONG BlockIndex;
    ULONG StorageLength;
    PHBIN Bin;

    PAGED_CODE();

    /* Sanity check */
    ASSERT(Hive);

    /* Is the hive signature valid? */
    if (Hive->Signature != HV_HHIVE_SIGNATURE)
    {
        /* Someone sent Fat Man type of nuclear bomb to this hive, bail out */
        DPRINT1("HvValidateHive(): Hive's signature corrupted (signature %lu)\n", Hive->Signature);
        return CM_CHECK_REGISTRY_HIVE_CORRUPT_SIGNATURE;
    }

    /*
     * Now loop each bin in the storage of this
     * hive.
     */
    for (StorageIndex = 0; StorageIndex < Hive->StorageTypeCount; StorageIndex++)
    {
        /* Get the storage length at this index */
        StorageLength = Hive->Storage[StorageIndex].Length;

        for (BlockIndex = 0; BlockIndex < StorageLength; BlockIndex++)
        {
            /* Go to the next if this bin does not exist */
            if (Hive->Storage[StorageIndex].BlockList[BlockIndex].BinAddress == (ULONG_PTR)NULL)
            {
                continue;
            }

            /*
             * Capture this bin and ensure that such
             * bin is within the offset and the size
             * is not bogus.
             */
            Bin = GET_HHIVE_BIN(Hive, StorageIndex, BlockIndex);
            if (Bin->Size > (StorageLength * HBLOCK_SIZE) ||
                Bin->FileOffset != (BlockIndex * HBLOCK_SIZE))
            {
                DPRINT1("HvValidateHive(): Bin size or offset is corrupt (bin size %lu, file offset %lu, storage length %lu)\n",
                        Bin->Size, Bin->FileOffset, StorageLength);
                return CM_CHECK_REGISTRY_BIN_SIZE_OR_OFFSET_CORRUPT;
            }

            /* Validate the rest of the bin */
            CmStatusCode = HvValidateBin(Hive, Bin);
            if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
            {
                DPRINT1("HvValidateHive(): This bin is not valid (bin %p)\n", Bin);
                return CmStatusCode;
            }
        }
    }

    return CM_CHECK_REGISTRY_GOOD;
}

/**
 * @brief
 * Checks the registry that is consistent and its
 * contents valid and not corrupted. More specifically
 * this function performs a deep check of the registry
 * for the following properties:
 *
 * - That the security cache cell of the registry is OK
 * - That bins and cells are consistent with each other
 * - That the child subkey cell points to the parent
 * - That the key itself has sane sizes
 * - That the class, values and subkeys lists are valid
 * - Much more
 *
 * @param[in] Hive
 * A pointer to a CM hive of the registry to be checked
 * in question.
 *
 * @param[in] Flags
 * A bit mask flag used to influence the process of volatile
 * keys purging. See Remarks for further information.
 *
 * @return
 * This function returns a CM (Configuration Manager) check
 * registry status code. A code of CM_CHECK_REGISTRY_GOOD of
 * value 0 indicates the registry hive is valid and not corrupted.
 * A non zero unsigned integer value indicates a failure. Consult
 * other private routines in this file for other failure status
 * codes.
 *
 * @remarks
 * During a load operation CmCheckRegistry can purge the volatile
 * data of registry (or not) depending on the submitted flag bit mask
 * by the caller. The following supported flags are:
 *
 * CM_CHECK_REGISTRY_DONT_PURGE_VOLATILES -- Tells the function that
 * volatile data purging must not be done.
 *
 * CM_CHECK_REGISTRY_PURGE_VOLATILES - Tells the function to purge out
 * volatile information data from a registry hive, on demand. Purging
 * doesn't come into action if no volatile data has been found.
 *
 * CM_CHECK_REGISTRY_BOOTLOADER_PURGE_VOLATILES - A special flag used
 * by FreeLdr and Environ. When this flag is set the function will not
 * clean up the volatile storage but it will unintialize the storage
 * instead (this is the case if the given registry hive for validation
 * is a XP Beta 1 hive or newer). Otherwise it will perform a normal
 * cleanup of the volatile storage.
 *
 * CM_CHECK_REGISTRY_VALIDATE_HIVE - Tells the function to perform a
 * thorough analysation of the underlying hive's bins and cells before
 * doing validation of the registry tree. HvValidateHive function is called
 * in this case.
 */
CM_CHECK_REGISTRY_STATUS
NTAPI
CmCheckRegistry(
    _In_ PCMHIVE RegistryHive,
    _In_ ULONG Flags)
{
    CM_CHECK_REGISTRY_STATUS CmStatusCode;
    PHHIVE Hive;

    PAGED_CODE();

    /* Make sure the registry hive is valid */
    ASSERT(RegistryHive);

#if !defined(CMLIB_HOST) && !defined(_BLDR_)
    /*
     * If this CM hive that we got is a volatile
     * hive (aka master hive), don't do any checks
     * to it.
     */
    if (RegistryHive == CmiVolatileHive)
    {
        DPRINT("CmCheckRegistry(): This is master registry hive, don't do anything!\n");
        return CM_CHECK_REGISTRY_GOOD;
    }
#endif

    /*
     * Obtain the hive and check if the caller wants
     * that the hive to be validated.
     */
    Hive = GET_HHIVE(RegistryHive);
    if (Flags & CM_CHECK_REGISTRY_VALIDATE_HIVE)
    {
        CmStatusCode = HvValidateHive(Hive);
        if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
        {
            DPRINT1("CmCheckRegistry(): The hive is not valid (hive pointer %p, check status code %lu)!\n", Hive, CmStatusCode);
            return CmStatusCode;
        }
    }

    /*
     * FIXME: Currently ReactOS does not implement security
     * caching algorithms so it's pretty pointless to implement
     * security descriptors validation checks at this moment.
     * When the time comes to implement these, we would need
     * to implement security checks here as well.
     */

    /* Call the internal API to do the rest of the work bulk */
    CmStatusCode = CmpValidateRegistryInternal(Hive, Flags, FALSE);
    if (!CM_CHECK_REGISTRY_SUCCESS(CmStatusCode))
    {
        DPRINT1("CmCheckRegistry(): The hive is not valid (hive pointer %p, check status code %lu)!\n", Hive, CmStatusCode);
        return CmStatusCode;
    }

    return CmStatusCode;
}

/* EOF */
