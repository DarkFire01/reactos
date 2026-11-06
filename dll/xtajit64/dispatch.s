/*
 * PROJECT:     ReactOS x64 emulator
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Transitions between x64 code and ARM64EC code
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    IMPORT XtapInterpret
    IMPORT XtapX64Ret

/* FIXME: TEB->Vdm holds the CHPE CPU area until the TEB has its ChpeV2CpuAreaInfo field */
TeCpuArea           EQU 0x1690

/* CHPE_V2_CPU_AREA_INFO */
CaInSimulation      EQU 0x00
CaStackBase         EQU 0x08
CaContext           EQU 0x18
CaThread            EQU 0x30

/* XTA_THREAD */
ThCpuArea           EQU 0x00
ThResumeSp          EQU 0x08
ThNativeTarget      EQU 0x10

/* ARM64EC_NT_CONTEXT, x64 register names in the comments */
CtX8                EQU 0x078   /* Rax */
CtX0                EQU 0x080   /* Rcx */
CtX27               EQU 0x090   /* Rbx */
CtSp                EQU 0x098   /* Rsp */
CtFp                EQU 0x0A0   /* Rbp */
CtX25               EQU 0x0A8   /* Rsi */
CtX2                EQU 0x0B8   /* R8 */
CtX4                EQU 0x0C8   /* R10 */
CtX19               EQU 0x0D8   /* R12 */
CtX21               EQU 0x0E8   /* R14 */
CtPc                EQU 0x0F8   /* Rip */
CtV                 EQU 0x1A0   /* Xmm0 */

    TEXTAREA

    /* Context in x16 from the native registers */
    MACRO
    SAVE_GUEST
        str     x8, [x16, #CtX8]
        stp     x0, x1, [x16, #CtX0]
        str     x27, [x16, #CtX27]
        str     fp, [x16, #CtFp]
        stp     x25, x26, [x16, #CtX25]
        stp     x2, x3, [x16, #CtX2]
        stp     x4, x5, [x16, #CtX4]
        stp     x19, x20, [x16, #CtX19]
        stp     x21, x22, [x16, #CtX21]
        add     x10, x16, #CtV
        stp     q0, q1, [x10, #0x00]
        stp     q2, q3, [x10, #0x20]
        stp     q4, q5, [x10, #0x40]
        stp     q6, q7, [x10, #0x60]
        stp     q8, q9, [x10, #0x80]
        stp     q10, q11, [x10, #0xA0]
        stp     q12, q13, [x10, #0xC0]
        stp     q14, q15, [x10, #0xE0]
    MEND

    /* Native registers from the context in x16, except x0 and x1 */
    MACRO
    LOAD_GUEST
        add     x10, x16, #CtV
        ldp     q0, q1, [x10, #0x00]
        ldp     q2, q3, [x10, #0x20]
        ldp     q4, q5, [x10, #0x40]
        ldp     q6, q7, [x10, #0x60]
        ldp     q8, q9, [x10, #0x80]
        ldp     q10, q11, [x10, #0xA0]
        ldp     q12, q13, [x10, #0xC0]
        ldp     q14, q15, [x10, #0xE0]
        ldr     x8, [x16, #CtX8]
        ldr     x27, [x16, #CtX27]
        ldr     fp, [x16, #CtFp]
        ldp     x25, x26, [x16, #CtX25]
        ldp     x2, x3, [x16, #CtX2]
        ldp     x4, x5, [x16, #CtX4]
        ldp     x19, x20, [x16, #CtX19]
        ldp     x21, x22, [x16, #CtX21]
    MEND

//
// VOID XtapRunNative(PXTA_THREAD Thread)
//
// Calls the ARM64EC function at Thread->NativeTarget through its entry thunk with the
// guest registers. RetToEntryThunk comes back here when the thunk returns to x64 code.
//
    NESTED_ENTRY XtapRunNative

    stp     fp, lr, [sp, #-0xA0]!
    stp     x19, x20, [sp, #0x10]
    stp     x21, x22, [sp, #0x20]
    stp     x23, x24, [sp, #0x30]
    stp     x25, x26, [sp, #0x40]
    stp     x27, x28, [sp, #0x50]
    stp     d8, d9, [sp, #0x60]
    stp     d10, d11, [sp, #0x70]
    stp     d12, d13, [sp, #0x80]
    stp     d14, d15, [sp, #0x90]

    /* Link this resume frame in front of the older ones */
    ldr     x9, [x0, #ThResumeSp]
    stp     x9, x0, [sp, #-16]!
    mov     x9, sp
    str     x9, [x0, #ThResumeSp]

    ldr     x17, [x0, #ThCpuArea]
    ldr     x16, [x17, #CaContext]
    ldr     x9, [x0, #ThNativeTarget]
    strb    wzr, [x17, #CaInSimulation]
    LOAD_GUEST

    /* The word before an ARM64EC function points at its entry thunk */
    ldursw  x17, [x9, #-4]
    and     x17, x17, #0xFFFFFFFFFFFFFFFC
    add     x17, x9, x17

    /* An aligned stack gives the thunk the return address in lr, otherwise an x64 ret pops it later */
    ldr     x4, [x16, #CtSp]
    tbz     x4, #3, RunNativeKeepReturn
    ldr     lr, [x4], #8
    mov     sp, x4
    b       RunNativeEnter

LABEL(RunNativeKeepReturn)
    mov     sp, x4
    adrp    x10, XtapX64Ret
    ldr     lr, [x10, XtapX64Ret]

LABEL(RunNativeEnter)
    ldp     x0, x1, [x16, #CtX0]
    br      x17

    NESTED_END XtapRunNative

//
// __os_arm64x_dispatch_ret: an entry thunk is done, lr is the x64 code to continue with.
//
    LEAF_ENTRY RetToEntryThunk

    ldr     x17, [x18, #TeCpuArea]
    ldr     x16, [x17, #CaContext]
    SAVE_GUEST
    mov     x9, sp
    str     x9, [x16, #CtSp]
    str     lr, [x16, #CtPc]
    mov     w9, #1
    strb    w9, [x17, #CaInSimulation]

    /* Resume the interpreter that called XtapRunNative */
    ldr     x0, [x17, #CaThread]
    ldr     x9, [x0, #ThResumeSp]
    mov     sp, x9
    ldp     x9, x0, [sp], #16
    str     x9, [x0, #ThResumeSp]

    ldp     d14, d15, [sp, #0x90]
    ldp     d12, d13, [sp, #0x80]
    ldp     d10, d11, [sp, #0x70]
    ldp     d8, d9, [sp, #0x60]
    ldp     x27, x28, [sp, #0x50]
    ldp     x25, x26, [sp, #0x40]
    ldp     x23, x24, [sp, #0x30]
    ldp     x21, x22, [sp, #0x20]
    ldp     x19, x20, [sp, #0x10]
    ldp     fp, lr, [sp], #0xA0
    ret

    LEAF_END RetToEntryThunk

//
// __os_arm64x_x64_jump: an adjustor thunk continues at x9, which may be x64 code.
//
    LEAF_ENTRY DispatchJump

    str     lr, [sp, #-8]!
    b       EnterX64

    LEAF_END DispatchJump

//
// __os_arm64x_dispatch_call_no_redirect: an exit thunk calls the x64 function at x9 with blr x16.
// The x64 code returns to lr, which the emulator pushes as its return address.
//
    LEAF_ENTRY ExitToX64

    str     lr, [sp, #-8]!

    ALTERNATE_ENTRY EnterX64

    ldr     x17, [x18, #TeCpuArea]
    ldr     x16, [x17, #CaContext]
    SAVE_GUEST
    mov     x10, sp
    str     x10, [x16, #CtSp]
    str     x9, [x16, #CtPc]
    mov     w10, #1
    strb    w10, [x17, #CaInSimulation]

    /* Interpret below any suspended interpreter frames */
    ldr     x0, [x17, #CaThread]
    ldr     x9, [x0, #ThResumeSp]
    cbnz    x9, ExitToX64Stack
    ldr     x9, [x17, #CaStackBase]
LABEL(ExitToX64Stack)
    mov     sp, x9
    bl      XtapInterpret

    /* The x64 code returned to the native caller */
    ldr     x17, [x18, #TeCpuArea]
    ldr     x16, [x17, #CaContext]
    strb    wzr, [x17, #CaInSimulation]
    LOAD_GUEST
    ldr     x9, [x16, #CtSp]
    mov     sp, x9
    ldr     x9, [x16, #CtPc]
    ldp     x0, x1, [x16, #CtX0]
    br      x9

    LEAF_END ExitToX64

//
// Starts x64 code from the context in the CPU area, on the emulator stack. Does not return.
//
    LEAF_ENTRY BeginSimulation

    ldr     x17, [x18, #TeCpuArea]
    ldr     x9, [x17, #CaStackBase]
    mov     sp, x9
    mov     w10, #1
    strb    w10, [x17, #CaInSimulation]
    ldr     x0, [x17, #CaThread]
    bl      XtapInterpret
    brk     #0xF003

    LEAF_END BeginSimulation

    END
