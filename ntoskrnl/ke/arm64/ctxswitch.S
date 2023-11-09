 #include <kxarm64.h>


/*!
 * KiSwapContext
 *
 * \brief
 *     The KiSwapContext routine switches context to another thread.
 *
 * BOOLEAN
 * KiSwapContext(
 *     _In_ KIRQL WaitIrql,
 *     _Inout_ PKTHREAD CurrentThread);
 *
 * \param WaitIrql <r0>
 *     ...
 *
 * \param CurrentThread <r1>
 *     Pointer to the KTHREAD of the current thread.
 *
 * \return
 *     The WaitStatus of the Target Thread.
 *
 * \remarks
 *     This is a wrapper around KiSwapContextInternal which will save all the
 *     non-volatile registers so that the Internal function can use all of
 *     them. It will also save the old current thread and set the new one.
 *
 *     The calling thread does not return after KiSwapContextInternal until
 *     another thread switches to it.
 *
 *--*/
    EXTERN BasicStack

    NESTED_ENTRY KiSwapContext
    PROLOG_END KiSwapContext
    /* We're a LONG way from remotely trying to implement this */
    b KiSwapContext
    NESTED_END KiSwapContext


    NESTED_ENTRY KiThreadStartup
    PROLOG_END KiThreadStartup
    NESTED_END KiThreadStartup


    NESTED_ENTRY KiSwitchThreads
    PROLOG_END KiSwitchThreads
    NESTED_END KiSwitchThreads

    END