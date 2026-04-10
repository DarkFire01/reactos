/* INCLUDES ******************************************************************/

#include <kxarm64.h>

    IMPORT log
    IMPORT exp

/* CODE **********************************************************************/

    TEXTAREA

    NESTED_ENTRY pow
    sub sp, sp, #32
    str lr, [sp, #24]
    str d0, [sp, #0]
    str d1, [sp, #8]
    PROLOG_END

    // y == 0 => 1
    fcmp d1, #0.0
    bne %F1
    fmov d0, #1.0
    b %F9

1
    // x == 0 => 0 for y>0, +inf otherwise
    fcmp d0, #0.0
    bne %F2
    fcmp d1, #0.0
    ble %F3
    fmov d0, xzr
    b %F9

3
    movz x2, #0x0000
    movk x2, #0x0000, lsl #16
    movk x2, #0x0000, lsl #32
    movk x2, #0x7ff0, lsl #48
    fmov d0, x2
    b %F9

2
    // exp(y * log(x))
    bl log
    ldr d1, [sp, #8]
    fmul d0, d0, d1
    bl exp

9
    ldr lr, [sp, #24]
    add sp, sp, #32
    ret
    NESTED_END pow

    END
/* EOF */