/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor freeze support for x64
 * COPYRIGHT:   Copyright 2023-2024 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/*

 IpiFrozen state graph (based on Windows behavior):

    +-----------------+     Freeze request      +-----------------+
    | RUNNING         |------------------------>| TARGET_FREEZE   |
    +-----------------+<---------               +-----------------+
            |^                  | Resume                |
     Freeze || Thaw        +-----------+ Thaw request   | Freeze IPI
            v|             | THAW      |<-----------\   v
    +-----------------+    +-----------+        +-----------------+
    | OWNER + ACTIVE  |         ^               | FROZEN          |
    +-----------------+         |               +-----------------+
            ^                   |                       ^
            | Kd proc switch    |                       | Kd proc switch
            v                   |                       v
    +-----------------+         |               +-----------------+
    | OWNER           |---------+               | FROZEN + ACTIVE |
    +-----------------+ Thaw request            +-----------------+

 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* NOT INCLUDES ANYMORE ******************************************************/

PKPRCB KiFreezeOwner;

/*
 * The i386 side of this protocol grew four protections that were never
 * brought across, and every one of them applies here just as well - the state
 * machine is the same, only the delivery differs (NMI rather than a maskable
 * IPI).  See ke/i386/freeze.c for the failures each was written for.
 *
 * KiFrozenProcessorCount: KeNumberProcessors is not stable across a freeze.
 * An application processor increments it from KiSystemStartup(), and on this
 * kernel every DbgPrint freezes and thaws, so the two really do overlap.
 * Reading the count again at thaw time makes the owner thaw a processor it
 * never froze.  Take it once and thaw exactly the set that was frozen.
 *
 * KiFreezeDepth: the owner may re-enter the freeze without doing anything,
 * but thawing had no matching guard, so an inner thaw released every
 * processor while the outer freeze was still relying on it.
 *
 * KiFreezeRequested / KiFreezeStalled: every wait here was unbounded, which
 * is only safe if a target can always answer.  A processor that has not
 * finished coming up, or one wedged before its NMI handler is usable, never
 * reports FROZEN and the owner spun for ever - silently, because the debugger
 * prints nothing until it owns the machine.  Bound the waits, record who did
 * not answer, and say so.  Everything asked to freeze is still released at
 * thaw, answered or not, so a timeout that fires early cannot strand a
 * healthy processor.
 *
 * Only the freeze owner touches these, and there is one of those at a time.
 */
static ULONG KiFrozenProcessorCount;
static ULONG KiFreezeDepth;
static KAFFINITY KiFreezeActiveSet;
static KAFFINITY KiFreezeRequested;
static KAFFINITY KiFreezeStalled;

/* How long to wait for one processor, in YieldProcessor() spins.  Generous:
   a false timeout costs a confusing debugger session, where too short a wait
   on a healthy machine would be a regression. */
#define KI_FREEZE_SPIN_LIMIT 500000000

/* FUNCTIONS *****************************************************************/

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Make sure this is a freeze request */
    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        /* Not a freeze request, return FALSE to signal it is unhandled */
        return FALSE;
    }

    /* We are frozen now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

    /* Save the processor state */
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
}

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Avoid blocking on recursive debug action */
    if (KiFreezeOwner == CurrentPrcb)
    {
        KiFreezeDepth++;
        return;
    }

    /* Try to acquire the freeze owner */
    while (InterlockedCompareExchangePointer(&KiFreezeOwner, CurrentPrcb, NULL))
    {
        /* Someone else was faster. We expect an NMI to freeze any time.
           Spin here until the freeze owner is available. */
        while (KiFreezeOwner != NULL)
        {
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
     * KiSystemStartup() does Cpu = KeNumberProcessors++ and publishes
     * KiProcessorBlock[Cpu] near its top, but only sets its bit in
     * KeActiveProcessors at the very bottom, after HalInitializeProcessor().
     * In between, the processor is counted and reachable through the block
     * array, its IpiFrozen reads as IPI_FROZEN_STATE_RUNNING because that
     * state is 0 and the PRCB is still zeroed - and it cannot answer anything,
     * because its local APIC is not set up yet.
     *
     * Iterating the count therefore asked such a processor to freeze while
     * KiIpiSend() below, which targets KeActiveProcessors, did not send it the
     * NMI to do it with.  It never reported FROZEN, and it was left holding
     * TARGET_FREEZE, so the next NMI it ever took would freeze it at a moment
     * nobody asked for.  One snapshot for both decisions keeps the set we ask
     * and the set we signal identical.
     */
    KiFrozenProcessorCount = (ULONG)KeNumberProcessors;
    KiFreezeActiveSet = KeActiveProcessors;

    /* Nothing requested or outstanding yet */
    KiFreezeRequested = 0;
    KiFreezeStalled = 0;

    /* Loop all processors */
    for (ULONG i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if ((TargetPrcb != CurrentPrcb) &&
            (KiFreezeActiveSet & AFFINITY_MASK(i)))
        {
            ULONG Spin = KI_FREEZE_SPIN_LIMIT;

            /*
             * Only the active processor is allowed to change IpiFrozen, and a
             * target should be running.  It may still be on its way out of a
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
    for (ULONG i = 0; i < KiFrozenProcessorCount; i++)
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

    /* Say so rather than pretending the machine is all ours.  A processor left
       running here is one that can still reach the debugger port on its own,
       which is what a failed KdpDebuggerLock acquire reports. */
    if (KiFreezeStalled != 0)
    {
        DPRINT1("KxFreezeExecution: processors %p did not freeze\n",
                (PVOID)KiFreezeStalled);
    }

    /* The targets that could answer are frozen, we can continue */
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

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
     * Leaving one of those behind would strand it for good.  A processor we
     * never asked is not ours to touch.
     */
    for (ULONG i = 0; i < KiFrozenProcessorCount; i++)
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
    for (ULONG i = 0; i < KiFrozenProcessorCount; i++)
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
    InterlockedExchangePointer(&KiFreezeOwner, NULL);
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    /* Make sure that the processor index is valid */
    ASSERT(ProcessorIndex < KeNumberProcessors);

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
        KdpDprintf("[%u] KxSwitchKdProcessor: ContinueSuccess\n", KeGetCurrentProcessorNumber());
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
        return ContinueSuccess;
    }

    /* We have been reselected, return to Kd to continue in the debugger */
    ASSERT(CurrentPrcb->IpiFrozen == (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));

    return ContinueProcessorReselected;
}
