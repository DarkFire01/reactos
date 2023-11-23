


#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

VOID
NTAPI
KiIpiGenericCallTarget(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID BroadcastFunction,
    _In_ PVOID Argument,
    _In_ PULONG Count)
{
    __debugbreak();
}

static
VOID
KiIpiSendRequest(
    _In_ KAFFINITY TargetSet,
    _In_ PKREQUEST_PACKET RequestPacket)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KAFFINITY RemainingSet;
    PKPRCB TargetPrcb;
    KIRQL OldIrql;
    ULONG ProcessorIndex;
    KAFFINITY ProcessorMask;
    ULONG CurrentIndex;

    /* Sanitize the target set */
    TargetSet &= KeActiveProcessors;

    /* Remove the current processor from the remaining set */
    RemainingSet = TargetSet & ~CurrentPrcb->SetMember;

    CurrentIndex = CurrentPrcb->Number;

    /* Loop while we have more processors */
    while (RemainingSet != 0)
    {
        ProcessorIndex = RtlFindLeastSignificantBit(RemainingSet);
        ASSERT(ProcessorIndex < KeNumberProcessors);
        ProcessorMask = (KAFFINITY)1 << ProcessorIndex;
        RemainingSet &= ~ProcessorMask;

        /* Get the target PRCB */
        TargetPrcb = KiProcessorBlock[ProcessorIndex];

        /* Wait for the mailbox slot to be available */
        while (TargetPrcb->SenderSummary & CurrentPrcb->SetMember)
        {
            KeMemoryBarrier();
        }

        /* Set up the request packet in the request mailbox */
        TargetPrcb->RequestMailbox[CurrentIndex].RequestPacket = *RequestPacket;
        TargetPrcb->RequestMailbox[CurrentIndex].RequestSummary = 1;

        /* Set the sender summary bit */
        InterlockedOr64(&TargetPrcb->SenderSummary, CurrentPrcb->SetMember);
    }

    /* Request an IPI with hal for all processors, except ourselves */
    HalRequestIpi(TargetSet & ~CurrentPrcb->SetMember);

    /* Run on the current processor, if requested */
    if (TargetSet & CurrentPrcb->SetMember)
    {
        PKIPI_WORKER WorkerRoutine = RequestPacket->WorkerRoutine;

        KeRaiseIrql(IPI_LEVEL, &OldIrql);
        WorkerRoutine(NULL,
                      RequestPacket->CurrentPacket[0],
                      RequestPacket->CurrentPacket[1],
                      RequestPacket->CurrentPacket[2]);
        KeLowerIrql(OldIrql);
    }
}

VOID
NTAPI
KiIpiSendPacket(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_ PKIPI_BROADCAST_WORKER BroadcastFunction,
    _In_ ULONG_PTR Context,
    _In_ PULONG Count)
{
    KREQUEST_PACKET RequestPacket;
    
    RequestPacket.CurrentPacket[0] = BroadcastFunction;
    RequestPacket.CurrentPacket[1] = (PVOID)Context;
    RequestPacket.CurrentPacket[2] = Count;
    RequestPacket.WorkerRoutine = WorkerRoutine;
    KiIpiSendRequest(TargetSet, &RequestPacket);
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
        HalSendSoftwareInterrupt(TargetSet, 0x1F);
    }
    else if (IpiRequest == IPI_DPC)
    {
        HalSendSoftwareInterrupt(TargetSet, 0x2F);
    }
    else
    {
        KiIpiSendPacket(TargetSet,
                        KiIpiGenericCallTarget,
                        NULL,
                        IpiRequest,
                        0);
    }
}
