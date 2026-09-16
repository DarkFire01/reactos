/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Toshiba BIOS SMI call
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <asm.inc>

.code

/*
 * UCHAR NTAPI HalpToshibaCallSmi(USHORT Port, USHORT Function, USHORT Selector, PUSHORT Data)
 *
 * Reading the SMI port traps into the BIOS with the function in AX, the
 * selector in BX and the data in CX. The BIOS returns its status in AH and
 * the data in CX, and may change the other general registers.
 */
PUBLIC _HalpToshibaCallSmi@16
_HalpToshibaCallSmi@16:
    push ebp
    push ebx
    push esi
    push edi

    /* The arguments start past the four saved registers and the return address */
    mov edx, dword ptr [esp + 32]
    movzx ecx, word ptr [edx]
    movzx ebx, word ptr [esp + 28]
    movzx eax, word ptr [esp + 24]
    movzx edx, word ptr [esp + 20]
    in al, dx

    mov edx, dword ptr [esp + 32]
    mov word ptr [edx], cx
    movzx eax, ah

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret 16

END
