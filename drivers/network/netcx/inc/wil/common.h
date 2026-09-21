/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The Windows Implementation Library flag macros
 *
 * Only the WI_ flag manipulation surface is here. The wil:: RAII types live in
 * wil/resource.h and are a separate matter.
 *
 * Every macro parenthesises its arguments and evaluates each one once, because
 * callers pass expressions with side effects.
 */

#pragma once

#include <wil/wistd_type_traits.h>

/* Tests. */
#define WI_IsFlagSet(Value, Flag)           (((Value) & (Flag)) != 0)
#define WI_IsFlagClear(Value, Flag)         (((Value) & (Flag)) == 0)
#define WI_IsAnyFlagSet(Value, Mask)        (((Value) & (Mask)) != 0)
#define WI_AreAllFlagsClear(Value, Mask)    (((Value) & (Mask)) == 0)

/*
 * True when the value has exactly one of the bits in the mask set. Clearing
 * the lowest set bit leaves zero only when there was a single bit to begin
 * with, which also rejects the no bits at all case.
 */
#define WI_IsSingleFlagSetInMask(Value, Mask) \
    (((Value) & (Mask)) != 0 && ((((Value) & (Mask)) & (((Value) & (Mask)) - 1)) == 0))

/* Mutations. */
#define WI_SetFlag(Value, Flag)             ((Value) |= (Flag))
#define WI_ClearFlag(Value, Flag)           ((Value) &= ~(Flag))
#define WI_SetAllFlags(Value, Mask)         ((Value) |= (Mask))
#define WI_ClearAllFlags(Value, Mask)       ((Value) &= ~(Mask))

#define WI_SetFlagIf(Value, Flag, Condition) \
    do { if (Condition) { WI_SetFlag(Value, Flag); } } while ((void)0, 0)

#define WI_UpdateFlag(Value, Flag, Condition) \
    do { if (Condition) { WI_SetFlag(Value, Flag); } \
         else { WI_ClearFlag(Value, Flag); } } while ((void)0, 0)
