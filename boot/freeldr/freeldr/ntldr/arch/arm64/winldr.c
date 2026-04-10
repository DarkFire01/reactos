

/* INCLUDES ***************************************************************/

#include <freeldr.h>
#include <ndk/asm.h>
#include <intrin.h>
#include "../../winldr.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(WINDOWS);

/* GLOBALS ***************************************************************/

PHARDWARE_PTE PxeBase;
PFN_NUMBER SharedUserDataPfn;

/* INTERNAL HELPERS *******************************************************/

#define ARM64_MAIR_ATTR_NORMAL_WB     0xFF
#define ARM64_MAIR_ATTR_DEVICE_nGnRnE 0x00
#define ARM64_MAIR_DEFAULT            ((ARM64_MAIR_ATTR_NORMAL_WB << 0) | (ARM64_MAIR_ATTR_DEVICE_nGnRnE << 8))

#define ARM64_PTE_CACHE_DEVICE        0
#define ARM64_PTE_CACHE_NORMAL_WB     3
#define ARM64_PTE_SHAREABILITY_INNER  3

#define ARM64_QEMU_VIRT_UART_BASE     0x09000000ULL

#define ARM64_SCTLR_M                 (1ULL << 0)
#define ARM64_SCTLR_C                 (1ULL << 2)
#define ARM64_SCTLR_I                 (1ULL << 12)

#define ARM64_TCR_T0SZ(_x)            ((ULONG64)(_x) << 0)
#define ARM64_TCR_IRGN0_WBWA          (1ULL << 8)
#define ARM64_TCR_ORGN0_WBWA          (1ULL << 10)
#define ARM64_TCR_SH0_INNER           (3ULL << 12)
#define ARM64_TCR_TG0_4KB             (0ULL << 14)
#define ARM64_TCR_T1SZ(_x)            ((ULONG64)(_x) << 16)
#define ARM64_TCR_EPD1                (1ULL << 23)
#define ARM64_TCR_IRGN1_WBWA          (1ULL << 24)
#define ARM64_TCR_ORGN1_WBWA          (1ULL << 26)
#define ARM64_TCR_SH1_INNER           (3ULL << 28)
#define ARM64_TCR_TG1_4KB             (2ULL << 30)
#define ARM64_TCR_IPS(_x)             ((ULONG64)(_x) << 32)

#define ARM64_TCR_DEFAULT(_ips)       (ARM64_TCR_T0SZ(16) | ARM64_TCR_IRGN0_WBWA | ARM64_TCR_ORGN0_WBWA | ARM64_TCR_SH0_INNER | ARM64_TCR_TG0_4KB | \
                                      ARM64_TCR_T1SZ(16) | ARM64_TCR_IRGN1_WBWA | ARM64_TCR_ORGN1_WBWA | ARM64_TCR_SH1_INNER | ARM64_TCR_TG1_4KB | \
                                      ARM64_TCR_IPS(_ips))

static
FORCEINLINE
VOID
Arm64Isb(VOID)
{
    __isb(0xF);
}

static
FORCEINLINE
VOID
Arm64Dsb(VOID)
{
    __dsb(0xB);
}

static
FORCEINLINE
ULONG64
Arm64ReadSctlrEl1(VOID)
{
#if defined(_MSC_VER)
    return _ReadStatusReg(ARM64_SYSREG(3, 0, 1, 0, 0));
#else
    ULONG64 Value;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(Value) :: "memory");
    return Value;
#endif
}

static
FORCEINLINE
ULONG64
Arm64ReadAa64Mmfr0El1(VOID)
{
#if defined(_MSC_VER)
    return _ReadStatusReg(ARM64_SYSREG(3, 0, 0, 7, 4));
#else
    ULONG64 Value;
    __asm__ __volatile__("mrs %0, id_aa64mmfr0_el1" : "=r"(Value) :: "memory");
    return Value;
#endif
}

static
FORCEINLINE
ULONG64
Arm64ReadTcrEl1(VOID)
{
#if defined(_MSC_VER)
    return _ReadStatusReg(ARM64_SYSREG(3, 0, 2, 0, 2));
#else
    ULONG64 Value;
    __asm__ __volatile__("mrs %0, tcr_el1" : "=r"(Value) :: "memory");
    return Value;
#endif
}

static
FORCEINLINE
ULONG64
Arm64ReadTtbr0El1(VOID)
{
#if defined(_MSC_VER)
    return _ReadStatusReg(ARM64_SYSREG(3, 0, 2, 0, 0));
#else
    ULONG64 Value;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(Value) :: "memory");
    return Value;
#endif
}

static
FORCEINLINE
ULONG64
Arm64ReadTtbr1El1(VOID)
{
#if defined(_MSC_VER)
    return _ReadStatusReg(ARM64_SYSREG(3, 0, 2, 0, 1));
#else
    ULONG64 Value;
    __asm__ __volatile__("mrs %0, ttbr1_el1" : "=r"(Value) :: "memory");
    return Value;
#endif
}

static
FORCEINLINE
ULONG64
Arm64ReadMairEl1(VOID)
{
#if defined(_MSC_VER)
    return _ReadStatusReg(ARM64_SYSREG(3, 0, 10, 2, 0));
#else
    ULONG64 Value;
    __asm__ __volatile__("mrs %0, mair_el1" : "=r"(Value) :: "memory");
    return Value;
#endif
}

static
FORCEINLINE
VOID
Arm64WriteSctlrEl1(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 1, 0, 0), Value);
#else
    __asm__ __volatile__("msr sctlr_el1, %0" :: "r"(Value) : "memory");
#endif
    Arm64Isb();
}

static
FORCEINLINE
VOID
Arm64WriteTtbr0El1(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 2, 0, 0), Value);
#else
    __asm__ __volatile__("msr ttbr0_el1, %0" :: "r"(Value) : "memory");
#endif
    Arm64Isb();
}

static
FORCEINLINE
VOID
Arm64WriteTtbr1El1(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 2, 0, 1), Value);
#else
    __asm__ __volatile__("msr ttbr1_el1, %0" :: "r"(Value) : "memory");
#endif
    Arm64Isb();
}

static
FORCEINLINE
VOID
Arm64WriteMairEl1(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 10, 2, 0), Value);
#else
    __asm__ __volatile__("msr mair_el1, %0" :: "r"(Value) : "memory");
#endif
    Arm64Isb();
}

static
FORCEINLINE
VOID
Arm64WriteTcrEl1(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 2, 0, 2), Value);
#else
    __asm__ __volatile__("msr tcr_el1, %0" :: "r"(Value) : "memory");
#endif
    Arm64Isb();
}

/* Forward declare assembly helper for ARM64 TLBI VMALLE1IS instruction.
 * MSVC intrinsics don't directly support TLBI instructions. */
extern VOID Arm64InvalidateTlbVmalle1Is(VOID);

static
FORCEINLINE
VOID
Arm64InvalidateAllTlb(VOID)
{
#if defined(_MSC_VER)
    /* TLBI VMALLE1IS is a system instruction (not MSR-accessible).
     * Call the ARM64 assembly helper which emits the raw TLBI instruction. */
    Arm64InvalidateTlbVmalle1Is();
#else
    __asm__ __volatile__("tlbi vmalle1is" ::: "memory");
    Arm64Dsb();
    Arm64Isb();
#endif
}

static
FORCEINLINE
ULONG64
Arm64GetTcrIps(VOID)
{
    ULONG64 Mmfr0;
    ULONG64 PaRange;

    Mmfr0 = Arm64ReadAa64Mmfr0El1();
    PaRange = Mmfr0 & 0xF;

    if (PaRange > 6)
    {
        PaRange = 0;
    }

    return PaRange;
}

static
FORCEINLINE
VOID
Arm64SetupTableDescriptor(PHARDWARE_PTE Entry, PFN_NUMBER PageFrameNumber)
{
    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->Valid = 1;
    Entry->NotLargePage = 1;
    Entry->Writable = 1;
    Entry->Accessed = 1;
    Entry->Shareability = ARM64_PTE_SHAREABILITY_INNER;
    Entry->CacheType = ARM64_PTE_CACHE_NORMAL_WB;
    Entry->PageFrameNumber = PageFrameNumber;
}

static
FORCEINLINE
VOID
Arm64SetupPageDescriptor(PHARDWARE_PTE Entry,
                         PFN_NUMBER PageFrameNumber,
                         BOOLEAN CacheDisabled)
{
    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->Valid = 1;
    Entry->NotLargePage = 1;
    Entry->Writable = 1;
    Entry->Accessed = 1;
    Entry->Shareability = ARM64_PTE_SHAREABILITY_INNER;
    Entry->CacheType = CacheDisabled ? ARM64_PTE_CACHE_DEVICE : ARM64_PTE_CACHE_NORMAL_WB;
    Entry->PageFrameNumber = PageFrameNumber;
}

static
BOOLEAN
MempAllocatePageTables(VOID)
{
    TRACE(">>> MempAllocatePageTables\n");

    /* Allocate a top-level table (PXE/PML4 equivalent). */
    PxeBase = MmAllocateMemoryWithType(PAGE_SIZE, LoaderMemoryData);
    if (!PxeBase)
    {
        ERR("failed to allocate top-level page table\n");
        return FALSE;
    }

    RtlZeroMemory(PxeBase, PAGE_SIZE);

    /* Create recursive self-map like AMD64 so PTE_BASE style math works. */
    Arm64SetupTableDescriptor(&PxeBase[VAtoPXI(PXE_BASE)], PtrToPfn(PxeBase));

    TRACE("<<< MempAllocatePageTables\n");
    return TRUE;
}

static
PHARDWARE_PTE
MempGetOrCreatePageDir(PHARDWARE_PTE PdeBase, ULONG Index)
{
    PHARDWARE_PTE SubDir;

    if (!PdeBase)
        return NULL;

    if (!PdeBase[Index].Valid)
    {
        SubDir = MmAllocateMemoryWithType(PAGE_SIZE, LoaderMemoryData);
        if (!SubDir)
            return NULL;

        RtlZeroMemory(SubDir, PAGE_SIZE);
        Arm64SetupTableDescriptor(&PdeBase[Index], PtrToPfn(SubDir));
    }
    else
    {
        SubDir = (PVOID)((ULONG64)PdeBase[Index].PageFrameNumber * PAGE_SIZE);
    }

    return SubDir;
}

static
BOOLEAN
MempMapSinglePageWithAttributes(ULONG64 VirtualAddress,
                                ULONG64 PhysicalAddress,
                                BOOLEAN CacheDisabled)
{
    PHARDWARE_PTE PpeBase, PdeBase, PteBase;
    ULONG Index;

    PpeBase = MempGetOrCreatePageDir(PxeBase, VAtoPXI(VirtualAddress));
    PdeBase = MempGetOrCreatePageDir(PpeBase, VAtoPPI(VirtualAddress));
    PteBase = MempGetOrCreatePageDir(PdeBase, VAtoPDI(VirtualAddress));

    if (!PteBase)
    {
        ERR("No table for VA %p\n", (PVOID)VirtualAddress);
        return FALSE;
    }

    Index = VAtoPTI(VirtualAddress);
    if (PteBase[Index].Valid)
    {
        if (PteBase[Index].PageFrameNumber != (PhysicalAddress / PAGE_SIZE))
        {
            ERR("VA already mapped: %p\n", (PVOID)VirtualAddress);
            return FALSE;
        }

        PteBase[Index].Shareability = ARM64_PTE_SHAREABILITY_INNER;
        PteBase[Index].CacheType = CacheDisabled ? ARM64_PTE_CACHE_DEVICE : ARM64_PTE_CACHE_NORMAL_WB;
        PteBase[Index].Writable = 1;
        PteBase[Index].Accessed = 1;
        return TRUE;
    }

    Arm64SetupPageDescriptor(&PteBase[Index], PhysicalAddress / PAGE_SIZE, CacheDisabled);

    return TRUE;
}

static
BOOLEAN
MempMapSinglePage(ULONG64 VirtualAddress, ULONG64 PhysicalAddress)
{
    return MempMapSinglePageWithAttributes(VirtualAddress, PhysicalAddress, FALSE);
}

static
PFN_NUMBER
MempMapRangeOfPages(ULONG64 VirtualAddress, ULONG64 PhysicalAddress, PFN_NUMBER PageCount)
{
    PFN_NUMBER i;

    for (i = 0; i < PageCount; ++i)
    {
        if (!MempMapSinglePage(VirtualAddress, PhysicalAddress))
            return i;

        VirtualAddress += PAGE_SIZE;
        PhysicalAddress += PAGE_SIZE;
    }

    return i;
}

BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage,
                IN PFN_NUMBER NumberOfPages,
                IN BOOLEAN KernelMapping)
{
    TRACE(">>> MempSetupPaging(0x%lx, %ld, %p)\n",
          StartPage,
          NumberOfPages,
          (PVOID)(StartPage * PAGE_SIZE + KSEG0_BASE));

    /* Keep identity maps so loader code/data stays valid through MMU switch. */
    if (MempMapRangeOfPages(StartPage * PAGE_SIZE,
                            StartPage * PAGE_SIZE,
                            NumberOfPages) != NumberOfPages)
    {
        ERR("Failed identity map for pages %ld..%ld\n", StartPage, StartPage + NumberOfPages - 1);
        return FALSE;
    }

    if (KernelMapping)
    {
        if (MempMapRangeOfPages(StartPage * PAGE_SIZE + KSEG0_BASE,
                                StartPage * PAGE_SIZE,
                                NumberOfPages) != NumberOfPages)
        {
            ERR("Failed kernel map for pages %ld..%ld\n", StartPage, StartPage + NumberOfPages - 1);
            return FALSE;
        }
    }

    return TRUE;
}

VOID
MempUnmapPage(IN PFN_NUMBER Page)
{
    UNREFERENCED_PARAMETER(Page);
}

static
BOOLEAN
WinLdrMapSpecialPages(VOID)
{
    PHARDWARE_PTE PpeBase, PdeBase;

    TRACE("Mapping KI_USER_SHARED_DATA VA=%p to PFN=0x%lx PA=%p\n",
          (PVOID)KI_USER_SHARED_DATA,
          SharedUserDataPfn,
          (PVOID)(SharedUserDataPfn * PAGE_SIZE));

    if (!MempMapSinglePage(KI_USER_SHARED_DATA, SharedUserDataPfn * PAGE_SIZE))
    {
        ERR("Could not map KI_USER_SHARED_DATA\n");
        return FALSE;
    }

    /* Keep the ARM64 QEMU debug UART reachable after switching TTBRs. */
    if (!MempMapSinglePageWithAttributes(ARM64_QEMU_VIRT_UART_BASE,
                                         ARM64_QEMU_VIRT_UART_BASE,
                                         TRUE))
    {
        ERR("Could not map ARM64 debug UART page\n");
        return FALSE;
    }

    /* Pre-create page-table hierarchy for HAL virtual mapping window. */
    PpeBase = MempGetOrCreatePageDir(PxeBase, VAtoPXI(MM_HAL_VA_START));
    PdeBase = MempGetOrCreatePageDir(PpeBase, VAtoPPI(MM_HAL_VA_START));
    MempGetOrCreatePageDir(PdeBase, VAtoPDI(MM_HAL_VA_START));
    MempGetOrCreatePageDir(PdeBase, VAtoPDI(MM_HAL_VA_START + 2 * 1024 * 1024));

    return TRUE;
}

VOID
WinLdrSetProcessorContext(
    _In_ USHORT OperatingSystemVersion)
{
    ULONG64 Sctlr;
    ULONG64 Mair;
    ULONG64 Tcr;
    ULONG64 Ttbr0;
    ULONG64 Ttbr1;
    ULONG64 TcrIps;

    UNREFERENCED_PARAMETER(OperatingSystemVersion);

    TRACE("WinLdrSetProcessorContext (ARM64)\n");
    TRACE("Initial SCTLR=%I64x TCR=%I64x MAIR=%I64x TTBR0=%I64x TTBR1=%I64x\n",
          Arm64ReadSctlrEl1(),
          Arm64ReadTcrEl1(),
          Arm64ReadMairEl1(),
          Arm64ReadTtbr0El1(),
          Arm64ReadTtbr1El1());

    Sctlr = Arm64ReadSctlrEl1();
    if (Sctlr & ARM64_SCTLR_M)
    {
        ULONG64 LiveTcr;
        ULONG64 PatchedTcr;

        /* The firmware TCR may have T1SZ=0 (TTBR1 unused / degenerate).
         * Patch the T1 fields to match our 4 KB, 48-bit (T1SZ=16) page
         * tables while preserving the T0 half (T0SZ, TG0, SH0, IRGN0,
         * ORGN0) and the IPS field.
         *
         * CRITICAL: do NOT write TTBR0 while running through the live
         * firmware translation tables.  The instruction fetch stream uses
         * TTBR0 at low VAs; replacing it mid-stream would require the new
         * root to have a byte-perfect identity map for the exact pages
         * being executed.  The firmware TTBR0 already provides that — only
         * TTBR1 (kernel high-VA window) is safe to update here. */
#define ARM64_TCR_T1_MASK \
    ((0x3FULL << 16) | ARM64_TCR_EPD1 | (3ULL << 24) | (3ULL << 26) | (3ULL << 28) | (3ULL << 30))

        Ttbr1    = (ULONG64)PxeBase;
        LiveTcr  = Arm64ReadTcrEl1();
        PatchedTcr = (LiveTcr & ~(ULONG64)ARM64_TCR_T1_MASK) |
                     ARM64_TCR_T1SZ(16) | ARM64_TCR_IRGN1_WBWA | ARM64_TCR_ORGN1_WBWA |
                     ARM64_TCR_SH1_INNER | ARM64_TCR_TG1_4KB;

        TRACE("Firmware MMU enabled: enabling TTBR1 walks, patching TCR T1 fields, updating TTBR1 only\n");
        TRACE("LiveTCR=%I64x PatchedTCR=%I64x TTBR1=%I64x MAIR=%I64x\n",
              LiveTcr, PatchedTcr, Ttbr1, Arm64ReadMairEl1());

        /* Ensure all page-table writes reach the table-walker before we
         * reprogram the translation registers. */
        Arm64Dsb();
        Arm64WriteTcrEl1(PatchedTcr);
        Arm64WriteTtbr1El1(Ttbr1);
        Arm64InvalidateAllTlb();

        TRACE("Post-switch TTBR0=%I64x TTBR1=%I64x MAIR=%I64x TCR=%I64x SCTLR=%I64x\n",
              Arm64ReadTtbr0El1(),
              Arm64ReadTtbr1El1(),
              Arm64ReadMairEl1(),
              Arm64ReadTcrEl1(),
              Arm64ReadSctlrEl1());
        TRACE("WinLdrSetProcessorContext (ARM64) exit\n");
        return;
    }

    /* Program a known 4KB translation regime instead of inheriting firmware TCR. */
    Mair = ARM64_MAIR_DEFAULT;
    TcrIps = Arm64GetTcrIps();
    Tcr = ARM64_TCR_DEFAULT(TcrIps);
    Ttbr0 = (ULONG64)PxeBase;
    Ttbr1 = (ULONG64)PxeBase;

    TRACE("Program MAIR=%I64x TCR=%I64x TTBR0=%I64x TTBR1=%I64x IPS=%I64x MMFR0=%I64x\n",
          Mair,
          Tcr,
          Ttbr0,
          Ttbr1,
          TcrIps,
          Arm64ReadAa64Mmfr0El1());
    Arm64WriteMairEl1(Mair);
    Arm64WriteTcrEl1(Tcr);
    /* Keep low-VA execution valid across MMU enable while still serving kernel VA. */
    Arm64WriteTtbr0El1(Ttbr0);
    Arm64WriteTtbr1El1(Ttbr1);
    Arm64InvalidateAllTlb();
    TRACE("Post-write TCR=%I64x MAIR=%I64x TTBR0=%I64x TTBR1=%I64x\n",
          Arm64ReadTcrEl1(),
          Arm64ReadMairEl1(),
          Arm64ReadTtbr0El1(),
          Arm64ReadTtbr1El1());

    /* Ensure MMU + caches are on before jumping into the kernel context. */
    TRACE("Pre-enable SCTLR=%I64x\n", Sctlr);
    Sctlr |= (ARM64_SCTLR_M | ARM64_SCTLR_C | ARM64_SCTLR_I);
    Arm64WriteSctlrEl1(Sctlr);
    TRACE("Post-enable SCTLR=%I64x\n", Arm64ReadSctlrEl1());
    TRACE("WinLdrSetProcessorContext (ARM64) exit\n");
}

VOID
WinLdrSetupMachineDependent(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PVOID SharedUserDataAddress;

    UNREFERENCED_PARAMETER(LoaderBlock);

    SharedUserDataAddress = MmAllocateMemoryWithType(MM_PAGE_SIZE, LoaderStartupPcrPage);
    if (SharedUserDataAddress == NULL)
    {
        UiMessageBox("Can't allocate SharedUserData page.");
        return;
    }

    SharedUserDataPfn = (PFN_NUMBER)((ULONG_PTR)SharedUserDataAddress >> MM_PAGE_SHIFT);
    RtlZeroMemory(SharedUserDataAddress, MM_PAGE_SIZE);

    TRACE("Allocated SharedUserData page VA=%p PFN=0x%lx\n",
          SharedUserDataAddress,
          SharedUserDataPfn);

    if (!MempAllocatePageTables())
    {
        UiMessageBox("Can't allocate ARM64 loader page tables.");
        return;
    }

    if (!WinLdrMapSpecialPages())
    {
        UiMessageBox("Can't map ARM64 special pages.");
    }
}

VOID
MempDump(VOID)
{
}
