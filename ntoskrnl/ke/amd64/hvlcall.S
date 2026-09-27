/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Issuing a hypercall that carries its payload in registers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <asm.inc>

.code64

/*
 * ULONG64 NTAPI HvlpExtendedHypercall(ULONG64 Control<rcx>,
 *                                     PVOID Payload<rdx>,
 *                                     ULONG64 Units<r8>,
 *                                     PVOID Extended<r9>,
 *                                     PVOID HypercallPage<rsp+40>);
 *
 * An extended hypercall carries its first sixteen bytes in two registers and
 * each further sixteen in one of XMM0 through XMM5, in order. Units counts all
 * of them, so a payload of one unit needs no XMM register at all.
 *
 * All six come back out whether the call had anything to say in them or not,
 * because which of them hold the answer depends on how long the payload was,
 * and the caller is the one that knows.
 */
PUBLIC HvlpExtendedHypercall
HvlpExtendedHypercall:
    push rbx
    push rsi

    /* Thirty two bytes for the callee, and eight to leave the stack aligned */
    sub rsp, 40

    mov rbx, r9
    mov rsi, rdx
    mov r10, [rsp + 96]
    mov r11, r8

    cmp r11, 2
    jb HvlpPayloadLoaded
    movups xmm0, [rsi + 16]
    cmp r11, 3
    jb HvlpPayloadLoaded
    movups xmm1, [rsi + 32]
    cmp r11, 4
    jb HvlpPayloadLoaded
    movups xmm2, [rsi + 48]
    cmp r11, 5
    jb HvlpPayloadLoaded
    movups xmm3, [rsi + 64]
    cmp r11, 6
    jb HvlpPayloadLoaded
    movups xmm4, [rsi + 80]
    cmp r11, 7
    jb HvlpPayloadLoaded
    movups xmm5, [rsi + 96]

HvlpPayloadLoaded:
    mov rdx, [rsi]
    mov r8, [rsi + 8]
    call r10

    movups [rbx], xmm0
    movups [rbx + 16], xmm1
    movups [rbx + 32], xmm2
    movups [rbx + 48], xmm3
    movups [rbx + 64], xmm4
    movups [rbx + 80], xmm5

    add rsp, 40
    pop rsi
    pop rbx
    ret

END
