/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Configuration Manager Library - Registry Self-Heal Routines
 * COPYRIGHT:       Copyright 2022 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include "cmlib.h"
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Removes a cell from a fast key index.
 *
 * @param[in,out] FastIndex
 * The fast key index where a cell has to be removed.
 *
 * @param[in] Index
 * The index which points to the location of the
 * cell that is to be removed.
 *
 * @param[in] UpdateCount
 * If set to TRUE, the function will update the fast
 * index count accordingly by one value less. If set
 * to FALSE, the count won't be updated. See Remarks
 * for further information.
 *
 * @return
 * Nothing.
 *
 * @remarks
 * In case where the fast index count is not updated is
 * when the key index is not a root but a leaf. In such
 * scenario such leaf is the actual key index itself
 * so updating the fast index count is not necessary (aka
 * UpdateCount is set to FALSE).
 */
VOID
CmpRemoveFastIndexKeyCell(
    _Inout_ PCM_KEY_FAST_INDEX FastIndex,
    _In_ ULONG Index,
    _In_ BOOLEAN UpdateCount)
{
    ULONG MoveCount;
    ASSERT(Index < FastIndex->Count);

    /* Calculate the number of trailing cells */
    MoveCount = FastIndex->Count - Index - 1;
    if (MoveCount != 0)
    {
        /* Remove the cell now by moving one location ahead */
        RtlMoveMemory(&FastIndex->List[Index],
                      &FastIndex->List[Index + 1],
                      MoveCount * sizeof(CM_INDEX));
    }

    /* Update the fast index count if asked */
    if (UpdateCount)
        FastIndex->Count--;
}

/**
 * @brief
 * Removes a cell from a normal key index.
 *
 * @param[in,out] KeyIndex
 * The key index where a cell has to be removed.
 *
 * @param[in] Index
 * The index which points to the location of the
 * cell that is to be removed.
 *
 * @return
 * Nothing.
 */
VOID
CmpRemoveIndexKeyCell(
    _Inout_ PCM_KEY_INDEX KeyIndex,
    _In_ ULONG Index)
{
    ULONG MoveCount;
    ASSERT(Index < KeyIndex->Count);

    /* Calculate the number of trailing cells */
    MoveCount = KeyIndex->Count - Index - 1;
    if (MoveCount != 0)
    {
        /* Remove the cell now by moving one location ahead */
        RtlMoveMemory(&KeyIndex->List[Index],
                      &KeyIndex->List[Index + 1],
                      MoveCount * sizeof(HCELL_INDEX));
    }

    /* Update the key index count */
    KeyIndex->Count--;
}

/**
 * @brief
 * Removes a cell from a key value list node.
 *
 * @param[in,out] ValueListNode
 * The value list node which is used by the
 * function to update the value list count.
 *
 * @param[in,out] ValueListData
 * The value list data of which a cell has to be removed.
 *
 * @param[in] Index
 * The index which points to the location of the
 * cell that is to be removed.
 *
 * @return
 * Nothing.
 */
VOID
CmpRemoveValueFromValueList(
    _Inout_ PCM_KEY_NODE ValueListNode,
    _Inout_ PCELL_DATA ValueListData,
    _In_ ULONG Index)
{
    ULONG MoveCount;
    ASSERT(Index < ValueListNode->ValueList.Count);

    /* Calculate the number of trailing values */
    MoveCount = ValueListNode->ValueList.Count - Index - 1;
    if (MoveCount != 0)
    {
        /* Remove the value now by moving one location ahead */
        RtlMoveMemory(&ValueListData->u.KeyList[Index],
                      &ValueListData->u.KeyList[Index + 1],
                      MoveCount * sizeof(HCELL_INDEX));
    }

    /* Update the value list count */
    ValueListNode->ValueList.Count--;
}

/**
 * @brief
 * Removes the offending subkey from a root index.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] RootIndex
 * The root index where a leaf is obtained from. Such
 * leaf is used to check deep down the leaf for the offending
 * subkey.
 *
 * @param[in] TargetKey
 * The offending target subkey to be removed.
 *
 * @return
 * Returns TRUE if the function successfully removed the target
 * key, FALSE otherwise.
 */
BOOLEAN
CmpRemoveSubkeyInRoot(
    _In_ PHHIVE Hive,
    _In_ PCM_KEY_INDEX RootIndex,
    _In_ HCELL_INDEX TargetKey)
{
    PCM_KEY_INDEX Leaf;
    PCM_KEY_FAST_INDEX FastIndex;
    HCELL_INDEX LeafCell;
    ULONG RootCountIndex;
    ULONG LeafCountIndex;

    PAGED_CODE();

    /* Sanity check */
    ASSERT(RootIndex);

    /* Loop the root index */
    for (RootCountIndex = 0; RootCountIndex < RootIndex->Count; RootCountIndex++)
    {
        /*
         * Release the leaf cell from previous iteration
         * of the loop. Make sure what we're releasing is
         * valid to begin with.
         */
        if (RootCountIndex)
        {
            ASSERT(Leaf);
            ASSERT(LeafCell == RootIndex->List[RootCountIndex - 1]);
            HvReleaseCell(Hive, LeafCell);
        }

        /* Get the leaf cell and the leaf for this index */
        LeafCell = RootIndex->List[RootCountIndex];
        Leaf = (PCM_KEY_INDEX)HvGetCell(Hive, LeafCell);
        if (!Leaf)
        {
            /* We couldn't obtain a leaf, bail out */
            DPRINT1("CmpRemoveSubkeyInRoot(): Couldn't get the leaf from cell!!!\n");
            return FALSE;
        }

        /* Start looping the leaf */
        for (LeafCountIndex = 0; LeafCountIndex < Leaf->Count; LeafCountIndex++)
        {
            /* Is the leaf a fast leaf or a hash one? */
            if ((Leaf->Signature == CM_KEY_FAST_LEAF) ||
                (Leaf->Signature == CM_KEY_HASH_LEAF))
            {
                /* It is one of the two, get the fast index */
                FastIndex = (PCM_KEY_FAST_INDEX)Leaf;

                /*
                 * Is the subkey cell from the fast
                 * index the one we one we're actually
                 * searching?
                 */
                if (FastIndex->List[LeafCountIndex].Cell == TargetKey)
                {
                    /*
                     * We have found the offending cell.
                     * Now we must discard it, mark the
                     * leaf cell dirty and do whatever is
                     * needed to repair the parent list
                     * from damage.
                     */
                    HvReleaseCell(Hive, LeafCell);
                    HvMarkCellDirty(Hive, LeafCell, FALSE);
                    CmpRemoveFastIndexKeyCell(FastIndex, LeafCountIndex, TRUE);
                    DPRINT1("CmpRemoveSubkeyInRoot(): The offending key cell has BEEN FOUND in fast index (fast index %p, index %lu)!\n",
                            FastIndex, LeafCountIndex);
                    return TRUE;
                }
            }
            else
            {
                /*
                 * The leaf is neither of the two. Check if
                 * the target offending cell is inside the leaf
                 * itself.
                 */
                if (Leaf->List[LeafCountIndex] == TargetKey)
                {
                    /*
                     * We have found the offending cell.
                     * Now we must discard it, mark the
                     * leaf cell dirty and do whatever is
                     * needed to repair the parent list
                     * from damage.
                     */
                    HvReleaseCell(Hive, LeafCell);
                    HvMarkCellDirty(Hive, LeafCell, FALSE);
                    CmpRemoveIndexKeyCell(Leaf, LeafCountIndex);
                    DPRINT1("CmpRemoveSubkeyInRoot(): The offending key cell has BEEN FOUND in leaf (leaf %p, index %lu)!\n",
                            Leaf, LeafCountIndex);
                    return TRUE;
                }
            }
        }
    }

    /*
     * We have searched everywhere but we couldn't
     * hunt the offending target key cell.
     */
    DPRINT1("CmpRemoveSubkeyInRoot(): No target key has been found to remove!\n");
    return FALSE;
}

/**
 * @brief
 * Removes the offending subkey from a leaf index.
 *
 * @param[in] Hive
 * A pointer to a hive.
 *
 * @param[in] KeyNode
 * A pointer to a key node of the parent. This node is
 * used by the function to mark the whole subkeys list
 * of the parent dirty.
 *
 * @param[in] Leaf
 * A pointer to a leaf key index of which the offending
 * subkey is to be removed from.
 *
 * @param[in] TargetKey
 * The offending target subkey to remove.
 *
 * @return
 * Returns TRUE if the function successfully removed the target
 * key, FALSE otherwise.
 */
BOOLEAN
CmpRemoveSubKeyInLeaf(
    _In_ PHHIVE Hive,
    _In_ PCM_KEY_NODE KeyNode,
    _In_ PCM_KEY_INDEX Leaf,
    _In_ HCELL_INDEX TargetKey)
{
    PCM_KEY_FAST_INDEX FastIndex;
    ULONG LeafIndex;

    /* Loop the leaf index */
    for (LeafIndex = 0; LeafIndex < Leaf->Count; LeafIndex++)
    {
        /*
         * Check if the main leaf is a fast
         * leaf or a hash one.
         */
        if ((Leaf->Signature == CM_KEY_FAST_LEAF) ||
            (Leaf->Signature == CM_KEY_HASH_LEAF))
        {
            /* It is one of the two, get the fast index */
            FastIndex = (PCM_KEY_FAST_INDEX)Leaf;

            /*
             * Is the subkey cell from the fast
             * index the one we're actually
             * searching?
             */
            if (FastIndex->List[LeafIndex].Cell == TargetKey)
            {
                /*
                 * We have found it. Now we must
                 * discard it and mark the node
                 * as dirty.
                 */
                HvMarkCellDirty(Hive, KeyNode->SubKeyLists[Stable], FALSE);
                CmpRemoveFastIndexKeyCell(FastIndex, LeafIndex, FALSE);

                /*
                 * Since this fast index actually came from the
                 * actual leaf index itself, just update its count
                 * rather than that of the fast index.
                 */
                Leaf->Count--;
                DPRINT1("CmpRemoveSubKeyInLeaf(): The offending key cell has BEEN FOUND in fast index (fast index %p, leaf index %lu)!\n",
                        FastIndex, LeafIndex);
                return TRUE;
            }
        }
        else
        {
            /*
             * The leaf is neither of the two. The offending
             * cell must come directly from the normal leaf
             * at this point.
             */
            if (Leaf->List[LeafIndex] == TargetKey)
            {
                /*
                 * We have found it. Now we must
                 * discard it and mark the node as
                 * dirty.
                 */
                HvMarkCellDirty(Hive, KeyNode->SubKeyLists[Stable], FALSE);
                CmpRemoveIndexKeyCell(Leaf, LeafIndex);
                DPRINT1("CmpRemoveSubKeyInLeaf(): The offending key cell has BEEN FOUND in leaf (leaf %p, index %lu)!\n",
                        Leaf, LeafIndex);
                return TRUE;
            }
        }
    }

    /*
     * We have searched everywhere but we couldn't
     * hunt the offending target key cell.
     */
    DPRINT1("CmpRemoveSubKeyInLeaf(): No target key has been found to remove!\n");
    return FALSE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Checks if self healing is permitted by the kernel and/or
 * bootloader.
 *
 * @return
 * Returns TRUE if self healing is possible, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmIsSelfHealEnabled(VOID)
{
    PAGED_CODE();

    /* Does the kernel permit self healing? */
    if (CmSelfHeal || (CmpBootType & HBOOT_TYPE_SELF_HEAL))
        return TRUE;

    /* We don't */
    return FALSE;
}

/**
 * @brief
 * Repairs the parent key from damage by removing the
 * offending subkey cell.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] TargetKey
 * The offending target cell to remove from the parent.
 *
 * @param[in] ParentKey
 * The damaged parent key cell to heal.
 *
 * @return
 * Returns TRUE if the function successfully healed the parent
 * key, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairParentKey(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX TargetKey,
    _In_ HCELL_INDEX ParentKey)
{
    PCM_KEY_INDEX KeyIndex;
    PCM_KEY_NODE KeyNode;
    BOOLEAN ParentRepaired;

    PAGED_CODE();

    DPRINT1("CmpRepairParentKey(): HIVE DAMAGED!!! Attempting repair...\n");

    /* The target key must NEVER be NIL! */
    ASSERT(TargetKey != HCELL_NIL);

    /* Assume the parent hasn't been repaired yet */
    ParentRepaired = FALSE;

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairParentKey(): Self healing not possible!\n");
        return ParentRepaired;
    }

    /* Obtain a node from the parent */
    KeyNode = (PCM_KEY_NODE)HvGetCell(Hive, ParentKey);
    if (!KeyNode)
    {
        /* Not getting the node here is bad, bail out */
        DPRINT1("CmpRepairSubkey(): Couldn't get the parent key node!\n");
        return ParentRepaired;
    }

    /* Obtain the index as well since we got the parent node */
    KeyIndex = (PCM_KEY_INDEX)HvGetCell(Hive, KeyNode->SubKeyLists[Stable]);
    if (!KeyIndex)
    {
        /* Man this key is so kaput it turned into ashes, bail out */
        DPRINT1("CmpRepairSubkey(): Couldn't get the key index from parent node!\n");
        HvReleaseCell(Hive, ParentKey);
        return ParentRepaired;
    }

    /* Check if this is a root */
    if (KeyIndex->Signature == CM_KEY_INDEX_ROOT)
    {
        /* It is, call the specific helper to discard the damaged key down the root */
        ParentRepaired = CmpRemoveSubkeyInRoot(Hive,
                                               KeyIndex,
                                               TargetKey);
    }
    else if ((KeyIndex->Signature == CM_KEY_INDEX_LEAF) ||
             (KeyIndex->Signature == CM_KEY_FAST_LEAF) ||
             (KeyIndex->Signature == CM_KEY_HASH_LEAF))
    {
        /* Otherwise call the leaf helper */
        ParentRepaired = CmpRemoveSubKeyInLeaf(Hive,
                                               KeyNode,
                                               KeyIndex,
                                               TargetKey);
    }
    else
    {
        /*
         * Generally CmCheckRegistry detects if a key index
         * in the subkeys list is totally broken (we understand
         * that if its signature is not root or leaf) and it will
         * purge the whole subkeys list in such cases. With that
         * being said, we should never reach this code path. But
         * if for whatever reason we reach here then something
         * is seriously wrong.
         */
        DPRINT1("CmpRepairParentKey(): The key index signature is invalid (KeyIndex->Signature == %lu)!", KeyIndex->Signature);
        ASSERT(FALSE);
    }

    /*
     * If we successfully removed the offending key
     * cell mark down the parent as dirty and punt down
     * the subkey count as well. Mark the hive as in
     * self heal mode as well.
     */
    if (ParentRepaired)
    {
        HvMarkCellDirty(Hive, ParentKey, FALSE);
        KeyNode->SubKeyCounts[Stable]--;
        Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
        DPRINT1("CmpRepairParentKey(): The subkey has been removed, the parent is now repaired!\n");
    }

    HvReleaseCell(Hive, KeyNode->SubKeyLists[Stable]);
    HvReleaseCell(Hive, ParentKey);
    return ParentRepaired;
}

/**
 * @brief
 * Repairs the parent of the node from damage due
 * to parent cell and parent node incosistency.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @param[in] ParentCell
 * The sane parent cell which is used by the
 * function for new parent node assignment.
 *
 * @param[in,out] CellData
 * The cell data of the current cell of which
 * its parent node is to be repaired.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * parent node, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairParentNode(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _In_ HCELL_INDEX ParentCell,
    _Inout_ PCELL_DATA CellData)
{
    PAGED_CODE();

    DPRINT1("CmpRepairParentNode(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairParentNode(): Self healing not possible!\n");
        return FALSE;
    }

    /*
     * Mark the cell where we got the actual
     * cell data as dirty and fix the node.
     */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    CellData->u.KeyNode.Parent = ParentCell;

    /* Mark the hive as in self healing mode since we repaired it */
    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    return TRUE;
}

/**
 * @brief
 * Repairs the key node signature from damage
 * due to signature corruption.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @param[in,out] CellData
 * The cell data of the current cell of which
 * its signature is to be repaired.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * key node signature, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairKeyNodeSignature(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData)
{
    PAGED_CODE();

    DPRINT1("CmpRepairKeyNodeSignature(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairKeyNodeSignature(): Self healing not possible!\n");
        return FALSE;
    }

    /*
     * Mark the cell where we got the actual
     * cell data as dirty and fix the key signature.
     */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    CellData->u.KeyNode.Signature = CM_KEY_NODE_SIGNATURE;

    /* Mark the hive as in self healing mode since we repaired it */
    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    return TRUE;
}

/**
 * @brief
 * Repairs the class from damage due to class
 * corruption within the node key.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @param[in,out] CellData
 * The cell data of the current cell of which
 * its class is to be repaired.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * class of node key, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairClassOfNodeKey(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData)
{
    PAGED_CODE();

    DPRINT1("CmpRepairClassOfNodeKey(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairClassOfNodeKey(): Self healing not possible!\n");
        return FALSE;
    }

    /*
     * Mark the cell where we got the actual
     * cell data as dirty and fix the class field
     * of key node.
     */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    CellData->u.KeyNode.Class = HCELL_NIL;
    CellData->u.KeyNode.ClassLength = 0;

    /* Mark the hive as in self healing mode since we repaired it */
    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    return TRUE;
}

/**
 * @brief
 * Repairs the value list count of key due to
 * corruption. The process involves by removing
 * one damaged value less from the list.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @param[in] ListCountIndex
 * The value count index which points to the actual
 * value in the list to be removed.
 *
 * @param[in,out] ValueListData
 * The value list cell data containing the actual list
 * of which the damaged is to be removed from.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * value list count, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairValueListCount(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _In_ ULONG ListCountIndex,
    _Inout_ PCELL_DATA ValueListData)
{
    PCM_KEY_NODE ValueListNode;

    PAGED_CODE();

    DPRINT1("CmpRepairValueListCount(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairValueListCount(): Self healing not possible!\n");
        return FALSE;
    }

    /*
     * Obtain a node from the cell that we mark it as dirty.
     * The node is that of the current cell of which its
     * value list is being validated.
     */
    ValueListNode = (PCM_KEY_NODE)HvGetCell(Hive, CurrentCell);
    if (!ValueListNode)
    {
        DPRINT1("CmpRepairValueListCount(): Could not get a node from the current cell!\n");
        return FALSE;
    }

    /*
     * Mark the current cell and value list as dirty
     * as we will be making changes onto them.
     */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    HvMarkCellDirty(Hive, ValueListNode->ValueList.List, FALSE);

    /*
     * Now remove the value from the list and mark the
     * hive as in self healing mode.
     */
    CmpRemoveValueFromValueList(ValueListNode, ValueListData, ListCountIndex);
    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    HvReleaseCell(Hive, CurrentCell);
    return TRUE;
}

/**
 * @brief
 * Repairs the value list due to corruption. The
 * process involes by purging the whole damaged
 * list.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * value list, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairValueList(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell)
{
    PCM_KEY_NODE ValueListNode;

    PAGED_CODE();

    DPRINT1("CmpRepairValueList(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairValueList(): Self healing not possible!\n");
        return FALSE;
    }

    /* Obtain a node */
    ValueListNode = (PCM_KEY_NODE)HvGetCell(Hive, CurrentCell);
    if (!ValueListNode)
    {
        DPRINT1("CmpRepairValueList(): Could not get a node from the current cell!\n");
        return FALSE;
    }

    /* Purge out the whole list */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    ValueListNode->ValueList.List = HCELL_NIL;
    ValueListNode->ValueList.Count = 0;

    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    HvReleaseCell(Hive, CurrentCell);
    return TRUE;
}

/**
 * @brief
 * Repairs the subkey list count due to corruption.
 * The process involves by fixing the count itself
 * with a sane count.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @param[in] Count
 * The healthy count which is used by the function
 * to fix the subkeys list count.
 *
 * @param[in,out] CellData
 * The cell data of the current cell of which its
 * subkeys list is to be fixed.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * subkeys list count, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairSubKeyCounts(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _In_ ULONG Count,
    _Inout_ PCELL_DATA CellData)
{
    PAGED_CODE();

    DPRINT1("CmpRepairSubKeyCounts(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairSubKeyCounts(): Self healing not possible!\n");
        return FALSE;
    }

    /*
     * Mark the cell where we got the actual
     * cell data as dirty and fix the subkey
     * counts.
     */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    CellData->u.KeyNode.SubKeyCounts[Stable] = Count;

    /* Mark the hive as in self healing mode since we repaired it */
    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    return TRUE;
}

/**
 * @brief
 * Repairs the subkey list due to corruption. The process
 * involves by purging the whole damaged subkeys list.
 *
 * @param[in,out] Hive
 * A pointer to a hive.
 *
 * @param[in] CurrentCell
 * The current cell to be marked as dirty.
 *
 * @param[in,out] CellData
 * The cell data of the current cell of which its
 * subkeys list is to be fixed.
 *
 * @return
 * Returns TRUE if the function successfully healed the
 * subkeys list, FALSE otherwise.
 */
BOOLEAN
CMAPI
CmpRepairSubKeyList(
    _Inout_ PHHIVE Hive,
    _In_ HCELL_INDEX CurrentCell,
    _Inout_ PCELL_DATA CellData)
{
    PAGED_CODE();

    DPRINT1("CmpRepairSubKeyList(): HIVE DAMAGED!!! Attempting repair...\n");

    /* Is self healing possible? */
    if (!CmIsSelfHealEnabled())
    {
        DPRINT1("CmpRepairSubKeyList(): Self healing not possible!\n");
        return FALSE;
    }

    /*
     * Mark the cell where we got the actual
     * cell data as dirty and fix the subkey
     * list.
     */
    HvMarkCellDirty(Hive, CurrentCell, FALSE);
    CellData->u.KeyNode.SubKeyLists[Stable] = HCELL_NIL;
    CellData->u.KeyNode.SubKeyCounts[Stable] = 0;

    /* Mark the hive as in self healing mode since we repaired it */
    Hive->BaseBlock->BootType |= HBOOT_TYPE_SELF_HEAL;
    return TRUE;
}

/* EOF */
