

#include <ntoskrnl.h>
#include <cportlib/cportlib.h>
#include <internal/ke.h>
#define NDEBUG
#include <debug.h>

#ifndef PRCB_MINOR_VERSION
#define PRCB_MINOR_VERSION 1
#endif

static KIPCR KiInitialPcr;
static LDR_DATA_TABLE_ENTRY LdrCoreEntries[3];

static
VOID
KiInitializeP0BootStructures(IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    RtlZeroMemory(&KiInitialPcr, sizeof(KiInitialPcr));

    LoaderBlock->Thread = (ULONG_PTR)&KiInitialThread;
    LoaderBlock->Process = (ULONG_PTR)&KiInitialProcess.Pcb;
    LoaderBlock->Prcb = (ULONG_PTR)&KiInitialPcr.Prcb;
}

static
VOID
KiInitModuleList(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY Entry;
    ULONG i;

    InitializeListHead(&PsLoadedModuleList);

    for (Entry = LoaderBlock->LoadOrderListHead.Flink, i = 0;
         Entry != &LoaderBlock->LoadOrderListHead && i < RTL_NUMBER_OF(LdrCoreEntries);
         Entry = Entry->Flink, i++)
    {
        LdrEntry = CONTAINING_RECORD(Entry,
                                     LDR_DATA_TABLE_ENTRY,
                                     InLoadOrderLinks);

        LdrCoreEntries[i] = *LdrEntry;
        InsertTailList(&PsLoadedModuleList, &LdrCoreEntries[i].InLoadOrderLinks);
    }
}

CODE_SEG("INIT")
VOID
NTAPI
KiInitializeKernel(IN PKPROCESS InitProcess,
                   IN PKTHREAD InitThread,
                   IN PVOID IdleStack,
                   IN PKPRCB Prcb,
                   IN CCHAR Number,
                   IN PLOADER_PARAMETER_BLOCK LoaderBlock);

CODE_SEG("INIT")
VOID
NTAPI
KiInitializeMachineType(VOID);

DECLSPEC_NORETURN
CODE_SEG("INIT")
VOID
NTAPI
KiSystemStartup(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG Cpu;
    PKIPCR Pcr;
    PKPRCB Prcb;
    PKTHREAD Thread;

    Cpu = KeNumberProcessors;
    if (Cpu == 0)
    {
        KeLoaderBlock = LoaderBlock;
        KiInitializeP0BootStructures(LoaderBlock);
    }

    KiInitializeMachineType();

    Thread = (PKTHREAD)LoaderBlock->Thread;
    Pcr = CONTAINING_RECORD((PKPRCB)(ULONG_PTR)LoaderBlock->Prcb, KIPCR, Prcb);
    Prcb = &Pcr->Prcb;
    /* Bootstrap the PCR/PRCB identity and basic versioning fields. */
    Pcr->Self = (PKPCR)Pcr;
    Pcr->MajorVersion = PCR_MAJOR_VERSION;
    Pcr->MinorVersion = PCR_MINOR_VERSION;
    Pcr->CurrentIrql = PASSIVE_LEVEL;

    Prcb->MajorVersion = PRCB_MAJOR_VERSION;
    Prcb->MinorVersion = PRCB_MINOR_VERSION;
    Prcb->BuildType = 0;
#ifndef CONFIG_SMP
    Prcb->BuildType |= PRCB_BUILD_UNIPROCESSOR;
#endif
#if DBG
    Prcb->BuildType |= PRCB_BUILD_DEBUG;
#endif

    Prcb->Number = (UCHAR)Cpu;
    Prcb->SetMember = 1ULL << Cpu;
    Prcb->MultiThreadProcessorSet = Prcb->SetMember;
    Prcb->MultiThreadSetMaster = Prcb;

    KiProcessorBlock[Cpu] = Prcb;
    Thread->ApcState.Process = (PKPROCESS)LoaderBlock->Process;

    Prcb->ParentNode = KeNodeBlock[0];
    Prcb->ParentNode->ProcessorMask |= Prcb->SetMember;

    PoInitializePrcb(Prcb);
    KiSaveProcessorControlState(&Prcb->ProcessorState);

    Prcb->CurrentThread = Thread;
    Prcb->NextThread = NULL;
    Prcb->IdleThread = Thread;


    HalInitializeProcessor(Cpu, KeLoaderBlock);

    KeActiveProcessors |= Prcb->SetMember;
    KeNumberProcessors++;

    if (Cpu == 0)
    {
        /* Initialize the module list (ntos, hal, kdcom) before KD init. */
        KiInitModuleList(KeLoaderBlock);

        KdInitSystem(0, KeLoaderBlock);
        DPRINT1("Processor %u is in KiSystemStartup\n", Cpu);
        if (KdPollBreakIn())
        {
            DPRINT1("Break into debugger on processor %u\n", Cpu);
            DbgBreakPointWithStatus(DBG_STATUS_CONTROL_C);
        }
    }
    DPRINT1("incrementing IRQL\n");
    KfRaiseIrql(HIGH_LEVEL);

    KiInitializeKernel((PKPROCESS)LoaderBlock->Process,
                       Thread,
                       (PVOID)LoaderBlock->KernelStack,
                       Prcb,
                       (CCHAR)Cpu,
                       KeLoaderBlock);

    Thread = KeGetCurrentThread();
    Thread->Priority = 0;

    _enable();
    KfLowerIrql(DISPATCH_LEVEL);

    Thread->WaitIrql = DISPATCH_LEVEL;

    KiIdleLoop();
}
