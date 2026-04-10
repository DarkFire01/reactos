/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY __rt_sdiv64
    sdiv x0, x0, x1
    ret
    LEAF_END __rt_sdiv64

    END
/* EOF */