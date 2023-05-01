/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI assembly exit code
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include <asm.inc>
#include <arch/pc/x86common.h>
#include <arch/pc/pcbios.h>

.code64
EXTERN UefiExitToKernel:PROC
EXTERN EndofExitStack:QWORD
PUBLIC gdtptr

.align 8
gdt:
    .word HEX(0000), HEX(0000), HEX(0000), HEX(0000) /* 00: NULL descriptor */
    .word HEX(0000), HEX(0000), HEX(0000), HEX(0000) /* 08:  */
    .word HEX(0000), HEX(0000), HEX(9800), HEX(0020) /* 10: long mode CS */
    .word HEX(FFFF), HEX(0000), HEX(F300), HEX(00CF) /* 18: long mode DS */
    .word HEX(FFFF), HEX(0000), HEX(9E00), HEX(0000) /* 20: 16-bit real mode CS */
    .word HEX(FFFF), HEX(0000), HEX(9200), HEX(0000) /* 28: 16-bit real mode DS */
    .word HEX(FFFF), HEX(0000), HEX(9B00), HEX(00CF) /* 30: compat mode CS */

/* GDT table pointer */
gdtptr:
    .word HEX(37) /* Limit */
#ifdef _USE_ML
    .quad gdt     /* Base Address */
#else
    .long gdt, 0  /* Base Address */
#endif

PUBLIC __ExitUefi
__ExitUefi:
#ifdef _USE_ML
    lgdt fword ptr gdtptr
#else
    lgdt cs:gdtptr
#endif
    mov rsp, EndofExitStack
	call UefiExitToKernel
	hlt
END
