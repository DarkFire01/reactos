 #include <kxarm64.h>

    NESTED_ENTRY KeZeroPages
    ;PROLOG_END KeZeroPages
    ret
    NESTED_END KeZeroPages


    NESTED_ENTRY KiPassiveRelease
    ;PROLOG_END KiPassiveRelease
    ret
    NESTED_END KiPassiveRelease

    NESTED_ENTRY KeSwitchKernelStack
    ;PROLOG_END KeSwitchKernelStack
    ret
    NESTED_END KeSwitchKernelStack

    END