/*
 * PROJECT:     ReactOS vcruntime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of _setjmp/_setjmpex for ARM64
 * COPYRIGHT:   Copyright Timo Kreuzer <timo.kreuzer@reactos.org>
 *              ARM64 port and wiring by contributors
 */

#include <kxarm64.h>

    TEXTAREA

    // Export all common entry points to the same implementation
    LEAF_ENTRY _setjmpex
_setjmp:
__intrinsic_setjmp:
__intrinsic_setjmpex:

    // x0: _JUMP_BUFFER* _Env
    // x1: void* _Frame (context)

    // Save Frame and zero Reserved
    str     x1, [x0, #0x00]        // Frame
    mov     x2, xzr
    str     x2, [x0, #0x08]        // Reserved = 0

    // Save callee-saved integer registers x19-x28
    stp     x19, x20, [x0, #0x10]
    stp     x21, x22, [x0, #0x20]
    stp     x23, x24, [x0, #0x30]
    stp     x25, x26, [x0, #0x40]
    stp     x27, x28, [x0, #0x50]

    // Save FP (x29) and LR (x30)
    stp     x29, x30, [x0, #0x60]

    // Save SP
    mov     x2, sp
    str     x2, [x0, #0x70]

    // Save FPCR/FPSR (32-bit each)
    mrs     x2, FPCR
    mrs     x3, FPSR
    str     w2, [x0, #0x78]
    str     w3, [x0, #0x7C]

    // Save floating point callee-saved d8-d15
    str     d8,  [x0, #0x80]
    str     d9,  [x0, #0x88]
    str     d10, [x0, #0x90]
    str     d11, [x0, #0x98]
    str     d12, [x0, #0xA0]
    str     d13, [x0, #0xA8]
    str     d14, [x0, #0xB0]
    str     d15, [x0, #0xB8]

    // Return 0 from setjmp
    mov     w0, #0
    ret

    LEAF_END _setjmpex

    END

/* EOF */
