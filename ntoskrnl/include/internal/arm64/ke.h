#ifndef __NTOSKRNL_INCLUDE_INTERNAL_ARM64_KE_H
#define __NTOSKRNL_INCLUDE_INTERNAL_ARM64_KE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARM64 (AArch64) kernel internal definitions for the kernel executive.
 */

/* -------------------------------------------------------------------------
 * ARM64 PSTATE / DAIF bit definitions
 * DAIF fields control Debug, SError, IRQ, FIQ masking in PSTATE.
 * Bit positions when using MSR DAIFSET/DAIFCLR immediates:
 *   bit 3 = Debug (D)
 *   bit 2 = SError (A)
 *   bit 1 = IRQ (I)
 *   bit 0 = FIQ (F)
 * In the PSTATE register proper the same bits appear at positions 9:6.
 * ------------------------------------------------------------------------- */
#define ARM64_DAIF_FIQ   0x1
#define ARM64_DAIF_IRQ   0x2
#define ARM64_DAIF_SERR  0x4
#define ARM64_DAIF_DBG   0x8

/* SPSR / PSTATE mode field (M[3:0] + M[4]) */
#define ARM64_SPSR_MODE_MASK    0x1F
#define ARM64_SPSR_MODE_EL0t    0x00 /* EL0, SP_EL0 */
#define ARM64_SPSR_MODE_EL1t    0x04 /* EL1, SP_EL0 */
#define ARM64_SPSR_MODE_EL1h    0x05 /* EL1, SP_EL1 (normal kernel mode) */

/* SPSR interrupt mask bits (mirrors DAIF in PSTATE) */
#define ARM64_SPSR_I    (1UL << 7) /* IRQ masked   */
#define ARM64_SPSR_F    (1UL << 6) /* FIQ masked   */

/* ESR_EL1 Exception Class (EC) field - bits [31:26] */
#define ARM64_ESR_EC_SHIFT           26
#define ARM64_ESR_EC_MASK            0x3F
#define ARM64_ESR_EC_DABT_LOW        0x24 /* Data abort from lower EL  */
#define ARM64_ESR_EC_DABT_SAME       0x25 /* Data abort from same EL   */
#define ARM64_ESR_EC_IABT_LOW        0x20 /* Instruction abort lower EL*/
#define ARM64_ESR_EC_IABT_SAME       0x21 /* Instruction abort same EL */
#define ARM64_ESR_EC_SVC64           0x15 /* SVC in AArch64            */
#define ARM64_ESR_EC_BRK             0x3C /* Breakpoint instruction     */

/* ESR_EL1 ISS fault status sub-fields (for data/instruction aborts) */
#define ARM64_ESR_ISS_DFSC_MASK      0x3F  /* DFSC / IFSC              */
#define ARM64_ESR_ISS_WNR            (1UL << 6)  /* Write-not-Read      */
#define ARM64_ESR_ISS_IND            (1UL << 4)  /* Instruction fetch    */

/* -------------------------------------------------------------------------
 * Debugger breakpoint
 *
 * ARM64 uses the BRK instruction (32-bit).
 * BRK encoding: 0xD4200000 | (imm16 << 5)
 * Windows KD convention: BRK #0xF000 = 0xD43E0000
 * ------------------------------------------------------------------------- */
#define KD_BREAKPOINT_TYPE        ULONG
#define KD_BREAKPOINT_SIZE        sizeof(ULONG)
#define KD_BREAKPOINT_VALUE       0xD43E0000UL  /* BRK #0xF000 */

/* -------------------------------------------------------------------------
 * Synchronisation level
 * On ARM64, SYNCH_LEVEL == DISPATCH_LEVEL (same as AMD64 / ARM32).
 * ------------------------------------------------------------------------- */
#define SYNCH_LEVEL DISPATCH_LEVEL

/* Kernel variable exported from ntoskrnl */
extern NTKERNELAPI volatile KSYSTEM_TIME KeTickCount;

#ifndef __ASM__

#include "intrin_i.h"

/* -------------------------------------------------------------------------
 * Context register accessors (portable one-liners)
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG_PTR
KeGetContextPc(PCONTEXT Context)
{
    return Context->Pc;
}

FORCEINLINE
VOID
KeSetContextPc(PCONTEXT Context, ULONG_PTR ProgramCounter)
{
    Context->Pc = ProgramCounter;
}

FORCEINLINE
ULONG_PTR
KeGetContextReturnRegister(PCONTEXT Context)
{
    return Context->X0;
}

FORCEINLINE
VOID
KeSetContextReturnRegister(PCONTEXT Context, ULONG_PTR ReturnValue)
{
    Context->X0 = ReturnValue;
}

FORCEINLINE
ULONG_PTR
KeGetContextStackRegister(PCONTEXT Context)
{
    return Context->Sp;
}

FORCEINLINE
ULONG_PTR
KeGetContextFrameRegister(PCONTEXT Context)
{
    return Context->Fp;
}

FORCEINLINE
VOID
KeSetContextFrameRegister(PCONTEXT Context, ULONG_PTR Frame)
{
    Context->Fp = Frame;
}

/* -------------------------------------------------------------------------
 * Trap frame accessors
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG_PTR
KeGetTrapFramePc(PKTRAP_FRAME TrapFrame)
{
    return TrapFrame->Pc;
}

FORCEINLINE
PKTRAP_FRAME
KiGetLinkedTrapFrame(PKTRAP_FRAME TrapFrame)
{
    return (PKTRAP_FRAME)TrapFrame->TrapFrame;
}

FORCEINLINE
ULONG_PTR
KeGetTrapFrameStackRegister(PKTRAP_FRAME TrapFrame)
{
    return TrapFrame->Sp;
}

FORCEINLINE
ULONG_PTR
KeGetTrapFrameFrameRegister(PKTRAP_FRAME TrapFrame)
{
    return TrapFrame->Fp;
}

/*
 * Macro to get the KTRAP_FRAME from a thread's initial stack.
 */
#define KeGetTrapFrame(Thread) \
    (PKTRAP_FRAME)((ULONG_PTR)((Thread)->InitialStack) - sizeof(KTRAP_FRAME))

/*
 * Macro to get the KEXCEPTION_FRAME from below the trap frame.
 */
#define KeGetExceptionFrame(Thread) \
    (PKEXCEPTION_FRAME)((ULONG_PTR)KeGetTrapFrame(Thread) - sizeof(KEXCEPTION_FRAME))

/*
 * Context switch counter lives in the PRCB on all non-x86 architectures.
 */
#define KeGetContextSwitches(Prcb)  (Prcb)->KeContextSwitches

/*
 * Second-level cache size field name: ARM64 (RISC) has separate D/I caches.
 */
#define KiGetSecondLevelDCacheSize() ((PKIPCR)KeGetPcr())->SecondLevelCacheSize

/*
 * Returns whether interrupts were enabled at the time the trap was taken.
 * On ARM64 the IRQ-enable state is encoded in SPSR.I (bit 7 of SPSR_EL1).
 * A cleared bit means IRQs were ENABLED (interrupts unmasked).
 */
#define KeGetTrapFrameInterruptState(TrapFrame) \
    (((TrapFrame)->Spsr & ARM64_SPSR_I) == 0)

/* -------------------------------------------------------------------------
 * Interrupt control
 * ARM64 uses DAIF to mask/unmask interrupts.
 * _disable() / _enable() map to MSR DAIFSET,2 / MSR DAIFCLR,2 in MSVC.
 * ------------------------------------------------------------------------- */
FORCEINLINE
BOOLEAN
KeDisableInterrupts(VOID)
{
    BOOLEAN WereEnabled;
    ULONG64 Daif;

    /* Read current DAIF state */
#if defined(_MSC_VER)
    Daif = _ReadStatusReg(ARM64_SYSREG(3, 3, 4, 2, 1)); /* DAIF */
#else
    __asm__ __volatile__ ("mrs %0, daif" : "=r"(Daif) :: "memory");
#endif

    /* IRQ enabled == DAIF.I bit CLEAR (bit 7 of DAIF register) */
    WereEnabled = ((Daif & (ARM64_DAIF_IRQ << 6)) == 0) ? TRUE : FALSE;

    _disable(); /* MSR DAIFSET, #2 - mask IRQ */
    return WereEnabled;
}

FORCEINLINE
VOID
KeRestoreInterrupts(BOOLEAN WereEnabled)
{
    if (WereEnabled) _enable(); /* MSR DAIFCLR, #2 - unmask IRQ */
}

/* -------------------------------------------------------------------------
 * TLB management
 * ------------------------------------------------------------------------- */
FORCEINLINE
VOID
KeInvalidateTlbEntry(PVOID Address)
{
    KeArm64InvalidateTlbEntry(Address);
}

FORCEINLINE
VOID
KeFlushProcessTb(VOID)
{
    KeArm64InvalidateAllTlb();
}

/* -------------------------------------------------------------------------
 * Instruction cache sweep
 * ARM64: "IC IALLUIS" invalidates instruction cache inner-shareable domain.
 * ------------------------------------------------------------------------- */
FORCEINLINE
VOID
KeSweepICache(PVOID BaseAddress, SIZE_T FlushSize)
{
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(FlushSize);
    KeArm64InvalidateICache();
}

/* -------------------------------------------------------------------------
 * Determine previous mode from the SPSR saved in the trap frame.
 * EL0t (M[4:0] == 0) is user mode; everything else is kernel.
 * ------------------------------------------------------------------------- */
#define KiGetPreviousMode(TrapFrame) \
    (((TrapFrame)->Spsr & ARM64_SPSR_MODE_MASK) == ARM64_SPSR_MODE_EL0t) \
        ? UserMode : KernelMode

/* -------------------------------------------------------------------------
 * Thread rundown hook (nothing to do on ARM64)
 * ------------------------------------------------------------------------- */
FORCEINLINE
VOID
KiRundownThread(PKTHREAD Thread)
{
    UNREFERENCED_PARAMETER(Thread);
}

/* -------------------------------------------------------------------------
 * No x86 performance counters on ARM64
 * ------------------------------------------------------------------------- */
#define Ki386PerfEnd()

/* Include memory-management helpers after all base definitions */
#include "mm.h"

DECLSPEC_NORETURN
VOID
KiExceptionExit(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame);

#endif /* !__ASM__ */

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* __NTOSKRNL_INCLUDE_INTERNAL_ARM64_KE_H */
