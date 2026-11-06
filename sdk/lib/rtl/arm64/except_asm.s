/*
 * PROJECT:     ReactOS runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 context capture and restore
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ksarm64.h>

    TEXTAREA

#ifdef _ARM64EC_

/* x64 CONTEXT_FULL, user mode code and stack selectors */
#define CONTEXT_FULL_AMD64      0x0010000B
#define KGDT64_R3_CODE_RPL      0x33
#define KGDT64_R3_DATA_RPL      0x2B

//
// The context has the x64 layout, each register goes to the x64 register it stands for
// and Rip is the return address. MXCSR is rebuilt from FPCR and FPSR.
//
    ARM64EC_ENTRY_THUNK A64NAME(RtlCaptureContext),1,0
    LEAF_ENTRY_COMDAT A64NAME(RtlCaptureContext)

    str     x8, [x0, #CxX8]
    stp     x0, x1, [x0, #CxX0]
    str     x27, [x0, #CxX27]
    mov     x16, sp
    stp     x16, fp, [x0, #CxSp]
    stp     x25, x26, [x0, #CxX25]
    stp     x2, x3, [x0, #CxX2]
    stp     x4, x5, [x0, #CxX4]
    stp     x19, x20, [x0, #CxX19]
    stp     x21, x22, [x0, #CxX21]
    str     lr, [x0, #CxPc]
    str     lr, [x0, #CxLr]
    str     x6, [x0, #CxX6]
    str     x7, [x0, #CxX7]
    str     x9, [x0, #CxX9]
    str     x10, [x0, #CxX10]
    str     x11, [x0, #CxX11]
    str     x12, [x0, #CxX12]
    str     x15, [x0, #CxX15]

    mov     w16, #KGDT64_R3_CODE_RPL
    strh    w16, [x0, #CxSegCs]
    mov     w16, #KGDT64_R3_DATA_RPL
    strh    w16, [x0, #CxSegSs]
    mov     w16, #0x202
    str     w16, [x0, #CxEFlags]

    /* MXCSR with every exception masked, rounding and flush to zero from FPCR, sticky flags from FPSR */
    mrs     x16, fpcr
    ubfx    x9, x16, #22, #2
    and     x10, x9, #1
    lsr     x9, x9, #1
    orr     x9, x9, x10, lsl #1
    mov     w17, #0x1F80
    orr     w17, w17, w9, lsl #13
    ubfx    x10, x16, #24, #1
    orr     w17, w17, w10, lsl #15
    mrs     x16, fpsr
    and     w10, w16, #1
    orr     w17, w17, w10
    ubfx    w10, w16, #1, #4
    orr     w17, w17, w10, lsl #2
    ubfx    w10, w16, #7, #1
    orr     w17, w17, w10, lsl #1
    str     w17, [x0, #CxMxCsr]
    str     w17, [x0, #CxMxCsrCopy]

    add     x16, x0, #CxV0
    stp     q0, q1, [x16, #0x00]
    stp     q2, q3, [x16, #0x20]
    stp     q4, q5, [x16, #0x40]
    stp     q6, q7, [x16, #0x60]
    stp     q8, q9, [x16, #0x80]
    stp     q10, q11, [x16, #0xA0]
    stp     q12, q13, [x16, #0xC0]
    stp     q14, q15, [x16, #0xE0]

    ldr     w16, =CONTEXT_FULL_AMD64
    str     w16, [x0, #CxContextFlags]
    ret

    LEAF_END A64NAME(RtlCaptureContext)

    TEXTAREA

//
// Loads the x64 context into the ARM64 registers that stand for them and continues at Rip.
//
    LEAF_ENTRY A64NAME(RtlpRestoreContextInternal)

    add     x16, x0, #CxV0
    ldp     q0, q1, [x16, #0x00]
    ldp     q2, q3, [x16, #0x20]
    ldp     q4, q5, [x16, #0x40]
    ldp     q6, q7, [x16, #0x60]
    ldp     q8, q9, [x16, #0x80]
    ldp     q10, q11, [x16, #0xA0]
    ldp     q12, q13, [x16, #0xC0]
    ldp     q14, q15, [x16, #0xE0]

    ldr     x8, [x0, #CxX8]
    ldr     x27, [x0, #CxX27]
    ldr     fp, [x0, #CxFp]
    ldp     x25, x26, [x0, #CxX25]
    ldp     x2, x3, [x0, #CxX2]
    ldp     x4, x5, [x0, #CxX4]
    ldp     x19, x20, [x0, #CxX19]
    ldp     x21, x22, [x0, #CxX21]
    ldr     lr, [x0, #CxLr]

    ldr     x16, [x0, #CxPc]
    ldr     x17, [x0, #CxSp]
    ldp     x0, x1, [x0, #CxX0]
    mov     sp, x17
    br      x16

    LEAF_END A64NAME(RtlpRestoreContextInternal)

#else

//
// VOID
// RtlCaptureContext(
//     _Out_ PCONTEXT ContextRecord);
//
// The context describes the caller right after the call: Pc is the
// return address, x0 and lr hold nothing useful and are stored as 0.
//
    LEAF_ENTRY RtlCaptureContext

    str     xzr, [x0, #CxX0]
    stp     x1, x2, [x0, #CxX1]
    stp     x3, x4, [x0, #CxX3]
    stp     x5, x6, [x0, #CxX5]
    stp     x7, x8, [x0, #CxX7]
    stp     x9, x10, [x0, #CxX9]
    stp     x11, x12, [x0, #CxX11]
    stp     x13, x14, [x0, #CxX13]
    stp     x15, x16, [x0, #CxX15]
    stp     x17, x18, [x0, #CxX17]
    stp     x19, x20, [x0, #CxX19]
    stp     x21, x22, [x0, #CxX21]
    stp     x23, x24, [x0, #CxX23]
    stp     x25, x26, [x0, #CxX25]
    stp     x27, x28, [x0, #CxX27]
    stp     x29, xzr, [x0, #CxFp]
    mov     x16, sp
    stp     x16, x30, [x0, #CxSp]

    mrs     x16, nzcv
    str     w16, [x0, #CxCpsr]
    mrs     x16, fpcr
    mrs     x17, fpsr
    str     w16, [x0, #CxFpcr]
    str     w17, [x0, #CxFpsr]

    add     x16, x0, #CxV
    stp     q0, q1, [x16, #0x000]
    stp     q2, q3, [x16, #0x020]
    stp     q4, q5, [x16, #0x040]
    stp     q6, q7, [x16, #0x060]
    stp     q8, q9, [x16, #0x080]
    stp     q10, q11, [x16, #0x0A0]
    stp     q12, q13, [x16, #0x0C0]
    stp     q14, q15, [x16, #0x0E0]
    stp     q16, q17, [x16, #0x100]
    stp     q18, q19, [x16, #0x120]
    stp     q20, q21, [x16, #0x140]
    stp     q22, q23, [x16, #0x160]
    stp     q24, q25, [x16, #0x180]
    stp     q26, q27, [x16, #0x1A0]
    stp     q28, q29, [x16, #0x1C0]
    stp     q30, q31, [x16, #0x1E0]

    ldr     w16, =CONTEXT_FULL
    str     w16, [x0, #CxContextFlags]
    ret

    LEAF_END RtlCaptureContext

//
// DECLSPEC_NORETURN
// VOID
// RtlpRestoreContextInternal(
//     _In_ PCONTEXT ContextRecord);
//
// Everything is loaded before sp moves, so the context may live below the
// new stack pointer. x16 and x17 carry the new pc and sp, which only call
// sites can afford, and x18 keeps pointing at the current TEB or PCR.
//
    LEAF_ENTRY RtlpRestoreContextInternal

    add     x16, x0, #CxV
    ldp     q0, q1, [x16, #0x000]
    ldp     q2, q3, [x16, #0x020]
    ldp     q4, q5, [x16, #0x040]
    ldp     q6, q7, [x16, #0x060]
    ldp     q8, q9, [x16, #0x080]
    ldp     q10, q11, [x16, #0x0A0]
    ldp     q12, q13, [x16, #0x0C0]
    ldp     q14, q15, [x16, #0x0E0]
    ldp     q16, q17, [x16, #0x100]
    ldp     q18, q19, [x16, #0x120]
    ldp     q20, q21, [x16, #0x140]
    ldp     q22, q23, [x16, #0x160]
    ldp     q24, q25, [x16, #0x180]
    ldp     q26, q27, [x16, #0x1A0]
    ldp     q28, q29, [x16, #0x1C0]
    ldp     q30, q31, [x16, #0x1E0]

    ldr     w16, [x0, #CxFpcr]
    msr     fpcr, x16
    ldr     w16, [x0, #CxFpsr]
    msr     fpsr, x16
    ldr     w16, [x0, #CxCpsr]
    msr     nzcv, x16

    ldp     x2, x3, [x0, #CxX2]
    ldp     x4, x5, [x0, #CxX4]
    ldp     x6, x7, [x0, #CxX6]
    ldp     x8, x9, [x0, #CxX8]
    ldp     x10, x11, [x0, #CxX10]
    ldp     x12, x13, [x0, #CxX12]
    ldp     x14, x15, [x0, #CxX14]
    ldp     x19, x20, [x0, #CxX19]
    ldp     x21, x22, [x0, #CxX21]
    ldp     x23, x24, [x0, #CxX23]
    ldp     x25, x26, [x0, #CxX25]
    ldp     x27, x28, [x0, #CxX27]
    ldp     x29, x30, [x0, #CxFp]

    ldr     x16, [x0, #CxPc]
    ldr     x17, [x0, #CxSp]
    ldp     x0, x1, [x0, #CxX0]
    mov     sp, x17
    br      x16

    LEAF_END RtlpRestoreContextInternal

#endif

    END
