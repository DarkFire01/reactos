

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

#define ARM64_QEMU_VIRT_UART_BASE 0x09000000ULL
#define ARM64_QEMU_VIRT_UART_DR   0x00ULL
#define ARM64_QEMU_VIRT_UART_FR   0x18ULL
#define ARM64_QEMU_VIRT_UART_TXFF 0x20UL

VOID
NTAPI
KdPortPutByteEx(IN PCPPORT PortInformation,
                IN UCHAR ByteToSend)
{
    volatile ULONG *FrReg;
    volatile ULONG *DrReg;

    UNREFERENCED_PARAMETER(PortInformation);

    FrReg = (volatile ULONG *)(ULONG_PTR)(ARM64_QEMU_VIRT_UART_BASE + ARM64_QEMU_VIRT_UART_FR);
    DrReg = (volatile ULONG *)(ULONG_PTR)(ARM64_QEMU_VIRT_UART_BASE + ARM64_QEMU_VIRT_UART_DR);

    while ((*FrReg & ARM64_QEMU_VIRT_UART_TXFF) != 0)
    {
    }

    *DrReg = ByteToSend;
}

ULONG
DbgPrintEarly(const char *fmt, ...)
{
    va_list args;
    unsigned int Count;
    CHAR Buffer[256];
    PCHAR String = Buffer;

    va_start(args, fmt);
    Count = vsprintf(Buffer, fmt, args);
    va_end(args);

    while (*String != 0)
    {
        if (*String == '\n')
        {
            KdPortPutByteEx(NULL, '\r');
        }

        KdPortPutByteEx(NULL, *String);
        ++String;
    }

    return Count;
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
VOID
NTAPI
KiInitializeExceptionHandling(VOID);

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

    /*
     * ARM64: write TPIDR_EL1 with this processor's PCR address NOW.
     * Every per-CPU accessor (KeGetCurrentIrql, KeGetCurrentPrcb,
     * KeGetCurrentThread, …) reads TPIDR_EL1 via KeGetPcr().  Without this
     * the register contains whatever UEFI left — typically 0 — so all
     * per-CPU reads return garbage and WinDbg reports "thread 0x100".
     */
    KeArm64TpidrEl1Set((ULONG64)Pcr);

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

    /*
     * KD state packets use KeGetCurrentThread(), which comes from PRCB.
     * Initialize it early to avoid bogus thread IDs during first breakpoints.
     */
    Prcb->CurrentThread = Thread;

    KiProcessorBlock[Cpu] = Prcb;
    DbgPrintEarly("ARM64 boot CPU %lu initialized\n", Cpu);
    Thread->ApcState.Process = (PKPROCESS)LoaderBlock->Process;

    HalInitializeProcessor(Cpu, KeLoaderBlock);
    DbgPrintEarly("ARM64 boot CPU %lu after HalInitializeProcessor\n", Cpu);
    KeActiveProcessors |= Prcb->SetMember;
    KeNumberProcessors++;
    KiInitializeExceptionHandling();
    if (Cpu == 0)
    {
        /* Initialize the module list (ntos, hal, kdcom) before KD init. */
        KiInitModuleList(KeLoaderBlock);

        KdInitSystem(0, KeLoaderBlock);

        if (KdPollBreakIn()) DbgBreakPointWithStatus(DBG_STATUS_CONTROL_C);
    }

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
