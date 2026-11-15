/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ARM64 boot stack and PCR setup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ksarm64.h>

/* KeInitializeThread builds the idle thread's frames right below its initial stack */
#define BOOT_STACK_RESERVE  (KTRAP_FRAME_LENGTH + KARM64_VFP_STATE_LENGTH + \
                             KEXCEPTION_FRAME_LENGTH + KSTART_FRAME_LENGTH + \
                             KSWITCH_FRAME_LENGTH)

    IMPORT KiSystemStartupBootStack

    TEXTAREA

//
// VOID
// KiArm64SetPcr(
//     _In_ PKIPCR Pcr);
//
// x18 is reserved for the platform, in kernel mode it holds the PCR.
// TPIDR_EL1 keeps a copy to reload x18 on entry from user mode.
//
    LEAF_ENTRY KiArm64SetPcr
    msr     tpidr_el1, x0
    mov     x18, x0
    ret
    LEAF_END KiArm64SetPcr

//
// DECLSPEC_NORETURN
// VOID
// KiSwitchToBootStack(
//     _In_ ULONG_PTR InitialStack);
//
    LEAF_ENTRY KiSwitchToBootStack
    sub     sp, x0, #BOOT_STACK_RESERVE
    mov     x29, #0
    mov     x30, #0
    b       KiSystemStartupBootStack
    LEAF_END KiSwitchToBootStack

    IMPORT KiApStartFailure

//
// Vectors a secondary processor runs with until KeInitExceptions, before it
// has a PCR to take a real trap with. Every exception is recorded in
// KiApStartFailure as 0xEA:ESR, ELR, FAR, and the processor parks.
//
#ifdef _MSC_VER
    AREA |.text$apv|,ALIGN=11,CODE,READONLY
#else
    .section .text$apv, "xr"
    .p2align 11
#endif

    LEAF_ENTRY KiApEarlyVectors
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128
    b       KiApEarlyTrap
    ALIGN   128

LABEL(KiApEarlyTrap)
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    ldr     x3, =KiApStartFailure
    str     x1, [x3, #8]
    str     x2, [x3, #16]
    mov     x4, #0xEA
    lsl     x4, x4, #32
    mov     w0, w0
    orr     x0, x0, x4
    dmb     ish
    str     x0, [x3]
    dsb     ish
LABEL(KiApEarlyPark)
    wfi
    b       KiApEarlyPark
    LTORG
    LEAF_END KiApEarlyVectors

    END
