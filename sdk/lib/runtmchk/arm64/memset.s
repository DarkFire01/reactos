/*
 * PROJECT:     MSVC runtime check support library
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * PURPOSE:     Fallback memset for the /RTCs frame fill on ARM64
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    TEXTAREA

//
// The compiler calls this from a prologue before it saves the incoming
// arguments past x2 and x8, the indirect result register, so only x0 to x2
// may change here. The fill runs from the end so x0 stays the destination.
//
// x0 = destination, w1 = fill byte, x2 = count, returns the destination.
//
    LEAF_ENTRY memset_fallback
    cbz     x2, Done
Fill
    sub     x2, x2, #1
    strb    w1, [x0, x2]
    cbnz    x2, Fill
Done
    ret
    LEAF_END memset_fallback

    END
