/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY atan
    // atan(x) ~= x / (1 + 0.25*x*x)
    fmul d1, d0, d0
    fmov d2, #0.25
    fmul d1, d1, d2
    fmov d2, #1.0
    fadd d1, d1, d2
    fdiv d0, d0, d1
    ret
    LEAF_END atan

    END
/* EOF */