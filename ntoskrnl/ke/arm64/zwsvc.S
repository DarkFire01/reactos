/*
 *
 * KiZwSystemService is the common trampoline for all Zw* system service stubs
 * that execute in kernel mode (ntoskrnl's Zw* exports).  Each ZwFoo stub loads
 * the address of its corresponding NtFoo function into x16 (IP0) and then
 * jumps here.
 *
 * On entry:
 *   x16  = address of NtXxx to dispatch to
 *   x0-x7 = arguments already in place (ARM64 ABI; no copying needed)
 *   lr   = return address back to the ZwXxx caller
 *
 * This function:
 *   1. Allocates a KEXCEPTION_FRAME on the stack (saves x19-x28, fp, lr).
 *   2. Reads the current KTHREAD* from TPIDR_EL1 -> KIPCR.Prcb.CurrentThread.
 *   3. Saves KTHREAD.TrapFrame and KTHREAD.PreviousMode.
 *   4. Sets TrapFrame = sp and PreviousMode = KernelMode (0).
 *   5. Calls NtXxx via BLR x19 (x0 = return status on exit).
 *   6. Restores TrapFrame and PreviousMode.
 *   7. Restores callee-saved registers and returns (x0 = NtXxx result).
 */

#ifdef _MSC_VER

    AREA |.text|,ALIGN=2,CODE,READONLY
#include <ksarm64.h>

/* PbCurrentThread fallback: only if ksarm64.h does not define it yet. */
    IF :LNOT: :DEF: PbCurrentThread
PbCurrentThread EQU 0x8
    ENDIF

/*
 * NESTED_ENTRY KiZwSystemService
 *
 * KEXCEPTION_FRAME layout (96 = 0x60 bytes, 16-byte aligned):
 *   [sp+ExX19=0x00] x19    [sp+0x08] x20
 *   [sp+ExX21=0x10] x21    [sp+0x18] x22
 *   [sp+ExX23=0x20] x23    [sp+0x28] x24
 *   [sp+ExX25=0x30] x25    [sp+0x38] x26
 *   [sp+ExX27=0x40] x27    [sp+0x48] x28
 *   [sp+ExFp =0x50] x29(fp)
 *   [sp+ExReturn=0x58] x30(lr)
 */
    NESTED_ENTRY KiZwSystemService

    ; ----------------------------------------------------------------
    ; Prologue: allocate KEXCEPTION_FRAME and save all callee-saves.
    ; x29 (frame pointer) is set to the Fp slot so the frame chain
    ; can be walked by a debugger.
    ; ----------------------------------------------------------------
    SUB  sp,  sp,  #KEXCEPTION_FRAME_LENGTH
    STP  x29, x30, [sp, #ExFp]
    ADD  x29, sp,  #ExFp
    STP  x19, x20, [sp, #ExX19]
    STP  x21, x22, [sp, #ExX21]
    STP  x23, x24, [sp, #ExX23]
    STP  x25, x26, [sp, #ExX25]
    STP  x27, x28, [sp, #ExX27]

    PROLOG_END

    ; ----------------------------------------------------------------
    ; Move NtXxx target out of x16 (IP0) now - x16 may be clobbered
    ; by the BLR instruction itself on some implementations.
    ; ----------------------------------------------------------------
    MOV  x19, x16

    ; ----------------------------------------------------------------
    ; Obtain current KTHREAD*:
    ;   TPIDR_EL1 -> KIPCR* -> add PcPrcb to get &KPRCB -> read CurrentThread
    ;
    ; KPRCB is embedded in KIPCR (not a pointer), so ADD gives a pointer
    ; to the KPRCB itself; then LDR reads the CurrentThread pointer field.
    ; ----------------------------------------------------------------
    MRS  x20, TPIDR_EL1
    ADD  x20, x20, #PcPrcb              ; x20 = KPRCB* (embedded in KIPCR)
    LDR  x20, [x20, #PbCurrentThread]  ; x20 = KTHREAD*

    ; ----------------------------------------------------------------
    ; Save the outgoing KTHREAD.TrapFrame and KTHREAD.PreviousMode.
    ; ----------------------------------------------------------------
    LDR  x21, [x20, #ThTrapFrame]           ; x21 = old TrapFrame
    LDRB w22, [x20, #ThPreviousMode]        ; w22 = old PreviousMode (byte)

    ; ----------------------------------------------------------------
    ; Install a fresh TrapFrame pointing to the base of this frame (sp).
    ; sp cannot be stored directly in ARM64; use ADD #0 to copy it.
    ; ----------------------------------------------------------------
    ADD  x23, sp, #0
    STR  x23, [x20, #ThTrapFrame]

    ; ----------------------------------------------------------------
    ; Set PreviousMode = KernelMode (0).
    ; ----------------------------------------------------------------
    STRB wzr, [x20, #ThPreviousMode]

    ; ----------------------------------------------------------------
    ; Dispatch to NtXxx.
    ; x0-x7 already hold the system-service arguments (ARM64 ABI);
    ; x0 will hold the NTSTATUS return value after BLR returns.
    ; ----------------------------------------------------------------
    BLR  x19

    ; ----------------------------------------------------------------
    ; Re-read KTHREAD* - the callee may have used x20 (callee-save, but
    ; re-reading is cheap and defensive against future register changes).
    ; ----------------------------------------------------------------
    MRS  x20, TPIDR_EL1
    ADD  x20, x20, #PcPrcb
    LDR  x20, [x20, #PbCurrentThread]

    ; ----------------------------------------------------------------
    ; Restore KTHREAD.TrapFrame and KTHREAD.PreviousMode.
    ; ----------------------------------------------------------------
    STR  x21, [x20, #ThTrapFrame]
    STRB w22, [x20, #ThPreviousMode]

    ; ----------------------------------------------------------------
    ; Epilogue: restore callee-saves, deallocate frame, return to caller.
    ; x0 still holds the NtXxx return value (NTSTATUS).
    ; ----------------------------------------------------------------
    LDP  x19, x20, [sp, #ExX19]
    LDP  x21, x22, [sp, #ExX21]
    LDP  x23, x24, [sp, #ExX23]
    LDP  x25, x26, [sp, #ExX25]
    LDP  x27, x28, [sp, #ExX27]
    LDP  x29, x30, [sp, #ExFp]
    ADD  sp,  sp,  #KEXCEPTION_FRAME_LENGTH
    RET

    NESTED_END KiZwSystemService

    END

#else /* GAS / GCC / Clang - TODO */

/*
 * ARM64 GAS (GCC/Clang) implementation is not yet written.
 * The Visual Studio (ARMASM64) path above is used for the VS-arm64 build.
 */

#endif /* _MSC_VER */
