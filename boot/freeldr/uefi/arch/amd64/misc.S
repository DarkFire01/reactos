#include <asm.inc>
#include <arch/pc/x86common.h>
#include <arch/pc/pcbios.h>

.code64

// void __lgdt(void *Source<rcx>);
PUBLIC __lgdt
__lgdt:
#ifdef _USE_ML
    lgdt fword ptr [rcx]
#else
    lgdt cs:[rcx]
#endif
    ret

// void __ltr(unsigned short Source<rcx>);
PUBLIC __ltr
__ltr:
    ltr cx
    ret

END
