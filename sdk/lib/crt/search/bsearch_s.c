/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     bsearch_s for the kernel runtime
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdlib.h>

typedef int (__cdecl *BSEARCH_S_COMPARE)(void *Context, const void *Key, const void *Element);

/**
 * @brief
 * Searches a sorted array with a comparison that receives a caller context.
 *
 * @param[in] Key
 * What to look for.
 *
 * @param[in] Base
 * The array, may be NULL when it is empty.
 *
 * @param[in] Count
 * Its element count.
 *
 * @param[in] Width
 * The size of one element.
 *
 * @param[in] Compare
 * Called with Context, Key and an element, returning their order.
 *
 * @param[in] Context
 * Passed through to Compare.
 *
 * @return
 * The matching element, or NULL when there is none or a parameter is bad.
 */
void *
__cdecl
bsearch_s(
    _In_ const void *Key,
    _In_reads_bytes_opt_(Count * Width) const void *Base,
    _In_ size_t Count,
    _In_ size_t Width,
    _In_ BSEARCH_S_COMPARE Compare,
    _In_opt_ void *Context)
{
    const char *Low, *High, *Middle;
    size_t Half, Below;
    int Order;

    if ((Base == NULL && Count != 0) || Width == 0 || Compare == NULL)
        return NULL;

    if (Count == 0)
        return NULL;

    Low = Base;
    High = (const char *)Base + (Count - 1) * Width;

    while (Low <= High)
    {
        Half = Count / 2;
        if (Half == 0)
        {
            /* One element left */
            if (Compare(Context, Key, Low) == 0)
                return (void *)Low;
            return NULL;
        }

        /* An even count leaves one more element above the probe than below it */
        Below = (Count & 1) ? Half : Half - 1;
        Middle = Low + Below * Width;

        Order = Compare(Context, Key, Middle);
        if (Order == 0)
            return (void *)Middle;

        if (Order < 0)
        {
            High = Middle - Width;
            Count = Below;
        }
        else
        {
            Low = Middle + Width;
            Count = Half;
        }
    }

    return NULL;
}
