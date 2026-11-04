/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     TLB, cache and barrier helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    TEXTAREA

//
// VOID
// HalpFlushTbEntry(
//     _In_ PVOID VirtualAddress);
//
    LEAF_ENTRY HalpFlushTbEntry
    lsr     x0, x0, #12
    dsb     ishst
    tlbi    vaae1is, x0
    dsb     ish
    isb
    ret
    LEAF_END HalpFlushTbEntry

//
// VOID
// HalpPublishPteWrites(VOID);
//
    LEAF_ENTRY HalpPublishPteWrites
    dsb     ishst
    isb
    ret
    LEAF_END HalpPublishPteWrites

//
// VOID
// HalpSweepDcacheRange(
//     _In_ PVOID BaseAddress,
//     _In_ SIZE_T Length);
//
// Cleans and invalidates a buffer to the point of coherency, so a DMA
// master that does not snoop the caches sees it and leaves no stale line.
//
    LEAF_ENTRY HalpSweepDcacheRange
    cbz     x1, HalpSweepDcacheDone
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4
    mov     x3, #4
    lsl     x3, x3, x2
    add     x1, x0, x1
    sub     x2, x3, #1
    bic     x0, x0, x2
LABEL(HalpSweepDcacheNext)
    dc      civac, x0
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    HalpSweepDcacheNext
LABEL(HalpSweepDcacheDone)
    dsb     sy
    ret
    LEAF_END HalpSweepDcacheRange

//
// VOID
// HalpWaitForInterrupt(VOID);
//
    LEAF_ENTRY HalpWaitForInterrupt
    dsb     sy
    wfi
    ret
    LEAF_END HalpWaitForInterrupt

//
// ULONG64
// HalpCallFirmware(
//     _In_ ULONG64 Function,
//     _In_ ULONG64 Argument1,
//     _In_ ULONG64 Argument2,
//     _In_ ULONG64 Argument3,
//     _In_ BOOLEAN UseHvc);
//
// PSCI call, the conduit comes from the FADT.
//
    LEAF_ENTRY HalpCallFirmware
    cbz     w4, HalpCallFirmwareSmc
    hvc     #0
    ret
LABEL(HalpCallFirmwareSmc)
    smc     #0
    ret
    LEAF_END HalpCallFirmware

    END
