

#include <ntoskrnl.h>
#include <cportlib/cportlib.h>
#include <internal/ke.h>
#define NDEBUG
#include <debug.h>

static LDR_DATA_TABLE_ENTRY LdrCoreEntries[3];

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

static
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

DECLSPEC_NORETURN
CODE_SEG("INIT")
VOID
NTAPI
KiSystemStartup(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG Cpu;
    PKPRCB Prcb;
    PKTHREAD Thread;

    DbgPrintEarly("ARM64 KiSystemStartup LoaderBlock=%p\n", LoaderBlock);

    KeLoaderBlock = LoaderBlock;
    Cpu = KeNumberProcessors;
    KiInitializeMachineType();

    Thread = (PKTHREAD)LoaderBlock->Thread;
    Prcb = KeGetCurrentPrcb();

    Prcb->Number = (UCHAR)Cpu;
    Prcb->SetMember = 1ULL << Cpu;

    HalInitializeProcessor(Cpu, KeLoaderBlock);

    KeActiveProcessors |= Prcb->SetMember;
    KeNumberProcessors++;

    if (Cpu == 0)
    {
        /* Initialize the module list (ntos, hal, kdcom) before KD init. */
        KiInitModuleList(KeLoaderBlock);

        KdInitSystem(0, KeLoaderBlock);
        DbgPrintEarly("ARM64 boot CPU post KdInitSystem\n");

        if (KdPollBreakIn())
        {
            DbgBreakPointWithStatus(DBG_STATUS_CONTROL_C);
        }
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
