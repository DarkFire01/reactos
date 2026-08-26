/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/ipi.c
 * PURPOSE:         Inter-Processor Packet Interface
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/* PRIVATE FUNCTIONS *********************************************************/

#ifndef _M_AMD64

/*
 * The x86 IPI protocol, as implemented by NT on this architecture.
 *
 * There are two independent channels between processors, and they are kept
 * apart because they have different urgency and different delivery rules:
 *
 *  - RequestSummary is a set of flags (IPI_APC, IPI_DPC, IPI_FREEZE). It says
 *    "something happened, look at yourself", carries no data, and several
 *    senders may contribute to the same summary before the target ever runs.
 *    The target takes the whole set in one InterlockedExchange, so a request
 *    is never seen twice and never lost between the test and the clear.
 *
 *  - SignalDone points at the sending PRCB and carries a packet: a worker
 *    routine plus three arguments, published in the sender's CurrentPacket.
 *    Only one packet may be in flight per target at a time, which is why the
 *    sender spins for the slot before claiming it.
 *
 * The low bit of the SignalDone pointer is a tag, not part of the address:
 * it is set when the sender has exactly one target. A sole target owns the
 * whole transaction and can clear TargetSet outright, where several targets
 * must each clear their own bit and the last one out releases PacketBarrier.
 * PRCBs are pool-aligned, so bit 0 is always free for this.
 */

#ifdef CONFIG_SMP
static
VOID
KiIpiPublishPacket(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY RemainingSet;
    ULONG_PTR Signal;
    ULONG Processor;

    /* Publish the packet before anyone can be told to look at it */
    Prcb->TargetSet = TargetSet;
    Prcb->WorkerRoutine = WorkerRoutine;
    Prcb->CurrentPacket[0] = Parameter1;
    Prcb->CurrentPacket[1] = Parameter2;
    Prcb->CurrentPacket[2] = Parameter3;

    /* A single target does not have to rendezvous with anyone */
    if ((TargetSet & (TargetSet - 1)) == 0)
    {
        Signal = (ULONG_PTR)Prcb | 1;
    }
    else
    {
        Prcb->PacketBarrier = TargetSet;
        Signal = (ULONG_PTR)Prcb;
    }

    /* Claim the mailbox slot on each target */
    RemainingSet = TargetSet;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&Processor, RemainingSet) != FALSE);
        ASSERT(Processor < (ULONG)KeNumberProcessors);
        RemainingSet &= ~AFFINITY_MASK(Processor);

        TargetPrcb = KiProcessorBlock[Processor];

        /* Wait until the target has finished with whatever it had, then take
           the slot. The exchange is what actually claims it - the wait above
           only keeps us from hammering the cache line. */
        while (InterlockedCompareExchangePointer((PVOID*)&TargetPrcb->SignalDone,
                                                 (PVOID)Signal,
                                                 NULL) != NULL)
        {
            while (TargetPrcb->SignalDone != NULL)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }
}
#endif /* CONFIG_SMP */

VOID
NTAPI
KiIpiGenericCallTarget(IN PKIPI_CONTEXT PacketContext,
                       IN PVOID BroadcastFunction,
                       IN PVOID Argument,
                       IN PVOID Count)
{
#ifdef CONFIG_SMP
    PKIPI_BROADCAST_WORKER Worker = (PKIPI_BROADCAST_WORKER)BroadcastFunction;

    /* Announce that we have arrived, then wait for everyone else, so that the
       broadcast function runs on all processors at once rather than as each
       one happens to get the interrupt */
    InterlockedDecrementUL((PULONG)Count);
    while (*(volatile ULONG *)Count != 0)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    Worker((ULONG_PTR)Argument);
#else
    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Argument);
    UNREFERENCED_PARAMETER(Count);
#endif
}

VOID
FASTCALL
KiIpiSend(IN KAFFINITY TargetProcessors,
          IN ULONG IpiRequest)
{
#ifdef CONFIG_SMP
    KAFFINITY RemainingSet;
    ULONG Processor;

    /* Record the request on every target before interrupting any of them. A
       target that is interrupted early must still see the complete summary,
       and a target that is already in the service routine must either see it
       or be interrupted again - which the HAL request below guarantees. */
    RemainingSet = TargetProcessors;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&Processor, RemainingSet) != FALSE);
        ASSERT(Processor < (ULONG)KeNumberProcessors);
        RemainingSet &= ~AFFINITY_MASK(Processor);

        InterlockedOr((PLONG)&KiProcessorBlock[Processor]->RequestSummary,
                      (LONG)IpiRequest);
    }

    /* One interrupt for the whole set */
    HalRequestIpi(TargetProcessors);
#else
    UNREFERENCED_PARAMETER(TargetProcessors);
    UNREFERENCED_PARAMETER(IpiRequest);
#endif
}

VOID
NTAPI
KiIpiSendPacket(IN KAFFINITY TargetProcessors,
                IN PKIPI_WORKER WorkerFunction,
                IN PKIPI_BROADCAST_WORKER BroadcastFunction,
                IN ULONG_PTR Context,
                IN PULONG Count)
{
#ifdef CONFIG_SMP
    KiIpiPublishPacket(TargetProcessors,
                       WorkerFunction,
                       (PVOID)BroadcastFunction,
                       (PVOID)Context,
                       (PVOID)Count);

    HalRequestIpi(TargetProcessors);
#else
    UNREFERENCED_PARAMETER(TargetProcessors);
    UNREFERENCED_PARAMETER(WorkerFunction);
    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Count);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDone(IN PKIPI_CONTEXT PacketContext)
{
#ifdef CONFIG_SMP
    ULONG_PTR Signal = (ULONG_PTR)PacketContext;
    PKPRCB SenderPrcb = (PKPRCB)(Signal & ~(ULONG_PTR)1);
    KAFFINITY SetMember = KeGetCurrentPrcb()->SetMember;

    if (Signal & 1)
    {
        /* We were the only target, so the whole set is ours to retire */
        SenderPrcb->TargetSet = 0;
    }
    else
    {
        /* Drop our bit. InterlockedXor answers with the value from before the
           exchange, so it equals our bit exactly when we cleared the last one
           and the set is now empty - that is who releases the barrier. */
        if ((KAFFINITY)InterlockedXor((PLONG)&SenderPrcb->TargetSet,
                                      (LONG)SetMember) == SetMember)
        {
            SenderPrcb->PacketBarrier = 0;
        }
    }
#else
    UNREFERENCED_PARAMETER(PacketContext);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(IN PKIPI_CONTEXT PacketContext,
                              IN volatile PULONG ReverseStall)
{
#ifdef CONFIG_SMP
    ULONG Original = *ReverseStall;

    /* Report completion, then hold here until the sender moves the stall on.
       The sender cannot release us before it has seen every target report,
       so this is what keeps all of us inside the packet until it says so. */
    KiIpiSignalPacketDone(PacketContext);

    while (Original == *ReverseStall)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }
#else
    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(ReverseStall);
#endif
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
KiIpiServiceRoutine(IN PKTRAP_FRAME TrapFrame,
                    IN PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKPRCB SenderPrcb;
    PKIPI_WORKER WorkerRoutine;
    ULONG_PTR Signal;
    ULONG Summary;

    /* Take everything that is pending in one move each. Testing a flag and
       then clearing it separately would lose any request that arrived in
       between, which on this path means a lost reschedule or a TLB entry
       that is never shot down. */
    Summary = (ULONG)InterlockedExchange((PLONG)&Prcb->RequestSummary, 0);
    Signal = (ULONG_PTR)InterlockedExchangePointer((PVOID*)&Prcb->SignalDone,
                                                   NULL);

    /* A freeze is answered before anything else and does not return until the
       debugger lets this processor go */
    if (Summary & IPI_FREEZE)
    {
        KiProcessorFreezeHandler(TrapFrame, ExceptionFrame);
    }

    /* Run the packet, if one was left for us */
    if (Signal != 0)
    {
        SenderPrcb = (PKPRCB)(Signal & ~(ULONG_PTR)1);
        WorkerRoutine = SenderPrcb->WorkerRoutine;

        /* Let the worker find the interrupted context if it wants it */
        Prcb->IpiFrame = TrapFrame;

        WorkerRoutine((PKIPI_CONTEXT)Signal,
                      SenderPrcb->CurrentPacket[0],
                      SenderPrcb->CurrentPacket[1],
                      SenderPrcb->CurrentPacket[2]);

        /* Report the packet complete on the worker's behalf. Workers that have
           to rendezvous first do it themselves with the Stall variant and this
           becomes the second, harmless report of an already empty set. */
        KiIpiSignalPacketDone((PKIPI_CONTEXT)Signal);
    }

    /* APC and DPC are requests to interrupt ourselves later, at the right
       IRQL - they are never run from here, at IPI_LEVEL */
    if (Summary & IPI_APC)
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }

    if (Summary & IPI_DPC)
    {
        Prcb->DpcInterruptRequested = TRUE;
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }
#else
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
#endif
    return TRUE;
}

/*
 * @implemented
 */
ULONG_PTR
NTAPI
KeIpiGenericCall(IN PKIPI_BROADCAST_WORKER Function,
                 IN ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, OldIrql2;
#ifdef CONFIG_SMP
    KAFFINITY Affinity;
    ULONG Count;
    PKPRCB Prcb = KeGetCurrentPrcb();
#endif

    /* Raise to DPC level if required */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

#ifdef CONFIG_SMP
    /* Get current processor count and affinity */
    Count = KeNumberProcessors;
    Affinity = KeActiveProcessors;

    /* Exclude ourselves */
    Affinity &= ~Prcb->SetMember;
#endif

    /* Acquire the IPI lock */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#ifdef CONFIG_SMP
    /* Make sure this is MP */
    if (Affinity)
    {
        /* Send an IPI */
        KiIpiSendPacket(Affinity,
                        KiIpiGenericCallTarget,
                        Function,
                        Argument,
                        &Count);

        /* Spin until the other processors are ready */
        while (Count != 1)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }
#endif

    /* Raise to IPI level */
    KeRaiseIrql(IPI_LEVEL, &OldIrql2);

#ifdef CONFIG_SMP
    /* Let the other processors know it is time */
    Count = 0;
#endif

    /* Call the function */
    Status = Function(Argument);

#ifdef CONFIG_SMP
    /* If this is MP, wait for the other processors to finish */
    if (Affinity)
    {
        /* Sanity check */
        ASSERT(Prcb == KeGetCurrentPrcb());

        /* Wait for every target to retire the packet */
        while (Prcb->TargetSet != 0)
        {
            YieldProcessor();
            KeMemoryBarrier();
        }
    }
#endif

    /* Lower back to DISPATCH_LEVEL before releasing the lock we took there */
    KeLowerIrql(OldIrql2);

    /* Release the lock */
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);

    /* Lower IRQL back */
    KeLowerIrql(OldIrql);
    return Status;
}

VOID
NTAPI
KiIpiSendRequest(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();
    KAFFINITY SetMember = Prcb->SetMember;
    KAFFINITY RemoteSet;
    KIRQL OldIrql;

    /* Only ever talk to processors that are actually running */
    TargetSet &= KeActiveProcessors;
    RemoteSet = TargetSet & ~SetMember;

    /* Nothing to do for anyone else - just run it here if we were included */
    if (RemoteSet == 0)
    {
        if (TargetSet & SetMember)
        {
            WorkerRoutine(NULL, Parameter1, Parameter2, Parameter3);
        }
        return;
    }

    /* Serialise senders: a processor may only have one packet outstanding,
       and TargetSet below is what tells us it has been retired */
    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

    KiIpiPublishPacket(RemoteSet,
                       WorkerRoutine,
                       Parameter1,
                       Parameter2,
                       Parameter3);

    HalRequestIpi(RemoteSet);

    /* Do our own share while the others get there */
    if (TargetSet & SetMember)
    {
        WorkerRoutine(NULL, Parameter1, Parameter2, Parameter3);
    }

    /* The caller is entitled to assume the work is done everywhere when we
       return - a TLB shootdown that is still in flight is worse than none */
    while (Prcb->TargetSet != 0)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);
#else
    UNREFERENCED_PARAMETER(TargetSet);
    WorkerRoutine(NULL, Parameter1, Parameter2, Parameter3);
#endif
}

#endif // !_M_AMD64
