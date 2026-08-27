/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY Gdt[128];
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
} APINFO, *PAPINFO;

typedef struct _AP_SETUP_STACK
{
    PVOID ReturnAddr;
    PVOID KxLoaderBlock;
} AP_SETUP_STACK, *PAP_SETUP_STACK; // Note: expected layout only for 32-bit x86

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack, DPCStack;
    ULONG_PTR StackTop;
    PAPINFO APInfo;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;

    /* NOTE: NT6+ HAL exports HalEnumerateProcessors() and
     * HalQueryMaximumProcessorCount() that help determining
     * the number of detected processors on the system. */
    MaximumProcessors = KeMaximumProcessors;

    /* Limit the number of processors we can start at run-time */
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    /* Limit also the number of processors we can start during boot-time */
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    // TODO: Support processor nodes

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DPCStack = NULL;

        // Allocate structures for a new CPU.
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DPCStack = MmCreateKernelStack(FALSE, 0);
        if (!DPCStack)
            break;

        // Prepare descriptor tables
        KDESCRIPTOR bspGdt, bspIdt;
        __sgdt(&bspGdt.Limit);
        __sidt(&bspIdt.Limit);

        /*
         * Share the boot processor's IDT rather than taking a copy.
         *
         * KeRegisterInterruptHandler() writes KeGetPcr()->IDT, so it only
         * updates the table of whichever processor happens to run it. A private
         * copy is a snapshot of what was registered by the time this processor
         * was created, and everything connected afterwards - which is every
         * driver ISR, since KeStartAllProcessors() runs in Phase 1 before the
         * device tree is walked - is missing from it. HalInitializeProcessor()
         * then puts this processor into HalpDefaultInterruptAffinity, so
         * interrupts do get routed here and are dispatched through stale
         * entries.
         *
         * One table for everyone is what NT does on x86 and costs nothing: the
         * double fault and NMI entries are task gates naming KGDT_DF_TSS and
         * KGDT_NMI_TSS, and a selector resolves through the GDT of whichever
         * processor took the fault, so each still lands in its own TSS.
         */
        // Initalize a new PCR for the specific AP
        KiInitializePcr(ProcessorCount,
                        &APInfo->Pcr,
                        (PKIDTENTRY)bspIdt.Base,
                        &APInfo->Gdt[0],
                        &APInfo->Tss,
                        (PKTHREAD)&APInfo->Thread,
                        DPCStack);

        RtlCopyMemory(&APInfo->Gdt, (PVOID)bspGdt.Base, bspGdt.Limit + 1);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_R0_PCR), (ULONG_PTR)&APInfo->Pcr);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_DF_TSS), (ULONG_PTR)&APInfo->TssDoubleFault);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_NMI_TSS), (ULONG_PTR)&APInfo->TssNMI);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS), (ULONG_PTR)&APInfo->Tss);
        // Clear TSS Busy flag (aka set the type to "TSS (Available)")
        KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS)->HighWord.Bits.Type = I386_TSS;

        /*
         * Build the double fault and NMI task state segments the way
         * Ki386InitializeTss() does for the boot processor.
         *
         * Only Esp0 and Esp used to be filled in, which leaves CR3, Eip and
         * every selector zero. A task gate loads all of those from the TSS, so
         * the first double fault or NMI on this processor switched to a task
         * with no address space and no entry point and the machine simply
         * stopped - no bugcheck, no output, just a busy TSS and a halted
         * processor. Both stack pointers also addressed the bottom of the
         * stack rather than the top, so the handler would have run off the end
         * of it immediately even had it started.
         */
        StackTop = (ULONG_PTR)&APInfo->NMIStackData[sizeof(APInfo->NMIStackData)];

        KiInitializeTSS(&APInfo->TssDoubleFault);
        APInfo->TssDoubleFault.CR3 = __readcr3();
        APInfo->TssDoubleFault.Esp0 = StackTop;
        APInfo->TssDoubleFault.Esp = StackTop;
        APInfo->TssDoubleFault.Eip = PtrToUlong(KiTrap08);
        APInfo->TssDoubleFault.Cs = KGDT_R0_CODE;
        APInfo->TssDoubleFault.Fs = KGDT_R0_PCR;
        APInfo->TssDoubleFault.Ss = Ke386GetSs();
        APInfo->TssDoubleFault.Es = KGDT_R3_DATA | RPL_MASK;
        APInfo->TssDoubleFault.Ds = KGDT_R3_DATA | RPL_MASK;

        KiInitializeTSS(&APInfo->TssNMI);
        APInfo->TssNMI.CR3 = __readcr3();
        APInfo->TssNMI.Esp0 = StackTop;
        APInfo->TssNMI.Esp = StackTop;
        APInfo->TssNMI.Eip = PtrToUlong(KiTrap02);
        APInfo->TssNMI.Cs = KGDT_R0_CODE;
        APInfo->TssNMI.Fs = KGDT_R0_PCR;
        APInfo->TssNMI.Ss = Ke386GetSs();
        APInfo->TssNMI.Es = KGDT_R3_DATA | RPL_MASK;
        APInfo->TssNMI.Ds = KGDT_R3_DATA | RPL_MASK;

        // Fill the processor state
        PKPROCESSOR_STATE ProcessorState = &APInfo->Pcr.Prcb->ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        ProcessorState->ContextFrame.SegCs = KGDT_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegEs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegSs = KGDT_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT_R0_PCR;

        ProcessorState->SpecialRegisters.Gdtr.Base = (ULONG_PTR)APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = bspIdt.Base;
        ProcessorState->SpecialRegisters.Idtr.Limit = bspIdt.Limit;

        ProcessorState->SpecialRegisters.Tr = KGDT_TSS;

        ProcessorState->ContextFrame.Esp = (ULONG_PTR)KernelStack;
        ProcessorState->ContextFrame.Eip = (ULONG_PTR)KiSystemStartup;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        ProcessorState->ContextFrame.Esp = (ULONG)((ULONG_PTR)ProcessorState->ContextFrame.Esp - sizeof(AP_SETUP_STACK));
        PAP_SETUP_STACK ApStack = (PAP_SETUP_STACK)ProcessorState->ContextFrame.Esp;
        ApStack->KxLoaderBlock = KeLoaderBlock;
        ApStack->ReturnAddr = NULL;

        // Update the LOADER_PARAMETER_BLOCK structure for the new processor
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.Prcb;
        /*
         * Hand over the thread itself. This used to be
         * &APInfo->Pcr.Prcb->IdleThread, which is the address of the PRCB's
         * IdleThread pointer rather than any thread, so KiSystemStartup() and
         * KiInitializeKernel() built the new processor's idle KTHREAD on top of
         * its own PRCB. APINFO carries storage for it, and KiInitializePcr()
         * above is already given the same pointer.
         */
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;

        // Start the CPU
        DPRINT("Attempting to Start a CPU with number: %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            break;
        }

        // And wait for it to start
        while (KeLoaderBlock->Prcb != 0)
        {
            //TODO: Add a time out so we don't wait forever
            KeMemoryBarrier();
            YieldProcessor();
        }
    }

    // The last CPU didn't start - clean the data
    ProcessorCount--;

    if (APInfo)
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
    if (KernelStack)
        MmDeleteKernelStack(KernelStack, FALSE);
    if (DPCStack)
        MmDeleteKernelStack(DPCStack, FALSE);

    DPRINT1("KeStartAllProcessors: Successful AP startup count is %lu\n", ProcessorCount);
}
