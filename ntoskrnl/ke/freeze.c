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

#ifdef CONFIG_SMP
    /* Architecture specific freeze code */
    KxFreezeExecution();
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

#ifdef CONFIG_SMP
    /* Architecture specific thaw code */
    KxThawExecution();
#endif

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
