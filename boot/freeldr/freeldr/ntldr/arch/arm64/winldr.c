
#include <freeldr.h>
#include <debug.h>
DBG_DEFAULT_CHANNEL(WINDOWS);
#include <internal/arm/mm.h>
//#include <internal/arm/intrin_i.h>
#include "../../winldr.h"

PLOADER_PARAMETER_BLOCK PubLoaderBlockVA;
KERNEL_ENTRY_POINT PubKiSystemStartup;

extern PVOID OsLoaderBase;
extern SIZE_T OsLoaderSize;

PHARDWARE_PTE Ttbr1_addr;
PHARDWARE_PTE Ttbr0_addr;
#define PtrToPfn(p) \
    ((((ULONGLONG)p) >> PAGE_SHIFT) & 0xfffffffULL)

    /* different names but same offset on arm64 */                                
#define PTI_SHIFT  12L
#define PDI_SHIFT  21L
#define PPI_SHIFT  30L
#define PXI_SHIFT  39L

#define VAtoPXI(va) ((((ULONG64)(va)) >> PXI_SHIFT) & 0x1FF)
#define VAtoPPI(va) ((((ULONG64)(va)) >> PPI_SHIFT) & 0x1FF)
#define VAtoPDI(va) ((((ULONG64)(va)) >> PDI_SHIFT) & 0x1FF)
#define VAtoPTI(va) ((((ULONG64)(va)) >> PTI_SHIFT) & 0x1FF)
#define PtrToPfn(p) \
    ((((ULONGLONG)p) >> PAGE_SHIFT) & 0xfffffffULL)

/* FUNCTIONS **************************************************************/

PFN_NUMBER PhysicalToVirtualAddress(UINT64 PhyAddr)
{
	return PhyAddr | (0b1111111111111111ULL << 48);
}

static
BOOLEAN
WinLdrMapSpecialPages()
{
    TRACE("WinLdrMapSpecialPages: Entry\n");
    /* TODO: Map in the kernel CPTs */
    return TRUE;
}


BOOLEAN
MempIsPageMapped(PVOID VirtualAddress)
{
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
        PdeBase[Index].Write = 1;
    }
    else
    {
        SubDir = (PVOID)((ULONG64)(PdeBase[Index].PageFrameNumber) * PAGE_SIZE);
    }
    return SubDir;
}

static
BOOLEAN
MempMapSinglePage(ULONG64 VirtualAddress, ULONG64 PhysicalAddress)
{
    PHARDWARE_PTE PpeBase, PdeBase, PteBase;
    ULONG Index;

    PpeBase = MempGetOrCreatePageDir(Ttbr1_addr, VAtoPXI(VirtualAddress));
    PdeBase = MempGetOrCreatePageDir(PpeBase, VAtoPPI(VirtualAddress));
    PteBase = MempGetOrCreatePageDir(PdeBase, VAtoPDI(VirtualAddress));

    if (!PteBase)
    {
        ERR("!!!No Dir %p, %p, %p, %p\n", Ttbr1_addr, PpeBase, PdeBase, PteBase);
        return FALSE;
    }

    Index = VAtoPTI(VirtualAddress);
    if (PteBase[Index].Valid)
    {
        ERR("!!!Already mapped %ld\n", Index);
        return FALSE;
    }

    PteBase[Index].Valid = 1;
    PteBase[Index].Write = 1;
    PteBase[Index].PageFrameNumber = PhysicalAddress / PAGE_SIZE;
    return TRUE;
}

static
PFN_NUMBER
MempMapRangeOfPages(ULONG64 VirtualAddress, ULONG64 PhysicalAddress, PFN_NUMBER cPages, BOOLEAN IsTtbr1)
{
    PFN_NUMBER i;

    for (i = 0; i < cPages; i++)
    {
        if (!MempMapSinglePage(VirtualAddress, PhysicalAddress))
        {
            ERR("Failed to map page %ld from %p to %p\n",
                    i, (PVOID)VirtualAddress, (PVOID)PhysicalAddress);
            return i;
        }
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
            StartPage, NumberOfPages, StartPage * PAGE_SIZE + KSEG0_BASE);

    /* Kernel mapping - TTBR1 */
    if (KernelMapping)
    {
        if (MempMapRangeOfPages(StartPage * PAGE_SIZE + KSEG0_BASE,
                                StartPage * PAGE_SIZE,
                                NumberOfPages, TRUE) != NumberOfPages)
        {
            ERR("Failed to map pages %ld, %ld\n",
                    StartPage, NumberOfPages);
            return FALSE;
        }
    }

    /* map this */
    return TRUE;
}

void write_ttbr1_el1(ULONG_PTR Tbbr1Addr);
void write_ttbr0_el1(ULONG_PTR Tbbr0Addr);
void SmashTlb();
UINT64 GetSCTLREL1();
void SetSCTLREL1(UINT64 Value);
void SetSCTLREL1_hold(UINT64 Value);
static
BOOLEAN
MempAllocatePageTables(VOID)
{
    TRACE(">>> MempAllocatePageTables\n");

    /* Allocate a page for the PML4 */
    Ttbr1_addr = MmAllocateMemoryWithType(PAGE_SIZE, LoaderMemoryData);
    TRACE("Page table base %X\n", Ttbr1_addr);
    if (!Ttbr1_addr)
    {
        ERR("failed to allocate PML4\n");
        return FALSE;
    }

    /* Zero the PML4 */
    RtlZeroMemory(Ttbr1_addr, PAGE_SIZE);

    /* The page tables are located at 0xfffff68000000000
     * We create a recursive self mapping through all 4 levels at
     * virtual address 0xfffff6fb7dbedf68 */
    Ttbr1_addr[VAtoPXI(PXE_BASE)].Valid = 1;
    Ttbr1_addr[VAtoPXI(PXE_BASE)].Write = 1;
    Ttbr1_addr[VAtoPXI(PXE_BASE)].PageFrameNumber = PtrToPfn(Ttbr1_addr);

   // PxeBase[VAtoPXI(PXE_BASE)].UserNoExecute = 1
    // FIXME: map PDE's for hals memory mapping

    TRACE(">>> leave MempAllocatePageTables\n");

    return TRUE;
}
void TriggerIsb();
void HoldForArmTests();
void  write_tcr(UINT64 tcr);
UINT64  read_tcr();
/* ************************************************************************/
#define MMUBIT 1;
#define DISABLE_MMU_BIT 0
VOID
WinLdrSetProcessorContext(VOID)
{

    TRACE("WinLdrSetProcessorContext: Entry\n");

    Ttbr0_addr[0].Valid = 1;
    Ttbr0_addr[0].Write = 1;
    Ttbr0_addr[0].PageFrameNumber = PtrToPfn(0);
    UINT64 tcr = read_tcr();

    tcr &= (0b10100 << 16) | (0b0 << 23) | (0b01 << 24) | (0b01 << 26) | (0b10 << 28) | (0b10 << 30) | ((UINT64)0b100 << 32) | ((UINT64)0b0 << 55) | ((UINT64)0b1 << 56);
    tcr |= (0b10100 << 16) | (0b0 << 23) | (0b01 << 24) | (0b01 << 26) | (0b10 << 28) | (0b10 << 30) | ((UINT64)0b100 << 32) | ((UINT64)0b0 << 55) | ((UINT64)0b1 << 56);
    TRACE("WinLdrSetProcessorContext: Entry\n");
    //UINT64 tcr = 0x4A5102510;

    UINT64 Systemctrl = 0;
    Systemctrl = GetSCTLREL1();
    Systemctrl |= DISABLE_MMU_BIT;
    SetSCTLREL1(Systemctrl);
    TRACE("Paging Disabled\n");
    /* FREE TO MODIFY SHIT I SHOULDNT >:D ((((((((((((((((((()))))))))))))))))))*/
    write_ttbr1_el1((ULONG_PTR)Ttbr1_addr);
    //write_ttbr0_el1((ULONG_PTR)Ttbr0_addr);

    write_tcr(tcr);
    TRACE(" setup new tcr\n");
    TriggerIsb();
    Systemctrl = 0;
    Systemctrl = GetSCTLREL1();
    Systemctrl |= MMUBIT;
    TRACE(" enabling paging\n");
    SetSCTLREL1_hold(Systemctrl);
    TriggerIsb();
    HoldForArmTests();

   // TRACE("Paging enabled\n");
    for(;;)
    {

    }
    /* enable paging */
}
extern KERNEL_ENTRY_POINT PubKiSystemStartup;
extern PLOADER_PARAMETER_BLOCK PubLoaderBlock;
VOID
ContinueExecution()
{
    TRACE("test");
    (*PubKiSystemStartup)(PubLoaderBlock);
    for(;;)
    {

    }
}
VOID
WinLdrSetupMachineDependent(
    PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    TRACE("\n\nWinLdrSetupMachineDependent: Entry\n");
    /* Create page tables */
    if (MempAllocatePageTables() == FALSE)
    {
        TRACE("WinLdrSetupMachineDependent: failed too allocate page tables\n");
        // FIXME: bugcheck
    }

    /* Map stuff like PCR, KI_USER_SHARED_DATA and Apic */
    WinLdrMapSpecialPages();
}

VOID
MempUnmapPage(IN PFN_NUMBER Page)
{
    TRACE("Unmapping %X\n", Page);
    return;
}

VOID
MempDump(VOID)
{
    return;
}
