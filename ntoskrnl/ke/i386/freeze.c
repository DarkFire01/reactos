/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor freeze support for i386
 * COPYRIGHT:
 */

/*
 * This is the x64 state machine in ntoskrnl/ke/amd64/freeze.c, on i386. The
 * states and the transitions between them are identical and documented there;
 * only the delivery differs. x64 freezes with an NMI, which arrives even at
 * IPI_LEVEL, while here the freeze is an ordinary IPI request and reaches us
 * through KiIpiServiceRoutine. That means a processor spinning at or above
 * IPI_LEVEL with interrupts disabled cannot be frozen until it lets one in,
 * so a hang inside such a region will time out rather than break in.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PKPRCB KiFreezeOwner;

/*
 * How many processors KxFreezeExecution() actually froze.
 *
 * KeNumberProcessors is not stable across a freeze. An application processor
 * increments it from KiSystemStartup() as it comes up, and on this kernel every
 * DbgPrint freezes and thaws - KdpPrint() enters the debugger - so the two do
 * overlap in practice. Reading the count again at thaw time made the boot
 * processor try to thaw a processor it had never frozen: the "Successful AP
 * startup count" print froze while the count was still one and thawed once the
 * application processor had made it two, and the target was still RUNNING.
 *
 * Only the freeze owner touches this, and there is one of those at a time.
 */
#ifdef CONFIG_SMP
static ULONG KiFrozenProcessorCount;
#endif

/* FUNCTIONS ******************************************************************/

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Make sure this is a freeze request */
    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        /* Not a freeze request, return FALSE to signal it is unhandled */
        return FALSE;
    }

    /* We are frozen now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

    /* Save the processor state, so the debugger can show this processor */
    KiSaveProcessorState(TrapFrame, ExceptionFrame);

    /* Wait for the freeze owner to release us */
    while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
    {
        /* Check for Kd processor switch */
        if (CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE)
        {
            KCONTINUE_STATUS ContinueStatus;

            /* Enter the debugger */
            ContinueStatus = KdReportProcessorChange();

            /* Set the state back to frozen */
            CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

            /* If the status is ContinueSuccess, we need to release the freeze owner */
            if (ContinueStatus == ContinueSuccess)
            {
                /* Release the freeze owner */
                KiFreezeOwner->IpiFrozen = IPI_FROZEN_STATE_THAW;
            }
        }

        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Restore the processor state */
    KiRestoreProcessorState(TrapFrame, ExceptionFrame);

    /* Flush the TLB on this processor */
    KxFlushEntireCurrentTb();

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Return TRUE to signal that we handled the freeze */
    return TRUE;
#else
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
    return FALSE;
#endif
}

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
#ifdef CONFIG_SMP
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    ULONG i;

    /* Avoid blocking on recursive debug action */
    if (KiFreezeOwner == CurrentPrcb)
    {
        return;
    }

    /* Try to acquire the freeze owner */
    while (InterlockedCompareExchangePointer((PVOID*)&KiFreezeOwner,
                                             CurrentPrcb,
                                             NULL) != NULL)
    {
        /* Someone else was faster. Spin here until the freeze owner is
           available again. */
        while (KiFreezeOwner != NULL)
        {
            /*
             * Answer a freeze aimed at us right here, rather than waiting for
             * the IPI to tell us about it.
             *
             * KeFreezeExecution() disabled interrupts before calling us, so the
             * freeze IPI the owner has just sent cannot be delivered and
             * KiIpiServiceRoutine() will never run for it. The owner would wait
             * for a FROZEN we are unable to report while we wait for it to
             * finish, and both processors would spin until the machine was
             * reset - which is what two processors entering the debugger at
             * once produced. x64 does not have to do this because it freezes
             * with an NMI, which is not maskable.
             *
             * Nothing saves our processor state here, so the debugger cannot
             * show this processor's context. It can still inspect the owner and
             * continue the system, which is the part that matters.
             */
            if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_TARGET_FREEZE)
            {
                CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

                while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
                {
                    YieldProcessor();
                    KeMemoryBarrier();
                }

                /* Flush the TLB on this processor, as the freeze handler does */
                KxFlushEntireCurrentTb();

                CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
            }

            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    /* We are the owner now and active */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;

    /* Take the processor count once, and thaw exactly this set later */
    KiFrozenProcessorCount = (ULONG)KeNumberProcessors;

    /* Loop all processors */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Only the active processor is allowed to change IpiFrozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING);

            /* Request target to freeze */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
        }
    }

    /* Send the freeze IPI */
    KiIpiSend(KeActiveProcessors & ~CurrentPrcb->SetMember, IPI_FREEZE);

    /* Wait for all targets to be frozen */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Wait for the target to be frozen */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    /* All targets are frozen, we can continue */
#endif
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
#ifdef CONFIG_SMP
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    ULONG i;

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    /* Loop the processors we froze, not however many there are now */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Make sure they are still frozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);

            /* Request target to thaw */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
        }
    }

    /* Wait for all targets to be running */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Wait for the target to be running again */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Release the freeze owner */
    InterlockedExchangePointer((PVOID*)&KiFreezeOwner, NULL);
#endif
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
#ifdef CONFIG_SMP
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    /* Make sure that the processor index is valid */
    ASSERT(ProcessorIndex < (ULONG)KeNumberProcessors);

    /* We are no longer active */
    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);
    CurrentPrcb->IpiFrozen &= ~IPI_FROZEN_FLAG_ACTIVE;

    /* Inform the target processor that it's his turn now */
    TargetPrcb = KiProcessorBlock[ProcessorIndex];
    TargetPrcb->IpiFrozen |= IPI_FROZEN_FLAG_ACTIVE;

    /* If we are not the freeze owner, we return back to the freeze loop */
    if (KiFreezeOwner != CurrentPrcb)
    {
        return ContinueNextProcessor;
    }

    /* Loop until it's our turn again */
    while (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_OWNER)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Check if we have been thawed */
    if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
    {
        /* Another CPU has completed, we can leave the debugger now */
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
        return ContinueSuccess;
    }

    /* We have been reselected, return to Kd to continue in the debugger */
    ASSERT(CurrentPrcb->IpiFrozen == (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));

    return ContinueProcessorReselected;
#else
    UNREFERENCED_PARAMETER(ProcessorIndex);
    return ContinueError;
#endif
}
