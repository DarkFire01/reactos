/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/mm/ARM3/mmsup.c
 * PURPOSE:         ARM Memory Manager Support Routines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

SIZE_T MmMinimumWorkingSetSize;
SIZE_T MmMaximumWorkingSetSize;
SIZE_T MmPagesAboveWsMinimum;

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
MmMapUserAddressesToPage(IN PVOID BaseAddress,
                         IN SIZE_T NumberOfBytes,
                         IN PVOID PageAddress)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
MmAdjustWorkingSetSize(IN SIZE_T WorkingSetMinimumInBytes,
                       IN SIZE_T WorkingSetMaximumInBytes,
                       IN ULONG SystemCache,
                       IN BOOLEAN IncreaseOkay)
{
    SIZE_T MinimumWorkingSetSize, MaximumWorkingSetSize;
    SSIZE_T Delta;
    PMMSUPPORT Ws;
    NTSTATUS Status;

    /* Check for special case: empty the working set */
    if ((WorkingSetMinimumInBytes == -1) &&
        (WorkingSetMaximumInBytes == -1))
    {
        UNIMPLEMENTED;
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Assume success */
    Status = STATUS_SUCCESS;

    /* Get the working set and lock it */
    Ws = &PsGetCurrentProcess()->Vm;
    MiLockWorkingSet(PsGetCurrentThread(), Ws);

    /* Calculate the actual minimum and maximum working set size to set */
    MinimumWorkingSetSize = (WorkingSetMinimumInBytes != 0) ?
        (WorkingSetMinimumInBytes / PAGE_SIZE) : Ws->MinimumWorkingSetSize;
    MaximumWorkingSetSize = (WorkingSetMaximumInBytes != 0) ?
        (WorkingSetMaximumInBytes / PAGE_SIZE) : Ws->MaximumWorkingSetSize;

    /* Check if the new maximum exceeds the global maximum */
    if (MaximumWorkingSetSize > MmMaximumWorkingSetSize)
    {
        MaximumWorkingSetSize = MmMaximumWorkingSetSize;
        Status = STATUS_WORKING_SET_LIMIT_RANGE;
    }

    /* Check if the new minimum is below the global minimum */
    if (MinimumWorkingSetSize < MmMinimumWorkingSetSize)
    {
        MinimumWorkingSetSize = MmMinimumWorkingSetSize;
        Status = STATUS_WORKING_SET_LIMIT_RANGE;
    }

    /* Check if the new minimum exceeds the new maximum */
    if (MinimumWorkingSetSize > MaximumWorkingSetSize)
    {
        DPRINT1("MinimumWorkingSetSize (%lu) > MaximumWorkingSetSize (%lu)\n",
                MinimumWorkingSetSize, MaximumWorkingSetSize);
        Status = STATUS_BAD_WORKING_SET_LIMIT;
        goto Cleanup;
    }

    /* Calculate the minimum WS size adjustment and check if we increase */
    Delta = MinimumWorkingSetSize - Ws->MinimumWorkingSetSize;
    if (Delta > 0)
    {
        /* Is increasing ok? */
        if (!IncreaseOkay)
        {
            DPRINT1("Privilege for WS size increase not held\n");
            Status = STATUS_PRIVILEGE_NOT_HELD;
            goto Cleanup;
        }

        /* Check if the number of available pages is large enough */
        if (((SIZE_T)Delta / 1024) > (MmAvailablePages - 128))
        {
            DPRINT1("Not enough available pages\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }

        /* Check if there are enough resident available pages */
        if ((SIZE_T)Delta >
            (MmResidentAvailablePages - MmSystemLockPagesCount - 256))
        {
            DPRINT1("Not enough resident pages\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
    }

    /* Update resident available pages */
    if (Delta != 0)
    {
        InterlockedExchangeAddSizeT(&MmResidentAvailablePages, -Delta);
    }

    /* Calculate new pages above minimum WS size */
    Delta += max((SSIZE_T)Ws->WorkingSetSize - MinimumWorkingSetSize, 0);

    /* Subtract old pages above minimum WS size */
    Delta -= max((SSIZE_T)Ws->WorkingSetSize - Ws->MinimumWorkingSetSize, 0);

    /* If it changed, add it to the global variable */
    if (Delta != 0)
    {
        InterlockedExchangeAddSizeT(&MmPagesAboveWsMinimum, Delta);
    }

    /* Set the new working set size */
    Ws->MinimumWorkingSetSize = MinimumWorkingSetSize;
    Ws->MaximumWorkingSetSize = MaximumWorkingSetSize;

Cleanup:

    /* Unlock the working set and return the status */
    MiUnlockWorkingSet(PsGetCurrentThread(), Ws);
    return Status;
}

/**
 * @brief Marks the pages of a mapped range as modified.
 *
 * @param[in] Address
 * First page of the range.
 *
 * @param[in] Length
 * Length of the range in bytes.
 *
 * @return TRUE if the range had a mapped page, FALSE otherwise.
 *
 * @remarks A flush only writes pages the page frame calls modified. A caller that
 * wrote through another mapping of these pages, an MDL of a cached view for one,
 * hands that state over here.
 *
 * @implemented
 */
BOOLEAN
NTAPI
MmSetAddressRangeModified(IN PVOID Address,
                          IN SIZE_T Length)
{
    PMMPTE PointerPte = MiAddressToPte(Address);
    PMMPTE LastPte = MiAddressToPte((PUCHAR)Address + Length - 1);
    BOOLEAN Mapped = FALSE;
    PMMPFN Pfn1;
    MMPTE TempPte;
    KIRQL OldIrql;

    OldIrql = MiAcquirePfnLock();

    for (; PointerPte <= LastPte; PointerPte++)
    {
        /* A range without a page table has nothing to hand over */
        if (!MmIsAddressValid(PointerPte))
            continue;

        TempPte = *PointerPte;
        if (!TempPte.u.Hard.Valid)
            continue;

        Mapped = TRUE;
        Pfn1 = MI_PFN_ELEMENT(PFN_FROM_PTE(&TempPte));
        Pfn1->u3.e1.Modified = 1;

        /* The page frame has it now, the next write marks the PTE again */
        if (MI_IS_PAGE_DIRTY(&TempPte))
        {
            MI_MAKE_CLEAN_PAGE(&TempPte);
            MI_UPDATE_VALID_PTE(PointerPte, TempPte);
            KeInvalidateTlbEntry(MiPteToAddress(PointerPte));
        }
    }

    MiReleasePfnLock(OldIrql);
    return Mapped;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
MmIsAddressValid(IN PVOID VirtualAddress)
{
#if _MI_PAGING_LEVELS >= 4
    /* Check if the PXE is valid */
    if (MiAddressToPxe(VirtualAddress)->u.Hard.Valid == 0) return FALSE;
#endif

#if _MI_PAGING_LEVELS >= 3
    /* Check if the PPE is valid */
    if (MiAddressToPpe(VirtualAddress)->u.Hard.Valid == 0) return FALSE;
#endif

#if _MI_PAGING_LEVELS >= 2
    /* Check if the PDE is valid */
    if (MiAddressToPde(VirtualAddress)->u.Hard.Valid == 0) return FALSE;
#endif

    /* Check if the PTE is valid */
    if (MiAddressToPte(VirtualAddress)->u.Hard.Valid == 0) return FALSE;

    /* This address is valid now, but it will only stay so if the caller holds
     * the PFN lock */
    return TRUE;
}

/**
 * @brief Tells whether an address lies in system space.
 */
BOOLEAN
NTAPI
MmIsKernelAddress(
    _In_ PVOID VirtualAddress)
{
    return (VirtualAddress >= MmSystemRangeStart);
}

/**
 * @brief Tells whether an address lies in the user range of the address space.
 */
BOOLEAN
NTAPI
MmIsUserAddress(
    _In_ PVOID VirtualAddress)
{
    return (VirtualAddress <= MM_HIGHEST_USER_ADDRESS);
}

/**
 * @brief Copies one page or less from a physical address through a temporary mapping.
 */
static
NTSTATUS
MiCopyFromPhysicalPage(
    _Out_writes_bytes_(Length) PVOID Target,
    _In_ ULONGLONG Source,
    _In_ SIZE_T Length)
{
    PHYSICAL_ADDRESS Page;
    NTSTATUS Status = STATUS_SUCCESS;
    PUCHAR Mapping;

    /* RAM keeps the attributes of its other mappings, anything else is read uncached */
    Page.QuadPart = Source & ~(ULONGLONG)(PAGE_SIZE - 1);
    Mapping = MmMapIoSpace(Page, PAGE_SIZE, MiGetPfnEntry((PFN_NUMBER)(Source >> PAGE_SHIFT)) ? MmCached : MmNonCached);
    if (!Mapping)
        return STATUS_INSUFFICIENT_RESOURCES;

    _SEH2_TRY
    {
        RtlCopyMemory(Target, Mapping + (Source & (PAGE_SIZE - 1)), Length);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    MmUnmapIoSpace(Mapping, PAGE_SIZE);
    return Status;
}

/**
 * @brief
 * Copies from a physical or virtual range without ever faulting a page in.
 *
 * @param[in] Flags
 * Exactly one of MM_COPY_MEMORY_PHYSICAL or MM_COPY_MEMORY_VIRTUAL.
 *
 * @param[out] NumberOfBytesTransferred
 * Receives how much was copied, also when the copy stops early.
 *
 * @return
 * STATUS_INVALID_PARAMETER for bad flags, an empty or wrapping range, or a virtual
 * range that is neither all user nor all system. The copy stops at the first page
 * that is not resident and returns its failure.
 */
NTSTATUS
NTAPI
MmCopyMemory(
    _In_ PVOID TargetAddress,
    _In_ MM_COPY_ADDRESS SourceAddress,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Flags,
    _Out_ PSIZE_T NumberOfBytesTransferred)
{
    ULONGLONG Source, End;
    PUCHAR Target = TargetAddress;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Chunk;
    BOOLEAN Physical;

    *NumberOfBytesTransferred = 0;

    if (!Flags || (Flags & ~(MM_COPY_MEMORY_PHYSICAL | MM_COPY_MEMORY_VIRTUAL)) || (Flags & (Flags - 1)) ||
        KeGetCurrentIrql() > APC_LEVEL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Physical = (Flags & MM_COPY_MEMORY_PHYSICAL) != 0;
    Source = Physical ? (ULONGLONG)SourceAddress.PhysicalAddress.QuadPart : (ULONG_PTR)SourceAddress.VirtualAddress;
    End = Source + NumberOfBytes;
    if (End <= Source)
        return STATUS_INVALID_PARAMETER;

    if (!Physical && Source <= (ULONG_PTR)MM_HIGHEST_USER_ADDRESS)
    {
        /* A user range is read from the current process and must not reach into system space */
        if (End - 1 > (ULONG_PTR)MM_HIGHEST_USER_ADDRESS)
            return STATUS_INVALID_PARAMETER;

        _SEH2_TRY
        {
            ProbeForRead((PVOID)(ULONG_PTR)Source, NumberOfBytes, 1);
            RtlCopyMemory(Target, (PVOID)(ULONG_PTR)Source, NumberOfBytes);
            *NumberOfBytesTransferred = NumberOfBytes;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        return Status;
    }

    if (!Physical && Source < (ULONG_PTR)MmSystemRangeStart)
        return STATUS_INVALID_PARAMETER;

    while (Source < End)
    {
        Chunk = (SIZE_T)min(End - Source, PAGE_SIZE - (Source & (PAGE_SIZE - 1)));

        if (Physical)
        {
            Status = MiCopyFromPhysicalPage(Target, Source, Chunk);
        }
        else if (!MmIsAddressValid((PVOID)(ULONG_PTR)Source))
        {
            Status = STATUS_ACCESS_VIOLATION;
        }
        else
        {
            _SEH2_TRY
            {
                RtlCopyMemory(Target, (PVOID)(ULONG_PTR)Source, Chunk);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
        }

        if (!NT_SUCCESS(Status))
            break;

        *NumberOfBytesTransferred += Chunk;
        Target += Chunk;
        Source += Chunk;
    }

    return Status;
}

/*
 * @unimplemented
 */
BOOLEAN
NTAPI
MmIsNonPagedSystemAddressValid(IN PVOID VirtualAddress)
{
    DPRINT1("WARNING: %s returns bogus result\n", __FUNCTION__);
    return MmIsAddressValid(VirtualAddress);
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
MmSetBankedSection(IN HANDLE ProcessHandle,
                   IN PVOID VirtualAddress,
                   IN ULONG BankLength,
                   IN BOOLEAN ReadWriteBank,
                   IN PVOID BankRoutine,
                   IN PVOID Context)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
MmIsRecursiveIoFault(VOID)
{
    PETHREAD Thread = PsGetCurrentThread();

    //
    // If any of these is true, this is a recursive fault
    //
    return ((Thread->DisablePageFaultClustering) | (Thread->ForwardClusterOnly));
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
MmIsThisAnNtAsSystem(VOID)
{
    /* Return if this is a server system */
    return MmProductType & 0xFF;
}

/*
 * @implemented
 */
MM_SYSTEMSIZE
NTAPI
MmQuerySystemSize(VOID)
{
    /* Return the low, medium or high memory system type */
    return MmSystemSize;
}

NTSTATUS
NTAPI
MmCreateMirror(VOID)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
