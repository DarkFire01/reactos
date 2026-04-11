
#include <syscalls.inc>

#if defined(_M_ARM) || defined(_ARM_)

	TEXTAREA

#define SVC_(name, argcount) STUB_U name

#include "w32ksvc32.h"

	END

#elif defined(_M_ARM64) || defined(_ARM64_) || defined(__arm64__) || defined(__aarch64__)

	TEXTAREA

#define SVC_(name, argcount) STUB_U name

#include "w32ksvc64.h"

	END

#else

#include <asm.inc>

.code

SyscallId = HEX(1000)
#define SVC_(name, argcount) STUB_U name, argcount

#ifdef _WIN64
#include <w32ksvc64.h>
#else
#include <w32ksvc32.h>
#endif

END

#endif
