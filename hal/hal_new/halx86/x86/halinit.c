/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/


/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
HalpArchInitializePhase0(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNIMPLEMENTED;
    for(;;)
    {

    }
}

VOID
NTAPI
HalpArchInitializePhase1(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNIMPLEMENTED;
    for(;;)
    {

    }
}

#ifdef _M_IX86
/* x86 fastcall wrappers */

#undef KeRaiseIrql
/*
 * @implemented
 */
VOID
NTAPI
KeRaiseIrql(KIRQL NewIrql,
            PKIRQL OldIrql)
{
    /* Call the fastcall function */
    *OldIrql = KfRaiseIrql(NewIrql);
}

#undef KeLowerIrql
/*
 * @implemented
 */
VOID
NTAPI
KeLowerIrql(KIRQL NewIrql)
{
    /* Call the fastcall function */
    KfLowerIrql(NewIrql);
}

#undef KeAcquireSpinLock
/*
 * @implemented
 */
VOID
NTAPI
KeAcquireSpinLock(PKSPIN_LOCK SpinLock,
                  PKIRQL OldIrql)
{
    /* Call the fastcall function */
    *OldIrql = KfAcquireSpinLock(SpinLock);
}

#undef KeReleaseSpinLock
/*
 * @implemented
 */
VOID
NTAPI
KeReleaseSpinLock(PKSPIN_LOCK SpinLock,
                  KIRQL NewIrql)
{
    /* Call the fastcall function */
    KfReleaseSpinLock(SpinLock, NewIrql);
}

#endif /* _M_IX86 */

