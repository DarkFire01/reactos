
/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

extern KSPIN_LOCK KiReverseStallIpiLock;

VOID
NTAPI
KiIpiGenericCallTarget(_In_ PKIPI_CONTEXT PacketContext,
                       _In_ PVOID BroadcastFunction,
                       _In_ PVOID Argument,
                       _In_ PULONG Count)
{
    //KPRCB* Prcb = KeGetCurrentPrcb();
    //UNREFERENCED_PARAMETER(Prcb);
    //__debugbreak();
    //*Count -= 1;
//
    ///* FIXME: TODO */
    //ASSERTMSG("Not yet implemented\n", FALSE);
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
    /* FIXME: TODO */
    ASSERTMSG("Not yet implemented\n", FALSE);
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
       // KiFreezeTargetExecution(TrapFrame, ExceptionFrame);
    }


    /* SYNCH_REQUEST we have a function pointer to execute! */
    if (InterlockedBitTestAndReset((PLONG)&Prcb->RequestSummary, IPI_SYNCH_REQUEST))
    {
#if defined(_M_ARM) || defined(_M_AMD64)
            DbgBreakPoint();
#else
            (void)InterlockedDecrementUL(&Prcb->CurrentPacket[1]);
            if (InterlockedCompareExchangeUL(&Prcb->CurrentPacket[2], 0, 0))
            {
                while (0 != InterlockedCompareExchangeUL(&Prcb->CurrentPacket[1], 0, 0))
                    ;
            }
            ((VOID(NTAPI *)(PVOID))(Prcb->WorkerRoutine))(Prcb->CurrentPacket[0]);
            InterlockedBitTestAndReset((PLONG)&Prcb->TargetSet, KeGetCurrentProcessorNumber());
            if (InterlockedCompareExchangeUL(&Prcb->CurrentPacket[2], 0, 0))
            {
                while (0 != InterlockedCompareExchangeUL(&Prcb->TargetSet, 0, 0))
                    ;
            }
        (void)InterlockedExchangePointer((PVOID *)&Prcb->SignalDone, NULL);
#endif // _M_ARM
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
    PKPRCB Prcb = KeGetCurrentPrcb();
#endif

    /* Raise to DPC level if required */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

#ifdef CONFIG_SMP
    /* Get current processor affinity */
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
        for (ULONG i = 0, Processor = 1; i < KeNumberProcessors; i++, Processor <<= 1)
        {
            if (Affinity & Processor)
            {
                Prcb = KiProcessorBlock[i];

                Prcb->WorkerRoutine = (PKIPI_BROADCAST_WORKER)Function;
                Prcb->CurrentPacket[0] = Argument;
                InterlockedBitTestAndSet((PLONG)&Prcb->RequestSummary, IPI_SYNCH_REQUEST);
            }
        }
    }
#endif

    /* Raise to IPI level */
    KeRaiseIrql(IPI_LEVEL, &OldIrql2);

    /* Call the function */
    Status = Function(Argument);

#ifdef CONFIG_SMP

    HalRequestIpi(Affinity);

    /* If this is MP, wait for the other processors to finish */
    if (Affinity)
    {
        /* Sanity check */
        ASSERT(Prcb == KeGetCurrentPrcb());

      // for(;;)
      // {
      //     
      // }
        /* FIXME: TODO */
       // ASSERTMSG("Not yet implemented\n", FALSE);
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

