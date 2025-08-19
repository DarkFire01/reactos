/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm/boot.s
 * PURPOSE:         Implements the kernel entry point for ARM machines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */ 

#include <ksarm.h>

    TEXTAREA

    IMPORT KiInitializeSystem
    
    EXPORT ArmTestStack
ArmTestStack PROC
    ; --- POSITIVE CONFIRMATION ---
    ; If we get here, the MMU is on. Write 'S' to the UART.
    ldr     r3, =0x09000000             ; QEMU PL011 UART Data Register
    mov     r4, #'C'
    str     r4, [r3]
    b ArmTestStack
    ENDP
    

    END
/* EOF */
