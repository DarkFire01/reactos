/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC indirect call check
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    IMPORT RtlIsEcCode

/* mov rax, rsp / mov [rax+20h], rbx / push rbp, then pop rbp / jmp rel32: an ARM64EC export's x64 entry */
#define FFS_PROLOG      0x5520588948C48B48
#define FFS_JUMP        0xE95D

    TEXTAREA

//
// __os_arm64x_dispatch_icall for ARM64EC code. x11 is the target, x10 its exit thunk.
// Native and ARM64EC targets come back unchanged in x11. For x64 code x11 becomes the exit
// thunk and x9 the x64 target. An x64 entry that only jumps to ARM64EC code resolves to it.
// x0-x8, x15 and q0-q7 are preserved for the call that follows.
//
    NESTED_ENTRY LdrpEcCallCheck

    stp     fp, lr, [sp, #-0xF0]!
    mov     fp, sp
    stp     x0, x1, [sp, #0x10]
    stp     x2, x3, [sp, #0x20]
    stp     x4, x5, [sp, #0x30]
    stp     x6, x7, [sp, #0x40]
    stp     x8, x15, [sp, #0x50]
    stp     x19, x20, [sp, #0x60]
    stp     q0, q1, [sp, #0x70]
    stp     q2, q3, [sp, #0x90]
    stp     q4, q5, [sp, #0xB0]
    stp     q6, q7, [sp, #0xD0]

    mov     x19, x11
    mov     x20, x10

LABEL(CallCheckTarget)
    mov     x0, x19
    bl      RtlIsEcCode
    cbnz    w0, CallCheckNative

    /* Fast forward sequences are 16 byte aligned */
    tst     x19, #15
    b.ne    CallCheckJump
    ldr     x9, [x19]
    ldr     x10, =FFS_PROLOG
    cmp     x9, x10
    b.ne    CallCheckJump
    ldrh    w9, [x19, #8]
    mov     w10, #FFS_JUMP
    cmp     w9, w10
    b.ne    CallCheckX64
    ldrsw   x9, [x19, #10]
    add     x19, x19, #14
    add     x19, x19, x9
    b       CallCheckTarget

    /* jmp qword ptr [rip+disp32], an import thunk */
LABEL(CallCheckJump)
    ldrh    w9, [x19]
    mov     w10, #0x25FF
    cmp     w9, w10
    b.ne    CallCheckX64
    ldrsw   x9, [x19, #2]
    add     x9, x19, x9
    ldr     x19, [x9, #6]
    b       CallCheckTarget

LABEL(CallCheckX64)
    mov     x9, x19
    mov     x11, x20
    b       CallCheckDone

LABEL(CallCheckNative)
    mov     x11, x19

LABEL(CallCheckDone)
    ldp     q6, q7, [sp, #0xD0]
    ldp     q4, q5, [sp, #0xB0]
    ldp     q2, q3, [sp, #0x90]
    ldp     q0, q1, [sp, #0x70]
    ldp     x19, x20, [sp, #0x60]
    ldp     x8, x15, [sp, #0x50]
    ldp     x6, x7, [sp, #0x40]
    ldp     x4, x5, [sp, #0x30]
    ldp     x2, x3, [sp, #0x20]
    ldp     x0, x1, [sp, #0x10]
    ldp     fp, lr, [sp], #0xF0
    ret

    NESTED_END LdrpEcCallCheck

//
// ULONG_PTR LdrpCallX64(PVOID ExitToX64, PVOID Target, ULONG_PTR Arg1, ULONG_PTR Arg2, ULONG_PTR Arg3)
//
// Calls x64 code from native code the way an exit thunk does: the arguments go to the registers
// that hold rcx, rdx and r8 and the x64 home space sits right above the return address the
// emulator pushes. rax comes back in x8.
//
    NESTED_ENTRY LdrpCallX64

    stp     fp, lr, [sp, #-0x10]!
    mov     fp, sp
    sub     sp, sp, #0x20

    mov     x16, x0
    mov     x9, x1
    mov     x0, x2
    mov     x1, x3
    mov     x2, x4
    mov     x3, xzr
    blr     x16
    mov     x0, x8

    mov     sp, fp
    ldp     fp, lr, [sp], #0x10
    ret

    NESTED_END LdrpCallX64

    END
