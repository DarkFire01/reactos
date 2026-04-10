/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY log
    // log(x) ~= 2 * (y + y^3/3 + y^5/5 + y^7/7), y=(x-1)/(x+1)
    fcmp d0, #0.0
    bgt %F1
    bne %F2

    // x == 0 => -inf
    movz x1, #0x0000
    movk x1, #0x0000, lsl #16
    movk x1, #0x0000, lsl #32
    movk x1, #0xfff0, lsl #48
    fmov d0, x1
    ret

2
    // x < 0 => NaN
    fmov d1, xzr
    fdiv d0, d1, d1
    ret

1
    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d2, d2, d3        // y
    fmul d3, d2, d2        // y^2

    fmov d0, d2            // sum = y
    fmul d4, d2, d3        // y^3
    mov w5, #3
    scvtf d5, w5
    fdiv d4, d4, d5
    fadd d0, d0, d4

    fmul d4, d4, d3        // y^5/3
    mov w5, #5
    scvtf d5, w5
    mov w6, #3
    scvtf d6, w6
    fmul d4, d4, d6
    fdiv d4, d4, d5        // y^5/5
    fadd d0, d0, d4

    fmul d4, d4, d3        // y^7/5
    mov w5, #7
    scvtf d5, w5
    mov w6, #5
    scvtf d6, w6
    fmul d4, d4, d6
    fdiv d4, d4, d5        // y^7/7
    fadd d0, d0, d4

    fmov d1, #2.0
    fmul d0, d0, d1
    ret
    LEAF_END log

    END
/* EOF */