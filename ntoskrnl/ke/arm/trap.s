/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm/trap.s
 * PURPOSE:         Support for exceptions and interrupts on ARM machines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

#include <ksarm.h>

    IMPORT KiUndefinedExceptionHandler
    IMPORT KiSoftwareInterruptHandler
    IMPORT KiPrefetchAbortHandler
    IMPORT KiDataAbortHandler
    IMPORT KiInterruptHandler

    TEXTAREA

    EXPORT KiArmVectorTable
KiArmVectorTable
        b .                                     // Reset
        ldr pc, _KiUndefinedInstructionJump     // Undefined Instruction
        ldr pc, _KiSoftwareInterruptJump        // Software Interrupt
        ldr pc, _KiPrefetchAbortJump            // Prefetch Abort
        ldr pc, _KiDataAbortJump                // Data Abort
        b .                                     // Reserved
        ldr pc, _KiInterruptJump                // Interrupt
        ldr pc, _KiFastInterruptJump            // Fast Interrupt

_KiUndefinedInstructionJump    DCD KiInterruptTemplate
_KiSoftwareInterruptJump       DCD KiInterruptTemplate
_KiPrefetchAbortJump           DCD KiInterruptTemplate
_KiDataAbortJump               DCD KiInterruptTemplate
_KiInterruptJump               DCD KiInterruptTemplate
_KiFastInterruptJump           DCD KiInterruptTemplate

    // Might need to move these to a custom header, when used by HAL as well

    MACRO
    TRAP_PROLOG $Abort
        ldr r0, =0x09000000
        mov r1, #'A'
        str r1, [r0]
    MEND

    MACRO
    SYSCALL_PROLOG $Abort
        ldr r0, =0x09000000
        mov r1, #'A'
        str r1, [r0]
    MEND

    MACRO
    TRAP_EPILOG $SystemCall
        __debugbreak
    MEND

    LEAF_ENTRY KiInterruptTemplate
    PROLOG_END KiInterruptTemplate
    ldr r0, =0x09000000
    mov r1, #'Z'
    str r1, [r0]
    b KiInterruptTemplate
    LEAF_END KiInterruptTemplate


    LEAF_ENTRY KiExceptionExit
    PROLOG_END KiExceptionExit
    ldr r0, =0x09000000
    mov r1, #'Z'
    str r1, [r0]
    b KiExceptionExit
    LEAF_END KiExceptionExit

    END
/* EOF */
