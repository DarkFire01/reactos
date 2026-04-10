/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY ldexp
    // d0 = value, w0 = exp
    cmp w0, #0
    beq %F9

    mov w1, w0
    cmp w1, #0
    blt %F4

1
    cbz w1, %F9
    cmp w1, #60
    ble %F2
    mov w2, #60
    b %F3
2
    mov w2, w1
3
    mov x3, #1
    lsl x3, x3, x2
    fmov d1, x3
    fmul d0, d0, d1
    sub w1, w1, w2
    b %B1

4
    // negative exponent path
    neg w1, w1
5
    cbz w1, %F9
    cmp w1, #60
    ble %F6
    mov w2, #60
    b %F7
6
    mov w2, w1
7
    mov x3, #1
    lsl x3, x3, x2
    fmov d1, x3
    fdiv d0, d0, d1
    sub w1, w1, w2
    b %B5

9
    ret
    LEAF_END ldexp

    END
/* EOF */