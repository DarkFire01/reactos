/* Purpose: This code is used to start up the APs */
/*     0x2000  Location of our stack */
/*     0x3000  Startup code for the APs (this code) */
#include <asm.inc>

.code16
_APboot: 
    cli /*Clear All interupts just incase.*/
    xor   ax, ax
	mov	  ds, ax
	mov	  ss, ax
   /* jmp _APPrep*/
    jmp $

    
_APGDT:
    .nulldesc:
        .word	HEX(0000)      /* Null descriptor*/
        .word	HEX(0000)  
        .word	HEX(0000) 
        .word	HEX(0000) 
    .codedesc:
        .word	HEX(ffff)   /* Kernel code descriptor */
        .word	HEX(0000)
        .word	HEX(9A00)
        .word	HEX(00CF)

    .datadesc:
        .word	HEX(FFFF)   /*  Kernel data descriptor */
        .word	HEX(0000)
        .word	HEX(9200)
        .word	HEX(00CF)
    .gdtend:

    .gdtdescriptor:
        _gdtsize:
            .word .gdtend - .nulldesc - 1
            .long .nulldesc
    /*
    codeseg:
        .word (.codedesc - .nulldesc)
    dataseg:
        .word (.datadesc - .nulldesc)
    
    gdtptr: 
        .long .gdtdescriptor*/
          
.endcode16

.code32
_APPrep:
#ifdef _USE_ML
    /*lgdt fword ptr .gdtdescriptor*/
#else
    /*lgdt .gdtdescriptor*/
#endif
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp _APProtectedMode

_APProtectedMode:
   /* mov     ax, dataseg*/
    mov		ds, ax
    mov		es, ax
    mov		fs, ax
    mov		gs, ax
    mov		ss, ax
    mov   eax, HEX(2000) /*ReactOS AP Stack.*/
    mov   esp, eax
    /* Jump to start of the kernel*/
    mov	     eax, HEX(2008)
    jmp      eax
END