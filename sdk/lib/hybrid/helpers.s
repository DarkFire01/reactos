/*
 * PROJECT:     ReactOS ARM64EC support library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Call helpers the ARM64EC compiler and import stubs depend on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    IMPORT __os_arm64x_dispatch_icall
    IMPORT __os_arm64x_dispatch_fptr

    TEXTAREA

//
// Import stubs land here with the IAT target in x11 and its exit thunk in x10.
// The check may swap x11 for the exit thunk, then we jump to whatever it left.
//
    NESTED_ENTRY __icall_helper_arm64ec

    stp     fp, lr, [sp, #-16]!
    mov     fp, sp
    adrp    xip0, __os_arm64x_dispatch_icall
    ldr     xip0, [xip0, __os_arm64x_dispatch_icall]
    blr     xip0
    ldp     fp, lr, [sp], #16
    br      x11
LABEL(IcallHelperEnd)

    NESTED_END __icall_helper_arm64ec

//
// C++ adjustor thunks land here with this already adjusted and the target in x11. Without an
// exit thunk (x10 is 0) the check leaves 0 in x11 for x64 code and the x64 target in x9, which
// the x64 jump continues at with our return address. Native and ARM64EC targets are jumped to.
//
    NESTED_ENTRY __adjthunk_helper

    stp     fp, lr, [sp, #-16]!
    mov     fp, sp
    mov     x10, xzr
    adrp    xip0, __os_arm64x_dispatch_icall
    ldr     xip0, [xip0, __os_arm64x_dispatch_icall]
    blr     xip0
    ldp     fp, lr, [sp], #16
    cbz     x11, AdjthunkX64
    br      x11
LABEL(AdjthunkX64)
    adrp    xip0, __os_arm64x_dispatch_fptr
    ldr     xip0, [xip0, __os_arm64x_dispatch_fptr]
    br      xip0
LABEL(AdjthunkHelperEnd)

    NESTED_END __adjthunk_helper

//
// void __RTC_memset(void *Destination, ULONG64 Pattern, SIZE_T Length)
// Fills /RTC stack frames, the pattern is already replicated to 64 bits.
//
    LEAF_ENTRY __RTC_memset

    and     x9, x2, #0xFFFFFFFFFFFFFFF0
    cbz     x9, RtcFillTail
    add     x9, x9, x0

LABEL(RtcFill16)
    stp     x1, x1, [x0], #16
    cmp     x0, x9
    blo     RtcFill16

LABEL(RtcFillTail)
    tbz     x2, #3, RtcFill4
    str     x1, [x0], #8
LABEL(RtcFill4)
    tbz     x2, #2, RtcFill2
    str     w1, [x0], #4
LABEL(RtcFill2)
    tbz     x2, #1, RtcFill1
    strh    w1, [x0], #2
LABEL(RtcFill1)
    tbz     x2, #0, RtcFillDone
    strb    w1, [x0]
LABEL(RtcFillDone)
    ret

    LEAF_END __RTC_memset

    XDATAAREA
LABEL(IcallHelperXdata)
    /* One code word, the epilog starts at code index 1 since it does not undo set_fp */
    dcd     ((IcallHelperEnd - __icall_helper_arm64ec) / 4) + 0x08600000
    dcb     0xE1                        // set_fp
    dcb     0x81                        // save_fplr_x 16
    dcb     0xE4                        // end
    dcb     0xE3                        // padding

    XDATAAREA
LABEL(AdjthunkHelperXdata)
    /* Same frame as above, set_fp is not undone by the epilog */
    dcd     ((AdjthunkHelperEnd - __adjthunk_helper) / 4) + 0x08600000
    dcb     0xE1                        // set_fp
    dcb     0x81                        // save_fplr_x 16
    dcb     0xE4                        // end
    dcb     0xE3                        // padding

    PDATAAREA
    DCRVA   __icall_helper_arm64ec
    DCRVA   IcallHelperXdata
    DCRVA   __adjthunk_helper
    DCRVA   AdjthunkHelperXdata

    END
