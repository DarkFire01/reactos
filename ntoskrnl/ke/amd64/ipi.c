/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IPI code for x64
 * COPYRIGHT:   Copyright 2023 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ***/

KSPIN_LOCK KiIpiSpinLock;

/*
 * How long a processor waits on the rest of them before it says so. Long
 * enough that one which is merely busy still gets there, and short enough that
 * a request nobody ever answered does not take the machine down with it in
 * silence.
 */
#define KI_IPI_WAIT_SPINS 0x20000000

/* FUNCTIONS *****************************************************************/

/*!
 * \brief Waits until every processor of the target set has cleared its bit.
 *
 * \param Prcb - The block whose target set the others are clearing.
 * \param Reason - What the wait is for, for the report.
 *
 * \remarks A processor that never answers is not recoverable from here: the
 *          caller is holding the machine still and cannot go on without it. So
 *          this keeps waiting, and only says who it is waiting on - which is
 *          the one thing a hang like that otherwise never tells anyone.
 */
static
VOID
KiIpiWaitForTargets(
    _In_ PKPRCB Prcb,
    _In_ PCSTR Reason)
{
    KAFFINITY Remaining;
    PKPRCB TargetPrcb, FreezeOwner;
    ULONG64 Spins = 0;
    ULONG Index;

    while (Prcb->TargetSet != 0)
    {
        YieldProcessor();
        KeMemoryBarrierWithoutFence();

        if (++Spins % KI_IPI_WAIT_SPINS != 0)
            continue;

        FreezeOwner = KiFreezeOwner;

        DPRINT1("Ki: processor %u is still waiting on %I64x to %s, active %I64x, "
                "freeze owner %d\n",
                Prcb->Number, Prcb->TargetSet, Reason, KeActiveProcessors,
                FreezeOwner != NULL ? (LONG)FreezeOwner->Number : -1);

        /*
         * Which half of the handshake is missing. A processor that still has
         * this one announced in its own summary never ran the service routine,
         * so the interrupt never got to it. One that has taken the packet and
         * not cleared its bit is in the routine and not coming back.
         */
        Remaining = Prcb->TargetSet;
        while (BitScanForwardAffinity(&Index, Remaining))
        {
            Remaining &= ~AFFINITY_MASK(Index);

            TargetPrcb = KiProcessorBlock[Index];
            if (TargetPrcb == NULL)
            {
                DPRINT1("Ki:   processor %lu has no block\n", Index);
                continue;
            }

            /*
             * A target's IRQL lives in its CR8 and cannot be read from here, so
             * what it can report is the state that is in the block: IpiFrozen is
             * the debugger's freeze state machine, and a non zero PacketBarrier
             * on any processor means a generic call whose targets are parked at
             * IPI_LEVEL and deaf to everything else.
             */
            DPRINT1("Ki:   processor %lu senders %I64x packet %I64x frozen %lx "
                    "barrier %I64x halted %u\n",
                    Index,
                    TargetPrcb->SenderSummary,
                    TargetPrcb->RequestMailbox[Prcb->Number].RequestSummary,
                    TargetPrcb->IpiFrozen,
                    TargetPrcb->PacketBarrier,
                    TargetPrcb->IdleHalt);
        }
    }
}

static PKIPI_BROADCAST_WORKER KiIpiBroadcastWorkerTable[] =
{
    NULL, // IPI_APC
    NULL, // IPI_DPC
    NULL, // IPI_FREEZE
};

static
VOID
NTAPI
KiIpiGenericCallWorker(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    PKIPI_BROADCAST_WORKER BroadcastWorker = Parameter1;
    ULONG_PTR Argument = (ULONG_PTR)Parameter2;
    PULONG Count = (PULONG)Parameter3;

    /* Acknowledge receival by decrementing the count */
    InterlockedDecrementUL(Count);

    /* Call the broadcast function */
    BroadcastWorker(Argument);
}

VOID
KiRequestIpi(
    _In_ KAFFINITY TargetSet)
{
    KIRQL OldIrql;

    /* Raise to sync level */
    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);

    /* Acquire the spinlock */
    KeAcquireSpinLockAtDpcLevel(&KiIpiSpinLock);

    /* Request the IPI */
    HalRequestIpi(TargetSet);

    /* Lower to IRQL */
    KeReleaseSpinLockFromDpcLevel(&KiIpiSpinLock);
    KeLowerIrql(OldIrql);
}

static
VOID
KiIpiSendRequestPacket(
    _In_ KAFFINITY TargetSet,
    _In_ PKREQUEST_PACKET RequestPacket)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KAFFINITY RemainingSet, SetMember;
    PKPRCB TargetPrcb;
    KIRQL OldIrql;
    ULONG ProcessorIndex;
    ULONG SenderIndex;

    /* Sanitize the target set */
    TargetSet &= KeActiveProcessors;

    /* Remove the current processor from the target set */
    CurrentPrcb->TargetSet = TargetSet & ~CurrentPrcb->SetMember;

    SenderIndex = CurrentPrcb->Number;

    /* Raise to sync level */
    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);

    /* Acquire the IPI spinlock */
    KeAcquireSpinLockAtDpcLevel(&KiIpiSpinLock);

    /* Raise to IPI level, so we don't get interrupted */

    /* Loop while we have more processors */
    RemainingSet = CurrentPrcb->TargetSet;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&ProcessorIndex, RemainingSet) != 0);
        ASSERT(ProcessorIndex < KeNumberProcessors);
        SetMember = AFFINITY_MASK(ProcessorIndex);
        RemainingSet &= ~SetMember;

        /* Get the target PRCB */
        TargetPrcb = KiProcessorBlock[ProcessorIndex];

        // TODO: Don't use the current processor's index, but find a free one,
        // so we can support more than 64 CPUs.

        /* Wait for the mailbox slot to be available */
        while (TargetPrcb->SenderSummary & CurrentPrcb->SetMember)
        {
            KeMemoryBarrier();
        }

        /* Set up the request packet in the request mailbox */
        TargetPrcb->RequestMailbox[SenderIndex].RequestPacket = *RequestPacket;

        /*
         * This form carries a routine of four arguments, which is not one of
         * the two KiIpiInterruptHandler serves. Nothing asks for it, and the
         * summary is left at a value the handler will not act on rather than
         * one it would act on wrongly: a target that answered this with the
         * one argument form would call the routine with three registers of
         * whatever happened to be in them.
         */
        TargetPrcb->RequestMailbox[SenderIndex].RequestSummary = 1;

        /* Set the sender summary bit */
        InterlockedOr64(&TargetPrcb->SenderSummary, CurrentPrcb->SetMember);
    }

    /* Request an IPI with hal for all processors, except ourselves */
    HalRequestIpi(TargetSet & ~CurrentPrcb->SetMember);

    /* Run on the current processor, if requested */
    if (TargetSet & CurrentPrcb->SetMember)
    {
        PKIPI_WORKER WorkerRoutine = RequestPacket->WorkerRoutine;

        WorkerRoutine(NULL,
                      RequestPacket->CurrentPacket[0],
                      RequestPacket->CurrentPacket[1],
                      RequestPacket->CurrentPacket[2]);
    }

    /* Wait for acknowledgement */
    KiIpiWaitForTargets(CurrentPrcb, "take a request packet");

    /* Lower to IRQL */
    KeReleaseSpinLockFromDpcLevel(&KiIpiSpinLock);
    KeLowerIrql(OldIrql);
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
    KREQUEST_PACKET RequestPacket;

    RequestPacket.WorkerRoutine = WorkerRoutine;
    RequestPacket.CurrentPacket[0] = Parameter1;
    RequestPacket.CurrentPacket[1] = Parameter2;
    RequestPacket.CurrentPacket[2] = Parameter3;
    KiIpiSendRequestPacket(TargetSet, &RequestPacket);
}

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    /* Check if we can send the IPI directly */
    if (IpiRequest == IPI_APC)
    {
        HalSendSoftwareInterrupt(TargetSet, APC_LEVEL);
    }
    else if (IpiRequest == IPI_DPC)
    {
        HalSendSoftwareInterrupt(TargetSet, DISPATCH_LEVEL);
    }
    else if (IpiRequest == IPI_FREEZE)
    {
        /* On x64 the freeze IPI is an NMI */
        HalSendNMI(TargetSet);
    }
    else
    {
        __debugbreak();
    }
}

/*!
 * \brief Drops a packet into the mailbox of every processor in the target set,
 *        then raises the IPI interrupt on them.
 *
 * \param TargetSet - The processors to run the routine on, never including self.
 * \param Request - IPI_PACKET_READY for a generic call, IPI_SYNCH_REQUEST for a
 *                  routine that is only acknowledged once it has run.
 * \param Function - The routine each target has to run.
 * \param Argument - The argument handed to the routine.
 */
static
VOID
KiIpiSendPackets(
    _In_ KAFFINITY TargetSet,
    _In_ LONG64 Request,
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    PKPRCB CurrentPrcb, TargetPrcb;
    PREQUEST_MAILBOX Mailbox;
    KAFFINITY RemainingSet;
    ULONG Index;

    CurrentPrcb = KeGetCurrentPrcb();

    RemainingSet = TargetSet;
    while (BitScanForwardAffinity(&Index, RemainingSet))
    {
        RemainingSet &= ~AFFINITY_MASK(Index);

        /* Every sender owns one slot in the mailbox of the target */
        TargetPrcb = KiProcessorBlock[Index];
        Mailbox = &TargetPrcb->RequestMailbox[CurrentPrcb->Number];
        Mailbox->RequestPacket.WorkerRoutine = (PVOID)(ULONG_PTR)Function;
        Mailbox->RequestPacket.CurrentPacket[0] = (PVOID)Argument;

        /* Publish the packet, and only then announce this sender */
        InterlockedExchange64((PLONG64)&Mailbox->RequestSummary, Request);
        InterlockedOr64((PLONG64)&TargetPrcb->SenderSummary,
                        (LONG64)CurrentPrcb->SetMember);
    }

    HalRequestIpi(TargetSet);
}

/*!
 * \brief Runs the generic call packets other processors left for this one.
 *        Called by KiIpiInterrupt, which has already raised to IPI_LEVEL.
 */
VOID
NTAPI
KiIpiInterruptHandler(VOID)
{
    PKPRCB CurrentPrcb, SenderPrcb;
    PREQUEST_MAILBOX Mailbox;
    PKIPI_BROADCAST_WORKER Function;
    ULONG_PTR Argument;
    KAFFINITY SenderSet;
    LONG64 Request;
    ULONG Index;

    CurrentPrcb = KeGetCurrentPrcb();

    /* Claim every pending sender in one go */
    SenderSet = (KAFFINITY)InterlockedExchange64((PLONG64)&CurrentPrcb->SenderSummary, 0);

    while (BitScanForwardAffinity(&Index, SenderSet))
    {
        SenderSet &= ~AFFINITY_MASK(Index);

        Mailbox = &CurrentPrcb->RequestMailbox[Index];
        Request = InterlockedExchange64((PLONG64)&Mailbox->RequestSummary, 0);

        /* Ignore a sender whose packet is already gone */
        if ((Request != IPI_PACKET_READY) && (Request != IPI_SYNCH_REQUEST))
            continue;

        SenderPrcb = KiProcessorBlock[Index];
        Function = (PKIPI_BROADCAST_WORKER)(ULONG_PTR)Mailbox->RequestPacket.WorkerRoutine;
        Argument = (ULONG_PTR)Mailbox->RequestPacket.CurrentPacket[0];

        /* No rendezvous here, the sender only waits for the work to be done */
        if (Request == IPI_SYNCH_REQUEST)
        {
            Function(Argument);
            InterlockedAnd64((PLONG64)&SenderPrcb->TargetSet,
                             ~(LONG64)CurrentPrcb->SetMember);
            continue;
        }

        /* Report that this processor is parked and no longer touching anything */
        InterlockedAnd64((PLONG64)&SenderPrcb->TargetSet,
                         ~(LONG64)CurrentPrcb->SetMember);

        /* Hold here until the sender has reached IPI_LEVEL as well */
        while (SenderPrcb->PacketBarrier != 0)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }

        Function(Argument);

        /* Report completion, the sender is waiting on this */
        InterlockedAnd64((PLONG64)&SenderPrcb->TargetSet,
                         ~(LONG64)CurrentPrcb->SetMember);
    }
}

/*!
 * \brief Runs a routine on this processor and on the other processors of the
 *        target set, and returns only once every one of them has run it.
 *
 * \param TargetSet - The processors to run the routine on. This processor is
 *                    always included.
 * \param Function - The routine, called at IPI_LEVEL on the other processors.
 * \param Argument - The argument handed to the routine.
 *
 * \remarks The caller has to be at SYNCH_LEVEL. That keeps a generic call from
 *          being started here, which would reuse the same mailbox slots.
 */
VOID
NTAPI
KiIpiSendSynchRequest(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    PKPRCB Prcb;

    ASSERT(KeGetCurrentIrql() == SYNCH_LEVEL);

    Prcb = KeGetCurrentPrcb();
    TargetSet &= KeActiveProcessors & ~Prcb->SetMember;

    if (TargetSet != 0)
    {
        /* Each target clears its bit after running the routine */
        InterlockedExchange64((PLONG64)&Prcb->TargetSet, (LONG64)TargetSet);
        KiIpiSendPackets(TargetSet, IPI_SYNCH_REQUEST, Function, Argument);
    }

    /* Do our own part while the others work on theirs */
    Function(Argument);

    /* IPI_LEVEL is above SYNCH_LEVEL, so requests aimed at us still get served */
    KiIpiWaitForTargets(Prcb, "run a synchronous request");
}

ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER BroadcastFunction,
    _In_ ULONG_PTR Argument)
{
    PKPRCB Prcb;
    KAFFINITY TargetSet;
    KIRQL OldIrql, DpcIrql;
    ULONG_PTR Status;

    /* Hold off the dispatcher first */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* Only one generic call may be in flight at a time */
    KeAcquireSpinLockAtDpcLevel(&KiIpiSpinLock);

    Prcb = KeGetCurrentPrcb();
    TargetSet = KeActiveProcessors & ~Prcb->SetMember;
    if (TargetSet != 0)
    {
        /* Targets clear their bit on arrival and then wait on the barrier */
        InterlockedExchange64((PLONG64)&Prcb->TargetSet, (LONG64)TargetSet);
        InterlockedExchange64((PLONG64)&Prcb->PacketBarrier, 1);

        KiIpiSendPackets(TargetSet, IPI_PACKET_READY, BroadcastFunction, Argument);

        /* Nothing may still be running elsewhere once the routine starts */
        KiIpiWaitForTargets(Prcb, "park for a generic call");
    }

    /* Everybody is parked, so go up and run the routine */
    KeRaiseIrql(IPI_LEVEL, &DpcIrql);

    if (TargetSet != 0)
    {
        /* Arm the set for the completion handshake before letting the targets go */
        InterlockedExchange64((PLONG64)&Prcb->TargetSet, (LONG64)TargetSet);
        InterlockedExchange64((PLONG64)&Prcb->PacketBarrier, 0);
    }

    Status = BroadcastFunction(Argument);

    if (TargetSet != 0)
    {
        /* The routine has to have finished everywhere before we return */
        KiIpiWaitForTargets(Prcb, "finish a generic call");
    }

    KeLowerIrql(DpcIrql);
    KeReleaseSpinLockFromDpcLevel(&KiIpiSpinLock);
    KeLowerIrql(OldIrql);

    return Status;
}
