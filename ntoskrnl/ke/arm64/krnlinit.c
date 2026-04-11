

#include <ntoskrnl.h>
#include <internal/ke.h>
#include <internal/arm64/intrin_i.h>
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
    ULONG64 Midr;

    KeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;

    /* Read Main ID Register: bits[19:16]=Architecture, bits[15:4]=PartNum,
     * bits[23:20]=Variant, bits[3:0]=Revision */
    Midr = KeArm64MidrGet();

    /* KeProcessorLevel = Part Number (12 bits, [15:4]) */
    KeProcessorLevel = (USHORT)((Midr >> 4) & 0xFFF);

    /* KeProcessorRevision: high nibble = Variant [23:20], low nibble = Revision [3:0]
     * Packed as a byte pair matching x86 convention (Stepping | (Model << 4)) */
    KeProcessorRevision = (USHORT)(((Midr >> 16) & 0xF0) | (Midr & 0xF));
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
        ULONG64 Isar0, Pfr0;
        ULONG FeatureBits = 0;

        /* Detect ARM64 ISA extension features for KeFeatureBits.
         * ID_AA64ISAR0_EL1 encodes AES/SHA1/SHA2/CRC32/Atomic etc.
         * ID_AA64PFR0_EL1  encodes FP and AdvSIMD availability.
         * ID_AA64ISAR1_EL1 encodes JSCVT, LRCPC etc. */
        Isar0 = KeArm64IdAa64Isar0Get();
        Pfr0  = KeArm64IdAa64Pfr0Get();

        /* ARMv8-A mandates FP and AdvSIMD; value 0xF in the field = absent */
        if (((Pfr0 >> 16) & 0xF) != 0xF)
            FeatureBits |= KF_ARM_VFP;
        if (((Pfr0 >> 20) & 0xF) != 0xF)
            FeatureBits |= KF_ARM_NEON;

        /* AES: ID_AA64ISAR0_EL1[7:4] != 0 */
        if (((Isar0 >> 4) & 0xF) != 0)
            FeatureBits |= KF_ARM_AES;

        /* PMULL: AES field >= 2 indicates PMULL support */
        if (((Isar0 >> 4) & 0xF) >= 2)
            FeatureBits |= KF_ARM_PMULL;

        /* SHA1: ID_AA64ISAR0_EL1[11:8] != 0 */
        if (((Isar0 >> 8) & 0xF) != 0)
            FeatureBits |= KF_ARM_SHA1;

        /* SHA256: ID_AA64ISAR0_EL1[15:12] != 0 */
        if (((Isar0 >> 12) & 0xF) != 0)
            FeatureBits |= KF_ARM_SHA256;

        /* CRC32: ID_AA64ISAR0_EL1[19:16] != 0 */
        if (((Isar0 >> 16) & 0xF) != 0)
            FeatureBits |= KF_ARM_CRC32;

        /* LSE atomics: ID_AA64ISAR0_EL1[23:20] != 0 */
        if (((Isar0 >> 20) & 0xF) != 0)
            FeatureBits |= KF_ARM_ATOMICS;

        KeFeatureBits = FeatureBits;

        /* ARM64 has hardware-maintained cache coherency between CPUs; no
         * need for software DMA coherency workarounds. */
        KiDmaIoCoherency = 0;

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
