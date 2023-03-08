
#include <asm.inc>
#include <arch/pc/x86common.h>
#include <arch/pc/pcbios.h>
PUBLIC _gdtptr
PUBLIC _i386idtptr
EXTERN _i386Idt:DWORD

.code32

PUBLIC __UefiExit
__UefiExit:
#ifdef _USE_ML
    lgdt fword ptr [_gdtptr]
#else
    lgdt cs:[_gdtptr]
#endif
    push 8
    push offset resume
    retf
resume:
    ret

PUBLIC __ChangeStack
__ChangeStack:

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
    .word HEX(27)       /* Limit */
    .long gdt, 0          /* Base Address */

_i386idtptr:
    .word 255           /* Limit */
    .long _i386Idt     /* Base Address */
//void _UefiExit();

END
