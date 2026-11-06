/*
 * PROJECT:     ReactOS runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 debug break and debug service routines
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

/* brk immediates the ARM64 kernel decodes */
#define BREAKPOINT_BREAK        0xF000
#define BREAKPOINT_SERVICE      0xF002

    TEXTAREA

    ARM64EC_ENTRY_THUNK A64NAME(DbgBreakPoint),0,0
    LEAF_ENTRY_COMDAT A64NAME(DbgBreakPoint)
    ALTERNATE_ENTRY A64NAME(DbgUserBreakPoint)
    brk     #BREAKPOINT_BREAK
    ret
    LEAF_END A64NAME(DbgBreakPoint)

    ARM64EC_ENTRY_THUNK A64NAME(DbgBreakPointWithStatus),1,0
    LEAF_ENTRY_COMDAT A64NAME(DbgBreakPointWithStatus)
    ALTERNATE_ENTRY A64NAME(RtlpBreakWithStatusInstruction)
    brk     #BREAKPOINT_BREAK
    ret
    LEAF_END A64NAME(DbgBreakPointWithStatus)

//
// NTSTATUS
// DebugService(
//     _In_ ULONG Service,
//     _In_ PVOID Buffer,
//     _In_ ULONG Length,
//     _In_ PVOID Argument1,
//     _In_ PVOID Argument2);
//
// The service number goes in x16 and the arguments move down to x0-x3.
// The kernel steps over the brk that follows the service brk.
//
    LEAF_ENTRY A64NAME(DebugService)
    mov     x16, x0
    mov     x0, x1
    mov     w1, w2
    mov     x2, x3
    mov     x3, x4
    brk     #BREAKPOINT_SERVICE
    brk     #BREAKPOINT_BREAK
    ret
    LEAF_END A64NAME(DebugService)

//
// VOID
// DebugService2(
//     _In_ PVOID Argument1,
//     _In_ PVOID Argument2,
//     _In_ ULONG Service);
//
    LEAF_ENTRY A64NAME(DebugService2)
    mov     w16, w2
    brk     #BREAKPOINT_SERVICE
    brk     #BREAKPOINT_BREAK
    ret
    LEAF_END A64NAME(DebugService2)

    END
