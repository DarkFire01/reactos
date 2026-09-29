/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/*
 * The interrupt table is missing from here on purpose. Every processor runs on
 * the one KiIdt: KeRegisterInterruptHandler only ever writes the table of the
 * processor it is called on, so a private copy would stop at whatever was
 * registered when this processor started and never see a vector added later.
 * Which is every device interrupt, and the one the hypervisor signals on.
 *
 * Nothing in an entry is per processor. The stack a fault is taken on is named
 * by an index into the task state, and each processor has its own of those.
 */
typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY64 Gdt[128];
    //DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS64 Tss;
    //KTSS64 TssDoubleFault;
    //KTSS64 TssNMI;
} APINFO, *PAPINFO;

VOID
NTAPI
KiSaveProcessorControlState(OUT PKPROCESSOR_STATE ProcessorState);

/* How long a processor is given to arrive before it is written off */
#define KE_START_PROCESSOR_SPINS 0x10000000

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack, DpcStack, DoubleFaultStack, NmiStack;
    ULONG ProcessorCount = 0;
    PAPINFO APInfo;
    PKPROCESSOR_STATE ProcessorState;
    ULONG64 Spins;

    //__debugbreak();
    //if (KeNumberProcessors <= 2) return;

    while (TRUE)
    {
        ProcessorCount++;
        KernelStack = NULL;
        DpcStack = NULL;
        DoubleFaultStack = NULL;
        NmiStack = NULL;

        /* Allocate structures for a new CPU. */
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(APINFO), '  eK');
        if (APInfo == NULL)
        {
            DPRINT1("Failed to allocate APInfo\n");
            break;
        }
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        /* Allocate a kernel stack */
        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (KernelStack == NULL)
        {
            DPRINT1("Failed to allocate kernel stack\n");
            break;
        }

        /* Allocate a DPC stack */
        DpcStack = MmCreateKernelStack(FALSE, 0);
        if (DpcStack == NULL)
        {
            DPRINT1("Failed to allocate DPC stack\n");
            break;
        }

        /* Allocate a double-fault stack */
        DoubleFaultStack = MmCreateKernelStack(FALSE, 0);
        if (DoubleFaultStack == NULL)
        {
            DPRINT1("Failed to allocate double-fault stack\n");
            break;
        }

        /* Allocate an NMI stack */
        NmiStack = MmCreateKernelStack(FALSE, 0);
        if (NmiStack == NULL)
        {
            DPRINT1("Failed to allocate NMI stack\n");
            break;
        }

        /* Zero the APInfo */
        RtlZeroMemory(APInfo, sizeof(APINFO));

        /* Copy the GDT, which does hold this processor's own descriptors */
        PKIPCR CurrentPcr = (PKIPCR)KeGetPcr();
        RtlCopyMemory(APInfo->Gdt, CurrentPcr->GdtBase, sizeof(APInfo->Gdt));

        /* Initialize PCR and TSS */
        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            APInfo->Gdt,
                                            KiIdt,
                                            &APInfo->Tss,
                                            &APInfo->Thread.Tcb,
                                            KernelStack,
                                            DpcStack,
                                            DoubleFaultStack,
                                            NmiStack);

        /* Set up the processor state */
        ProcessorState = &APInfo->Pcr.Prcb.ProcessorState;
        KiSaveProcessorControlState(ProcessorState);

        /* Set up GDT and IDT in the ProcessorState */
        ProcessorState->SpecialRegisters.Gdtr.Base = APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = KiIdt;
        ProcessorState->SpecialRegisters.Idtr.Limit = KiIdtDescriptor.Limit;

        /* Set up parameters for entry point */
        ProcessorState->ContextFrame.Rsp = (ULONG64)KernelStack - 5 * 8;
        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartup;
        ProcessorState->ContextFrame.Rcx = (ULONG64)KeLoaderBlock;

        /* Set up the loader-block */
        KeLoaderBlock->KernelStack = (ULONG64)KernelStack;
        KeLoaderBlock->Thread = (ULONG64)&APInfo->Thread;
        KeLoaderBlock->Process = (ULONG64)PsIdleProcess;
        KeLoaderBlock->Prcb = (ULONG64)&APInfo->Pcr.Prcb;

        /* Start the next processor */
        DPRINT1("Attempting to start processor #%u\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            DPRINT1("Failed to start processor #%u\n", ProcessorCount);
            break;
        }

        /* Wait for it to start, which it says by clearing the block */
        Spins = 0;
        while (KeLoaderBlock->Prcb)
        {
            KeMemoryBarrier();
            YieldProcessor();

            if (++Spins == KE_START_PROCESSOR_SPINS)
            {
                /*
                 * The controller took the startup and the processor never
                 * arrived. Going on without it would leave a processor the
                 * rest of the system counts as active and waits on forever,
                 * so this one is given up on and the count stops here.
                 */
                DPRINT1("Processor #%u took the startup and never arrived\n",
                        ProcessorCount);
                break;
            }
        }

        if (KeLoaderBlock->Prcb)
        {
            /*
             * Everything handed to it stays where it is. A processor that is
             * only late still comes up on this stack and in this block, and
             * giving either back now would put them under something else.
             */
            KeLoaderBlock->Prcb = 0;
            KernelStack = NULL;
            DpcStack = NULL;
            DoubleFaultStack = NULL;
            NmiStack = NULL;
            APInfo = NULL;
            break;
        }
    }

    if (KernelStack != NULL)
    {
        MmDeleteKernelStack(KernelStack, FALSE);
    }

    if (DpcStack != NULL)
    {
        MmDeleteKernelStack(DpcStack, FALSE);
    }

    if (DoubleFaultStack != NULL)
    {
        MmDeleteKernelStack(DoubleFaultStack, FALSE);
    }

    if (NmiStack != NULL)
    {
        MmDeleteKernelStack(NmiStack, FALSE);
    }

    if (APInfo != NULL)
    {
        ExFreePoolWithTag(APInfo, '  eK');
    }
}
