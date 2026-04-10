/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY __rt_udiv64
    udiv x0, x0, x1
    ret
    LEAF_END __rt_udiv64

    END
/* EOF */