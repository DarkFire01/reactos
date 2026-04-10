/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm64/thrdini.c
 * PURPOSE:         ARM64 thread startup/idle loop helpers
 */

#include <ntoskrnl.h>
#include <internal/ke.h>
#define NDEBUG
#include <debug.h>

DECLSPEC_NORETURN
VOID
KiIdleLoop(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKTHREAD OldThread, NewThread;

    for (;;)
    {
        _enable();
        YieldProcessor();
        YieldProcessor();
        _disable();

        if ((Prcb->DpcData[0].DpcQueueDepth) ||
            (Prcb->TimerRequest) ||
            (Prcb->DeferredReadyListHead.Next))
        {
            HalClearSoftwareInterrupt(DISPATCH_LEVEL);
            KiRetireDpcList(Prcb);
        }

        if (Prcb->NextThread)
        {
            _enable();

            OldThread = Prcb->CurrentThread;
            NewThread = Prcb->NextThread;

            Prcb->NextThread = NULL;
            Prcb->CurrentThread = NewThread;

#ifdef CONFIG_SMP
            KfRaiseIrql(SYNCH_LEVEL);
#endif

            KiSwapContext(APC_LEVEL, OldThread);

#ifdef CONFIG_SMP
            KeLowerIrql(DISPATCH_LEVEL);
#endif
        }
        else
        {
            if (Prcb->PowerState.IdleFunction)
            {
                Prcb->PowerState.IdleFunction(&Prcb->PowerState);
            }
            else
            {
                /* WFI: halt until the next interrupt so QEMU can yield the host CPU. */
                _enable();
                __wfi();
                _disable();
            }
        }
    }
}
