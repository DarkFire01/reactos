/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Routines for freezing and unfreezing processors for
 *              kernel debugger synchronization.
 * COPYRIGHT:   Copyright 2009 Alex Ionescu  <alex.ionescu@reactos.org>
 *              Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */


/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
//#define NDEBUG
#include <debug.h>

#ifdef NDEBUG
#define KdpDprintf(...)
#endif

#define IPI_FROZEN_RUNNING 0
#define IPI_FROZEN_THAWING 3
#define IPI_FROZEN_HALTED 2

/* GLOBALS ********************************************************************/

/* Freeze data */
KIRQL KiOldIrql;
ULONG KiFreezeFlag;

VOID
NTAPI
KiFreezeTargetExecution(_In_ PKTRAP_FRAME TrapFrame,
                        _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB Prcb;
    ULONG IpiFreezeState;
    /* The active CPUs that haven't caused the debug trigger come into this routine */
    Prcb = KeGetCurrentPrcb();
//    KiSaveProcessorState(TrapFrame, NULL);
    KdpDprintf("CPU %d entering FROZEN state\n", KeGetCurrentProcessorNumber());

    /* Set the IpiFrozen flag to signify that this CPU is in this function and ready for orders*/
    Prcb->IpiFrozen = IPI_FROZEN_HALTED;
    IpiFreezeState = Prcb->IpiFrozen;

    /* While we aren't thawing */
    while (IpiFreezeState != IPI_FROZEN_THAWING)
    {
        /* check to see if we were told to thaw */
        KeStallExecutionProcessor(100);
        IpiFreezeState = Prcb->IpiFrozen;
    }
    KdpDprintf("CPU %d entering RUNNING state\n", KeGetCurrentProcessorNumber());
    /* we are coming back to life! */
  //  KdpDprintf("CPU %d returning to RUNNING state\n", KeGetCurrentProcessorNumber());
    Prcb->IpiFrozen = IPI_FROZEN_RUNNING;
    //InterlockedBitTestAndReset((PLONG)&Prcb->IpiFrozen, IPI_FROZEN_RUNNING);
    //KiRestoreProcessorState(TrapFrame, NULL);
}

/* FUNCTIONS ******************************************************************/

BOOLEAN
NTAPI
KeFreezeExecution(IN PKTRAP_FRAME TrapFrame,
                  IN PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    KAFFINITY TargetAffinity;
    KAFFINITY Current;
    PKPRCB TargetPrcb;
    ULONG i;
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
    //KdpDprintf("Freezing CPUs\n");
    TargetAffinity = KeActiveProcessors;
    TargetAffinity &= ~Prcb->SetMember;
    /* Only IF there's more than one CPU */
    if (TargetAffinity)
    {
        KdpDprintf("CPU %d has frozen all cores \n", KeGetCurrentProcessorNumber());
        //KdpDprintf("freezing all processors called by processor :%d\n", KeGetCurrentProcessorNumber());
        /* unlike other times we send IPIs we do this one at a time */
        for (i = 0, Current = 1; i < KeNumberProcessors; i++, Current <<= 1)
        {
            /* We want this to be EXTREMELY accurate so we fire and check 1 CPU at a time */
            if (TargetAffinity & Current)
            {
                KiIpiSend(Current, IPI_FREEZE);
                TargetPrcb = KiProcessorBlock[i];
                //KdpDprintf("Waiting for frozen confirmation from processor: %d\n", i);
                while (TargetPrcb->IpiFrozen != IPI_FROZEN_HALTED)
                {
                    KeStallExecutionProcessor(100);
                }

               //KdpDprintf("processor: %d has frozen\n", i);
            }
        }
        //KdpDprintf("All processors have been told to freeze by processor :%d\n", KeGetCurrentProcessorNumber());
    }
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
    LONG i;
    PKPRCB TargetPrcb;
    KAFFINITY Current;
    KAFFINITY TargetAffinity;

    PKPRCB Prcb = KeGetCurrentPrcb();
    TargetAffinity = KeActiveProcessors;
    TargetAffinity &= ~Prcb->SetMember;
    /* Only IF there's more than one CPU */
    if (TargetAffinity)
    {
        for (i = 0, Current = 1; i < KeNumberProcessors; i++, Current <<= 1)
        {
            if (TargetAffinity & Current)
            {
                //KdpDprintf("Unfreezing Processor %d\n", i);
                TargetPrcb = KiProcessorBlock[i];
                TargetPrcb->IpiFrozen = IPI_FROZEN_THAWING;
                //KdpDprintf("Waiting to thaw confirmation from processor: %d\n", i);
                while (Prcb->IpiFrozen != IPI_FROZEN_RUNNING)
                {
                    KeStallExecutionProcessor(100);
                }
               //KdpDprintf("processor: %d has continued execution", i);
            }
        }
    }
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
