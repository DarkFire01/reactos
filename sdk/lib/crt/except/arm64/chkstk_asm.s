
/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/
    TEXTAREA

    LEAF_ENTRY __chkstk
    //
    // ARM64 __chkstk implementation  
    // On entry: x15 = size to allocate (in bytes)
    // Probes stack pages to ensure they're committed
    // Does NOT modify SP or any registers except x16, x17
    //
    
    // x16 = PAGE_SIZE (4096)
    mov    x16, #4096
    lsl    x16, x16, #0         // x16 = 4096
    
    // If size < PAGE_SIZE, nothing to probe
    cmp    x15, x16
    ble    %F2
    
    // x17 = target address (SP - size)
    sub    x17, sp, x15
    
    // x16 = probe pointer, start at (SP - PAGE_SIZE)
    mov    x15, #4096
    lsl    x15, x15, #0         // x15 = 4096  
    sub    x16, sp, x15
    
1   // ProbeLoop: touch each page
    str    xzr, [x16]           // Touch the page
    sub    x16, x16, x15        // x16 -= PAGE_SIZE
    cmp    x16, x17             // Compare to target
    bge    %B1                  // Continue if x16 >= target
    
2   // Done
    ret
    LEAF_END __chkstk

    LEAF_ENTRY __alloca_probe
    //
    // __alloca_probe is same as __chkstk on ARM64
    //
    b       __chkstk
    LEAF_END __alloca_probe

    END
/* EOF */
