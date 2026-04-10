/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY exp
    // exp(x) ~= 1 + x + x^2/2 + x^3/6 + x^4/24 + x^5/120
    fmov d1, #1.0
    fmov d2, d0
    fmul d3, d0, d0
    fmul d4, d3, d0
    fmul d5, d4, d0
    fmul d6, d5, d0

    mov w7, #2
    scvtf d7, w7
    fdiv d3, d3, d7

    mov w7, #6
    scvtf d7, w7
    fdiv d4, d4, d7

    mov w7, #24
    scvtf d7, w7
    fdiv d5, d5, d7

    mov w7, #120
    scvtf d7, w7
    fdiv d6, d6, d7

    fadd d0, d1, d2
    fadd d0, d0, d3
    fadd d0, d0, d4
    fadd d0, d0, d5
    fadd d0, d0, d6
    ret
    LEAF_END exp

    END
/* EOF */