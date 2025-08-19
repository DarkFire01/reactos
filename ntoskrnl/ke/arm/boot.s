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
    
    NESTED_ENTRY KiSystemStartupLoc
    PROLOG_END KiSystemStartupLoc
QEMU_UART_BASE EQU 0x09000000
    ; --- DEBUG BREADCRUMB TRAIL ---
    ldr     r2, =QEMU_UART_BASE
    
    ; Print " 1"
    mov     r1, #' '
    str     r1, [r2]
    mov     r1, #'N'
    str     r1, [r2]

    ; Print " 1"
    mov     r1, #' '
    str     r1, [r2]
    mov     r1, #'T'
    str     r1, [r2]

    /* Put us in FIQ mode, set IRQ stack */
    b .
    mrs r3, cpsr
    orr r3, r1, #CPSRM_FIQ
    //msr cpsr, r3
    msr cpsr_fc, r3
    ldr sp, [a1, #LpbKernelStack]

    /* Repeat for IRQ mode */
    mov r3, #CPSRM_INT
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]

    /* Put us in ABORT mode and set the panic stack */
    mov r3, #CPSRM_ABT
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]

    /* Repeat for UDF (Undefined) mode */
    mov r3, #CPSRM_UDF
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]

    /* Put us into SVC (Supervisor) mode and set the kernel stack */
    mov r3, #CPSRM_SVC
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]

    /* Go to C code */
    b KiInitializeSystem

    NESTED_END KiSystemStartupLoc

    END
/* EOF */
