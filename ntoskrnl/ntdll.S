
#include <syscalls.inc>

#if defined(_M_ARM) || defined(_ARM_)

    TEXTAREA

#define SVC_(name, argcount) STUB_U name

#include <sysfuncs.h>

    END

#elif defined(_M_ARM64) || defined(_ARM64_) || defined(__arm64__) || defined(__aarch64__)

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
