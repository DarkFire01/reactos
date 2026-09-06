/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor freeze support for x64
 * COPYRIGHT:   Copyright 2023-2024 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/*

 IpiFrozen state graph (based on Windows behavior):

                          Freeze signal, taken
    +-----------------+ -----------------------> +-----------------+
    | RUNNING         |                          | FROZEN          |
    +-----------------+ <----------------------- +-----------------+
            |    ^          Thaw, then resume         |    ^
     Freeze |    | Thaw                               |    |
            v    |                                    |    | Kd proc switch
    +-----------------+                               v    |
    | OWNER + ACTIVE  |                          +-----------------+
    +-----------------+                          | FROZEN + ACTIVE |
            |    ^                               +-----------------+
   Kd proc  |    |
   switch   v    |          Thaw request
    +-----------------+ -----------------------> +-----------------+
    | OWNER           |                          | THAW            |
    +-----------------+                          +-----------------+

 A target moves itself from RUNNING to FROZEN when it takes the freeze signal;
 the owner never writes that transition on a target's behalf.  That is why
 IPI_FROZEN_STATE_TARGET_FREEZE no longer appears here - see the comment in
 KiProcessorFreezeHandler.

 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

PKPRCB KiFreezeOwner;

/*
 * This protocol is written against KeFreezeExecution, KiFreezeTargetExecution
 * and KiSendThawExecution in the Windows kernel.
 *
 * The freeze signal here is an NMI rather than the maskable IPI the reference
 * uses, so that a processor which has interrupts disabled can still be taken;
 * that is the one deliberate difference and everything below is written the
 * way KeFreezeExecution, KiFreezeTargetExecution and KiSendThawExecution are.
 */
/*
 * State for one freeze, written only by its owner - and there is one of those
 * at a time.
 *
 * KiFrozenProcessorCount: KeNumberProcessors is not stable across a freeze.
 * An application processor increments it from KiSystemStartup(), and on this
 * kernel every DbgPrint freezes and thaws, so the two really do overlap.
 * Reading the count again at thaw time makes the owner thaw a processor it
 * never froze.  Take it once and thaw exactly the set that was frozen.
 *
 * KiFreezeDepth: the owner may re-enter the freeze without doing anything -
 * KeFreezeExecution returns early when it already owns the machine - but
 * thawing has no matching guard of its own, so without this an inner thaw
 * released every processor while the outer freeze was still relying on it.
 *
 * KiFreezeRequested: the set that was signalled, snapshotted so that thaw
 * covers exactly it.
 *
 * KiFreezeStalled: who did not answer in time.  The reference only records
 * that something did not, in KiFreezeFlag; this keeps the set as well, for
 * anyone who breaks in and looks.
 */
static ULONG KiFrozenProcessorCount;
static ULONG KiFreezeDepth;
static KAFFINITY KiFreezeRequested;
static KAFFINITY KiFreezeStalled;

/*
 * How long to wait, the way KeFreezeExecution waits: a budget of 20000 stalls
 * of 50us, about a second, and - this is the part that is easy to miss - spent
 * across the whole enumeration rather than reset for each processor.  When it
 * runs out the remaining targets are given up on immediately.
 *
 * That is deliberate.  A machine with one wedged processor still reaches the
 * debugger in a second, where a per-processor timeout would cost a second for
 * every one of them; on sixteen processors the difference is the difference
 * between a usable debugger and one that appears to have hung as well.
 */
#define KI_FREEZE_STALL_COUNT 20000
#define KI_FREEZE_STALL_TIME  50

/* FUNCTIONS *****************************************************************/

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /*
     * Decide from the state of the freeze as a whole, not from a request
     * written into this PRCB.
     *
     * KiFreezeTargetExecution does the same: it takes the freeze if
     * KiFreezeExecutionLock, KiFreezeLockBackup or a bugcheck is in progress,
     * and never looks for anything addressed to itself.  Note that the
     * reference's own KiFreezeOwner is not that flag - it is written once, in
     * KeFreezeExecution, and never cleared.  The flag is the lock, taken before
     * any target is signalled and released by KeThawExecution only after
     * KiSendThawExecution has finished waiting.  This kernel has no separate
     * lock: KiFreezeOwner is acquired by compare-exchange and released by
     * exchange over exactly that span, so it carries both jobs.
     *
     * This used to work the other way round: the owner wrote
     * IPI_FROZEN_STATE_TARGET_FREEZE into each target and the handler acted
     * only on that.  Two processors then had to agree on a value only one of
     * them could see change, and every way of losing that agreement stranded
     * one of them.  A signal that arrived after the owner had given up found
     * THAW or RUNNING instead of the state it was written for and declined the
     * freeze, so the machine took an unexplained NMI; a request that was
     * written and never taken left TARGET_FREEZE behind for some later,
     * unrelated signal to act on.  Both were seen, as a processor reported
     * stuck at IpiFrozen 5 and as freezes that rotated between victims.
     *
     * Deciding here removes the agreement.  The freeze is on or it is not, the
     * answer is the same for every processor, and nothing is left behind
     * afterwards - which is also why the owner no longer has to inspect a
     * target's state before asking it, nor to ask twice.
     */
    if ((KiFreezeOwner == NULL) || (KiFreezeOwner == CurrentPrcb))
    {
        /* Not a freeze request, return FALSE to signal it is unhandled */
        return FALSE;
    }

    /* We are frozen now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

    /* Save the processor state, so the debugger can show this processor */
    KiSaveProcessorState(TrapFrame, ExceptionFrame);

    /*
     * Wait for the freeze owner to release us.
     *
     * Anything that is no longer FROZEN releases us, which is how
     * KiFreezeTargetExecution waits - while ((IpiFrozen & 0xF) == 2) - and it
     * is load bearing rather than cosmetic.  KxThawExecution writes THAW over
     * a target it finds frozen and RUNNING over one it does not, and a target
     * that took the signal just after the owner's wait had passed over it gets
     * the second of those.  Waiting for an exact THAW would leave it here for
     * good; in the reference that write is the release.
     *
     * The state is masked because the debugger switching to this processor
     * sets IPI_FROZEN_FLAG_ACTIVE on top of FROZEN, and that is handled below
     * rather than being an exit - the reference tests Prcb == KiDebuggerOwner
     * in the same place, for the same reason.
     */
    while ((CurrentPrcb->IpiFrozen & IPI_FROZEN_STATE_MASK) ==
           IPI_FROZEN_STATE_FROZEN)
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
    ULONG StallsLeft;
    ULONG i;

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
     * Ask the active set, as KeFreezeExecution does - it signals
     * KeActiveProcessors without this processor and nothing else.  Driving it
     * from the count instead would reach a processor that is counted but not
     * yet active: KiSystemStartup() does Cpu = KeNumberProcessors++ and
     * publishes KiProcessorBlock[Cpu] near its top, but only sets its bit in
     * KeActiveProcessors at the very bottom, after HalInitializeProcessor().
     * In between it is counted and reachable, and cannot answer anything,
     * because its local APIC is not set up yet.
     */
    KiFrozenProcessorCount = (ULONG)KeNumberProcessors;
    KiFreezeRequested = KeActiveProcessors & ~CurrentPrcb->SetMember;
    KiFreezeStalled = 0;

    /* Ask them, and let each one freeze itself */
    KiIpiSend(KiFreezeRequested, IPI_FREEZE);

    /* Wait for them to report it, spending one budget across them all */
    StallsLeft = KI_FREEZE_STALL_COUNT;
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];

        /* A processor can be counted before its block exists: KiSystemStartup
           does Cpu = KeNumberProcessors++ and publishes KiProcessorBlock[Cpu]
           afterwards, so a freeze that lands in that window - a DbgPrint from
           KeStartAllProcessors is enough - finds a hole here */
        if (TargetPrcb == NULL)
        {
            continue;
        }

        if ((KiFreezeRequested & AFFINITY_MASK(i)) == 0)
        {
            continue;
        }

        while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
        {
            if (StallsLeft == 0)
            {
                /* Out of time - give up on this one and on the rest */
                KiFreezeStalled |= AFFINITY_MASK(i);
                break;
            }

            KeStallExecutionProcessor(KI_FREEZE_STALL_TIME);
            StallsLeft--;
        }
    }

    /*
     * Report it the way the reference does.
     *
     * KeFreezeExecution sets KiFreezeFlag |= 2 when a target does not reach
     * FROZEN inside its timeout and says nothing itself; KdEnterDebugger
     * prints "Some processors not frozen in debugger!" once it has the
     * machine.  That check is already here in kdapi.c and nothing has ever set
     * the bit for it.
     *
     * Doing it that way is not just fidelity, it is the only safe way: this
     * function runs under KdEnterDebugger, so a DbgPrint from here re-enters
     * the debugger, and from KxThawExecution - which has already dropped the
     * freeze depth to zero by the time it would print - that re-entry ran the
     * whole thaw body again and recursed until the stack was gone.
     */
    if (KiFreezeStalled != 0)
    {
        KiFreezeFlag |= 2;
    }

    /* The targets that could answer are frozen, we can continue */
}

_Must_inspect_result_
BOOLEAN
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KAFFINITY Thawing = 0;
    KAFFINITY Stalled = 0;
    ULONG StallsLeft;
    ULONG i;

    /* Only the outermost thaw releases anybody */
    ASSERT(KiFreezeDepth != 0);
    if (--KiFreezeDepth != 0)
    {
        /* An inner thaw releases nothing, and must not be told that it did */
        return FALSE;
    }

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /*
     * Release whoever actually froze, and put everybody else back to RUNNING.
     *
     * This is KiSendThawExecution: it walks every processor, moves the ones
     * that read FROZEN to THAW and collects them into a set to wait on, and
     * writes RUNNING over anything else.  That second half is the part worth
     * keeping - it is what stops a processor carrying a stale IpiFrozen out of
     * one cycle and into the next, and this loop is the only place in the
     * protocol that can clean up after a target which was asked and never
     * answered.
     *
     * The state is masked because a processor the debugger switched to reads
     * FROZEN with IPI_FROZEN_FLAG_ACTIVE set, and it still has to be thawed.
     */
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];

        /* A processor can be counted before its block exists: KiSystemStartup
           does Cpu = KeNumberProcessors++ and publishes KiProcessorBlock[Cpu]
           afterwards, so a freeze that lands in that window - a DbgPrint from
           KeStartAllProcessors is enough - finds a hole here */
        if (TargetPrcb == NULL)
        {
            continue;
        }

        if (TargetPrcb == CurrentPrcb)
        {
            continue;
        }

        if ((TargetPrcb->IpiFrozen & IPI_FROZEN_STATE_MASK) ==
            IPI_FROZEN_STATE_FROZEN)
        {
            /* Request target to thaw */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
            Thawing |= AFFINITY_MASK(i);
        }
        else
        {
            /* Not frozen - make sure it is not left holding anything */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
        }
    }

    /*
     * Wait for those to pick it up.  The reference spins here without a limit;
     * bound it on the same budget as the freeze side, because hanging here
     * would lose a machine that had already finished with the debugger.
     */
    StallsLeft = KI_FREEZE_STALL_COUNT;
    for (i = 0; i < KiFrozenProcessorCount; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];

        /* A processor can be counted before its block exists: KiSystemStartup
           does Cpu = KeNumberProcessors++ and publishes KiProcessorBlock[Cpu]
           afterwards, so a freeze that lands in that window - a DbgPrint from
           KeStartAllProcessors is enough - finds a hole here */
        if (TargetPrcb == NULL)
        {
            continue;
        }

        if ((Thawing & AFFINITY_MASK(i)) == 0)
        {
            continue;
        }

        while (TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
        {
            if (StallsLeft == 0)
            {
                Stalled |= AFFINITY_MASK(i);
                break;
            }

            KeStallExecutionProcessor(KI_FREEZE_STALL_TIME);
            StallsLeft--;
        }
    }

    /* Release the freeze owner */
    InterlockedExchangePointer(&KiFreezeOwner, NULL);

    /*
     * Keep the set for anyone who breaks in and looks, but do not raise
     * KiFreezeFlag for it: KeThawExecution clears that flag immediately after
     * this returns, so nothing would ever read the bit.  The reference reports
     * nothing from the thaw either - KiSendThawExecution simply waits.
     */
    KiFreezeStalled |= Stalled;

    return TRUE;
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
