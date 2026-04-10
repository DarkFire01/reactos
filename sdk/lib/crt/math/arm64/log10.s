/* INCLUDES ******************************************************************/

#include <kxarm64.h>

    IMPORT log

/* CODE **********************************************************************/

    TEXTAREA

    NESTED_ENTRY log10
    str lr, [sp, #-16]!
    PROLOG_END

    bl log

    // log10(e)
    movz x1, #0xe50e
    movk x1, #0x1526, lsl #16
    movk x1, #0xcb7b, lsl #32
    movk x1, #0x3fdb, lsl #48
    fmov d1, x1
    fmul d0, d0, d1

    ldr lr, [sp], #16
    ret
    NESTED_END log10

    END
/* EOF */