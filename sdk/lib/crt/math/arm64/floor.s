/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY floor
    frintm d0, d0
    ret
    LEAF_END floor

    END
/* EOF */