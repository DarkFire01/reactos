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

    NESTED_ENTRY KiSystemStartupLOC
    PROLOG_END KiSystemStartupLOC
    /* Print character 'A' to 0x09000000 */
    ldr r0, =0x09000000
    mov r1, #'A'
    str r1, [r0]
    /* Put us in FIQ mode, set IRQ stack */

    mrs r3, cpsr
    orr r3, r1, #CPSRM_FIQ
    //msr cpsr, r3
    msr cpsr_fc, r3
    ldr sp, [a1, #LpbKernelStack]
    ldr r0, =0x09000000
    mov r1, #'B'
    str r1, [r0]
    /* Repeat for IRQ mode */
    mov r3, #CPSRM_INT
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]
    ldr r0, =0x09000000
    mov r1, #'C'
    str r1, [r0]
    /* Put us in ABORT mode and set the panic stack */
    mov r3, #CPSRM_ABT
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]
    ldr r0, =0x09000000
    mov r1, #'D'
    str r1, [r0]
    /* Repeat for UDF (Undefined) mode */
    mov r3, #CPSRM_UDF
    msr cpsr_c, r3
    ldr sp, [a1, #LpbKernelStack]
    ldr r0, =0x09000000
    mov r1, #'E'
    str r1, [r0]
    /* Put us into SVC (Supervisor) mode and set the kernel stack */
    mov r3, #CPSRM_SVC
    msr cpsr_c, r3

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
