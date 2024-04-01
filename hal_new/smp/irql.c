
/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

#ifndef KeGetCurrentIrql
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    /* Return the IRQL */
    return KeGetPcr()->Irql;
}
#endif

FORCEINLINE
VOID
KeSetCurrentIrql(
    _In_ KIRQL NewIrql)
{
    /* Set new current IRQL */
    KeGetPcr()->Irql = NewIrql;
}

VOID
FASTCALL
KfLowerIrql(
    IN KIRQL OldIrql)
{

}

KIRQL
FASTCALL
KfRaiseIrql(
    IN KIRQL NewIrql)
{
    KIRQL OldIrql;

    /* Read the current IRQL */
    OldIrql = KeGetCurrentIrql();

    /* Return old IRQL */
    return OldIrql;
}

