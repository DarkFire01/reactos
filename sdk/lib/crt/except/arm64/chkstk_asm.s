
/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/
    TEXTAREA

    LEAF_ENTRY __chkstk
    /* TODO: add an assert fail call, as this is unimplemented */
    ret
    LEAF_END __chkstk

    LEAF_ENTRY __alloca_probe
    /* TODO: add an assert fail call, as this is unimplemented */
    ret
    LEAF_END __alloca_probe
    
    LEAF_ENTRY _local_unwind
    /* TODO: add an assert fail call, as this is unimplemented */
    ret
    LEAF_END _local_unwind

    END
/* EOF */
