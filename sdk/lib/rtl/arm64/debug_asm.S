 #include <kxarm64.h>

    NESTED_ENTRY ExpInterlockedPopEntrySList
    ;PROLOG_END ExpInterlockedPopEntrySList
    ret
    NESTED_END ExpInterlockedPopEntrySList

    NESTED_ENTRY ExpInterlockedPushEntrySList
    ;PROLOG_END ExpInterlockedPushEntrySList
    ret
    NESTED_END ExpInterlockedPushEntrySList

    NESTED_ENTRY DbgBreakPointNoBugCheck
    ;PROLOG_END DbgBreakPointNoBugCheck
    ret
    NESTED_END DbgBreakPointNoBugCheck

    NESTED_ENTRY DbgUserBreakPoint
    ;PROLOG_END DbgUserBreakPoint
    ret
    NESTED_END DbgUserBreakPoint

    NESTED_ENTRY DbgBreakPoint
    ;PROLOG_END DbgBreakPoint
    ret
    NESTED_END DbgBreakPoint

    NESTED_ENTRY DbgBreakPointWithStatus
    ;PROLOG_END DbgBreakPointWithStatus
    ret
    NESTED_END DbgBreakPointWithStatus

    NESTED_ENTRY DebugService2
    ;PROLOG_END DebugService2
    ret
    NESTED_END DebugService2

    NESTED_ENTRY DebugService
    ;PROLOG_END DebugService
    ret
    NESTED_END DebugService

    NESTED_ENTRY RtlCaptureContext
    ;PROLOG_END RtlCaptureContext
    ret
    NESTED_END RtlCaptureContext

    NESTED_ENTRY RtlpBreakWithStatusInstruction
    ;PROLOG_END RtlpBreakWithStatusInstruction
    ret
    NESTED_END RtlpBreakWithStatusInstruction

    END
