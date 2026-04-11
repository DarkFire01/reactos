;
; PROJECT:     ReactOS msvcrt.dll
; LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
; PURPOSE:     MSVC ARM64 assembly for Wine SEH (see msvcrt/except_arm64.c)
;

    EXPORT call_exc_handler

    AREA    |.text|, CODE, READONLY, ALIGN=4

; void *call_exc_handler(void *handler, ULONG_PTR frame, UINT flags, BYTE *nonvol_regs);

call_exc_handler PROC
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    str     x1, [sp, #-16]!

    ldp     x19, x20, [x3, #0]
    ldp     x21, x22, [x3, #16]
    ldp     x23, x24, [x3, #32]
    ldp     x25, x26, [x3, #48]
    ldp     x27, x28, [x3, #64]
    ldr     x29, [x3, #80]
    blr     x0

    add     sp, sp, #16
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret
    ENDP

    END
