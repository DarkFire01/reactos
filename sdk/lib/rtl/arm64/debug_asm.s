
#include <ksarm64.h>

    TEXTAREA

    LEAF_ENTRY DbgBreakPoint
    ARMASM_DEBUGBREAK
    ret
    LEAF_END DbgBreakPoint

    LEAF_ENTRY DbgUserBreakPoint
    ARMASM_DEBUGBREAK
    ret
    LEAF_END DbgUserBreakPoint

    LEAF_ENTRY DbgBreakPointWithStatus
    ALTERNATE_ENTRY RtlpBreakWithStatusInstruction
    ARMASM_DEBUGBREAK
    ret
    LEAF_END DbgBreakPointWithStatus

    /*
     * DebugService / DebugService2: BRK #0xF001 (ARM64_DEBUG_SERVICE).
     * AAPCS64 arguments are already in x0-x4 / x0-x2; execution may resume
     * after the debugger returns.
     */
    LEAF_ENTRY DebugService
    ARMASM_DEBUGSERVICE
    ret
    LEAF_END DebugService

    LEAF_ENTRY DebugService2
    ARMASM_DEBUGSERVICE
    ret
    LEAF_END DebugService2

    END
