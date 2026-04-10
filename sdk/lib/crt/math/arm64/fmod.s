/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY fmod
    // d0 = x, d1 = y
    fcmp d1, #0.0
    bne %F1
    // NaN for division by zero
    fmov d2, xzr
    fdiv d0, d2, d2
    ret
1
    fdiv d2, d0, d1
    frintz d2, d2
    fmul d2, d2, d1
    fsub d0, d0, d2
    ret
    LEAF_END fmod

    END
/* EOF */