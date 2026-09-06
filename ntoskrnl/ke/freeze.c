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

#ifdef CONFIG_SMP
/*
 * How long to wait for the debugger port, the way KeFreezeExecution waits for
 * it: 500000 tries with a 4us stall between them, about two seconds.
 */
#define KI_PORT_LOCK_ATTEMPTS 500000
#define KI_PORT_LOCK_STALL 4

static
BOOLEAN
KiAcquireDebuggerPort(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    ULONG Attempts = KI_PORT_LOCK_ATTEMPTS;
#ifndef _M_AMD64
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
#endif

    while (Attempts-- != 0)
    {
        if (KeTryToAcquireSpinLockAtDpcLevel(&KdpDebuggerLock))
        {
            return TRUE;
        }

#ifndef _M_AMD64
        /*
         * Answer a freeze while we wait.
         *
         * Our caller has already disabled interrupts, so the freeze IPI another
         * processor may have just sent us cannot be delivered on its own and it
         * would time out waiting for a FROZEN we are not in a position to
         * report.  KeFreezeExecution has the same problem in the same place and
         * calls KiIpiProcessRequests here; this is that call, narrowed to the
         * one request that can matter while we hold nothing.
         *
         * x64 needs no equivalent, because it freezes with an NMI.
         */
        if (InterlockedAnd((PLONG)&CurrentPrcb->RequestSummary,
                           ~IPI_FREEZE) & IPI_FREEZE)
        {
            KiProcessorFreezeHandler(TrapFrame, ExceptionFrame);
        }
#endif

        KeStallExecutionProcessor(KI_PORT_LOCK_STALL);
    }

    return FALSE;
}
#endif

/* FUNCTIONS ******************************************************************/

BOOLEAN
NTAPI
KeFreezeExecution(IN PKTRAP_FRAME TrapFrame,
                  IN PKEXCEPTION_FRAME ExceptionFrame)
{
    BOOLEAN Enable;
    KIRQL OldIrql;

#ifndef CONFIG_SMP
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
#endif

    /* Disable interrupts, get previous state and set the freeze flag */
    Enable = KeDisableInterrupts();
    KiFreezeFlag = 4;

    /*
     * Raise to DISPATCH_LEVEL, on one processor and on many alike.
     *
     * The SMP path used to raise to HIGH_LEVEL here. That buys nothing on this
     * processor - KeDisableInterrupts() above has already made it
     * uninterruptible - and it costs the debugger the ability to allocate,
     * because pool allocation is not legal above DISPATCH_LEVEL. KDBG does
     * allocate: loading a driver's symbols out of DbgLoadImageSymbols() does,
     * and on the MP kernel that turned every driver load into a kernel stack
     * overflow and a double fault. The other processors are held by
     * KxFreezeExecution() below, which does not depend on our IRQL.
     */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        OldIrql = KeRaiseIrqlToDpcLevel();

    /*
     * Take the debugger port before anybody is frozen.
     *
     * This used to be a single try in KdEnterDebugger, made after the freeze had
     * already happened, and it is the wrong way round.  KdPollBreakIn() runs
     * from the clock interrupt on every processor and holds this lock across a
     * whole KdReceivePacket(); freeze a processor inside that window and the
     * lock it holds can never be released, so the try always fails, the
     * debugger reports "Port lock was not acquired!" and then writes to the
     * port anyway, on top of the half finished packet of the processor it
     * froze.
     *
     * KeFreezeExecution takes KdDebuggerLock before KiFreezeExecutionLock for
     * exactly this reason: a processor holding the port is still running, so it
     * finishes and lets go.  The retry loop is the reference's, less its habit
     * of restarting the count whenever another processor holds the freeze -
     * unnecessary here, because this kernel's freeze is bounded to about a
     * second and two seconds of retrying already outlasts a whole cycle.
     *
     * A nested entry keeps the port the outer one owns, and must not try for
     * it again - KeFreezeExecution returns before its own attempt for the same
     * reason.  Taking a spinlock we already hold would fail, clear
     * KdpPortLocked, and leave the outer KeThawExecution believing it had
     * nothing to release.
     */
#ifdef CONFIG_SMP
    if (KiFreezeOwner != KeGetCurrentPrcb())
    {
        KdpPortLocked = KiAcquireDebuggerPort(TrapFrame, ExceptionFrame);
    }

    /* Architecture specific freeze code */
    KxFreezeExecution();
#else
    KdpPortLocked = KeTryToAcquireSpinLockAtDpcLevel(&KdpDebuggerLock);
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
    KIRQL OldIrql = KiOldIrql;

    /*
     * Let the port go once the machine is whole again, and only if this was the
     * outermost thaw.  KeThawExecution releases KdDebuggerLock after
     * KiSendThawExecution has finished waiting, for the same reason.
     */
#ifdef CONFIG_SMP
    /* Architecture specific thaw code */
    if (KxThawExecution() && KdpPortLocked)
#else
    if (KdpPortLocked)
#endif
    {
        KdpPortLocked = FALSE;
        KdpPortUnlock();
    }

    /* Clear the freeze flag */
    KiFreezeFlag = 0;

    /* Cleanup CPU caches */
    KxFlushEntireCurrentTb();

    /* Restore the old IRQL */
    if (OldIrql < DISPATCH_LEVEL)
        KeLowerIrql(OldIrql);

    /* Re-enable interrupts */
    KeRestoreInterrupts(Enable);
}
