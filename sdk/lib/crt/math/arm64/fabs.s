/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY fabs
    fabs d0, d0
    ret
    LEAF_END fabs

    END
/* EOF */