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
 * Reading the SMI port traps into the BIOS, which takes the function in AX,
 * the selector in BX and the data in CX. It returns its result code in AH
 * and any data in CX.
 */
PUBLIC _HalpToshibaCallSmi@16
_HalpToshibaCallSmi@16:
    push ebp
    mov ebp, esp
    push ebx

    mov ecx, dword ptr [ebp + 20]
    movzx ecx, word ptr [ecx]
    movzx ebx, word ptr [ebp + 16]
    movzx eax, word ptr [ebp + 12]
    movzx edx, word ptr [ebp + 8]
    in al, dx

    mov edx, dword ptr [ebp + 20]
    mov word ptr [edx], cx
    movzx eax, ah

    pop ebx
    pop ebp
    ret 16

END
