/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY __rt_srsh
    cmp x1, #0
    beq %F2

    cmp x1, #63
    bgt %F3

    mov x2, #1
    sub x3, x1, #1
    lsl x2, x2, x3
    add x0, x0, x2
    asr x0, x0, x1
2
    ret

3
    asr x0, x0, #63
    ret
    LEAF_END __rt_srsh

    END
/* EOF */