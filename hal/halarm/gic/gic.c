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
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    /* Return the IRQL */
    return KeGetPcr()->CurrentIrql;
}

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
 * @implemented
 */
KIRQL
FASTCALL
KfRaiseIrql(IN KIRQL NewIrql)
{
    UNIMPLEMENTED;
    while (TRUE);
    return 0;
}

/*
 * @implemented
 */
VOID
FASTCALL
KfLowerIrql(IN KIRQL NewIrql)
{
    UNIMPLEMENTED;
    while (TRUE);
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