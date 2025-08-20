/*
 * PROJECT:         ReactOS Boot Loader
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            boot/freeldr/freeldr/arch/arm/winldr.c
 * PURPOSE:         Full-featured ARMv7 UEFI Kernel Loader with all MMU fixes
 */


#include <freeldr.h>
#include <debug.h>
#include <internal/arm/mm.h>
#include <internal/arm/intrin_i.h>
#include "../../winldr.h"
#include <stdint.h>
DBG_DEFAULT_CHANNEL(WINDOWS);


#ifdef UEFIBOOT
extern PVOID OsLoaderBase;
extern SIZE_T OsLoaderSize;
#endif


/*
 * Disables the MMU, I-cache, and D-cache.
 */
void ArmDisableMMUAndCaches(void);

/*
 * Invalidates the entire instruction cache.
 */
void ArmInvalidateICache(void);

/*
 * Cleans and invalidates the entire data/unified cache.
 */
void ArmCleanAndInvalidateDCache(void);

/*
 * Invalidates the entire Translation Lookaside Buffer (TLB).
 */
void ArmInvalidateTlb(void);

/*
 * The final step: Enables the MMU and caches by writing to the
 * system control register (SCTLR).
 */
void ArmEnableMMU(unsigned int TtbRegister,
                  unsigned int DomainRegister,
                  unsigned int ControlRegister,
                  unsigned int StackPointerVA);

/*
 * Disables IRQ and FIQ interrupts by setting the I and F bits in the CPSR.
 */
void ArmDisableInterrupts(void);

void ArmDisableDCache(void);

EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;


// LPAE: 3-level page tables, 64-bit entries
#define LPAE_PGD_ENTRIES 512
#define LPAE_PMD_ENTRIES 512
#define LPAE_PTE_ENTRIES 512

typedef uint64_t lpae_pgd_entry_t;
typedef uint64_t lpae_pmd_entry_t;
typedef uint64_t lpae_pte_entry_t;

static lpae_pgd_entry_t* LPAE_PGD = NULL;
static lpae_pmd_entry_t* LPAE_PMD_POOL = NULL;
static lpae_pte_entry_t* LPAE_PTE_POOL = NULL;
static ULONG g_NextFreePMD = 0;
static ULONG g_NextFreePTE = 0;

// Base address for the new kernel stack
static PVOID g_KernelStackPhysicalBase = NULL;
ULONG_PTR BasicStack;
/*
 * ============================================================================
 * ARMv7 Short-Descriptor Page Table Entry Definitions
 * ============================================================================
 */


// LPAE descriptor bits
#define LPAE_TYPE_BLOCK         (0x1UL << 0)
#define LPAE_TYPE_TABLE         (0x3UL << 0)
#define LPAE_TYPE_PAGE          (0x3UL << 0)

#define LPAE_AF                 (1UL << 10) // Access Flag
#define LPAE_SH_NONE            (0UL << 8)
#define LPAE_SH_OUTER           (2UL << 8)
#define LPAE_SH_INNER           (3UL << 8)
#define LPAE_AP_RW              (0UL << 6)
#define LPAE_AP_RO              (1UL << 6)
#define LPAE_AP_USER            (1UL << 7)
#define LPAE_ATTRINDX(idx)      ((idx) << 2)

// MAIR: Memory Attribute Indirection Register
#define LPAE_MAIR_ATTR_DEVICE_nGnRE 0x00UL
#define LPAE_MAIR_ATTR_NORMAL_WB_RA_WA 0xFFUL

#define LPAE_MAIR_VALUE ((LPAE_MAIR_ATTR_DEVICE_nGnRE << 0) | (LPAE_MAIR_ATTR_NORMAL_WB_RA_WA << 8))

#define LPAE_ATTR_DEVICE 0
#define LPAE_ATTR_NORMAL 1

// Virtual address for the top of our new stack.
#define KERNEL_STACK_VIRTUAL_TOP 0x80200000

// Globals provided by the UEFI entry point
extern EFI_HANDLE GlobalImageHandle;
extern EFI_SYSTEM_TABLE* GlobalSystemTable;

/* FORWARD DECLARATIONS ***************************************************/

static void LPAE_MapPage(uintptr_t va, uintptr_t pa, uint64_t attr_idx, uint64_t sh, uint64_t ap);
BOOLEAN MempSetupPaging(IN PFN_NUMBER StartPage, IN PFN_COUNT NumberOfPages, IN BOOLEAN KernelMapping);

/* FUNCTIONS **************************************************************/
extern KERNEL_ENTRY_POINT PubKiSystemStartup;
extern  PLOADER_PARAMETER_BLOCK PubLoaderBlockVA;



static void LPAE_MapPage(uintptr_t va, uintptr_t pa, uint64_t attr_idx, uint64_t sh, uint64_t ap)
{
    // 3-level translation: PGD (L1), PMD (L2), PTE (L3)
    size_t pgd_idx = (va >> 30) & 0x1FF;
    size_t pmd_idx = (va >> 21) & 0x1FF;
    size_t pte_idx = (va >> 12) & 0x1FF;

    // Allocate PMD if not present
    if (!(LPAE_PGD[pgd_idx] & 0x1)) {
        lpae_pmd_entry_t* new_pmd = &LPAE_PMD_POOL[g_NextFreePMD * LPAE_PMD_ENTRIES];
        g_NextFreePMD++;
        RtlZeroMemory(new_pmd, LPAE_PMD_ENTRIES * sizeof(lpae_pmd_entry_t));
        LPAE_PGD[pgd_idx] = ((uintptr_t)new_pmd & ~0xFFFUL) | LPAE_TYPE_TABLE;
    }
    lpae_pmd_entry_t* pmd = (lpae_pmd_entry_t*)(LPAE_PGD[pgd_idx] & ~0xFFFUL);

    // Allocate PTE if not present
    if (!(pmd[pmd_idx] & 0x1)) {
        lpae_pte_entry_t* new_pte = &LPAE_PTE_POOL[g_NextFreePTE * LPAE_PTE_ENTRIES];
        g_NextFreePTE++;
        RtlZeroMemory(new_pte, LPAE_PTE_ENTRIES * sizeof(lpae_pte_entry_t));
        pmd[pmd_idx] = ((uintptr_t)new_pte & ~0xFFFUL) | LPAE_TYPE_TABLE;
    }
    lpae_pte_entry_t* pte = (lpae_pte_entry_t*)(pmd[pmd_idx] & ~0xFFFUL);

    // Set PTE
    pte[pte_idx] = (pa & ~0xFFFUL) | LPAE_TYPE_PAGE | LPAE_AF | sh | ap | LPAE_ATTRINDX(attr_idx);
}

VOID

WinLdrSetupMachineDependent(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    BasicStack = ((ULONG_PTR)0x32000 + (ULONG_PTR)MmAllocateMemoryWithType(0x32000, LoaderOsloaderStack));

    #define KERNEL_STACK_SIZE_LOC (16 * 1024)
    TRACE("WinLdrSetupMachineDependent: Setting up LPAE page tables...\n");

    g_KernelStackPhysicalBase = MmAllocateMemoryWithType(KERNEL_STACK_SIZE_LOC, LoaderLoadedProgram);

    // Allocate and zero LPAE page tables
    LPAE_PGD = (lpae_pgd_entry_t*)MmAllocateMemoryWithType(4096, LoaderMemoryData); // 4KB aligned
    LPAE_PMD_POOL = (lpae_pmd_entry_t*)MmAllocateMemoryWithType(4096 * 8, LoaderMemoryData); // 8 PMD tables
    LPAE_PTE_POOL = (lpae_pte_entry_t*)MmAllocateMemoryWithType(4096 * 32, LoaderMemoryData); // 32 PTE tables
    g_NextFreePMD = 0;
    g_NextFreePTE = 0;
    RtlZeroMemory(LPAE_PGD, 4096);
    RtlZeroMemory(LPAE_PMD_POOL, 4096 * 8);
    RtlZeroMemory(LPAE_PTE_POOL, 4096 * 32);

    // Map loader (identity and kernel VA)
    ULONG_PTR FreeldrBase = (ULONG_PTR)OsLoaderBase;
    ULONG_PTR FreeldrEnd = FreeldrBase + OsLoaderSize;
    for (ULONG_PTR pa = FreeldrBase; pa < FreeldrEnd; pa += 4096)
    {
        LPAE_MapPage(pa, pa, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
        LPAE_MapPage(pa + 0x80000000, pa, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
    }

    // Map stack
    for (int i = 0; i < (KERNEL_STACK_SIZE_LOC / 4096); i++)
    {
        uintptr_t pa = ((uintptr_t)BasicStack - KERNEL_STACK_SIZE_LOC) + (i * 4096);
        uintptr_t va = (BasicStack - KERNEL_STACK_SIZE_LOC) + (i * 4096);
        LPAE_MapPage(va, pa, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
    }

    // Map UART as device
    #define QEMU_UART_BASE 0x09000000
    LPAE_MapPage(QEMU_UART_BASE, QEMU_UART_BASE, LPAE_ATTR_DEVICE, LPAE_SH_NONE, LPAE_AP_RW);
    TRACE("Manually mapping UART at PA/VA 0x%X for kernel use.\n", QEMU_UART_BASE);
    /* Map KI_USER_SHARED_DATA */

    
    /* Allocate 2 pages for PCR: one for the boot processor PCR and one for KI_USER_SHARED_DATA */
    ULONG_PTR KiSharedNuts = (ULONG_PTR)MmAllocateMemoryWithType(1 * MM_PAGE_SIZE, LoaderStartupPcrPage);
    LPAE_MapPage(KI_USER_SHARED_DATA, (uintptr_t)KiSharedNuts, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
    LPAE_MapPage(0xFFFF0000, 0xFFFF0000, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
    LPAE_MapPage(0, 0, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
    // Optionally: map more RAM, page table pool, etc.
}

VOID

WinLdrSetProcessorContext(_In_ USHORT OperatingSystemVersion)
{
    EFI_STATUS Status;
    UINTN MapKey, DescriptorSize;
    EFI_MEMORY_DESCRIPTOR* MemoryMap;
    UINT32 DescriptorVersion;
    UINTN MemoryMapSize = 0;

    GlobalSystemTable->BootServices->GetMemoryMap(&MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    GlobalSystemTable->BootServices->AllocatePool(EfiLoaderData, MemoryMapSize, (VOID**)&MemoryMap);
    Status = GlobalSystemTable->BootServices->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR(Status))
    {
        for(;;);
    }
    Status = GlobalSystemTable->BootServices->ExitBootServices(GlobalImageHandle, MapKey);
    if (EFI_ERROR(Status))
    {
        for (;;);
    }
    // Clean/invalidate caches and TLBs
    ArmDisableMMUAndCaches();
    ArmCleanAndInvalidateDCache();
    ArmInvalidateICache();
    ArmInvalidateTlb();

    // Prepare SCTLR value (enable MMU, I-cache, D-cache)
    unsigned int sctlr = 0;
    sctlr |= 0x1;        // Enable MMU
    sctlr |= (1 << 12);  // I-cache
    sctlr |= (1 << 2);   // D-cache

    // Call ArmEnableMMU for LPAE: r0=TTBR0 (PGD), r1=0 (unused), r2=SCTLR, r3=stack
    ArmEnableMMU((unsigned int)LPAE_PGD, 0, sctlr, (unsigned int)BasicStack);

    // Should not return
    for(;;);
}

VOID
MempDump(VOID)
{
    return;
}


void
JumpToKerneTwol()
{
    TRACE("Hello from paged mode, KiSystemStartup %p, LoaderBlockVA %p!\n",
          PubKiSystemStartup, PubLoaderBlockVA);
    (*PubKiSystemStartup)(PubLoaderBlockVA);
}

void
JumpToKernel()
{
    TRACE("\nPREPPING JUMP....\n");
 JumpToKerneTwol();
    TRACE("two\n");
    for(;;)
    {

    }   
}



BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage, IN PFN_COUNT NumberOfPages, IN BOOLEAN KernelMapping)
{
    for (PFN_COUNT i = 0; i < NumberOfPages; i++)
    {
        uintptr_t p_addr = (StartPage + i) * 4096;
        LPAE_MapPage(p_addr, p_addr, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
        if (KernelMapping)
        {
            uintptr_t v_addr = p_addr + 0x80000000;
            LPAE_MapPage(v_addr, p_addr, LPAE_ATTR_NORMAL, LPAE_SH_INNER, LPAE_AP_RW);
        }
    }
    return TRUE;
}
