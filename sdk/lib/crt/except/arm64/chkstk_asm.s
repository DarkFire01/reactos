
/* INCLUDES ******************************************************************/

/* We need one of these first! */
#include <kxarm64.h>
#define PAGE_SIZE 4096
/* CODE **********************************************************************/
    TEXTAREA

    LEAF_ENTRY __chkstk


        lsl    x16, x15, #4
        mov    x17, sp
1:
        ldr    xzr, [x17]

        ret
    LEAF_END __chkstk

    LEAF_ENTRY __alloca_probe
    LEAF_END __alloca_probe

    END
/* EOF */

