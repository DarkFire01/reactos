/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Funclet call helper for the ARM64 C specific handler
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    TEXTAREA

//
// LONG
// __C_specific_handler_call(
//     _In_ PVOID Argument,
//     _In_ PVOID EstablisherFrame,
//     _In_ PVOID Funclet,
//     _In_ PUCHAR NonVolatileRegisters);
//
// Filter and termination funclets run with the nonvolatile registers of the
// function they belong to, and get its frame pointer as second argument.
//
    NESTED_ENTRY A64NAME(__C_specific_handler_call)

    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
#ifndef _ARM64EC_
    stp     x23, x24, [sp, #48]
#endif
    stp     x25, x26, [sp, #64]
#ifdef _ARM64EC_
    str     x27, [sp, #80]
#else
    stp     x27, x28, [sp, #80]
#endif

    /* The buffer is laid out as x19-x29, EC has no x23, x24 or x28 */
    ldp     x19, x20, [x3, #0]
    ldp     x21, x22, [x3, #16]
#ifndef _ARM64EC_
    ldp     x23, x24, [x3, #32]
#endif
    ldp     x25, x26, [x3, #48]
#ifdef _ARM64EC_
    ldr     x27, [x3, #64]
#else
    ldp     x27, x28, [x3, #64]
#endif
    ldr     x1, [x3, #80]
    blr     x2

#ifdef _ARM64EC_
    ldr     x27, [sp, #80]
#else
    ldp     x27, x28, [sp, #80]
#endif
    ldp     x25, x26, [sp, #64]
#ifndef _ARM64EC_
    ldp     x23, x24, [sp, #48]
#endif
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #96
    ret
LABEL(__C_specific_handler_call_end)

    NESTED_END A64NAME(__C_specific_handler_call)

    XDATAAREA
LABEL(__C_specific_handler_call_xdata)
#ifdef _ARM64EC_
    /* The epilog reuses the prolog codes, three code words */
    dcd     ((__C_specific_handler_call_end - A64NAME(__C_specific_handler_call)) / 4) + 0x18200000
    dcb     0xD2, 0x0A                  // save_reg x27, 80
    dcb     0xC9, 0x88                  // save_regp x25, 64
    dcb     0xC8, 0x84                  // save_regp x21, 32
    dcb     0xC8, 0x02                  // save_regp x19, 16
    dcb     0x8B                        // save_fplr_x 96
    dcb     0xE4                        // end
    dcb     0xE3, 0xE3                  // padding
#else
    /* The epilog reuses the prolog codes, four code words */
    dcd     ((__C_specific_handler_call_end - A64NAME(__C_specific_handler_call)) / 4) + 0x20200000
    dcb     0xCA, 0x0A                  // save_regp x27, 80
    dcb     0xC9, 0x88                  // save_regp x25, 64
    dcb     0xC9, 0x06                  // save_regp x23, 48
    dcb     0xC8, 0x84                  // save_regp x21, 32
    dcb     0xC8, 0x02                  // save_regp x19, 16
    dcb     0x8B                        // save_fplr_x 96
    dcb     0xE4                        // end
    dcb     0xE3, 0xE3, 0xE3, 0xE3      // padding
#endif

    PDATAAREA
    DCRVA   A64NAME(__C_specific_handler_call)
    DCRVA   __C_specific_handler_call_xdata

    END
