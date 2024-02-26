/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

//
// Flags for KPRCB::IpiFrozen
//
// Values shown with !ipi extension in WinDbg:
// 0 = [Running], 1 = [Unknown], 2 = [Frozen], 3 = [Thaw], 4 = [Freeze Owner]
// 5 = [Target Freeze], 6-15 = [Unknown]
// 0x20 = [Active] (flag)
//
#define IPI_FROZEN_STATE_RUNNING 0
#define IPI_FROZEN_STATE_FROZEN 2
#define IPI_FROZEN_STATE_THAW 3
#define IPI_FROZEN_STATE_OWNER 4
#define IPI_FROZEN_STATE_TARGET_FREEZE 5
#define IPI_FROZEN_FLAG_ACTIVE 0x20

PKPRCB KiFreezeOwner;

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
   // KiSaveProcessorState(TrapFrame, ExceptionFrame);

    /* Wait for the freeze owner to release us */
    while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Restore the processor state */
    //KiRestoreProcessorState(TrapFrame, ExceptionFrame);

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

    /* We are the owner now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER;

    /* Loop all processors */
    for (ULONG i = 0; i < KeNumberProcessors; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Nobody else is allowed to change IpiFrozen, except the freeze owner */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING);

            /* Request target to freeze  */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
        }
    }

    /* Send the freeze IPI */
    KiIpiSend(KeActiveProcessors & ~CurrentPrcb->SetMember, IPI_FREEZE);

    /* Wait for all targets to be frozen */
    for (ULONG i = 0; i < KeNumberProcessors; i++)
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
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Loop all processors */
    for (ULONG i = 0; i < KeNumberProcessors; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Make sure they are still frozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);

            /* Request target to thaw  */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
        }
    }

    /* Wait for all targets to be running */
    for (ULONG i = 0; i < KeNumberProcessors; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb != CurrentPrcb)
        {
            /* Wait for the target to be running again */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                KeMemoryBarrier();
            }
        }
    }

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Release the freeze owner */
    InterlockedExchangePointer(&KiFreezeOwner, NULL);
}
