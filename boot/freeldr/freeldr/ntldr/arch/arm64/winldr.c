/*
 * PROJECT:         EFI Windows Loader
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            boot/freeldr/freeldr/ntldr/arch/arm64/winldr.c
 * PURPOSE:         ARM64 Memory and CPU context routines
 */

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

#define ARM64_MAIR_ATTR_DEVICE_nGnRnE 0x00
#define ARM64_MAIR_ATTR_NORMAL_WB     0xFF
#define ARM64_MAIR_DEFAULT            ((ARM64_MAIR_ATTR_DEVICE_nGnRnE << 0) | (ARM64_MAIR_ATTR_NORMAL_WB << 8))

#define ARM64_SCTLR_M                 (1ULL << 0)
#define ARM64_SCTLR_C                 (1ULL << 2)
#define ARM64_SCTLR_I                 (1ULL << 12)

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
Arm64InvalidateAllTlb(VOID)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(1, 4, 8, 3, 0), 0);
#else
    __asm__ __volatile__("tlbi vmalle1is" ::: "memory");
#endif
    Arm64Dsb();
    Arm64Isb();
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
    PxeBase[VAtoPXI(PXE_BASE)].Valid = 1;
    PxeBase[VAtoPXI(PXE_BASE)].NotLargePage = 1;
    PxeBase[VAtoPXI(PXE_BASE)].Writable = 1;
    PxeBase[VAtoPXI(PXE_BASE)].Accessed = 1;
    PxeBase[VAtoPXI(PXE_BASE)].PageFrameNumber = PtrToPfn(PxeBase);

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
        PdeBase[Index].PageFrameNumber = PtrToPfn(SubDir);
        PdeBase[Index].Valid = 1;
        PdeBase[Index].NotLargePage = 1;
        PdeBase[Index].Writable = 1;
        PdeBase[Index].Accessed = 1;
    }
    else
    {
        SubDir = (PVOID)((ULONG64)PdeBase[Index].PageFrameNumber * PAGE_SIZE);
    }

    return SubDir;
}

static
BOOLEAN
MempMapSinglePage(ULONG64 VirtualAddress, ULONG64 PhysicalAddress)
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
        ERR("VA already mapped: %p\n", (PVOID)VirtualAddress);
        return FALSE;
    }

    PteBase[Index].Valid = 1;
    PteBase[Index].NotLargePage = 1;
    PteBase[Index].Writable = 1;
    PteBase[Index].Accessed = 1;
    PteBase[Index].PageFrameNumber = PhysicalAddress / PAGE_SIZE;

    return TRUE;
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

    if (!MempMapSinglePage(KI_USER_SHARED_DATA, SharedUserDataPfn * PAGE_SIZE))
    {
        ERR("Could not map KI_USER_SHARED_DATA\n");
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

    UNREFERENCED_PARAMETER(OperatingSystemVersion);

    TRACE("WinLdrSetProcessorContext (ARM64)\n");

    /* Install loader's kernel half translation table and attributes. */
    Arm64WriteMairEl1(ARM64_MAIR_DEFAULT);
    Arm64WriteTtbr1El1((ULONG64)PxeBase);
    Arm64InvalidateAllTlb();

    /* Ensure MMU + caches are on before jumping into the kernel context. */
    Sctlr = Arm64ReadSctlrEl1();
    Sctlr |= (ARM64_SCTLR_M | ARM64_SCTLR_C | ARM64_SCTLR_I);
    Arm64WriteSctlrEl1(Sctlr);
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
