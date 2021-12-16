RAW(""),
RAW("#include <kxarm64.h>"),
RAW(""),

#define CONTEXT_FULL 0x400007
#define CONTEXT_CONTROL 0x400001
#define CONTEXT_INTEGER 0x400002
#define CONTEXT_FLOATING_POINT 0x400004
#define CONTEXT_DEBUG_REGISTERS 0x400008

#define CxContextFlags 0x0
#define CxCpsr 0x4
#define CxX 0x8
#define CxFp 0xf0
#define CxLr 0xf8
#define CxSp 0x100
#define CxPc 0x108
#define CxFpsr 0x314
#define CxFpcr 0x310
#define CxV 0x110
#define CxBvr 0x338
#define CxBcr 0x318
#define CxWvr 0x380
#define CxWcr 0x378
#define CONTEXT_FRAME_LENGTH 0x390
#define CONTEXT_ALIGN 0x10
#define DEBUG_ACTIVE_DBG 0x1
#define DEBUG_ACTIVE_DBG_BIT 0x0
#define DEBUG_ACTIVE_DBG_INSTRUMENTED 0x3
#define DEBUG_ACTIVE_INSTRUMENTED 0x2
#define DEBUG_ACTIVE_MINIMAL_THREAD 0x4
#define DEBUG_ACTIVE_MINIMAL_THREAD_BIT 0x2