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
    EXTERN P0BootStack
    EXTERN KiArmVectorTable
    EXTERN HoldSystem
    NESTED_ENTRY KiSystemStartupLOC
    PROLOG_END KiSystemStartupLOC

    /* Put us in FIQ mode, set IRQ stack */
    mrs r3, cpsr
    orr r3, r1, #CPSRM_FIQ
    //msr cpsr, r3
    msr cpsr_fc, r3
    ldr sp, =P0BootStack

    /* Repeat for IRQ mode */
    mov r3, #CPSRM_INT
    msr cpsr_c, r3
    ldr sp, =P0BootStack

    /* Put us in ABORT mode and set the panic stack */
    mov r3, #CPSRM_ABT
    msr cpsr_c, r3
    ldr sp, =P0BootStack

    /* Repeat for UDF (Undefined) mode */
    mov r3, #CPSRM_UDF
    msr cpsr_c, r3
    ldr sp, =P0BootStack

    /* Put us into SVC (Supervisor) mode and set the kernel stack */
    mov r3, #CPSRM_SVC
    msr cpsr_c, r3
    ldr sp, =P0BootStack

    /* Go to C code */
    bx lr
    NESTED_END KiSystemStartupLOC



    NESTED_ENTRY pArmControlRegisterGet
    PROLOG_END pArmControlRegisterGet
    mrc p15, 0, r0, c1, c0, 0
    bx lr
    NESTED_END pArmControlRegisterGet





    NESTED_ENTRY  pArmControlRegisterSet
    PROLOG_END pArmControlRegisterSet
    mcr p15, 0, r0, c1, c0, 0
    bx lr
    NESTED_END pArmControlRegisterSet
    END
/* EOF */
