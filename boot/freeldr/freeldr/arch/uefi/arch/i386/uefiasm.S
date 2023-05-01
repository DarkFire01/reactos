/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI assembly exit code
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include <asm.inc>
#include <arch/pc/x86common.h>
#include <arch/pc/pcbios.h>

.code32
PUBLIC _gdtptr
PUBLIC _i386idtptr
EXTERN _i386Idt:DWORD
EXTERN _EndofExitStack:DWORD
EXTERN _UefiExitToKernel:PROC

// VOID _LeaveUEFIStack(VOID);
PUBLIC __LeaveUEFIStack
__LeaveUEFIStack:
#ifdef _USE_ML
    lgdt fword ptr _gdtptr
    lidt fword ptr ds:[_i386idtptr]
#else
    lgdt cs:_gdtptr
    lidt _i386idtptr
#endif
    mov esp, _EndofExitStack
    call _UefiExitToKernel

// void __reloadsegment(VOID)
PUBLIC __reloadsegment
__reloadsegment:
    mov ax, PMODE_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    nop
    ret

    .align 4    /* force 4-byte alignment */
gdt:
    /* NULL Descriptor */
   .word HEX(0000)
   .word HEX(0000)
   .word HEX(0000)
   .word HEX(0000)

    /* 32-bit flat CS */
    .word HEX(FFFF)
    .word HEX(0000)
    .word HEX(9A00)
    .word HEX(00CF)

    /* 32-bit flat DS */
    .word HEX(FFFF)
    .word HEX(0000)
    .word HEX(9200)
    .word HEX(00CF)

    /* 16-bit real mode CS */
    .word HEX(FFFF)
    .word HEX(0000)
    .word HEX(9E00)
    .word HEX(0000)

    /* 16-bit real mode DS */
    .word HEX(FFFF)
    .word HEX(0000)
    .word HEX(9200)
    .word HEX(0000)

/* GDT table pointer */
_gdtptr:
    .word HEX(27)   /* Limit */
    .long gdt, 0    /* Base Address */

_i386idtptr:
    .word 255       /* Limit */
    .long _i386Idt  /* Base Address */
END
