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

/* FUNCTIONS *****************************************************************/

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
    while (CurrentPrcb->TargetSet != 0)
    {
        KeMemoryBarrier();
    }

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
 * \brief Drops a generic call packet into the mailbox of every processor in the
 *        target set, then raises the IPI interrupt on them.
 *
 * \param TargetSet - The processors to run the routine on, never including self.
 * \param Function - The routine each target has to run.
 * \param Argument - The argument handed to the routine.
 */
static
VOID
KiIpiSendGenericCall(
    _In_ KAFFINITY TargetSet,
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
        InterlockedExchange64((PLONG64)&Mailbox->RequestSummary, IPI_PACKET_READY);
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
    ULONG Index;

    CurrentPrcb = KeGetCurrentPrcb();

    /* Claim every pending sender in one go */
    SenderSet = (KAFFINITY)InterlockedExchange64((PLONG64)&CurrentPrcb->SenderSummary, 0);

    while (BitScanForwardAffinity(&Index, SenderSet))
    {
        SenderSet &= ~AFFINITY_MASK(Index);

        /* Ignore a sender whose packet is already gone */
        Mailbox = &CurrentPrcb->RequestMailbox[Index];
        if (InterlockedExchange64((PLONG64)&Mailbox->RequestSummary, 0) != IPI_PACKET_READY)
            continue;

        SenderPrcb = KiProcessorBlock[Index];
        Function = (PKIPI_BROADCAST_WORKER)(ULONG_PTR)Mailbox->RequestPacket.WorkerRoutine;
        Argument = (ULONG_PTR)Mailbox->RequestPacket.CurrentPacket[0];

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

        KiIpiSendGenericCall(TargetSet, BroadcastFunction, Argument);

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

    Status = BroadcastFunction(Argument);

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
    KeReleaseSpinLockFromDpcLevel(&KiIpiSpinLock);
    KeLowerIrql(OldIrql);

    return Status;
}
