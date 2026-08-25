#include <syscalls.inc>

/* The Win10 syscall table is emitted whole by ntdll.S. Nothing is left for
   a separate NT6 half, but the target is kept so the modules that link it
   keep building; they reach the syscalls through ntdll's import library. */

#ifndef _M_ARM
.code
#endif

END
