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

/* GLOBALS ********************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/* FUNCTIONS *****************************************************************/

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
        ASSERT(FALSE);
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
    while (Prcb->TargetSet != 0)
    {
        YieldProcessor();
        KeMemoryBarrierWithoutFence();
    }
}

ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
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
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

    Prcb = KeGetCurrentPrcb();
    TargetSet = KeActiveProcessors & ~Prcb->SetMember;
    if (TargetSet != 0)
    {
        /* Targets clear their bit on arrival and then wait on the barrier */
        InterlockedExchange64((PLONG64)&Prcb->TargetSet, (LONG64)TargetSet);
        InterlockedExchange64((PLONG64)&Prcb->PacketBarrier, 1);

        KiIpiSendPackets(TargetSet, IPI_PACKET_READY, Function, Argument);

        /* Nothing may still be running elsewhere once the routine starts */
        while (Prcb->TargetSet != 0)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }

    /* Everybody is parked, so go up and run the routine */
    KeRaiseIrql(IPI_LEVEL, &DpcIrql);

    if (TargetSet != 0)
    {
        /* Arm the set for the completion handshake before letting the targets go */
        InterlockedExchange64((PLONG64)&Prcb->TargetSet, (LONG64)TargetSet);
        InterlockedExchange64((PLONG64)&Prcb->PacketBarrier, 0);
    }

    Status = Function(Argument);

    if (TargetSet != 0)
    {
        /* The routine has to have finished everywhere before we return */
        while (Prcb->TargetSet != 0)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }

    KeLowerIrql(DpcIrql);
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);

    return Status;
}
