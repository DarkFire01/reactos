
#ifdef _MSC_VER

    AREA |.text|, ALIGN=2, CODE, READONLY

    EXTERN UefiServiceStack
    EXTERN BasicStack
    EXTERN ExecuteLoaderCleanly
    EXTERN UefiExitBootServices

    /* void _exituefi(VOID) */
    EXPORT _exituefi
_exituefi PROC
    /* Keep LR and one callee-saved register to restore the original stack. */
    stp     x19, x30, [sp, #-16]!

    mov     x19, sp

    ldr     x0, =UefiServiceStack
    ldr     x0, [x0]
    and     x0, x0, #0xFFFFFFFFFFFFFFF0
    mov     sp, x0

    bl      UefiExitBootServices

    mov     sp, x19
    ldp     x19, x30, [sp], #16
    ret
_exituefi ENDP

    /* void _changestack(VOID) */
    EXPORT _changestack
_changestack PROC
    /* x0 = previous stack pointer (argument to ExecuteLoaderCleanly). */
    mov     x0, sp

    ldr     x1, =BasicStack
    ldr     x1, [x1]
    and     x1, x1, #0xFFFFFFFFFFFFFFF0
    mov     sp, x1

    bl      ExecuteLoaderCleanly

    /* ExecuteLoaderCleanly is noreturn; keep a return for safety. */
    ret
_changestack ENDP

    END

#else

/*
 * ARM64 GAS (GCC/Clang) path is not yet needed for the VS ARM64 build.
 */

#endif
