/*
 * PROJECT:     ReactOS Arbitrartion Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Entry allocation pipeline
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntifs.h>
#include <ndk/rtlfuncs.h>
#include "arbiter.h"

#define NDEBUG
#include <debug.h>

/* ENTRY PIPELINE *************************************************************/

/**
 * @brief
 * Per-entry hook invoked before each placement attempt. The
 * library default does nothing and succeeds.
 *
 * @param[in] Arbiter
 * The arbiter instance placing the entry.
 *
 * @param[in,out] ArbState
 * The allocation state of the entry about to be placed.
 *
 * @return
 * Returns STATUS_SUCCESS.
 *
 * @remarks
 * Arbiters that need to massage a requirement before arbitration
 * install their own PreprocessEntry; the root arbiters keep this
 * default.
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
ArbiterLibPreprocessEntry(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE ArbState)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Arbiter);
    UNREFERENCED_PARAMETER(ArbState);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Orders an arbitration list most-constrained first, so fixed
 * requirements are placed before flexible ones can steal their
 * ranges.
 *
 * @param[in,out] ArbitrationList
 * The list of ARBITER_LIST_ENTRY nodes to sort, keyed on the
 * WorkSpace constrainedness score in ascending order. The sort is
 * stable: equally scored entries keep their relative order.
 *
 * @return
 * Returns STATUS_SUCCESS.
 *
 * @remarks
 * Without this ordering the backtracking solver degrades toward
 * exponential behavior on real hardware: a wide requirement placed
 * first keeps landing on the one range a later fixed requirement
 * must have, forcing a backtrack for every such collision.
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
ArbiterLibSortArbitrationList(
    _Inout_ PLIST_ENTRY ArbitrationList)
{
    LIST_ENTRY Sorted;

    PAGED_CODE();

    InitializeListHead(&Sorted);

    while (!IsListEmpty(ArbitrationList))
    {
        PLIST_ENTRY ListEntry = RemoveHeadList(ArbitrationList);
        PARBITER_LIST_ENTRY Entry = CONTAINING_RECORD(ListEntry, ARBITER_LIST_ENTRY, ListEntry);
        PLIST_ENTRY Position;

        for (Position = Sorted.Flink; Position != &Sorted; Position = Position->Flink)
        {
            if (CONTAINING_RECORD(Position, ARBITER_LIST_ENTRY, ListEntry)->WorkSpace >
                Entry->WorkSpace)
            {
                break;
            }
        }

        InsertTailList(Position, ListEntry);  /* insert before Position (stable) */
    }

    if (!IsListEmpty(&Sorted))
    {
        ArbitrationList->Flink = Sorted.Flink;
        Sorted.Flink->Blink = ArbitrationList;
        ArbitrationList->Blink = Sorted.Blink;
        Sorted.Blink->Flink = ArbitrationList;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Places the whole stack of entries with backtracking: when an
 * entry cannot be placed, the previous entry's tentative choice is
 * withdrawn and varied, first lower within its window, then via
 * its next window or alternative.
 *
 * @param[in] Arbiter
 * The arbiter instance whose walker callbacks
 * (GetNextAllocationRange, FindSuitableRange, AddAllocation,
 * BacktrackAllocation) drive each placement.
 *
 * @param[in,out] ArbState
 * The first ARBITER_ALLOCATION_STATE of the allocation stack, one
 * per entry in most-constrained-first order, terminated by a state
 * whose Entry is NULL.
 *
 * @return
 * Returns STATUS_SUCCESS once every entry has a placement (the
 * results are packed into each entry's Assignment and Result),
 * STATUS_UNSUCCESSFUL if even the first entry has no solution, or
 * a PreprocessEntry / PackResource failure status.
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
ArbiterLibAllocateEntry(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE ArbState)
{
    PARBITER_ALLOCATION_STATE Current = ArbState;
    BOOLEAN Backtracking = FALSE;
    NTSTATUS Status;

    PAGED_CODE();

    while (Current >= ArbState && Current->Entry != NULL)
    {
        BOOLEAN Found = FALSE;
        BOOLEAN RetrySameRange = FALSE;

        Status = Arbiter->PreprocessEntry(Arbiter, Current);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Backtracking)
        {
            PARBITER_ALTERNATIVE FailedAlternative = Current->CurrentAlternative;

            /*
             * The entry after this one could not be placed.  Withdraw this
             * entry's tentative choice and vary it: first lower within the same
             * window, then via the next window / alternative.
             */
            (Current + 1)->CurrentAlternative = NULL;
            Backtracking = FALSE;

            if (FailedAlternative == NULL || FailedAlternative->Length == 0)
                goto Backtrack;  /* nothing to vary here */

            Arbiter->BacktrackAllocation(Arbiter, Current);

            if (Current->Start > Current->CurrentMinimum)
            {
                Current->CurrentMaximum = Current->Start - 1;
                RetrySameRange = TRUE;
            }
        }

        for (;;)
        {
            if (!RetrySameRange)
            {
                if (!Arbiter->GetNextAllocationRange(Arbiter, Current))
                    break;
            }
            RetrySameRange = FALSE;

            if (Arbiter->FindSuitableRange(Arbiter, Current))
            {
                Found = TRUE;
                break;
            }
        }

        if (Found)
        {
            if (Current->CurrentAlternative->Length != 0)
                Arbiter->AddAllocation(Arbiter, Current);
            else
                Current->Entry->Result = ArbiterResultNullRequest;

            Current++;
            continue;
        }

Backtrack:
        if (Current == ArbState)
        {
            /* Even the first entry has no solution. */
            if (Current->Entry != NULL)
                Current->Entry->Result = ArbiterResultExternalConflict;
            return STATUS_UNSUCCESSFUL;
        }
        Backtracking = TRUE;
        Current--;
    }

    /* Complete solution found - report it back to the requesters. */
    for (Current = ArbState; Current->Entry != NULL; ++Current)
    {
        Current->Entry->SelectedAlternative = Current->CurrentAlternative->Descriptor;

        if (Current->Entry->Assignment != NULL && Arbiter->PackResource != NULL)
        {
            Status = Arbiter->PackResource(Current->CurrentAlternative->Descriptor,
                                           Current->Start,
                                           Current->Entry->Assignment);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        if (Current->Entry->Result != ArbiterResultNullRequest)
            Current->Entry->Result = ArbiterResultSuccess;
    }

    return STATUS_SUCCESS;
}
