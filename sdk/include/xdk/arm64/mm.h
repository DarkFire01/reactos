$if (_NTDDK_)
/*
 * ARM64 memory management definitions.
 *
 * ARM64 uses a 4-level page table structure identical to AMD64:
 *   4KB granule, 48-bit virtual address space, 512 entries per level.
 * The self-map slot and base virtual addresses match the AMD64 layout on
 * Windows ARM64.
 */

/* Page table level index bit-shift values */
#define PTI_SHIFT   12L
#define PDI_SHIFT   21L
#define PPI_SHIFT   30L
#define PXI_SHIFT   39L

/* Entries per page-table page at each level */
#define PTE_PER_PAGE 512
#define PDE_PER_PAGE 512
#define PPE_PER_PAGE 512
#define PXE_PER_PAGE 512

/* Index masks (applied after the corresponding right-shift) */
#define PTI_MASK_ARM64  (PTE_PER_PAGE - 1)
#define PDI_MASK_ARM64  (PDE_PER_PAGE - 1)
#define PPI_MASK        (PPE_PER_PAGE - 1)
#define PXI_MASK        (PXE_PER_PAGE - 1)

/*
 * Self-map virtual addresses.
 * Windows ARM64 uses the same self-map slot as AMD64; the constants are
 * therefore identical.
 */
#define PXE_BASE    0xFFFFF6FB7DBED000ULL
#define PXE_SELFMAP 0xFFFFF6FB7DBEDF68ULL
#define PPE_BASE    0xFFFFF6FB7DA00000ULL
#define PDE_BASE    0xFFFFF6FB40000000ULL
#define PTE_BASE    0xFFFFF68000000000ULL

#define PXE_TOP     0xFFFFF6FB7DBEDFFFULL
#define PPE_TOP     0xFFFFF6FB7DBFFFFFULL
#define PDE_TOP     0xFFFFF6FB7FFFFFFFULL
#define PTE_TOP     0xFFFFF6FFFFFFFFFFULL

extern NTKERNELAPI PVOID   MmHighestUserAddress;
extern NTKERNELAPI PVOID   MmSystemRangeStart;
extern NTKERNELAPI ULONG64 MmUserProbeAddress;

#define MM_HIGHEST_USER_ADDRESS MmHighestUserAddress
#define MM_SYSTEM_RANGE_START   MmSystemRangeStart

#if defined(_LOCAL_COPY_USER_PROBE_ADDRESS_)
#define MM_USER_PROBE_ADDRESS _LOCAL_COPY_USER_PROBE_ADDRESS_
extern ULONG64 _LOCAL_COPY_USER_PROBE_ADDRESS_;
#else
#define MM_USER_PROBE_ADDRESS MmUserProbeAddress
#endif

#define MM_LOWEST_USER_ADDRESS   ((PVOID)(ULONG_PTR)0x10000)
#define MM_LOWEST_SYSTEM_ADDRESS ((PVOID)0xFFFF080000000000ULL)

$endif /* _NTDDK_ */
