/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     qsort_s for the kernel runtime
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdlib.h>

typedef int (__cdecl *QSORT_S_COMPARE)(void *Context, const void *First, const void *Second);

/* Ranges at or below this many elements are finished with a selection sort */
#define QSORT_S_SMALL_RANGE 8

/* The larger half is deferred and the smaller sorted first, so one slot per address bit suffices */
#define QSORT_S_STACK_DEPTH (8 * sizeof(void *) - 2)

static
void
QsortSwap(
    _Inout_updates_bytes_(Width) char *First,
    _Inout_updates_bytes_(Width) char *Second,
    _In_ size_t Width)
{
    char Byte;

    if (First == Second)
        return;

    while (Width--)
    {
        Byte = *First;
        *First++ = *Second;
        *Second++ = Byte;
    }
}

/* Moves the largest element to the end of the range, then shrinks the range by one */
static
void
QsortSelectionSort(
    _In_ char *Low,
    _In_ char *High,
    _In_ size_t Width,
    _In_ QSORT_S_COMPARE Compare,
    _In_opt_ void *Context)
{
    char *Largest, *Probe;

    for (; High > Low; High -= Width)
    {
        Largest = Low;
        for (Probe = Low + Width; Probe <= High; Probe += Width)
        {
            if (Compare(Context, Probe, Largest) > 0)
                Largest = Probe;
        }

        QsortSwap(Largest, High, Width);
    }
}

/**
 * @brief
 * Splits Low..High around a median of three pivot.
 *
 * @param[out] LeftEnd
 * Last element of the part not above the pivot, with the run equal to the pivot left out.
 *
 * @param[out] RightStart
 * First element of the part above the pivot.
 */
static
void
QsortPartition(
    _In_ char *Low,
    _In_ char *High,
    _In_ size_t Width,
    _In_ QSORT_S_COMPARE Compare,
    _In_opt_ void *Context,
    _Out_ char **LeftEnd,
    _Out_ char **RightStart)
{
    char *Pivot, *Up, *Down;

    Pivot = Low + (((size_t)(High - Low) / Width + 1) / 2) * Width;

    /* Order the first, middle and last element so the pivot is their median */
    if (Compare(Context, Low, Pivot) > 0)
        QsortSwap(Low, Pivot, Width);
    if (Compare(Context, Low, High) > 0)
        QsortSwap(Low, High, Width);
    if (Compare(Context, Pivot, High) > 0)
        QsortSwap(Pivot, High, Width);

    Up = Low;
    Down = High;
    for (;;)
    {
        /* Below the pivot Up stops at its slot, past it Up may run to the end */
        if (Up < Pivot)
        {
            do
            {
                Up += Width;
            } while (Up < Pivot && Compare(Context, Up, Pivot) <= 0);
        }

        if (Up >= Pivot)
        {
            do
            {
                Up += Width;
            } while (Up <= High && Compare(Context, Up, Pivot) <= 0);
        }

        do
        {
            Down -= Width;
        } while (Down > Pivot && Compare(Context, Down, Pivot) > 0);

        if (Down < Up)
            break;

        QsortSwap(Up, Down, Width);

        /* The pivot moved with the swap, follow it */
        if (Pivot == Down)
            Pivot = Up;
    }

    /* Elements equal to the pivot are already in place, leave them out of the left part */
    Down += Width;
    if (Pivot < Down)
    {
        do
        {
            Down -= Width;
        } while (Down > Pivot && Compare(Context, Down, Pivot) == 0);
    }

    if (Pivot >= Down)
    {
        do
        {
            Down -= Width;
        } while (Down > Low && Compare(Context, Down, Pivot) == 0);
    }

    *LeftEnd = Down;
    *RightStart = Up;
}

/**
 * @brief
 * Sorts an array with a comparison that receives a caller context.
 *
 * @param[in,out] Base
 * The array, may be NULL when it is empty.
 *
 * @param[in] Count
 * Its element count.
 *
 * @param[in] Width
 * The size of one element.
 *
 * @param[in] Compare
 * Called with Context and two elements, returning their order.
 *
 * @param[in] Context
 * Passed through to Compare.
 */
void
__cdecl
qsort_s(
    _Inout_updates_bytes_opt_(Count * Width) void *Base,
    _In_ size_t Count,
    _In_ size_t Width,
    _In_ QSORT_S_COMPARE Compare,
    _In_opt_ void *Context)
{
    char *LowStack[QSORT_S_STACK_DEPTH];
    char *HighStack[QSORT_S_STACK_DEPTH];
    char *Low, *High, *LeftEnd, *RightStart;
    size_t Depth;

    if ((Base == NULL && Count != 0) || Width == 0 || Compare == NULL)
        return;

    if (Count < 2)
        return;

    Depth = 0;
    Low = Base;
    High = (char *)Base + (Count - 1) * Width;

    for (;;)
    {
        if ((size_t)(High - Low) / Width + 1 <= QSORT_S_SMALL_RANGE)
        {
            QsortSelectionSort(Low, High, Width, Compare, Context);
        }
        else
        {
            QsortPartition(Low, High, Width, Compare, Context, &LeftEnd, &RightStart);

            /* Defer the larger part and keep going with the smaller one */
            if (LeftEnd - Low >= High - RightStart)
            {
                if (Low < LeftEnd)
                {
                    LowStack[Depth] = Low;
                    HighStack[Depth] = LeftEnd;
                    Depth++;
                }

                if (RightStart < High)
                {
                    Low = RightStart;
                    continue;
                }
            }
            else
            {
                if (RightStart < High)
                {
                    LowStack[Depth] = RightStart;
                    HighStack[Depth] = High;
                    Depth++;
                }

                if (Low < LeftEnd)
                {
                    High = LeftEnd;
                    continue;
                }
            }
        }

        if (Depth == 0)
            return;

        Depth--;
        Low = LowStack[Depth];
        High = HighStack[Depth];
    }
}
