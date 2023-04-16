#include "gicp.h"

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

#undef KeGetCurrentIrql

/* GLOBALS ********************************************************************/

/* FUNCTIONS ******************************************************************/

VOID
HalpInitializeInterrupts(VOID)
{
   UNIMPLEMENTED;
   while (TRUE);
}

/* IRQL MANAGEMENT ************************************************************/

/*
 * @implemented
 */
ULONG
HalGetInterruptSource(VOID)
{
   UNIMPLEMENTED;
   while (TRUE);
   return 0;
}

/*
 * @implemented
 */


/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    UNIMPLEMENTED;
    while (TRUE);
    return 0;
}

/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    UNIMPLEMENTED;
    while (TRUE);
    return 0;
}

/*
 * @unimplemented
 */
KIRQL
FASTCALL
KfRaiseIrql(IN KIRQL NewIrql)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;
    /* Read current IRQL */
    CurrentIrql = Pcr->CurrentIrql;

#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > NewIrql)
    {
        /* Crash system */
        //Pcr->Irql = PASSIVE_LEVEL;
        //KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif
    /* Set new IRQL */
    Pcr->CurrentIrql = NewIrql;

    /* Return old IRQL */
    return CurrentIrql;
}

/*
 * @unmplemented
 */
VOID
FASTCALL
KfLowerIrql(IN KIRQL NewIrql)
{

    PKPCR Pcr = KeGetPcr();
#ifdef IRQL_DEBUG
    /* Validate correct lower */
    if (OldIrql > Pcr->Irql)
    {
        /* Crash system */
       // Pcr->Irql = HIGH_LEVEL;
        //KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif

    /* Save the new IRQL and restore interrupt state */
    Pcr->CurrentIrql = NewIrql;
}

/* SOFTWARE INTERRUPTS ********************************************************/

/*
 * @implemented
 */
VOID
FASTCALL
HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/*
 * @implemented
 */
VOID
FASTCALL
HalClearSoftwareInterrupt(IN KIRQL Irql)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/* SYSTEM INTERRUPTS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalEnableSystemInterrupt(IN ULONG Vector,
                         IN KIRQL Irql,
                         IN KINTERRUPT_MODE InterruptMode)
{
    UNIMPLEMENTED;
    while (TRUE);
    return FALSE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalDisableSystemInterrupt(IN ULONG Vector,
                          IN KIRQL Irql)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalBeginSystemInterrupt(IN KIRQL Irql,
                        IN ULONG Vector,
                        OUT PKIRQL OldIrql)
{
    UNIMPLEMENTED;
    while (TRUE);
    return FALSE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalEndSystemInterrupt(IN KIRQL OldIrql,
                      IN PKTRAP_FRAME TrapFrame)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/* EOF */