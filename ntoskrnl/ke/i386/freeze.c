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

/*
 * How deep the freeze owner is nested.
 *
 * KxFreezeExecution() lets the owner re-enter without doing anything, so that
 * a debug action taken while already frozen does not block on itself. Thawing
 * had no matching guard, so the inner thaw released every processor and
 * cleared the ownership while the outer freeze was still relying on it, and
 * the outer thaw then found its own IPI_FROZEN_FLAG_ACTIVE gone and asserted.
 * Because that assertion enters the debugger, which freezes and thaws again,
 * the failure sustained itself and buried the boot in repeats of it.
 *
 * Only the owner reads or writes this, and there is one of those at a time.
 */
static ULONG KiFreezeDepth;

/*
 * Which processors this freeze asked to stop, and which actually answered.
 *
 * Every wait below used to be unbounded. That is only safe if a target can
 * always answer, and it cannot: KeFreezeExecution() disables interrupts before
 * calling here, so a processor that is already uninterruptible somewhere else
 * never takes the freeze IPI and never reports FROZEN. The owner then spun for
 * ever - and because the debugger prints nothing until it owns the machine,
 * that is a completely silent hang rather than the assertion or breakpoint
 * message that was on its way out. Seven targets instead of one makes it seven
 * times as likely.
 *
 * So the waits are bounded now. A target that does not answer in time is left
 * running and recorded here; the debugger gets the machine and says which
 * processors it could not stop, which is far better than saying nothing at all.
 * Anything asked to freeze is still released at thaw, answered or not, so a
 * timeout that fires early cannot strand a healthy processor.
 *
 * Only the freeze owner touches these, and there is one of those at a time.
 */
static KAFFINITY KiFreezeActiveSet;
static KAFFINITY KiFreezeRequested;
static KAFFINITY KiFreezeStalled;

/*
 * How long to wait for one processor, in YieldProcessor() spins. Generous: a
 * false timeout costs a confusing debugger session, where too short a wait on
 * a healthy machine would be a regression.
 */
#define KI_FREEZE_SPIN_LIMIT 500000000
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

    /*
     * Put the control registers back, but leave the trap frame alone.
     *
     * KiSaveProcessorState() above snapshots this processor into
     * Prcb->ProcessorState so the debugger can show it. Feeding that snapshot
     * back through KeContextToTrapFrame() is only useful if the debugger edited
     * it, and it is not a lossless round trip: ProcessorState is per-processor
     * state that other debugger paths also write, so the context restored here
     * need not belong to the frame we are returning through. When it did not,
     * KiEspToTrapFrame() saw a user mode Esp against a kernel mode frame,
     * refused to lower the stack pointer, and brought the machine down with
     * SET_OF_INVALID_CONTEXT. We were interrupted by an IPI and are returning
     * to exactly where we left off, so the frame is already correct.
     */
    KiRestoreProcessorControlState(&CurrentPrcb->ProcessorState);

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
        KiFreezeDepth++;
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
            else if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
            {
                /*
                 * Answer a thaw as well, not just a freeze.
                 *
                 * The owner sets a target to THAW and then waits for it to
                 * report RUNNING. Reacting only to TARGET_FREEZE above left
                 * that transition to whoever was inside the inner wait, and a
                 * processor that had already completed a cycle and gone back
                 * to spinning here was not: it sat with IpiFrozen at THAW while
                 * the owner waited for a RUNNING that nobody was going to
                 * write, and both spun until the machine was reset.
                 */
                KxFlushEntireCurrentTb();

                CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
            }

            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    /* We are the owner now and active */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
    KiFreezeDepth = 1;

    /*
     * Take the processor count once, and thaw exactly this set later.
     *
     * Take the active set too, and drive everything below from it rather than
     * from the count.  The two disagree for a window during AP startup:
     * KiSystemStartup() counts a processor and publishes KiProcessorBlock[Cpu]
     * well before it sets that processor bit in KeActiveProcessors, and in
     * between the PRCB is still zeroed - which reads as
     * IPI_FROZEN_STATE_RUNNING, that state being 0 - while the processor is in
     * no position to answer anything.
     *
     * Iterating the count therefore asked such a processor to freeze while
     * KiIpiSend() below, which targets KeActiveProcessors, did not signal it.
     * The bounded waits keep that from hanging, but it still left the target
     * holding TARGET_FREEZE, so the next freeze IPI it ever took would stop it
     * at a moment nobody asked for.  One snapshot for both decisions keeps the
     * set we ask and the set we signal identical.
     */
    KiFrozenProcessorCount = (ULONG)KeNumberProcessors;
    KiFreezeActiveSet = KeActiveProcessors;

    /* Nothing requested or outstanding yet */
    KiFreezeRequested = 0;
    KiFreezeStalled = 0;

    /* Loop all processors */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if ((TargetPrcb != CurrentPrcb) &&
            (KiFreezeActiveSet & AFFINITY_MASK(i)))
        {
            ULONG Spin = KI_FREEZE_SPIN_LIMIT;

            /*
             * Only the active processor is allowed to change IpiFrozen, and a
             * target should be running. It may still be on its way out of a
             * previous cycle, so wait a bounded while for it rather than
             * asserting - an assertion here would call DbgPrint() and re-enter
             * this function with the targets half set up.
             */
            while ((TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING) &&
                   (--Spin != 0))
            {
                YieldProcessor();
                KeMemoryBarrier();
            }

            if (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                /* Leave it alone: it is not ours to drive in this state */
                KiFreezeStalled |= TargetPrcb->SetMember;
                continue;
            }

            /* Request target to freeze */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
            KiFreezeRequested |= TargetPrcb->SetMember;
        }
    }

    /* Send the freeze IPI */
    KiIpiSend(KiFreezeActiveSet & ~CurrentPrcb->SetMember, IPI_FREEZE);

    /* Wait for the targets we asked to be frozen */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if ((TargetPrcb != CurrentPrcb) &&
            (KiFreezeRequested & TargetPrcb->SetMember))
        {
            ULONG Spin = KI_FREEZE_SPIN_LIMIT;

            /* Wait for the target to be frozen, but not for ever */
            while ((TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN) &&
                   (--Spin != 0))
            {
                YieldProcessor();
                KeMemoryBarrier();
            }

            if (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
                KiFreezeStalled |= TargetPrcb->SetMember;
        }
    }

    /* Say so rather than pretending the machine is all ours */
    if (KiFreezeStalled != 0)
    {
        DPRINT1("KxFreezeExecution: processors %p did not freeze\n",
                (PVOID)KiFreezeStalled);
    }

    /* The targets that could answer are frozen, we can continue */
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

    /* Only the outermost thaw releases anybody */
    ASSERT(KiFreezeDepth != 0);
    if (--KiFreezeDepth != 0)
    {
        return;
    }

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    /*
     * Release everything this freeze asked to stop - including a processor we
     * gave up waiting on, which may have frozen a moment after the timeout.
     * Leaving one of those behind would strand it for good. A processor we
     * never asked is not ours to touch.
     */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if ((TargetPrcb != CurrentPrcb) &&
            (KiFreezeRequested & TargetPrcb->SetMember))
        {
            /* Request target to thaw */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
        }
    }

    /* Wait for those targets to be running again, but not for ever: this is
       the same trap as the freeze side, and hanging here would lose a machine
       that had already finished with the debugger. */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if ((TargetPrcb != CurrentPrcb) &&
            (KiFreezeRequested & TargetPrcb->SetMember))
        {
            ULONG Spin = KI_FREEZE_SPIN_LIMIT;

            while ((TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING) &&
                   (--Spin != 0))
            {
                YieldProcessor();
                KeMemoryBarrier();
            }

            if (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                DPRINT1("KxThawExecution: processor %lu did not thaw\n", i);
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
