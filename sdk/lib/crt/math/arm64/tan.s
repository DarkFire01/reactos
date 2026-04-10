/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY tan
    // tan(x) ~= x + x^3/3 + 2*x^5/15
    fmul d1, d0, d0
    fmul d2, d1, d0
    fmul d3, d2, d1

    mov w4, #3
    scvtf d4, w4
    fdiv d2, d2, d4

    mov w4, #15
    scvtf d4, w4
    fmov d5, #2.0
    fmul d3, d3, d5
    fdiv d3, d3, d4

    fadd d0, d0, d2
    fadd d0, d0, d3
    ret
    LEAF_END tan

    END
/* EOF */