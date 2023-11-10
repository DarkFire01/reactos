

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS *********************************************************/

/* This is just a small hack for QEMU */

#define QEMUUART 0x09000000
volatile unsigned int * UART0DR = (unsigned int *) QEMUUART;
VOID
NTAPI
QemuWriteByteToUart(UCHAR ByteToSend)
{
    *UART0DR = ByteToSend;
}

ULONG
DbgPrintEarly(const char *fmt, ...)
{
    va_list args;
    unsigned int i;
    char Buffer[1024];
    PCHAR String = Buffer;

    va_start(args, fmt);
    i = vsprintf(Buffer, fmt, args);
    va_end(args);

    /* Output the message */
    while (*String != 0)
    {
        if (*String == '\n')
        {

            QemuWriteByteToUart('\r');
        }
        QemuWriteByteToUart(*String);
        String++;
    }

    return STATUS_SUCCESS;
}


/* FUNCTIONS *****************************************************************/

VOID
NTAPI
KiInitializePcr(IN ULONG ProcessorNumber,
                IN PKIPCR Pcr,
                IN PKTHREAD IdleThread)
{
    DbgPrintEarly("KiInitializePcr: Entry\n");

    /* Set the Current Thread */
    Pcr->Prcb.CurrentThread = IdleThread;
        DbgPrintEarly("Setting IdleThread\n");
    /* Set pointers to ourselves */
    Pcr->Self = (PKPCR)Pcr;
    Pcr->CurrentPrcb = &Pcr->Prcb;
        DbgPrintEarly("Setting version\n");
    /* Set the PCR Version */
    Pcr->MajorVersion = PCR_MAJOR_VERSION;
    Pcr->MinorVersion = PCR_MINOR_VERSION;

    /* Set the PCRB Version */
    Pcr->Prcb.MajorVersion = PRCB_MAJOR_VERSION;
    Pcr->Prcb.MinorVersion = PRCB_MINOR_VERSION;

    /* Set the Build Type */
    Pcr->Prcb.BuildType = 0;
#ifndef CONFIG_SMP
    Pcr->Prcb.BuildType |= PRCB_BUILD_UNIPROCESSOR;
#endif
#if DBG
    Pcr->Prcb.BuildType |= PRCB_BUILD_DEBUG;
#endif

    /* Set the Processor Number and current Processor Mask */
    Pcr->Prcb.Number = (UCHAR)ProcessorNumber;
    Pcr->Prcb.SetMember = 1 << ProcessorNumber;
          DbgPrintEarly("Setting processor block\n");
    /* Set the PRCB for this Processor */
    KiProcessorBlock[ProcessorNumber] = Pcr->CurrentPrcb;

    /* Start us out at PASSIVE_LEVEL */
    Pcr->CurrentIrql = PASSIVE_LEVEL;

    /* Set default stall factor */
    Pcr->StallScaleFactor = 50;

}

#define PROCESSOR_ARCHITECTURE_ARM64            12

VOID
NTAPI
KiInitializeKernel(IN PKPROCESS InitProcess,
                   IN PKTHREAD InitThread,
                   IN PVOID IdleStack,
                   IN PKPRCB Prcb,
                   IN CCHAR Number,
                   IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PKIPCR Pcr = (PKIPCR)LoaderBlock->u.Arm64.PcrPage;
    ULONG PageDirectory[2];

    /* Set the default NX policy (opt-in) */
    //SharedUserData->NXSupportPolicy = NX_SUPPORT_POLICY_OPTIN;
    DPRINT1("Setting up spinlocks\n");
    /* Initialize spinlocks and DPC data */
    KiInitSpinLocks(Prcb, Number);

    /* Set stack pointers */
    //Pcr->InitialStack = IdleStack;
    Pcr->Prcb.SpBase = IdleStack; // ???

    /* Check if this is the Boot CPU */
    if (!Number)
    {
        /* Set DMA coherency */
        KiDmaIoCoherency = 0;

        /* Sweep D-Cache */
        DPRINT1("Setting up Globals\n");
        /* Set boot-level flags */
        KeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
        KeFeatureBits = 0;
        /// FIXME: just a wild guess
#if 0
        /* Set the current MP Master KPRCB to the Boot PRCB */
        Prcb->MultiThreadSetMaster = Prcb;
#endif
        DPRINT1("Lowering IRQL\n\n");
        /* Lower to APC_LEVEL */
        KeLowerIrql(APC_LEVEL);
        DPRINT1("Lowering Irql Success\n");
        /* Initialize portable parts of the OS */
        KiInitSystem();

        DPRINT1("KiInitSystem success\n");
        /* Initialize the Idle Process and the Process Listhead */
        InitializeListHead(&KiProcessListHead);
        PageDirectory[0] = 0;
        PageDirectory[1] = 0;
        KeInitializeProcess(InitProcess,
                            0,
                            0xFFFFFFFF,
                            (PLONG_PTR)PageDirectory,
                            FALSE);
        DPRINT1("Initial Processo success\n");
        InitProcess->QuantumReset = MAXCHAR;
    }
    else
    {
        DPRINT1("SMP for ARM64 Not yet supported\n");
    }
    DPRINT1("Setting up idle thread\n");
    /* Setup the Idle Thread */
    KeInitializeThread(InitProcess,
                       InitThread,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       IdleStack);
    DPRINT1("Setting up idle thread success\n");
    InitThread->NextProcessor = Number;
    InitThread->Priority = HIGH_PRIORITY;
    InitThread->State = Running;
    InitThread->Affinity = 1 << Number;
    InitThread->WaitIrql = DISPATCH_LEVEL;
    InitProcess->ActiveProcessors = 1 << Number;

    /* HACK for MmUpdatePageDir */
    ((PETHREAD)InitThread)->ThreadsProcess = (PEPROCESS)InitProcess;

    /* Set up the thread-related fields in the PRCB */
    Prcb->CurrentThread = InitThread;
    Prcb->NextThread = NULL;
    Prcb->IdleThread = InitThread;
    DPRINT1("Going to executive\n");
    /* Initialize the Kernel Executive */
    ExpInitializeExecutive(Number, LoaderBlock);

    /* Only do this on the boot CPU */
    if (!Number)
    {
        /* Calculate the time reciprocal */
        KiTimeIncrementReciprocal =
            KiComputeReciprocal(KeMaximumIncrement,
                                &KiTimeIncrementShiftCount);

        /* Update DPC Values in case they got updated by the executive */
        Prcb->MaximumDpcQueueDepth = KiMaximumDpcQueueDepth;
        Prcb->MinimumDpcRate = KiMinimumDpcRate;
        Prcb->AdjustDpcThreshold = KiAdjustDpcThreshold;
    }

    /* Raise to Dispatch */
    KfRaiseIrql(DISPATCH_LEVEL);

    /* Set the Idle Priority to 0. This will jump into Phase 1 */
    KeSetPriorityThread(InitThread, 0);

    /* If there's no thread scheduled, put this CPU in the Idle summary */
    KiAcquirePrcbLock(Prcb);
    if (!Prcb->NextThread) KiIdleSummary |= 1 << Number;
    KiReleasePrcbLock(Prcb);

    /* Raise back to HIGH_LEVEL and clear the PRCB for the loader block */
    KfRaiseIrql(HIGH_LEVEL);
    LoaderBlock->Prcb = 0;
}

CODE_SEG("INIT")
DECLSPEC_NORETURN
VOID
NTAPI
KiSystemStartup(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    DbgPrintEarly("ReactOS ARM64 Port\n");
    DbgPrintEarly("KiSystemStartup: Entry\n");
    ULONG Cpu;
    PKIPCR Pcr = (PKIPCR)LoaderBlock->u.Arm64.PcrPage;
    PKTHREAD InitialThread;
    PKPROCESS InitialProcess;

    /* Flush the TLB */
    //KeFlushTb(); //TODO:

    /* Save the loader block and get the current CPU */
    KeLoaderBlock = LoaderBlock;
    Cpu = KeNumberProcessors;

    /* Save the initial thread and process */
    InitialThread = (PKTHREAD)LoaderBlock->Thread;
    InitialProcess = (PKPROCESS)LoaderBlock->Process;

    /* Clean the APC List Head */
    InitializeListHead(&InitialThread->ApcState.ApcListHead[KernelMode]);

    DbgPrintEarly("TODO: KiInitializeMachineType\n");
    /* Initialize the machine type */
   // KiInitializeMachineType(); //TODO: ARM64

    /* Skip initial setup if this isn't the Boot CPU */
    if (Cpu) goto AppCpuInit;

    /* Initialize the PCR */
    RtlZeroMemory(Pcr, PAGE_SIZE);

    //TODO: Add more stuff
    KiInitializePcr(Cpu,
                    Pcr,
                    InitialThread);

    DbgPrintEarly("Return from Init PCR\n");
    /* Set us as the current process */
    InitialThread->ApcState.Process = InitialProcess;

AppCpuInit:
    /* Setup CPU-related fields */
    Pcr->Prcb.Number = Cpu;
    Pcr->Prcb.SetMember = 1 << Cpu;

    DbgPrintEarly("Going into First HAL call\n");
    /* Initialize the Processor with HAL */
    HalInitializeProcessor(Cpu, KeLoaderBlock);
    DbgPrintEarly("returned from first HAL call\n");

    /* Set active processors */
    KeActiveProcessors |= Pcr->Prcb.SetMember;
    KeNumberProcessors++;

    /* Check if this is the boot CPU */
    if (!Cpu)
    {
        DbgPrintEarly("Starting primary debugging system\n");

        /* Initialize debugging system */
        KdInitSystem(0, KeLoaderBlock);

        /* Check for break-in */
       /// if (KdPollBreakIn()) DbgBreakPointWithStatus(DBG_STATUS_CONTROL_C);
    }

    KiInitializeKernel(InitialProcess,
                       InitialThread,
                       NULL,
                       &Pcr->Prcb,
                       Cpu,
                       LoaderBlock);
    DPRINT1("That's all for now folks!\n");
    for(;;)
    {

    }
}
