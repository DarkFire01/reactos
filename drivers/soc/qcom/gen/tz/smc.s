/*
 * PROJECT:     ReactOS Qualcomm TrustZone driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Secure monitor call and cache maintenance stubs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    TEXTAREA

//
// LONG
// TzSmcCommandBuffer(
//     _In_ ULONG_PTR CommandAddress);
//
// Legacy convention. A positive return means the monitor was interrupted,
// and the call is reissued with the registers it left behind.
//
    NESTED_ENTRY TzSmcCommandBuffer
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x2, x0
    mov     x1, sp
    mov     w0, #1
LABEL(TzSmcCommandBufferIssue)
    smc     #0
    cmp     w0, #0
    b.gt    TzSmcCommandBufferIssue
    ldp     x29, x30, [sp], #16
    ret
    NESTED_END TzSmcCommandBuffer

//
// LONG
// TzSmcFast(
//     _In_ ULONG FunctionId,
//     _In_ ULONG ParamId,
//     _Inout_ PULONG64 Registers,
//     _Out_ PULONG64 Session);
//
// Registers[0..3] go out in x2..x5 and x1..x3 come back in Registers[0..2].
//
    NESTED_ENTRY TzSmcFast
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    mov     x19, x2
    mov     x20, x3
    ldp     x2, x3, [x19]
    ldp     x4, x5, [x19, #16]
LABEL(TzSmcFastIssue)
    smc     #0
    cmp     w0, #0
    b.gt    TzSmcFastIssue
    stp     x1, x2, [x19]
    str     x3, [x19, #16]
    str     x6, [x20]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    NESTED_END TzSmcFast

//
// VOID
// TzCleanInvalidateDcache(
//     _In_ PVOID Buffer,
//     _In_ SIZE_T Length);
//
    LEAF_ENTRY TzCleanInvalidateDcache
    cbz     x1, TzCleanInvalidateDone
    dsb     sy
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4
    mov     x3, #4
    lsl     x3, x3, x2
    add     x1, x0, x1
    sub     x2, x3, #1
    bic     x0, x0, x2
LABEL(TzCleanInvalidateNext)
    dc      civac, x0
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    TzCleanInvalidateNext
    dsb     sy
LABEL(TzCleanInvalidateDone)
    ret
    LEAF_END TzCleanInvalidateDcache

//
// VOID
// TzInvalidateDcache(
//     _In_ PVOID Buffer,
//     _In_ SIZE_T Length);
//
    LEAF_ENTRY TzInvalidateDcache
    cbz     x1, TzInvalidateDone
    dsb     sy
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4
    mov     x3, #4
    lsl     x3, x3, x2
    add     x1, x0, x1
    sub     x2, x3, #1
    bic     x0, x0, x2
LABEL(TzInvalidateNext)
    dc      ivac, x0
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    TzInvalidateNext
    dsb     sy
LABEL(TzInvalidateDone)
    ret
    LEAF_END TzInvalidateDcache

    END
