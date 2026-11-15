/*
 * PROJECT:     MSVC runtime check support library
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * PURPOSE:     Fallback memset for the /RTCs frame fill on ARM64
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    TEXTAREA

//
// The compiler calls this from a prologue before it saves x8, the indirect
// result register, so only x0 to x3 may change here.
//
// x0 = destination, w1 = fill byte, x2 = count, returns the destination.
//
    LEAF_ENTRY memset_fallback
    mov     x3, x0
    cbz     x2, Done
Fill
    strb    w1, [x3], #1
    subs    x2, x2, #1
    b.ne    Fill
Done
    ret
    LEAF_END memset_fallback

    END
