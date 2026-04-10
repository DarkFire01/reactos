

#include <ntoskrnl.h>
#include <internal/ke.h>
#define NDEBUG
#include <debug.h>

#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif

CODE_SEG("INIT")
VOID
NTAPI
KiInitializeMachineType(VOID)
{
    KeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
    KeProcessorLevel = 0;
    KeProcessorRevision = 0;
}

CODE_SEG("INIT")
VOID
NTAPI
KiInitializeKernel(IN PKPROCESS InitProcess,
                   IN PKTHREAD InitThread,
                   IN PVOID IdleStack,
                   IN PKPRCB Prcb,
                   IN CCHAR Number,
                   IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG_PTR PageDirectory[2] = {0, 0};

    /* Keep ARM64 startup behavior close to the ARM path until dedicated
     * ARM64 structures are fully implemented. */
    SharedUserData->NXSupportPolicy = NX_SUPPORT_POLICY_OPTIN;

    KiInitSpinLocks(Prcb, Number);

    if (Number == 0)
    {
        KeFeatureBits = 0;

        KeLowerIrql(APC_LEVEL);

        KiInitSystem();

        InitializeListHead(&KiProcessListHead);
        KeInitializeProcess(InitProcess,
                            0,
                            MAXULONG_PTR,
                            PageDirectory,
                            FALSE);
        InitProcess->QuantumReset = MAXCHAR;
    }

    KeInitializeThread(InitProcess,
                       InitThread,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       IdleStack);

    InitThread->NextProcessor = Number;
    InitThread->Priority = HIGH_PRIORITY;
    InitThread->State = Running;
    InitThread->Affinity = (KAFFINITY)1 << Number;
    InitThread->WaitIrql = DISPATCH_LEVEL;
    InitProcess->ActiveProcessors = (KAFFINITY)1 << Number;

    ((PETHREAD)InitThread)->ThreadsProcess = (PEPROCESS)InitProcess;

    Prcb->CurrentThread = InitThread;
    Prcb->NextThread = NULL;
    Prcb->IdleThread = InitThread;
    Prcb->MultiThreadProcessorSet = Prcb->SetMember;
    Prcb->MultiThreadSetMaster = Prcb;

    ExpInitializeExecutive(Number, LoaderBlock);

    if (Number == 0)
    {
        KiTimeIncrementReciprocal =
            KiComputeReciprocal(KeMaximumIncrement, &KiTimeIncrementShiftCount);

        Prcb->MaximumDpcQueueDepth = KiMaximumDpcQueueDepth;
        Prcb->MinimumDpcRate = KiMinimumDpcRate;
        Prcb->AdjustDpcThreshold = KiAdjustDpcThreshold;
    }

    KfRaiseIrql(DISPATCH_LEVEL);

    KeSetPriorityThread(InitThread, 0);

    KiAcquirePrcbLock(Prcb);
    if (!Prcb->NextThread)
    {
        KiIdleSummary |= (KAFFINITY)1 << Number;
    }
    KiReleasePrcbLock(Prcb);

    KfRaiseIrql(HIGH_LEVEL);
    LoaderBlock->Prcb = 0;
}
