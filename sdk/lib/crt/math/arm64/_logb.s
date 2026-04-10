/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY _logb
    // Extract exponent from IEEE-754 double in d0
    fmov x1, d0
    ubfx x2, x1, #52, #11

    // Zero / denorm -> -inf
    cbnz x2, %F1
    movz x3, #0x0000
    movk x3, #0x0000, lsl #16
    movk x3, #0x0000, lsl #32
    movk x3, #0xfff0, lsl #48
    fmov d0, x3
    ret

1
    // Inf / NaN -> +inf
    mov x3, #2047
    cmp x2, x3
    bne %F2
    movz x3, #0x0000
    movk x3, #0x0000, lsl #16
    movk x3, #0x0000, lsl #32
    movk x3, #0x7ff0, lsl #48
    fmov d0, x3
    ret

2
    sub x2, x2, #1023
    scvtf d0, x2
    ret
    LEAF_END _logb

    END
/* EOF */