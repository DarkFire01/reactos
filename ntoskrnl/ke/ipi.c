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

#ifdef NDEBUG
#define KdpDprintf(...)
#endif

/* PRIVATE FUNCTIONS *********************************************************/

#ifndef _M_AMD64

VOID
NTAPI
KiIpiGenericCallTarget(IN PKIPI_CONTEXT PacketContext,
                       IN PVOID BroadcastFunction,
                       IN PVOID Argument,
                       IN PVOID Count)
{
    /* FIXME: TODO */
    ASSERTMSG("Not yet implemented\n", FALSE);
}

VOID
FASTCALL
KiIpiSend(IN KAFFINITY TargetProcessors,
          IN ULONG IpiRequest)
{
    /* FIXME: TODO */
    ASSERTMSG("Not yet implemented\n", FALSE);
}

/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Inter-Processor Packet Interface
 * COPYRIGHT:   Copyright 2006 Alex Ionescu (alex.ionescu@reactos.org)
 *              Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
//#define NDEBUG
#include <debug.h>

#ifdef NDEBUG
#define KdpDprintf(...)
#endif

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;


/* PRIVATE FUNCTIONS *********************************************************/

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
               // DPRINT1("Count is %X\n", Count);
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
       __debugbreak();
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

#if 0
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

            InterlockedBitTestAndSet((PLONG)&Prcb->IpiFrozen, IpiRequest);
            HalRequestIpi(i);
        }
    }
#endif
}

VOID
NTAPI
KiIpiSendPacket(IN KAFFINITY TargetSet,
                IN PKIPI_BROADCAST_WORKER WorkerRoutine,
                IN ULONG_PTR Argument,
                IN ULONG Count,
                IN BOOLEAN Synchronize)
{
#ifdef CONFIG_SMP
    KAFFINITY Processor;
    LONG i;
    PKPRCB Prcb, CurrentPrcb;
    KIRQL oldIrql;

    ASSERT(KeGetCurrentIrql() == SYNCH_LEVEL);

    CurrentPrcb = KeGetCurrentPrcb();
    (void)InterlockedExchangeUL(&CurrentPrcb->TargetSet, TargetSet);
    (void)InterlockedExchangeUL(&CurrentPrcb->WorkerRoutine, (ULONG_PTR)WorkerRoutine);
    (void)InterlockedExchangePointer(&CurrentPrcb->CurrentPacket[0], Argument);
    (void)InterlockedExchangeUL(&CurrentPrcb->CurrentPacket[1], Count);
    (void)InterlockedExchangeUL(&CurrentPrcb->CurrentPacket[2], Synchronize ? 1 : 0);

    for (i = 0, Processor = 1; i < KeNumberProcessors; i++, Processor <<= 1)
    {
        if (TargetSet & Processor)
        {
            Prcb = KiProcessorBlock[i];
            while (0 != InterlockedCompareExchangeUL(&Prcb->SignalDone, (LONG)CurrentPrcb, 0));
            InterlockedBitTestAndSet((PLONG)&Prcb->IpiFrozen, IPI_SYNCH_REQUEST);
            if (Processor != CurrentPrcb->SetMember)
            {
                HalRequestIpi(i);
            }
        }
    }
    if (TargetSet & CurrentPrcb->SetMember)
    {
        KeRaiseIrql(IPI_LEVEL, &oldIrql);
        KiIpiServiceRoutine(NULL, NULL);
        KeLowerIrql(oldIrql);
    }
#endif
}
#endif

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
   // ASSERT(KeGetCurrentIrql() == IPI_LEVEL);
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
        //KiFreezeTargetExecution(TrapFrame, ExceptionFrame);
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

        /* FIXME: TODO */
        ASSERTMSG("Not yet implemented\n", FALSE);
    }
#endif

    /* Release the lock */
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);

    /* Lower IRQL back */
    KeLowerIrql(OldIrql);
    return Status;
}

#endif // !_M_AMD64
