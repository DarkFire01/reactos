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

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY64 Idt[256];
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
    ULONG_PTR EFlags;
    ULONG WaitUs;

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

        /* Copy the GDT and IDT */
        PKIPCR CurrentPcr = (PKIPCR)KeGetPcr();
        RtlCopyMemory(APInfo->Gdt, CurrentPcr->GdtBase, sizeof(APInfo->Gdt));
        RtlCopyMemory(APInfo->Idt, CurrentPcr->IdtBase, sizeof(APInfo->Idt));

        /* Initialize PCR and TSS */
        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            APInfo->Gdt,
                                            APInfo->Idt,
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
        ProcessorState->SpecialRegisters.Idtr.Base = APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = sizeof(APInfo->Idt) - 1;

        /* Set up parameters for entry point */
        ProcessorState->ContextFrame.Rsp = (ULONG64)KernelStack - 5 * 8;
        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartup;
        ProcessorState->ContextFrame.Rcx = (ULONG64)KeLoaderBlock;

        /* Set up the loader-block */
        KeLoaderBlock->KernelStack = (ULONG64)KernelStack;
        KeLoaderBlock->Thread = (ULONG64)&APInfo->Thread;
        KeLoaderBlock->Process = (ULONG64)PsIdleProcess;
        KeLoaderBlock->Prcb = (ULONG64)&APInfo->Pcr.Prcb;

        /* Start the next processor.
         * Disable interrupts across the INIT-SIPI-SIPI sequence AND the wait for
         * the AP to come up: otherwise the clock ISR (now 1024Hz, and it takes
         * the CMOS spinlock) can fire on the BSP mid-sequence and skew the tight
         * APIC timing, intermittently losing the AP. KeStallExecutionProcessor
         * (used inside) is a TSC busy-wait and works fine with interrupts off. */
        DPRINT1("Attempting to start processor #%u\n", ProcessorCount);
        EFlags = __readeflags();
        _disable();

        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            __writeeflags(EFlags);
            DPRINT1("Failed to start processor #%u\n", ProcessorCount);
            break;
        }

        /* Wait for it to start, with a timeout so a processor that fails to come
         * up can NOT hang boot forever (the AP clears LoaderBlock->Prcb when up). */
        WaitUs = 0;
        while (KeLoaderBlock->Prcb)
        {
            KeMemoryBarrier();
            KeStallExecutionProcessor(50);
            WaitUs += 50;
            if (WaitUs > 3000000) /* 3 seconds */
            {
                DPRINT1("Processor #%u did not come up; continuing without it\n",
                        ProcessorCount);
                break;
            }
        }

        __writeeflags(EFlags);

        /* If it never came up, stop launching further APs (the shared loader
         * block is still claimed by the stuck one). The system boots with the
         * processors that did start. */
        if (KeLoaderBlock->Prcb)
            break;
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
