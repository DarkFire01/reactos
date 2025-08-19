/*
 * PROJECT:         ReactOS Boot Loader
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            boot/freeldr/freeldr/arch/arm/winldr.c
 * PURPOSE:         ARM Kernel Loader
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES ***************************************************************/

#include <freeldr.h>
#include <debug.h>
#include <internal/arm/mm.h>
#include <internal/arm/intrin_i.h>
#include "../../winldr.h"
DBG_DEFAULT_CHANNEL(WINDOWS);


#ifdef UEFIBOOT
extern PVOID OsLoaderBase;
extern SIZE_T OsLoaderSize;
#endif


/*
 * Disables the MMU, I-cache, and D-cache.
 * This is a safe preliminary step before reconfiguring the MMU.
 */
void ArmDisableMMUAndCaches(void);

/*
 * Invalidates the entire instruction cache.
 */
void ArmInvalidateICache(void);

/*
 * Cleans and invalidates the entire data/unified cache.
 * This is critical to ensure page table writes are visible to the MMU.
 */
void ArmCleanAndInvalidateDCache(void);

/*
 * Invalidates the entire Translation Lookaside Buffer (TLB).
 */
void ArmInvalidateTlb(void);

/*
 * An instruction synchronization barrier, used to ensure previous
 * instructions (like an MCR to change system state) have completed.
 */
void ArmInstructionSynchronizationBarrier(void);

/*
 * The final step: Enables the MMU and caches by writing to the
 * system control register (SCTLR).
 *
 * @param TtbRegister The value for the Translation Table Base Register (TTBR0).
 * @param DomainRegister The value for the Domain Access Control Register (DACR).
 * @param ControlRegister The final value for the System Control Register (SCTLR).
 */
void ArmEnableMMU(unsigned int TtbRegister,
                  unsigned int DomainRegister,
                  unsigned int ControlRegister);

/*
 * Disables IRQ and FIQ interrupts by setting the I and F bits in the CPSR.
 */
void ArmDisableInterrupts(void);


EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

// The L1 Page Table will be allocated and aligned dynamically.
static unsigned int* L1_PageTable = NULL;

// The L2 Page Table Pool will be allocated dynamically.
static PUCHAR g_L2PageTablePoolBuffer = NULL;
static ULONG g_L2PageTablePoolSizeInKb = 0;
static ULONG g_NextFreeL2Table = 0;

// Base address for the new kernel stack
static PVOID g_KernelStackPhysicalBase = NULL;

/*
 * ============================================================================
 * ARMv7 Short-Descriptor Page Table Entry Definitions
 * ============================================================================
 */
#define L1_TYPE_FAULT           (0 << 0)
#define L1_TYPE_COARSE_L2       (1 << 0)
#define L1_TYPE_SECTION         (2 << 0)
#define L1_SECT_AP_RW_ALL       (3 << 10)
#define L1_SECT_DOMAIN_0        (0 << 5)
#define L1_SECT_ATTR_NORMAL_WBWA ( (1 << 12) | (3 << 2) )
#define L1_SECT_ATTR_DEVICE     (0)

#define L2_TYPE_SMALL_PAGE      (2 << 0)
#define L2_AP_RW_ALL            ( (3 << 4) )
#define L2_ATTR_NORMAL_WBWA     ( (1 << 12) | (3 << 2) )

// Virtual address for the top of our new stack.
#define KERNEL_STACK_VIRTUAL_TOP 0x80200000

// Globals provided by the UEFI entry point
extern EFI_HANDLE GlobalImageHandle;
extern EFI_SYSTEM_TABLE* GlobalSystemTable;
extern PVOID OsLoaderBase;
extern SIZE_T OsLoaderSize;


/* FORWARD DECLARATIONS ***************************************************/
static void ArmMapSmallPage(unsigned int VirtualAddr, unsigned int PhysicalAddr, unsigned int Attributes);
BOOLEAN MempSetupPaging(IN PFN_NUMBER StartPage, IN PFN_COUNT NumberOfPages, IN BOOLEAN KernelMapping);

/* FUNCTIONS **************************************************************/
extern KERNEL_ENTRY_POINT PubKiSystemStartup;
extern  PLOADER_PARAMETER_BLOCK PubLoaderBlockVA;
static void
ArmMapSmallPage(unsigned int VirtualAddr, unsigned int PhysicalAddr, unsigned int L2Attributes)
{
    unsigned int l1_index = VirtualAddr >> 20;
    unsigned int l2_index = (VirtualAddr >> 12) & 0xFF;
    unsigned int l1_entry = L1_PageTable[l1_index];
    unsigned int* l2_table;

    if ((l1_entry & 0x3) == L1_TYPE_SECTION) return; // Already covered by a 1MB section

    if ((l1_entry & 0x3) != L1_TYPE_COARSE_L2)
    {
        if (g_NextFreeL2Table >= g_L2PageTablePoolSizeInKb)
        {
            TRACE("FATAL: Out of L2 page tables!\n");
            for(;;);
        }
        l2_table = (unsigned int*)(g_L2PageTablePoolBuffer + (g_NextFreeL2Table * 1024));
        g_NextFreeL2Table++;

        RtlZeroMemory(l2_table, 1024);
        L1_PageTable[l1_index] = ((unsigned int)l2_table & 0xFFFFFC00) | L1_TYPE_COARSE_L2;
    }
    else
    {
        l2_table = (unsigned int*)(l1_entry & 0xFFFFFC00);
    }
    l2_table[l2_index] = (PhysicalAddr & 0xFFFFF000) | L2Attributes;
}

VOID
WinLdrSetupMachineDependent(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG_PTR UnalignedBuffer, PageTablePhysAddr;
    const ULONG Alignment = 16384;

    TRACE("WinLdrSetupMachineDependent: Setting up full page tables...\n");

    // 1. Allocate memory for the new Kernel Stack
    g_KernelStackPhysicalBase = MmAllocateMemoryWithType(KERNEL_STACK_SIZE, LoaderLoadedProgram);
    if (!g_KernelStackPhysicalBase)
    {
        TRACE("FATAL: Failed to allocate kernel stack!\n");
        for(;;);
    }
    
    // 2. Allocate and manually align the L1 Page Table.
    UnalignedBuffer = (ULONG_PTR)MmAllocateMemoryWithType(Alignment + (Alignment - 1), LoaderMemoryData);
    if (!UnalignedBuffer)
    {
        TRACE("FATAL: Failed to allocate memory for L1 page table!\n");
        for(;;);
    }
    L1_PageTable = (unsigned int*)((UnalignedBuffer + (Alignment - 1)) & ~(Alignment - 1));
    RtlZeroMemory(L1_PageTable, Alignment);
    PageTablePhysAddr = (ULONG_PTR)L1_PageTable;
    
    // 3. Allocate the L2 Page Table Pool.
    ULONG num_l2_tables_for_phys = (TotalPagesInLookupTable * 4096) / (1024 * 1024) + 1;
    ULONG num_l2_tables_for_kernel = num_l2_tables_for_phys;
    g_L2PageTablePoolSizeInKb = (num_l2_tables_for_phys + num_l2_tables_for_kernel);
    g_L2PageTablePoolBuffer = MmAllocateMemoryWithType(g_L2PageTablePoolSizeInKb * 1024, LoaderMemoryData);
    if (!g_L2PageTablePoolBuffer)
    {
        TRACE("FATAL: Failed to allocate L2 page table pool!\n");
        for(;;);
    }
    g_NextFreeL2Table = 0;
    
    // 4. Create the critical 1-to-1 identity mappings BEFORE mapping anything else.
    unsigned int ram_attrs = L1_TYPE_SECTION | L1_SECT_AP_RW_ALL | L1_SECT_DOMAIN_0 | L1_SECT_ATTR_NORMAL_WBWA;
    
    // Map the first 1MB of RAM for exception vectors.
    L1_PageTable[0] = (0x00000000 & 0xFFF00000) | ram_attrs;

    // Map the L1 Page Table's own memory region for cache coherency.
    ULONG_PTR PageTableSection = PageTablePhysAddr & 0xFFF00000;
    L1_PageTable[PageTableSection >> 20] = (PageTableSection & 0xFFF00000) | ram_attrs;

    // Map the bootloader's own code region.
    ULONG_PTR FreeldrBase = (ULONG_PTR)OsLoaderBase;
    ULONG_PTR FreeldrEnd = FreeldrBase + OsLoaderSize;
    for (ULONG_PTR Addr = (FreeldrBase & 0xFFF00000); Addr < FreeldrEnd; Addr += (1024*1024))
    {
        L1_PageTable[Addr >> 20] = (Addr & 0xFFF00000) | ram_attrs;
    }

    // --- FIX: Manually add the UART mapping for positive confirmation ---
    #define QEMU_UART_BASE 0x09000000
    unsigned int device_attrs = L1_TYPE_SECTION | L1_SECT_AP_RW_ALL | L1_SECT_DOMAIN_0 | L1_SECT_ATTR_DEVICE;
    L1_PageTable[QEMU_UART_BASE >> 20] = (QEMU_UART_BASE & 0xFFF00000) | device_attrs;
    TRACE("Manually mapping UART at PA/VA 0x%X for verification.\n", QEMU_UART_BASE);

    // 5. Map the newly allocated kernel stack to its virtual address
    const unsigned int l2_stack_attributes = L2_TYPE_SMALL_PAGE | L2_AP_RW_ALL | L2_ATTR_NORMAL_WBWA;
    for (int i = 0; i < (KERNEL_STACK_SIZE / 4096); i++)
    {
        unsigned int pa = (unsigned int)g_KernelStackPhysicalBase + (i * 4096);
        unsigned int va = (KERNEL_STACK_VIRTUAL_TOP - KERNEL_STACK_SIZE) + (i * 4096);
        ArmMapSmallPage(va, pa, l2_stack_attributes);
    }
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

    ArmDisableInterrupts();
    ArmDisableMMUAndCaches();
    ArmCleanAndInvalidateDCache();
    ArmInvalidateICache();
    ArmInvalidateTlb();

    unsigned int ttb_register = (unsigned int)L1_PageTable;
    unsigned int domain_register = 0x1;
    unsigned int control_register = (1 << 12) | (1 << 11) | (1 << 2) | (1 << 0); // I, Z, C, M

    ArmEnableMMU(ttb_register, domain_register, control_register);

    PubKiSystemStartup(PubLoaderBlockVA);
}

VOID
MempDump(VOID)
{
    return;
}

BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage,
                IN PFN_COUNT NumberOfPages,
                IN BOOLEAN KernelMapping)
{
    const unsigned int l2_mem_attributes = L2_TYPE_SMALL_PAGE | L2_AP_RW_ALL | L2_ATTR_NORMAL_WBWA;

    for (PFN_COUNT i = 0; i < NumberOfPages; i++)
    {
        unsigned int p_addr = (StartPage + i) * 4096;
        ArmMapSmallPage(p_addr, p_addr, l2_mem_attributes);

        if (KernelMapping)
        {
            #define KSEG0_BASE 0x80000000
            unsigned int v_addr = KSEG0_BASE + p_addr;
            ArmMapSmallPage(v_addr, p_addr, l2_mem_attributes);
        }
    }

    return TRUE;
}
