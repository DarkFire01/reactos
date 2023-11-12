/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     i386 AP spinup setup
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 *              Copyright 2021 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern PPROCESSOR_IDENTITY HalpProcessorIdentity;
extern PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
extern PVOID HalpLowStub;

// The data necessary for a boot (stored inside HalpLowStub)
extern PVOID HalpAPEntry16;
extern PVOID HalpAPEntryData;
extern PVOID HalpAPEntry32;
extern PVOID HalpAPEntry16End;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

ULONG HalpStartedProcessorCount = 1;

typedef struct _AP_ENTRY_DATA
{
    ULONG Jump32Offset;
    ULONG Jump32Segment;
    PVOID SelfPtr;
    PFN_NUMBER PageTableRoot;
    PKPROCESSOR_STATE ProcessorState;
    KDESCRIPTOR Gdtr;
} AP_ENTRY_DATA, *PAP_ENTRY_DATA;

typedef struct _AP_SETUP_STACK
{
    PVOID ReturnAddr;
    PVOID KxLoaderBlock;
} AP_SETUP_STACK, *PAP_SETUP_STACK;

/* FUNCTIONS *****************************************************************/

static
VOID
HalpMapAddressFlat(
    _Inout_ PMMPDE PageDirectory,
    _In_ PVOID VirtAddress,
    _In_ PVOID TargetVirtAddress)
{
    if (TargetVirtAddress == NULL)
        TargetVirtAddress = VirtAddress;

    PMMPDE currentPde;

    currentPde = &PageDirectory[MiAddressToPdeOffset(TargetVirtAddress)];

    // Allocate a Page Table if there is no one for this address
    if (currentPde->u.Long == 0)
    {
        PMMPTE pageTable = ExAllocatePoolZero(NonPagedPoolMustSucceed , PAGE_SIZE, TAG_HAL);

        currentPde->u.Hard.PageFrameNumber = MmGetPhysicalAddress(pageTable).QuadPart >> PAGE_SHIFT;
        currentPde->u.Hard.Valid = TRUE;
        currentPde->u.Hard.Write = TRUE;
    }
    
    // Map the Page Table so we can add our VirtAddress there (hack around I/O memory mapper for that)
    PHYSICAL_ADDRESS b = {.QuadPart = (ULONG_PTR)currentPde->u.Hard.PageFrameNumber << PAGE_SHIFT};

    PMMPTE pageTable = MmMapIoSpace(b, PAGE_SIZE, MmCached);

    PMMPTE currentPte = &pageTable[MiAddressToPteOffset(TargetVirtAddress)];
    currentPte->u.Hard.PageFrameNumber = MmGetPhysicalAddress(VirtAddress).QuadPart >> PAGE_SHIFT;
    currentPte->u.Hard.Valid = TRUE;
    currentPte->u.Hard.Write = TRUE;

    MmUnmapIoSpace(pageTable, PAGE_SIZE);

    DPRINT("Map %p -> %p, PDE %u PTE %u\n",
           TargetVirtAddress,
           MmGetPhysicalAddress(VirtAddress).LowPart,
           MiAddressToPdeOffset(TargetVirtAddress),
           MiAddressToPteOffset(TargetVirtAddress));
}

static
PHYSICAL_ADDRESS
HalpSetupTemporaryMappings(
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    PHYSICAL_ADDRESS Cr3PhysicalAddress;
    Cr3PhysicalAddress.QuadPart = ProcessorState->SpecialRegisters.Cr3;

    PMMPDE pageDirectory = MmMapIoSpace(Cr3PhysicalAddress, PAGE_SIZE, MmCached);
    ASSERT(pageDirectory);

    // Map the low stub
    HalpMapAddressFlat(pageDirectory, HalpLowStub, (PVOID)(ULONG_PTR)HalpLowStubPhysicalAddress.QuadPart);
    HalpMapAddressFlat(pageDirectory, HalpLowStub, NULL);

    return MmGetPhysicalAddress(pageDirectory);
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    
    /* Write KeLoaderBlock into Stack */
    ProcessorState->ContextFrame.Esp = (ULONG)((ULONG_PTR)ProcessorState->ContextFrame.Esp - sizeof(AP_SETUP_STACK));
    PAP_SETUP_STACK ApStack = (PAP_SETUP_STACK)ProcessorState->ContextFrame.Esp;
    ApStack->KxLoaderBlock = KeLoaderBlock;
    ApStack->ReturnAddr = NULL;

    if (HalpStartedProcessorCount == HalpApicInfoTable.ProcessorCount)
        return FALSE;

    // Put the bootstrap code into low memory
    RtlCopyMemory(HalpLowStub, &HalpAPEntry16,  ((ULONG_PTR)&HalpAPEntry16End - (ULONG_PTR)&HalpAPEntry16));

    // Get a pointer to apEntryData 
    PAP_ENTRY_DATA apEntryData = (PVOID)((ULONG_PTR)HalpLowStub + ((ULONG_PTR)&HalpAPEntryData - (ULONG_PTR)&HalpAPEntry16));

    *apEntryData = (AP_ENTRY_DATA){
        .Jump32Offset = (ULONG)&HalpAPEntry32,
        .Jump32Segment = (ULONG)ProcessorState->ContextFrame.SegCs,
        .SelfPtr = (PVOID)apEntryData,
        .PageTableRoot = HalpSetupTemporaryMappings(ProcessorState).QuadPart,
        .ProcessorState = ProcessorState,
        .Gdtr = ProcessorState->SpecialRegisters.Gdtr,
    };

    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;

    return TRUE;
}

