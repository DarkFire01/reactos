/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/freeze.c
 * PURPOSE:         Routines for freezing and unfreezing processors for
 *                  kernel debugger synchronization.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Freeze data */
KIRQL KiOldIrql;
ULONG KiFreezeFlag;

/* FUNCTIONS ******************************************************************/
//TODO: Replace with finish func
VOID
NTAPI
KiIdleMultiprocessor(IN PKIPI_CONTEXT PacketContext,
                     IN PVOID Ignored1,
                     IN PVOID Ignored2,
                     IN PVOID Ignored3)
{
    while (KiFreezeFlag != 4){
        __asm {
            mov eax, 0xDEADBEEF
        };
    }

}


BOOLEAN
NTAPI
KeFreezeExecution(IN PKTRAP_FRAME TrapFrame,
                  IN PKEXCEPTION_FRAME ExceptionFrame)
{
    #ifdef CONFIG_SMP
    KAFFINITY TargetAffinity;
    PKPRCB Prcb = KeGetCurrentPrcb();
    #endif
    BOOLEAN Enable;
    KIRQL OldIrql;

#ifndef CONFIG_SMP
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
#endif

    /* Disable interrupts, get previous state and set the freeze flag */
    Enable = KeDisableInterrupts();
    KiFreezeFlag = 4;

#ifndef CONFIG_SMP
    /* Raise IRQL if we have to */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        OldIrql = KeRaiseIrqlToDpcLevel();
#else
    /* Raise IRQL to HIGH_LEVEL */
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
#endif

#ifdef CONFIG_SMP
    UNREFERENCED_PARAMETER(TargetAffinity);
    UNREFERENCED_PARAMETER(Prcb);
#if 1
    if (KeNumberProcessors > 1)
    {

    /* Raise the IRQL for the TB Flush */
        OldIrql = KeRaiseIrqlToSynchLevel();


        /* Get the current processor affinity, and exclude ourselves */
         TargetAffinity = KeActiveProcessors;
         TargetAffinity &= ~Prcb->SetMember;

    /* Make sure this is MP */
         if (TargetAffinity)
         {
            /* Send an IPI TB flush to the other processors */
               KiIpiSendPacket(TargetAffinity,
                        KiIdleMultiprocessor,
                        NULL,
                        0,
                        NULL);
         }
        }
    #endif
#endif

    /* Save the old IRQL to be restored on unfreeze */
    KiOldIrql = OldIrql;

    /* Return whether interrupts were enabled */
    return Enable;
}

VOID
NTAPI
KeThawExecution(IN BOOLEAN Enable)
{
#ifdef CONFIG_SMP
    // TODO: Add SMP support.
#endif

    /* Clear the freeze flag */
    KiFreezeFlag = 0;

    /* Cleanup CPU caches */
    KeFlushCurrentTb();

    /* Restore the old IRQL */
#ifndef CONFIG_SMP
    if (KiOldIrql < DISPATCH_LEVEL)
#endif
    KeLowerIrql(KiOldIrql);

    /* Re-enable interrupts */
    KeRestoreInterrupts(Enable);
}
