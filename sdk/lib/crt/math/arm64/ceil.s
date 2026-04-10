/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY ceil
    frintp d0, d0
    ret
    LEAF_END ceil

    END
/* EOF */