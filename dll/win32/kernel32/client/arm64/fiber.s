/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fiber context switch code for ARM64
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ksarm64.h>

    IMPORT A64NAME(BaseThreadStartup)

    TEXTAREA

//
// A new fiber gets here from SwitchToFiber with its start address in x0
// and its parameter in x1.
//
    LEAF_ENTRY A64NAME(BaseFiberStartup)
    b       A64NAME(BaseThreadStartup)
    LEAF_END A64NAME(BaseFiberStartup)

#ifdef _ARM64EC_

//
// The FIBER block has the x64 layout here, each register goes to the x64 slot it stands for.
// FPCR and FPSR have no slot, they stay with the thread.
//
    LEAF_ENTRY A64NAME(SwitchToFiber)

    ldr     x9, [x18, #TeFiberData]
    add     x10, x9, #FbFiberContext

    str     x27, [x10, #CxX27]
    stp     x25, x26, [x10, #CxX25]
    stp     x19, x20, [x10, #CxX19]
    stp     x21, x22, [x10, #CxX21]
    mov     x11, sp
    stp     x11, x29, [x10, #CxSp]
    str     x30, [x10, #CxLr]
    str     x30, [x10, #CxPc]

    add     x11, x10, #CxV8
    str     d8, [x11, #0x00]
    str     d9, [x11, #0x10]
    str     d10, [x11, #0x20]
    str     d11, [x11, #0x30]
    str     d12, [x11, #0x40]
    str     d13, [x11, #0x50]
    str     d14, [x11, #0x60]
    str     d15, [x11, #0x70]

    ldr     x11, [x18, #TeStackLimit]
    str     x11, [x9, #FbStackLimit]
    ldr     x11, [x18, #TeFlsData]
    str     x11, [x9, #FbFlsData]
    ldr     w11, [x18, #TeGuaranteedStackBytes]
    str     w11, [x9, #FbGuaranteedStackBytes]

    /* Make the target the current fiber */
    str     x0, [x18, #TeFiberData]
    ldr     x11, [x0, #FbStackBase]
    str     x11, [x18, #TeStackBase]
    ldr     x11, [x0, #FbStackLimit]
    str     x11, [x18, #TeStackLimit]
    ldr     x11, [x0, #FbDeallocationStack]
    str     x11, [x18, #TeDeallocationStack]
    ldr     x11, [x0, #FbFlsData]
    str     x11, [x18, #TeFlsData]
    ldr     w11, [x0, #FbGuaranteedStackBytes]
    str     w11, [x18, #TeGuaranteedStackBytes]

    add     x10, x0, #FbFiberContext
    add     x11, x10, #CxV8
    ldr     d8, [x11, #0x00]
    ldr     d9, [x11, #0x10]
    ldr     d10, [x11, #0x20]
    ldr     d11, [x11, #0x30]
    ldr     d12, [x11, #0x40]
    ldr     d13, [x11, #0x50]
    ldr     d14, [x11, #0x60]
    ldr     d15, [x11, #0x70]

    ldr     x27, [x10, #CxX27]
    ldp     x25, x26, [x10, #CxX25]
    ldp     x19, x20, [x10, #CxX19]
    ldp     x21, x22, [x10, #CxX21]
    ldp     x11, x29, [x10, #CxSp]
    mov     sp, x11
    ldr     x30, [x10, #CxLr]
    ldr     x16, [x10, #CxPc]

    /* A fiber that never ran takes its start address from Rcx and its parameter from Rdx */
    ldr     x0, [x10, #CxX0]
    ldr     x1, [x10, #CxX1]
    br      x16

    LEAF_END A64NAME(SwitchToFiber)

#else

//
// VOID
// WINAPI
// SwitchToFiber(
//     _In_ LPVOID lpFiber);
//
// Keeps the nonvolatile registers and the stack of the current fiber in its
// FIBER block, then continues the target fiber where it left off.
//
    LEAF_ENTRY SwitchToFiber

    ldr     x9, [x18, #TeFiberData]
    add     x10, x9, #FbFiberContext

    stp     x19, x20, [x10, #CxX19]
    stp     x21, x22, [x10, #CxX21]
    stp     x23, x24, [x10, #CxX23]
    stp     x25, x26, [x10, #CxX25]
    stp     x27, x28, [x10, #CxX27]
    stp     x29, x30, [x10, #CxFp]
    mov     x11, sp
    stp     x11, x30, [x10, #CxSp]

    add     x11, x10, #CxV
    str     d8, [x11, #0x80]
    str     d9, [x11, #0x90]
    str     d10, [x11, #0xA0]
    str     d11, [x11, #0xB0]
    str     d12, [x11, #0xC0]
    str     d13, [x11, #0xD0]
    str     d14, [x11, #0xE0]
    str     d15, [x11, #0xF0]
    mrs     x12, fpcr
    mrs     x13, fpsr
    str     w12, [x10, #CxFpcr]
    str     w13, [x10, #CxFpsr]

    ldr     x11, [x18, #TeStackLimit]
    str     x11, [x9, #FbStackLimit]
    ldr     x11, [x18, #TeFlsData]
    str     x11, [x9, #FbFlsData]
    ldr     w11, [x18, #TeGuaranteedStackBytes]
    str     w11, [x9, #FbGuaranteedStackBytes]

    /* Make the target the current fiber */
    str     x0, [x18, #TeFiberData]
    ldr     x11, [x0, #FbStackBase]
    str     x11, [x18, #TeStackBase]
    ldr     x11, [x0, #FbStackLimit]
    str     x11, [x18, #TeStackLimit]
    ldr     x11, [x0, #FbDeallocationStack]
    str     x11, [x18, #TeDeallocationStack]
    ldr     x11, [x0, #FbFlsData]
    str     x11, [x18, #TeFlsData]
    ldr     w11, [x0, #FbGuaranteedStackBytes]
    str     w11, [x18, #TeGuaranteedStackBytes]

    add     x10, x0, #FbFiberContext
    add     x11, x10, #CxV
    ldr     d8, [x11, #0x80]
    ldr     d9, [x11, #0x90]
    ldr     d10, [x11, #0xA0]
    ldr     d11, [x11, #0xB0]
    ldr     d12, [x11, #0xC0]
    ldr     d13, [x11, #0xD0]
    ldr     d14, [x11, #0xE0]
    ldr     d15, [x11, #0xF0]
    ldr     w12, [x10, #CxFpcr]
    ldr     w13, [x10, #CxFpsr]
    msr     fpcr, x12
    msr     fpsr, x13

    ldp     x19, x20, [x10, #CxX19]
    ldp     x21, x22, [x10, #CxX21]
    ldp     x23, x24, [x10, #CxX23]
    ldp     x25, x26, [x10, #CxX25]
    ldp     x27, x28, [x10, #CxX27]
    ldp     x29, x30, [x10, #CxFp]
    ldp     x11, x16, [x10, #CxSp]
    mov     sp, x11

    /* A fiber that never ran takes its start address and parameter from here */
    ldp     x0, x1, [x10, #CxX0]
    br      x16

    LEAF_END SwitchToFiber

#endif

    END
