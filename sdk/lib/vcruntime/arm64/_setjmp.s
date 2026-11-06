/*
 * PROJECT:     ReactOS vcruntime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of _setjmp for ARM64
 * COPYRIGHT:   Copyright Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ksarm64.h>

    TEXTAREA

//
// int _setjmpex(_JUMP_BUFFER* _Env, void* _Frame);
//
#ifdef _ARM64EC_

    // The buffer has the x64 layout, each ARM64 register goes to the x64 register it stands for
    LEAF_ENTRY A64NAME(_setjmpex)

    str     x1, [x0, #JbFrame]
    str     x27, [x0, #JbX27]
    stp     x25, x26, [x0, #JbX25]
    stp     x19, x20, [x0, #JbX19]
    stp     x21, x22, [x0, #JbX21]
    mov     x2, sp
    stp     x2, fp, [x0, #JbSp]
    str     lr, [x0, #JbLr]

    // MXCSR with every exception masked, rounding and flush to zero from FPCR
    mrs     x2, fpcr
    ubfx    x3, x2, #22, #2
    and     x4, x3, #1
    lsr     x3, x3, #1
    orr     x3, x3, x4, lsl #1
    mov     w5, #0x1F80
    orr     w5, w5, w3, lsl #13
    ubfx    x4, x2, #24, #1
    orr     w5, w5, w4, lsl #15

    // Sticky flags from FPSR, IDC lands on DE which sits between IE and ZE
    mrs     x2, fpsr
    and     w4, w2, #1
    orr     w5, w5, w4
    ubfx    w4, w2, #1, #4
    orr     w5, w5, w4, lsl #2
    ubfx    w4, w2, #7, #1
    orr     w5, w5, w4, lsl #1
    str     w5, [x0, #JbMxCsr]

    // Default x87 control word
    mov     w2, #0x27F
    strh    w2, [x0, #JbFpCsr]

    // Only the low halves of v8-v15 are nonvolatile for EC callers
    str     d8, [x0, #JbV8]
    str     d9, [x0, #JbV8 + 16]
    str     d10, [x0, #JbV10]
    str     d11, [x0, #JbV10 + 16]
    str     d12, [x0, #JbV12]
    str     d13, [x0, #JbV12 + 16]
    str     d14, [x0, #JbV14]
    str     d15, [x0, #JbV14 + 16]

    mov     w0, #0
    ret

    LEAF_END A64NAME(_setjmpex)

    IMPORT A64NAME(__intrinsic_setjmpex), WEAK A64NAME(_setjmpex)
    IMPORT A64NAME(__intrinsic_setjmp), WEAK A64NAME(_setjmpex)
    IMPORT A64NAME(_setjmp), WEAK A64NAME(_setjmpex)
    IMPORT A64NAME(setjmp), WEAK A64NAME(_setjmpex)

#else

    LEAF_ENTRY _setjmpex

    // Save the Frame passed in x1
    str     x1, [x0, #JbFrame]

    // Save the non-volatile registers
    stp     x19, x20, [x0, #JbX19]
    stp     x21, x22, [x0, #JbX21]
    stp     x23, x24, [x0, #JbX23]
    stp     x25, x26, [x0, #JbX25]
    stp     x27, x28, [x0, #JbX27]
    stp     fp, lr, [x0, #JbFp]
    mov     x2, sp
    str     x2, [x0, #JbSp]

    // Save floating point state
    mrs     x2, fpcr
    str     w2, [x0, #JbFpcr]
    mrs     x2, fpsr
    str     w2, [x0, #JbFpsr]
    stp     d8, d9, [x0, #JbD8]
    stp     d10, d11, [x0, #JbD10]
    stp     d12, d13, [x0, #JbD12]
    stp     d14, d15, [x0, #JbD14]

    // Return 0
    mov     w0, #0
    ret

    LEAF_END _setjmpex

    IMPORT __intrinsic_setjmpex, WEAK _setjmpex
    IMPORT __intrinsic_setjmp, WEAK _setjmpex
    IMPORT _setjmp, WEAK _setjmpex
    IMPORT setjmp, WEAK _setjmpex

#endif

    END

/* EOF */
