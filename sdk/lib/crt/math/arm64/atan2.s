
/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY atan2
    // d0 = y, d1 = x
    fmov d2, d0
    fcmp d1, #0.0
    bgt %F1
    blt %F2

    // x == 0
    fcmp d2, #0.0
    bgt %F3
    blt %F4
    fmov d0, xzr
    ret

1
    // x > 0: atan(y/x)
    fdiv d0, d2, d1
    fmul d3, d0, d0
    fmov d4, #0.25
    fmul d3, d3, d4
    fmov d4, #1.0
    fadd d3, d3, d4
    fdiv d0, d0, d3
    ret

2
    // x < 0: atan(y/x) +/- pi
    fdiv d0, d2, d1
    fmul d3, d0, d0
    fmov d4, #0.25
    fmul d3, d3, d4
    fmov d4, #1.0
    fadd d3, d3, d4
    fdiv d0, d0, d3

    movz x5, #0x2d18
    movk x5, #0x5444, lsl #16
    movk x5, #0x21fb, lsl #32
    movk x5, #0x4009, lsl #48
    fmov d5, x5 // pi

    fcmp d2, #0.0
    bge %F5
    fsub d0, d0, d5
    ret
5
    fadd d0, d0, d5
    ret

3
    // +pi/2
    movz x5, #0x2d18
    movk x5, #0x5444, lsl #16
    movk x5, #0x21fb, lsl #32
    movk x5, #0x3ff9, lsl #48
    fmov d0, x5
    ret

4
    // -pi/2
    movz x5, #0x2d18
    movk x5, #0x5444, lsl #16
    movk x5, #0x21fb, lsl #32
    movk x5, #0xbff9, lsl #48
    fmov d0, x5
    ret
    LEAF_END atan2

    END
/* EOF */
