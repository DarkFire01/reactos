#include "gicp.h"

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PUCHAR KdComPortInUse;

/* FUNCTIONS ******************************************************************/

VOID
HalpInitializeInterrupts(VOID)
{
   UNIMPLEMENTED;
   while (TRUE);
}


ULONG
HalGetInterruptSource(VOID)
{
   UNIMPLEMENTED;
   while (TRUE);
   return 0;
}

CODE_SEG("INIT")
VOID
NTAPI
HalReportResourceUsage(VOID)
{
    UNICODE_STRING HalString;

    /* Build HAL usage */
    RtlInitUnicodeString(&HalString, L"ARM UEFI GIC HAL");
}

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
        Pcr->CurrentIrql = PASSIVE_LEVEL;
        //KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif
    /* Set new IRQL */
    Pcr->CurrentIrql = NewIrql;

    /* Return old IRQL */
    return CurrentIrql;
}

VOID
FASTCALL
KfLowerIrql(IN KIRQL NewIrql)
{

    PKPCR Pcr = KeGetPcr();
#ifdef IRQL_DEBUG
    /* Validate correct lower */
    if (OldIrql > Pcr->CurrentIrql)
    {
        /* Crash system */
        Pcr->CurrentIrql = HIGH_LEVEL;
       // KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif

    /* Save the new IRQL and restore interrupt state */
    Pcr->CurrentIrql = NewIrql;
}

#undef KeGetCurrentIrql

KIRQL
NTAPI
KeGetCurrentIrql()
{
     PKPCR Pcr = KeGetPcr();
    /* Return the IRQL */
    return Pcr->CurrentIrql;
}


/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;

    /* Save and update IRQL */
    CurrentIrql = Pcr->CurrentIrql;
    Pcr->CurrentIrql = DISPATCH_LEVEL;

#ifdef IRQL_DEBUG
    /* Validate correct raise */
  //  if (CurrentIrql > DISPATCH_LEVEL) KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
#endif

    /* Return the previous value */
    return CurrentIrql;
}

/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;

    /* Save and update IRQL */
    CurrentIrql = Pcr->CurrentIrql;
    Pcr->CurrentIrql = SYNCH_LEVEL;

#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > SYNCH_LEVEL)
    {
        /* Crash system */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     CurrentIrql,
                     SYNCH_LEVEL,
                     0,
                     1);
    }
#endif

    /* Return the previous value */
    return CurrentIrql;
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

VOID
NTAPI
HalpEnableInterruptHandler(IN UCHAR Flags,
                           IN ULONG BusVector,
                           IN ULONG SystemVector,
                           IN KIRQL Irql,
                           IN PVOID Handler,
                           IN KINTERRUPT_MODE Mode)
{
    /* Register the routine */
    UNIMPLEMENTED;
    while (TRUE);
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
