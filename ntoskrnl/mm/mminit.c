/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/mm/mminit.c
 * PURPOSE:         Memory Manager Initialization
 * PROGRAMMERS:
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "ARM3/miarm.h"

/* GLOBALS *******************************************************************/

BOOLEAN Mm64BitPhysicalAddress = FALSE;
ULONG MmReadClusterSize;
//
// 0 | 1 is on/off paging, 2 is undocumented
//
UCHAR MmDisablePagingExecutive = 1; // Forced to off
PMMPTE MmSharedUserDataPte;
PMMSUPPORT MmKernelAddressSpace;

/* PRIVATE FUNCTIONS *********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
MiDbgDumpAddressSpace(VOID)
{
    //
    // Print the memory layout
    //
    DPRINT1("          0x%p - 0x%p\t%s\n",
            KSEG0_BASE,
            (ULONG_PTR)KSEG0_BASE + MmBootImageSize,
            "Boot Loaded Image");
#ifdef _M_IX86
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmSystemPteSpaceStart,
            (PVOID)((ULONG_PTR)MmSystemPteSpaceStart +
                    (MmNumberOfSystemPtes + 1) * PAGE_SIZE),
            "System PTE Space");
#endif
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmPfnDatabase,
            (ULONG_PTR)MmPfnDatabase + (MxPfnAllocation << PAGE_SHIFT),
            "PFN Database");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmNonPagedPoolStart,
            (ULONG_PTR)MmNonPagedPoolStart + MmSizeOfNonPagedPoolInBytes,
            "ARM3 Non Paged Pool");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MiSystemViewStart,
            (ULONG_PTR)MiSystemViewStart + MmSystemViewSize,
            "System View Space");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmSessionBase,
            MiSessionSpaceEnd,
            "Session Space");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            PTE_BASE, PTE_TOP,
            "Page Tables");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            PDE_BASE, PDE_TOP,
            "Page Directories");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            HYPER_SPACE, HYPER_SPACE_END,
            "Hyperspace");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmSystemCacheStart, MmSystemCacheEnd,
            "System Cache");
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmPagedPoolStart,
            (ULONG_PTR)MmPagedPoolStart + MmSizeOfPagedPoolInBytes,
            "ARM3 Paged Pool");
#ifndef _M_IX86
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmSystemPteSpaceStart,
            (PVOID)((ULONG_PTR)MmSystemPteSpaceStart +
                    ((MmNumberOfSystemPtes + 1) * PAGE_SIZE)),
            "System PTE Space");
#endif
    DPRINT1("          0x%p - 0x%p\t%s\n",
            MmNonPagedPoolExpansionStart, MmNonPagedPoolEnd,
            "Non Paged Pool Expansion PTE Space");
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmInitBsmThread(VOID)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ThreadHandle;

    /* Create the thread */
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  &ObjectAttributes,
                                  NULL,
                                  NULL,
                                  KeBalanceSetManager,
                                  NULL);

    /* Close the handle and return status */
    ZwClose(ThreadHandle);
    return Status;
}

CODE_SEG("INIT")
BOOLEAN
NTAPI
MmInitSystem(IN ULONG Phase,
             IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    extern MMPTE ValidKernelPte;
    PMMPTE PointerPte;
    MMPTE TempPte = ValidKernelPte;
    PFN_NUMBER PageFrameNumber;
    PLIST_ENTRY ListEntry;
    PLDR_DATA_TABLE_ENTRY DataTableEntry;

    /* Initialize the kernel address space */
    ASSERT(Phase == 1);

    MmKernelAddressSpace = &PsIdleProcess->Vm;

    /* Dump the address space */
    MiDbgDumpAddressSpace();

    MmInitSectionImplementation();
    MmInitPagingFile();

    //
    // Create a PTE to double-map the shared data section. We allocate it
    // from paged pool so that we can't fault when trying to touch the PTE
    // itself (to map it), since paged pool addresses will already be mapped
    // by the fault handler.
    //
    MmSharedUserDataPte = ExAllocatePoolWithTag(PagedPool,
                          sizeof(MMPTE),
                          TAG_MM);
    if (!MmSharedUserDataPte) return FALSE;

    //
    // Now get the PTE for shared data, and read the PFN that holds it
    //
    PointerPte = MiAddressToPte((PVOID)KI_USER_SHARED_DATA);
    ASSERT(PointerPte->u.Hard.Valid == 1);
    PageFrameNumber = PFN_FROM_PTE(PointerPte);

    /* Build the PTE and write it */
    MI_MAKE_HARDWARE_PTE_KERNEL(&TempPte,
                                PointerPte,
                                MM_READONLY,
                                PageFrameNumber);
    *MmSharedUserDataPte = TempPte;

    /* Initialize session working set support */
    MiInitializeSessionWsSupport();

    /* Setup session IDs */
    MiInitializeSessionIds();

    /* Setup the memory threshold events */
    if (!MiInitializeMemoryEvents()) return FALSE;

    /* Initialize the balance set manager, it runs the working set manager */
    KeInitializeEvent(&MmWorkingSetManagerEvent, SynchronizationEvent, FALSE);
    MmInitBsmThread();

    /* Start writing modified pages to paging files and mapped files */
    MiInitializeDataFileMaps();
    MiInitializeModifiedPageWriter();

    /* Loop the boot loaded images (under lock) */
    ExAcquireResourceExclusiveLite(&PsLoadedModuleResource, TRUE);
    for (ListEntry = PsLoadedModuleList.Flink;
         ListEntry != &PsLoadedModuleList;
         ListEntry = ListEntry->Flink)
    {
        /* Get the data table entry */
        DataTableEntry = CONTAINING_RECORD(ListEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        /* Set up the image protection */
        MiWriteProtectSystemImage(DataTableEntry->DllBase);
    }
    ExReleaseResourceLite(&PsLoadedModuleResource);

    return TRUE;
}

