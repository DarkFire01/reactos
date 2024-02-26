
/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

extern KSPIN_LOCK KiReverseStallIpiLock;

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame);;

VOID
NTAPI
KiIpiGenericCallTarget(_Inout_ PKIPI_CONTEXT PacketContext,
                       _In_ PKIPI_BROADCAST_WORKER BroadcastFunction,
                       _In_ PVOID Argument,
                       _In_ PULONG Count)
{
    *Count -= 1;
    while (*Count != 0)
    {
        KeMemoryBarrier();
        YieldProcessor();
    }

    if (BroadcastFunction)
    {
        /* Call the function pointer */
        (*BroadcastFunction)((ULONG_PTR)Argument);
    }

    /* we're done! */
    KiIpiSignalPacketDone(PacketContext);
}

/**
 * @brief
 * Send a interrupt of whatever type is assigned in IpiRequest to the target CPU set
 *
 * @param[in] TargetSet
 * List of CPUs being sent IPIs
 *
 * @param[in] IpiRequest
 * The Interrupt type being sent to target CPUs
 */
VOID
NTAPI
KiIpiSendRequest(IN KAFFINITY TargetSet,
                 IN ULONG IpiRequest)
{
#ifdef CONFIG_SMP
    LONG i;
    PKPRCB Prcb;
    KAFFINITY Current;

    for (i = 0, Current = 1; i < KeNumberProcessors; i++, Current <<= 1)
    {
        if (TargetSet & Current)
        {
            /* Get the PRCB for this CPU */
            Prcb = KiProcessorBlock[i];

            InterlockedBitTestAndSet((PLONG)&Prcb->RequestSummary, IpiRequest);
        }
    }

    /* HalRequestIpi does its own mask check =*/
    HalRequestIpi(TargetSet);
#endif
}

VOID
NTAPI
KiIpiSendPacket(
    IN KAFFINITY TargetProcessors,
    IN PKIPI_WORKER WorkerFunction,
    IN PKIPI_BROADCAST_WORKER BroadcastFunction,
    IN ULONG_PTR Context,
    IN PULONG Count)
{
#ifdef CONFIG_SMP
    KAFFINITY Processor;
    LONG i;
    PKPRCB Prcb, CurrentPrcb;
    KIRQL oldIrql;

    /* Parse processor list and prep the routine to be serviced */
    CurrentPrcb = KeGetCurrentPrcb();
    for (i = 0, Processor = 1; i < KeNumberProcessors; i++, Processor <<= 1)
    {
        if (TargetProcessors & Processor)
        {
            Prcb = KiProcessorBlock[i];
            Prcb->CurrentPacket[1] = BroadcastFunction;
            Prcb->CurrentPacket[2] = (PVOID)Context;
            Prcb->CurrentPacket[3] = Count;
            Prcb->WorkerRoutine = WorkerFunction;
            InterlockedBitTestAndSet((PLONG)&Prcb->RequestSummary, IPI_SYNCH_REQUEST);
            if (Processor != CurrentPrcb->SetMember)
            {

            }
        }
    }

    /* If the processor entering this routine ALSO needs to execute, service the routine */
    if (TargetProcessors & CurrentPrcb->SetMember)
    {
        KeRaiseIrql(IPI_LEVEL, &oldIrql);
        KiIpiServiceRoutine(NULL, NULL);
        KeLowerIrql(oldIrql);
    }

    /* Fire off to the processors! */
    HalRequestIpi(TargetProcessors);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDone(IN PKIPI_CONTEXT PacketContext)
{
    KPRCB* Prcb = KeGetCurrentPrcb();

    Prcb->WorkerRoutine = NULL;
    /* FIXME: TODO */
   // ASSERTMSG("Not yet implemented\n", FALSE);
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(IN PKIPI_CONTEXT PacketContext,
                              IN volatile PULONG ReverseStall)
{
    /* FIXME: TODO */
    ASSERTMSG("Not yet implemented\n", FALSE);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
KiIpiServiceRoutine(IN PKTRAP_FRAME TrapFrame, IN PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb;
    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);
    Prcb = KeGetCurrentPrcb();

    /* APC level! Trigger an APC interrupt */
    if (InterlockedBitTestAndReset((PLONG)&Prcb->RequestSummary, IPI_APC))
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }
    /* DPC level! Trigger an DPC interrupt */
    if (InterlockedBitTestAndReset((PLONG)&Prcb->RequestSummary, IPI_DPC))
    {
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }
    /* Freeze level! Trigger a FREEZE interrupt */
    if (InterlockedBitTestAndReset((PLONG)&Prcb->RequestSummary, IPI_FREEZE))
    {
       KiProcessorFreezeHandler(TrapFrame, ExceptionFrame);
    }


    /* SYNCH_REQUEST we have a function pointer to execute! */
    if (InterlockedBitTestAndReset((PLONG)&Prcb->RequestSummary, IPI_SYNCH_REQUEST))
    {
        PKIPI_WORKER WorkerCall;
        WorkerCall = Prcb->WorkerRoutine;
        WorkerCall(Prcb->CurrentPacket[0],  //PKIPI_CONTEXT PacketContext,
                   Prcb->CurrentPacket[1],  //PVOID BroadcastFunction,
                   Prcb->CurrentPacket[2],  //PVOID Argument,
                   Prcb->CurrentPacket[3]); //PULONG Count)
    }
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

        for (ULONG i = 0, Processor = 1; i < KeNumberProcessors; i++, Processor <<= 1)
        {
            Prcb = KiProcessorBlock[i];

            if (Prcb != KeGetCurrentPrcb())
            {
                while(Prcb->WorkerRoutine != NULL)
                {
                    YieldProcessor();
                    KeMemoryBarrierWithoutFence();
                }
            }
        }
    }
#endif

    /* Release the lock */
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);

    /* Lower IRQL back */
    KeLowerIrql(OldIrql);
    return Status;
}

/**
 * @brief
 * Send a interrupt of whatever type is assigned in IpiRequest to the target CPU set
 *
 * @param[in] TargetSet
 * List of CPUs being sent IPIs
 *
 * @param[in] IpiRequest
 * The Interrupt type being sent to target CPUs
 */
VOID
FASTCALL
KiIpiSend(IN KAFFINITY TargetProcessors, IN ULONG IpiRequest)
{
    /* Call private function */
    KiIpiSendRequest(TargetProcessors, IpiRequest);
}

