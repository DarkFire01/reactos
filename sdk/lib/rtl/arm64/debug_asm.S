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
    brk #0xF00
    ret
    NESTED_END DbgBreakPointNoBugCheck

    NESTED_ENTRY DbgUserBreakPoint
    ;PROLOG_END DbgUserBreakPoint
     brk #0xF00
    ret
    NESTED_END DbgUserBreakPoint

    NESTED_ENTRY DbgBreakPoint
    ;PROLOG_END DbgBreakPoint
    brk #0xF00
    ret
    NESTED_END DbgBreakPoint

    NESTED_ENTRY DbgBreakPointWithStatus
    ;PROLOG_END DbgBreakPointWithStatus
    brk #0xF00
    ret
    NESTED_END DbgBreakPointWithStatus

    NESTED_ENTRY DebugService2
    ;PROLOG_END DebugService2
    brk #0xF00
    ret
    NESTED_END DebugService2










   NESTED_ENTRY HoldForExe
   wfi
   NESTED_END HoldForExe



    NESTED_ENTRY DebugService

   ; implementme
   brk #0xF00
   wfi
   b HoldForExe

   /* Return */
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
