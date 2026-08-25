
#include <syscalls.inc>

/* The table is a single Win10-ordered list now, so every syscall stub comes
   from here. It cannot be split into an NT5 half and an NT6 half any more:
   the two are interleaved by the Win10 numbering. */

#ifdef _M_ARM

    TEXTAREA

#define SVC_(name, argcount) STUB_U name

#include <sysfuncs.h>

    END

#else

.code

#define SVC_(name, argcount) STUB_U name, argcount

#include <sysfuncs.h>

END
#endif
