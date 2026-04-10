

#ifdef _MSC_VER

    AREA |.text|,ALIGN=11,CODE,READONLY

#include <ksarm64.h>

//
// External symbol declarations
//
    IMPORT  KiSynchronousExceptionC

//
// ARM64 Exception Vector Table
// Must be 2KB (0x800) aligned, each entry gets 32 bytes (8 instructions)
// Exceptions from EL1 to EL1, using SP_EL1
//
    ALIGN 2048
    EXPORT KiExceptionVector32
    EXPORT KiSetVbarEl1

KiExceptionVector32

    // Entry 0: Synchronous exception (EL1 from EL1 with SP_EL0)
    // This shouldn't normally occur - we use SP_EL1
    BRK     #0xF002    ; Software breakpoint
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 1: IRQ (EL1 from EL1 with SP_EL0)
    // This shouldn't normally occur - we use SP_EL1
    BRK     #0xF002    ; Software breakpoint
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 2: FIQ (EL1 from EL1 with SP_EL0)
    // This shouldn't normally occur - we use SP_EL1
    BRK     #0xF002    ; Software breakpoint
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 3: SError (EL1 from EL1 with SP_EL0)
    // This shouldn't normally occur - we use SP_EL1
    BRK     #0xF002    ; Software breakpoint
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 4: Synchronous (EL1 from EL1 with SP_EL1) - Main synchronous handler
    B       KiSynchronousException
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 5: IRQ (EL1 from EL1 with SP_EL1)
    B       KiInterruptException
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 6: FIQ (EL1 from EL1 with SP_EL1)
    B       KiFastInterruptException
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 7: SError (EL1 from EL1 with SP_EL1)
    B       KiSystemErrorException
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 8: Synchronous (EL1 from EL0, AArch64)
    B       KiSynchronousExceptionFromEL0
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 9: IRQ (EL1 from EL0, AArch64)
    B       KiInterruptExceptionFromEL0
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 10: FIQ (EL1 from EL0, AArch64)
    B       KiFastInterruptExceptionFromEL0
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 11: SError (EL1 from EL0, AArch64)
    B       KiSystemErrorExceptionFromEL0
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 12: Synchronous (EL1 from EL0, AArch32) - Not supported
    BRK     #0xF000
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 13: IRQ (EL1 from EL0, AArch32) - Not supported
    BRK     #0xF000
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 14: FIQ (EL1 from EL0, AArch32) - Not supported
    BRK     #0xF000
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    // Entry 15: SError (EL1 from EL0, AArch32) - Not supported
    BRK     #0xF000
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

    //
    // KiSynchronousException - Handle synchronous exceptions from kernel mode
    // This is where BRK instructions are caught
    //
    NESTED_ENTRY KiSynchronousException

    // Preserve volatile argument registers and key context regs before
    // any instruction clobbers them.  FP (x29) and LR (x30) are needed
    // to give WinDbg a valid stack frame; x0-x2 for DebugService2 args.
    MOV     x8, x0          ; save orig x0
    MOV     x9, x1          ; save orig x1
    MOV     x10, x2         ; save orig x2
    MOV     x11, x29        ; save FP (x29) before BL clobbers nothing yet
    MOV     x12, x30        ; save LR (x30) — BL will overwrite x30

    // Read exception control registers.
    MRS     x0, ESR_EL1     ; Exception Status Register
    MRS     x1, ELR_EL1     ; Exception Link Register
    MRS     x2, SPSR_EL1    ; Saved Processor State Register

    // Build arg list for KiSynchronousExceptionC(Esr, Elr, Spsr,
    //   origX0, origX1, origX2, Sp, Fp, Lr).
    // x6 = interrupted SP: captured NOW before we touch the stack.
    MOV     x3, x8          ; origX0
    MOV     x4, x9          ; origX1
    MOV     x5, x10         ; origX2
    MOV     x6, sp          ; interrupted SP (no push has happened yet)
    MOV     x7, x11         ; interrupted FP

    // 9th arg (Lr) must go on the stack per AAPCS64.
    // Allocate 16 bytes (padding keeps stack 16-byte aligned before BL).
    STP     x12, xzr, [sp, #-16]!  ; [sp+0]=Lr, [sp+8]=padding

    // Call C handler — returns the resume PC in x0.
    BL      KiSynchronousExceptionC

    // Discard the stacked 9th arg and resume at the updated PC.
    ADD     sp, sp, #16
    MSR     ELR_EL1, x0
    ERET

    NESTED_END KiSynchronousException

    //
    // KiInterruptException - Handle IRQ from kernel mode
    // Silently ERET until the HAL interrupt stack is initialized.
    // The QEMU virt timer (and GIC) keep firing from UEFI config;
    // a BRK stub here would spam KD with spurious breakpoints.
    //
    NESTED_ENTRY KiInterruptException
    ERET
    NESTED_END KiInterruptException

    //
    // KiFastInterruptException - Handle FIQ from kernel mode
    //
    NESTED_ENTRY KiFastInterruptException
    ERET
    NESTED_END KiFastInterruptException

    //
    // KiSystemErrorException - Handle SError from kernel mode
    //
    NESTED_ENTRY KiSystemErrorException
    ERET
    NESTED_END KiSystemErrorException

    //
    // EL0 exception handlers - EL0 code should not run yet; BRK is fine.
    //
    NESTED_ENTRY KiSynchronousExceptionFromEL0
    BRK     #0xF000    ; Not yet implemented
    NESTED_END KiSynchronousExceptionFromEL0

    NESTED_ENTRY KiInterruptExceptionFromEL0
    BRK     #0xF000    ; Not yet implemented
    NESTED_END KiInterruptExceptionFromEL0

    NESTED_ENTRY KiFastInterruptExceptionFromEL0
    BRK     #0xF000    ; Not yet implemented
    NESTED_END KiFastInterruptExceptionFromEL0

    NESTED_ENTRY KiSystemErrorExceptionFromEL0
    BRK     #0xF000    ; Not yet implemented
    NESTED_END KiSystemErrorExceptionFromEL0

    //
    // KiSetVbarEl1 - Set the VBAR_EL1 register (Vector Base Address Register, EL1)
    // This must be called with x0 containing the vector base address
    //
    LEAF_ENTRY KiSetVbarEl1
        MSR     VBAR_EL1, x0      ; Set the exception vector base address
        DSB     SY                ; Data Synchronization Barrier (system scope)
        ISB                       ; Instruction Synchronization Barrier
        RET
    LEAF_END KiSetVbarEl1

    END

#endif
