

#ifdef _MSC_VER

    AREA |.text|,ALIGN=11,CODE,READONLY

#include <ksarm64.h>

//
// External symbol declarations
//
    IMPORT  KiSynchronousExceptionC
    IMPORT  KiInterruptDispatch

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
    // KiInterruptException - Handle IRQ from EL1 (kernel mode)
    //
    // Saves all caller-saved registers plus FP/LR in a 192-byte frame,
    // preserves ELR_EL1 and SPSR_EL1 so nested synchronous exceptions cannot
    // corrupt them, then dispatches through HalpDispatchIrq (gic.c).
    //
    // Frame layout on entry to BL (SP points to base):
    //   [SP+  0] x29 (FP), [SP+ 8] x30 (LR)  <- FP/LR of interrupted context
    //   [SP+ 16] x0,       [SP+24]  x1
    //   [SP+ 32] x2,       [SP+40]  x3
    //   [SP+ 48] x4,       [SP+56]  x5
    //   [SP+ 64] x6,       [SP+72]  x7
    //   [SP+ 80] x8,       [SP+88]  x9
    //   [SP+ 96] x10,      [SP+104] x11
    //   [SP+112] x12,      [SP+120] x13
    //   [SP+128] x14,      [SP+136] x15
    //   [SP+144] x16,      [SP+152] x17
    //   [SP+160] x18,      [SP+168] xzr (padding)
    //   [SP+176] ELR_EL1,  [SP+184] SPSR_EL1
    //   [SP+192]           <- original SP before IRQ
    //
    NESTED_ENTRY KiInterruptException
    STP     x29, x30, [sp, #-192]!  // allocate frame; save FP/LR
    STP     x0,  x1,  [sp, #16]
    STP     x2,  x3,  [sp, #32]
    STP     x4,  x5,  [sp, #48]
    STP     x6,  x7,  [sp, #64]
    STP     x8,  x9,  [sp, #80]
    STP     x10, x11, [sp, #96]
    STP     x12, x13, [sp, #112]
    STP     x14, x15, [sp, #128]
    STP     x16, x17, [sp, #144]
    STP     x18, xzr, [sp, #160]    // x18 + zero padding
    MRS     x0,  ELR_EL1            // read after volatile saves so x0 is free
    MRS     x1,  SPSR_EL1
    STP     x0,  x1,  [sp, #176]    // save ELR / SPSR
    MOV     x0,  sp                 // pass frame base pointer to C
    BL      KiInterruptDispatch
    LDP     x0,  x1,  [sp, #176]   // restore ELR / SPSR
    MSR     ELR_EL1, x0
    MSR     SPSR_EL1, x1
    LDP     x18, xzr, [sp, #160]
    LDP     x16, x17, [sp, #144]
    LDP     x14, x15, [sp, #128]
    LDP     x12, x13, [sp, #112]
    LDP     x10, x11, [sp, #96]
    LDP     x8,  x9,  [sp, #80]
    LDP     x6,  x7,  [sp, #64]
    LDP     x4,  x5,  [sp, #48]
    LDP     x2,  x3,  [sp, #32]
    LDP     x0,  x1,  [sp, #16]
    LDP     x29, x30, [sp], #192
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
