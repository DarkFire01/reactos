/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     First code of a secondary processor started through PSCI
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

/* HALP_AP_START, see halp.h */
#define ApMair          0x00
#define ApTcr           0x08
#define ApSctlr         0x10
#define ApCpacr         0x18
#define ApIdentityRoot  0x20
#define ApRoot          0x28
#define ApStack         0x30
#define ApEntry         0x38
#define ApArgument      0x40
#define ApPageVa        0x48
#define ApProgress      0x50
#define ApVbar          0x58

    TEXTAREA

//
// HalpApTrampoline is copied to a page of its own and runs there with the MMU off,
// x0 holds the physical address of the HALP_AP_START that follows it in that page.
// A temporary TTBR0 maps the page one to one, so the code survives turning the MMU
// on, then it moves to the page's kernel address and puts the real root in TTBR0.
// Only position independent code, it runs from a copy.
//
    LEAF_ENTRY HalpApTrampoline
    mov     x19, x0
    mov     x1, #1
    str     x1, [x19, #ApProgress]

    ldr     x1, [x19, #ApMair]
    msr     mair_el1, x1
    ldr     x1, [x19, #ApTcr]
    msr     tcr_el1, x1
    ldr     x1, [x19, #ApIdentityRoot]
    msr     ttbr0_el1, x1
    ldr     x1, [x19, #ApRoot]
    msr     ttbr1_el1, x1
    ldr     x1, [x19, #ApCpacr]
    msr     cpacr_el1, x1
    isb
    tlbi    vmalle1
    ic      iallu
    dsb     nsh
    isb

    /* Everything the kernel half needs, read before the identity map goes */
    ldr     x20, [x19, #ApRoot]
    ldr     x21, [x19, #ApStack]
    ldr     x22, [x19, #ApEntry]
    ldr     x23, [x19, #ApArgument]
    ldr     x24, [x19, #ApPageVa]
    ldr     x25, [x19, #ApVbar]

    ldr     x1, [x19, #ApSctlr]
    msr     sctlr_el1, x1
    isb

    /* Same page through its kernel address */
    adr     x2, HalpApTrampolineHigh
    and     x2, x2, #0xFFF
    add     x2, x2, x24
    br      x2

LABEL(HalpApTrampolineHigh)
    and     x3, x19, #0xFFF
    add     x3, x3, x24
    mov     x1, #2
    str     x1, [x3, #ApProgress]

    dsb     ish
    msr     ttbr0_el1, x20
    isb
    tlbi    vmalle1
    dsb     nsh
    isb

    /* Early vectors from the kernel, so a fault before it has a PCR is not lost */
    cbz     x25, HalpApTrampolineNoVbar
    msr     vbar_el1, x25
    isb
LABEL(HalpApTrampolineNoVbar)
    mov     sp, x21
    mov     x29, #0
    mov     x30, #0
    mov     x0, x23
    br      x22
    LEAF_END HalpApTrampoline

//
// Marks the end of the code HalpApTrampoline copies.
//
    LEAF_ENTRY HalpApTrampolineEnd
    ret
    LEAF_END HalpApTrampolineEnd

    END
