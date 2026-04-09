/*
 * kernel internal memory management definitions for ARM64
 *
 * ARM64 uses 4-level paging (PXE/PPE/PDE/PTE) with 4 KB pages and a 48-bit
 * virtual address space, identical paging-level structure to AMD64.
 * Memory region layout confirmed against Win10 ARM64
 */
#pragma once

#define _MI_PAGING_LEVELS 4
#define _MI_HAS_NO_EXECUTE 1

/* Memory layout base addresses (mirrors AMD64 / Win10 ARM64) */
#define MI_USER_PROBE_ADDRESS           (PVOID)0x000007FFFFFF0000ULL
#define MI_DEFAULT_SYSTEM_RANGE_START   (PVOID)0xFFFF080000000000ULL
#define MI_REAL_SYSTEM_RANGE_START             0xFFFF800000000000ULL
#define HYPER_SPACE                            0xFFFFF70000000000ULL
#define HYPER_SPACE_END                        0xFFFFF77FFFFFFFFFULL
#define MI_SYSTEM_CACHE_WS_START               0xFFFFF78000001000ULL /* confirmed via MiSystemWorkingSetsBase symbol */
#define MM_SYSTEM_SPACE_START                  0xFFFFF88000000000ULL
#define MI_DEBUG_MAPPING                (PVOID)0xFFFFF89FFFFFF000ULL
#define MI_PAGED_POOL_START             (PVOID)0xFFFFF8A000000000ULL
#define MI_SESSION_SPACE_END                   0xFFFFF98000000000ULL
#define MI_SYSTEM_CACHE_START                  0xFFFFF98000000000ULL
#define MI_SYSTEM_CACHE_END                    0xFFFFFA7FFFFFFFFFULL
#define MI_PFN_DATABASE                        0xFFFFFA8000000000ULL
#define MI_NONPAGED_POOL_END            (PVOID)0xFFFFFFFFFFBFFFFFULL
#define MI_HIGHEST_SYSTEM_ADDRESS       (PVOID)0xFFFFFFFFFFFFFFFFULL
#define MmSystemRangeStart              ((PVOID)MI_REAL_SYSTEM_RANGE_START)

/* WOW64 address definitions */
#define MM_HIGHEST_USER_ADDRESS_WOW64   0x7FFEFFFF
#define MM_SYSTEM_RANGE_START_WOW64     0x80000000

/* The size of the virtual memory area mapped by a single PDE */
#define PDE_MAPPED_VA (PTE_PER_PAGE * PAGE_SIZE)

/* Misc address definitions */
#define MI_SYSTEM_PTE_BASE              (PVOID)MiAddressToPte(KSEG0_BASE)
#define MM_HIGHEST_VAD_ADDRESS          (PVOID)((ULONG_PTR)MM_HIGHEST_USER_ADDRESS - (16 * PAGE_SIZE))
#define MI_MAPPING_RANGE_START          HYPER_SPACE
#define MI_MAPPING_RANGE_END            (MI_MAPPING_RANGE_START + MI_HYPERSPACE_PTES * PAGE_SIZE)
#define MI_DUMMY_PTE                        (MI_MAPPING_RANGE_END + PAGE_SIZE)
#define MI_VAD_BITMAP                       (MI_DUMMY_PTE + PAGE_SIZE)
#define MI_WORKING_SET_LIST                 (MI_VAD_BITMAP + PAGE_SIZE)

/* Memory sizes (same as AMD64; large 64-bit address space) */
#define MI_MIN_PAGES_FOR_NONPAGED_POOL_TUNING   ((255 * _1MB) >> PAGE_SHIFT)
#define MI_MIN_PAGES_FOR_SYSPTE_TUNING          ((19 * _1MB) >> PAGE_SHIFT)
#define MI_MIN_PAGES_FOR_SYSPTE_BOOST           ((32 * _1MB) >> PAGE_SHIFT)
#define MI_MIN_PAGES_FOR_SYSPTE_BOOST_BOOST     ((256 * _1MB) >> PAGE_SHIFT)
#define MI_MIN_INIT_PAGED_POOLSIZE              (32 * _1MB)
#define MI_MAX_INIT_NONPAGED_POOL_SIZE          (128ULL * 1024 * 1024 * 1024)
#define MI_MAX_NONPAGED_POOL_SIZE               (128ULL * 1024 * 1024 * 1024)
#define MI_SYSTEM_VIEW_SIZE                     (512 * _1MB)
#define MI_SESSION_VIEW_SIZE                    (512 * _1MB)
#define MI_SESSION_POOL_SIZE                    (64 * _1MB)
#define MI_SESSION_IMAGE_SIZE                   (16 * _1MB)
#define MI_SESSION_WORKING_SET_SIZE             (16 * _1MB)
#define MI_SESSION_SIZE                         (MI_SESSION_VIEW_SIZE + \
                                                 MI_SESSION_POOL_SIZE + \
                                                 MI_SESSION_IMAGE_SIZE + \
                                                 MI_SESSION_WORKING_SET_SIZE)
#define MI_MIN_ALLOCATION_FRAGMENT              (4 * _1KB)
#define MI_ALLOCATION_FRAGMENT                  (64 * _1KB)
#define MI_MAX_ALLOCATION_FRAGMENT              (2  * _1MB)

/* Misc constants */
#define MM_PTE_SOFTWARE_PROTECTION_BITS         5
#define MI_MIN_SECONDARY_COLORS                 8
#define MI_SECONDARY_COLORS                     64
#define MI_MAX_SECONDARY_COLORS                 1024
#define MI_NUMBER_SYSTEM_PTES                   (22000 * 22)
#define MI_MAX_FREE_PAGE_LISTS                  4
#define MI_HYPERSPACE_PTES                     (256 - 1)
#define MI_ZERO_PTES                           (32)
#define MI_MAX_ZERO_BITS                        52 /* ARM64: 52-bit IPA in some configs; 48-bit VA -> max user zero bits */
#define SESSION_POOL_LOOKASIDES                 21

/* MMPTE related defines */
#define MM_EMPTY_PTE_LIST  ((ULONG64)0xFFFFFFFF)
#define MM_EMPTY_LIST  ((ULONG_PTR)-1)

/* Easy accessing PFN in PTE */
#define PFN_FROM_PTE(v) ((v)->u.Hard.PageFrameNumber)
#define PFN_FROM_PDE(v) ((v)->u.Hard.PageFrameNumber)
#define PFN_FROM_PPE(v) ((v)->u.Hard.PageFrameNumber)
#define PFN_FROM_PXE(v) ((v)->u.Hard.PageFrameNumber)

/*
 * Macros for portable PTE modification - ARM64 differences:
 *  - Dirty is INVERTED: NotDirty=0 means dirty, NotDirty=1 means clean
 *  - No CacheDisable/WriteThrough bits; cache policy is in CacheType (AttrIndx)
 *  - NX is split into PrivilegedNoExecute and UserNoExecute
 *  - Write permission via Writable bit (AP[2] in hardware)
 *  - Large page via NotLargePage=0 (block descriptor)
 */
#define MI_MAKE_DIRTY_PAGE(x)      ((x)->u.Hard.NotDirty = 0)
#define MI_MAKE_CLEAN_PAGE(x)      ((x)->u.Hard.NotDirty = 1)
#define MI_MAKE_ACCESSED_PAGE(x)   ((x)->u.Hard.Accessed = 1)
#define MI_PAGE_DISABLE_CACHE(x)   ((x)->u.Hard.CacheType = 0) /* AttrIndx=0 = Device nGnRnE */
#define MI_PAGE_WRITE_THROUGH(x)   ((x)->u.Hard.CacheType = 2) /* AttrIndx=2 = Write-through */
#define MI_PAGE_WRITE_COMBINED(x)  ((x)->u.Hard.CacheType = 1) /* AttrIndx=1 = Normal, write-back */
#define MI_IS_PAGE_LARGE(x)        ((x)->u.Hard.NotLargePage == 0)
#define MI_IS_PAGE_WRITEABLE(x)    ((x)->u.Hard.Writable == 1)
#define MI_IS_PAGE_COPY_ON_WRITE(x)((x)->u.Hard.CopyOnWrite == 1)
#define MI_IS_PAGE_EXECUTABLE(x)   (((x)->u.Hard.PrivilegedNoExecute == 0) && \
                                    ((x)->u.Hard.UserNoExecute == 0))
#define MI_IS_PAGE_DIRTY(x)        ((x)->u.Hard.NotDirty == 0)
#define MI_MAKE_OWNER_PAGE(x)      ((x)->u.Hard.Owner = 1)
#define MI_MAKE_WRITE_PAGE(x)      ((x)->u.Hard.Writable = 1)

/*
 * Macros to identify the page fault reason from the ESR_EL1.EC/ISS.
 * On ARM64 faults arrive via ESR_EL1; the FaultCode passed is the ISS field.
 * WnR (bit 6) indicates a write fault; InD (bit 4) an instruction fetch.
 * A non-present fault is indicated by DFSC/IFSC codes 0b0000xx (translation fault).
 */
#define MI_IS_NOT_PRESENT_FAULT(FaultCode)  (((FaultCode) & 0x3C) == 0x04) /* Translation fault (level 1-3) */
#define MI_IS_WRITE_ACCESS(FaultCode)        BooleanFlagOn(FaultCode, 0x40) /* WnR bit */
#define MI_IS_INSTRUCTION_FETCH(FaultCode)   BooleanFlagOn(FaultCode, 0x10) /* InD bit (instruction abort) */

/* On ARM64, like AMD64, PPE and PXE are present */
#define MI_WRITE_VALID_PPE MI_WRITE_VALID_PTE
#define ValidKernelPpe ValidKernelPde

/* Convert an address to a corresponding PTE
 * Identical formula to AMD64: 4-level, 9-bit indices, 12-bit page offset */
FORCEINLINE
PMMPTE
_MiAddressToPte(PVOID Address)
{
    ULONG64 Offset = (ULONG64)Address >> (PTI_SHIFT - 3);
    Offset &= 0xFFFFFFFFFULL << 3;
    return (PMMPTE)(PTE_BASE + Offset);
}
#define MiAddressToPte(x) _MiAddressToPte((PVOID)(x))

/* Convert an address to a corresponding PDE */
FORCEINLINE
PMMPTE
_MiAddressToPde(PVOID Address)
{
    ULONG64 Offset = (ULONG64)Address >> (PDI_SHIFT - 3);
    Offset &= 0x7FFFFFF << 3;
    return (PMMPTE)(PDE_BASE + Offset);
}
#define MiAddressToPde(x) _MiAddressToPde((PVOID)(x))

/* Convert an address to a corresponding PPE */
FORCEINLINE
PMMPTE
MiAddressToPpe(PVOID Address)
{
    ULONG64 Offset = (ULONG64)Address >> (PPI_SHIFT - 3);
    Offset &= 0x3FFFF << 3;
    return (PMMPTE)(PPE_BASE + Offset);
}

/* Convert an address to a corresponding PXE */
FORCEINLINE
PMMPTE
MiAddressToPxe(PVOID Address)
{
    ULONG64 Offset = (ULONG64)Address >> (PXI_SHIFT - 3);
    Offset &= PXI_MASK << 3;
    return (PMMPTE)(PXE_BASE + Offset);
}

/* Convert an address to a corresponding PTE offset/index */
FORCEINLINE
ULONG
MiAddressToPti(PVOID Address)
{
    return ((((ULONG64)Address) >> PTI_SHIFT) & 0x1FF);
}
#define MiAddressToPteOffset(x) MiAddressToPti(x)

/* Convert an address to a corresponding PDE offset/index */
FORCEINLINE
ULONG
MiAddressToPdi(PVOID Address)
{
    return ((((ULONG64)Address) >> PDI_SHIFT) & 0x1FF);
}
#define MiAddressToPdeOffset(x) MiAddressToPdi(x)
#define MiGetPdeOffset(x) MiAddressToPdi(x)

/* Convert an address to a corresponding PXE offset/index */
FORCEINLINE
ULONG
MiAddressToPxi(PVOID Address)
{
    return ((((ULONG64)Address) >> PXI_SHIFT) & 0x1FF);
}

/* Convert a PTE into a corresponding address (signed shift for canonical form) */
FORCEINLINE
PVOID
MiPteToAddress(PMMPTE PointerPte)
{
    return (PVOID)(((LONG64)PointerPte << 25) >> 16);
}

/* Convert a PDE into a corresponding address */
FORCEINLINE
PVOID
MiPdeToAddress(PMMPTE PointerPde)
{
    return (PVOID)(((LONG64)PointerPde << 34) >> 16);
}

/* Convert a PPE into a corresponding address */
FORCEINLINE
PVOID
MiPpeToAddress(PMMPTE PointerPpe)
{
    return (PVOID)(((LONG64)PointerPpe << 43) >> 16);
}

/* Convert a PXE into a corresponding address */
FORCEINLINE
PVOID
MiPxeToAddress(PMMPTE PointerPxe)
{
    return (PVOID)(((LONG64)PointerPxe << 52) >> 16);
}

/* Convert a PDE into its lowest PTE */
FORCEINLINE
PMMPTE
MiPdeToPte(PMMPDE PointerPde)
{
    return (PMMPTE)MiPteToAddress(PointerPde);
}

/* Convert a PPE into its lowest PTE */
FORCEINLINE
PMMPTE
MiPpeToPte(PMMPPE PointerPpe)
{
    return (PMMPTE)MiPdeToAddress(PointerPpe);
}

/* Convert a PXE into its lowest PTE */
FORCEINLINE
PMMPTE
MiPxeToPte(PMMPXE PointerPxe)
{
    return (PMMPTE)MiPpeToAddress(PointerPxe);
}

/* Convert a PTE to a corresponding PDE */
FORCEINLINE
PMMPDE
MiPteToPde(PMMPTE PointerPte)
{
    return (PMMPDE)MiAddressToPte(PointerPte);
}

/* Convert a PTE to a corresponding PPE */
FORCEINLINE
PMMPPE
MiPteToPpe(PMMPTE PointerPte)
{
    return (PMMPPE)MiAddressToPde(PointerPte);
}

/* Convert a PTE to a corresponding PXE */
FORCEINLINE
PMMPXE
MiPteToPxe(PMMPTE PointerPte)
{
    return (PMMPXE)MiAddressToPpe(PointerPte);
}

/* Convert a PDE to a corresponding PPE */
FORCEINLINE
PMMPDE
MiPdeToPpe(PMMPDE PointerPde)
{
    return (PMMPPE)MiAddressToPte(PointerPde);
}

/* Convert a PDE to a corresponding PXE */
FORCEINLINE
PMMPXE
MiPdeToPxe(PMMPDE PointerPde)
{
    return (PMMPXE)MiAddressToPde(PointerPde);
}

/* Check P*E boundaries */
#define MiIsPteOnPdeBoundary(PointerPte) \
    ((((ULONG_PTR)PointerPte) & (PAGE_SIZE - 1)) == 0)
#define MiIsPteOnPpeBoundary(PointerPte) \
    ((((ULONG_PTR)PointerPte) & (PDE_PER_PAGE * PAGE_SIZE - 1)) == 0)
#define MiIsPteOnPxeBoundary(PointerPte) \
    ((((ULONG_PTR)PointerPte) & (PPE_PER_PAGE * PDE_PER_PAGE * PAGE_SIZE - 1)) == 0)

/*
 * Decodes a Prototype PTE into the underlying PTE.
 * ProtoAddress is a 48-bit signed field; sign-extend to 64 bits.
 */
#define MiProtoPteToPte(x) \
    (PMMPTE)(((LONG64)(x)->u.Long) >> 16)

/*
 * Decodes a Subsection PTE into the subsection pointer.
 * SubsectionAddress is a 48-bit signed field already at the right position.
 */
#define MiSubsectionPteToSubsection(x) \
    (PMMPTE)((LONG64)(x)->u.Subsect.SubsectionAddress)

FORCEINLINE
VOID
MI_MAKE_SUBSECTION_PTE(
    _Out_ PMMPTE NewPte,
    _In_ PVOID Segment)
{
    NewPte->u.Long = 0;
    NewPte->u.Subsect.Prototype = 1;
    NewPte->u.Subsect.SubsectionAddress = ((ULONG_PTR)Segment & 0x0000FFFFFFFFFFFF);
}

FORCEINLINE
VOID
MI_MAKE_PROTOTYPE_PTE(
    _Out_ PMMPTE NewPte,
    _In_ PMMPTE PointerPte)
{
    NewPte->u.Long = (ULONG64)PointerPte << 16;
    NewPte->u.Proto.Prototype = 1;
    ASSERT(MiProtoPteToPte(NewPte) == PointerPte);
}

FORCEINLINE
BOOLEAN
MI_IS_MAPPED_PTE(PMMPTE PointerPte)
{
    return ((PointerPte->u.Hard.Valid != 0) ||
            (PointerPte->u.Proto.Prototype != 0) ||
            (PointerPte->u.Trans.Transition != 0) ||
            (PointerPte->u.Hard.PageFrameNumber != 0));
}

FORCEINLINE
BOOLEAN
MiIsPdeForAddressValid(PVOID Address)
{
    return ((MiAddressToPxe(Address)->u.Hard.Valid) &&
            (MiAddressToPpe(Address)->u.Hard.Valid) &&
            (MiAddressToPde(Address)->u.Hard.Valid));
}
