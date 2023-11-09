 #include <kxarm64.h>

    NESTED_ENTRY KiServiceExit
    ret
    NESTED_END KiServiceExit

    NESTED_ENTRY KiExceptionExit
    ret
    NESTED_END KiExceptionExit

    NESTED_ENTRY KiSwapProcess
    ret
    NESTED_END KiSwapProcess

    END
    