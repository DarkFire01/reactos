

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

/* End of bad hack ********************/

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

    /* Set pointers to ourselves */
    Pcr->Self = (PKPCR)Pcr;
    Pcr->CurrentPrcb = &Pcr->Prcb;

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

    /* Set the PRCB for this Processor */
    KiProcessorBlock[ProcessorNumber] = Pcr->CurrentPrcb;

    /* Start us out at PASSIVE_LEVEL */
    Pcr->CurrentIrql = PASSIVE_LEVEL;

    /* Set default stall factor */
    Pcr->StallScaleFactor = 50;

}

CODE_SEG("INIT")
DECLSPEC_NORETURN
VOID
NTAPI
KiSystemStartup(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    DbgPrintEarly("KiSystemStartup: Entry\n");
    DbgPrintEarly("ReactOS ARM64 Port\n");
    ULONG Cpu;
    PKIPCR Pcr = (PKIPCR)KeGetPcr();
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

    DbgPrintEarly("Going into first ListHead\n");
    /* Clean the APC List Head */
    InitializeListHead(&InitialThread->ApcState.ApcListHead[KernelMode]);
    DbgPrintEarly("return from ListHead\n");

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
        if (KdPollBreakIn()) DbgBreakPointWithStatus(DBG_STATUS_CONTROL_C);
    }

    /* Raise to HIGH_LEVEL */
    KfRaiseIrql(HIGH_LEVEL);
    DPRINT1("That's all for now folks! - TODO: KiInitializeKernel for ARM64\n");
    __debugbreak();
    for(;;)
    {

    }
}
