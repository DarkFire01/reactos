/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ARM64 multiprocessor startup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* One secondary processor's control region and idle thread */
typedef struct _KI_AP_BLOCK
{
    KIPCR Pcr;
    ETHREAD IdleThread;
} KI_AP_BLOCK, *PKI_AP_BLOCK;

#define TAG_KI_AP_BLOCK     'pAiK'

/* How long a started processor has to reach its idle loop */
#define KI_AP_TIMEOUT_MS    5000

/* GLOBALS *******************************************************************/

/* The DPC stack of the processor being started, its startup code takes it */
PVOID KiStartingDpcStack;

extern volatile ULONG KiApStartStep;
extern KAFFINITY KiFreezeUnanswered;
extern volatile ULONG_PTR KiApStartFailure[5];

/* FUNCTIONS *****************************************************************/

/**
 * @brief Starts every processor the HAL can find, one at a time.
 *
 * Each processor enters KiSystemStartup with the loader block describing its
 * PCR, idle thread and stacks, and clears LoaderBlock->Prcb once it idles.
 */
CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    KPROCESSOR_STATE ProcessorState;
    PKI_AP_BLOCK Block;
    PVOID KernelStack, DpcStack;
    ULONG Number, Waited;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    ASSERT(KeNumberProcessors == 1);

    for (;;)
    {
        Number = (ULONG)KeNumberProcessors;
        if (Number >= MAXIMUM_PROCESSORS)
            break;

        if (KeBootprocSpecified && (Number >= KeBootprocSpecified))
            break;

        Block = ExAllocatePoolZero(NonPagedPool, sizeof(*Block), TAG_KI_AP_BLOCK);
        KernelStack = MmCreateKernelStack(FALSE, 0);
        DpcStack = MmCreateKernelStack(FALSE, 0);
        if (!Block || !KernelStack || !DpcStack)
        {
            DPRINT1("No memory to start processor %lu\n", Number);
            if (Block)
                ExFreePoolWithTag(Block, TAG_KI_AP_BLOCK);
            if (KernelStack)
                MmDeleteKernelStack(KernelStack, FALSE);
            if (DpcStack)
                MmDeleteKernelStack(DpcStack, FALSE);
            break;
        }

        ASSERT(((ULONG_PTR)&Block->Pcr & 0xF) == 0);

        /* The DPC stack carries it until it moves to its boot stack */
        RtlZeroMemory(&ProcessorState, sizeof(ProcessorState));
        ProcessorState.ContextFrame.Pc = (ULONG64)KiSystemStartup;
        ProcessorState.ContextFrame.Sp = (ULONG64)DpcStack;
        ProcessorState.ContextFrame.X0 = (ULONG64)KeLoaderBlock;

        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Thread = (ULONG_PTR)&Block->IdleThread.Tcb;
        KeLoaderBlock->Process = (ULONG_PTR)&KiInitialProcess.Pcb;
        KiStartingDpcStack = DpcStack;
        KiApStartStep = 0;
        KiApStartFailure[0] = 0;
        KeMemoryBarrier();
        KeLoaderBlock->Prcb = (ULONG_PTR)&Block->Pcr.Prcb;
        KeMemoryBarrier();

        if (!HalStartNextProcessor(KeLoaderBlock, &ProcessorState))
        {
            KeLoaderBlock->Prcb = 0;
            KiStartingDpcStack = NULL;
            ExFreePoolWithTag(Block, TAG_KI_AP_BLOCK);
            MmDeleteKernelStack(KernelStack, FALSE);
            MmDeleteKernelStack(DpcStack, FALSE);
            break;
        }

        for (Waited = 0; KeLoaderBlock->Prcb && !KiApStartFailure[0] && (Waited < KI_AP_TIMEOUT_MS); Waited++)
        {
            KeStallExecutionProcessor(1000);
            KeMemoryBarrier();

            /* Shows this processor is still alive while the other one comes up */
            if ((Waited % 1000) == 999)
                DPRINT1("Waiting for processor %lu, it is at step %lu\n", Number, KiApStartStep);
        }

        /* It runs somewhere with this block, nothing can be freed */
        /* A trap or bugcheck on it: code, then the trap PC and syndrome or its parameters */
        if (KiApStartFailure[0])
        {
            DPRINT1("Processor %lu stopped at step %lu: %Ix %Ix %Ix %Ix %Ix\n", Number, KiApStartStep,
                    KiApStartFailure[0], KiApStartFailure[1], KiApStartFailure[2],
                    KiApStartFailure[3], KiApStartFailure[4]);
            KeBugCheckEx(PHASE1_INITIALIZATION_FAILED,
                         (KiApStartStep << 8) | Number,
                         KiApStartFailure[0],
                         KiApStartFailure[1],
                         KiApStartFailure[2]);
        }

        if (KeLoaderBlock->Prcb)
        {
            DPRINT1("Processor %lu never checked in, it got to step %lu\n", Number, KiApStartStep);
            KeBugCheckEx(PHASE1_INITIALIZATION_FAILED, 0x5C1, Number, KiApStartStep, KeNumberProcessors);
        }

        ASSERT(KeNumberProcessors == (CCHAR)(Number + 1));
        ASSERT(KiProcessorBlock[Number] == &Block->Pcr.Prcb);
        ASSERT(KeActiveProcessors & AFFINITY_MASK(Number));
        DPRINT1("Processor %lu is up\n", Number);
        if (KiFreezeUnanswered)
            DPRINT1("Processors %Ix never answered the debugger's freeze IPI\n", KiFreezeUnanswered);
    }

    KiStartingDpcStack = NULL;
    DPRINT1("%u processor(s) running\n", KeNumberProcessors);
}

/* What stopped the processor being started, read by KeStartAllProcessors */
volatile ULONG_PTR KiApStartFailure[5];

/**
 * @brief Parks a processor that is still starting and hit a trap or a bugcheck.
 *
 * The debugger would freeze the running processors and then fault again on a
 * processor that is half set up, so the failure is only recorded here.
 */
VOID
NTAPI
KiParkStartingProcessor(
    _In_ ULONG_PTR Code,
    _In_ ULONG_PTR Parameter1,
    _In_ ULONG_PTR Parameter2,
    _In_ ULONG_PTR Parameter3,
    _In_ ULONG_PTR Parameter4)
{
    PKPRCB Prcb = KeGetCurrentPrcb();

    if (!Prcb->Number || (KeActiveProcessors & Prcb->SetMember))
        return;

    KiApStartFailure[1] = Parameter1;
    KiApStartFailure[2] = Parameter2;
    KiApStartFailure[3] = Parameter3;
    KiApStartFailure[4] = Parameter4;
    KeMemoryBarrier();
    KiApStartFailure[0] = Code;
    KeMemoryBarrier();

    for (;;)
    {
        _disable();
        __wfi();
    }
}
