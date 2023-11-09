 #include <kxarm64.h>

    EXTERN BasicStack
    EXTERN UefiExitBootServices
    EXTERN ExecuteLoaderCleanly

    NESTED_ENTRY _changestack
    ldr x1, =BasicStack
    mov sp, x1
    b ExecuteLoaderCleanly
    NESTED_END _changestack

    NESTED_ENTRY _exituefi
    b UefiExitBootServices
    ret
    NESTED_END _exituefi

    END