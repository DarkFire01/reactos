
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
PHARDWARE_PTE PxeBase;




























 
static
BOOLEAN
WinLdrMapSpecialPages()
{
    TRACE("WinLdrMapSpecialPages: Entry\n");
    /* TODO: Map in the kernel CPTs */
    return TRUE;
}
#define VA_MASK 0x0000FFFFFFFFFFFFUL

#define PtrToPfn(p) \
    ((((ULONGLONG)p) >> PAGE_SHIFT) & 0xfffffffULL)

#define VAtoPXI(va) ((((ULONG64)(va)) >> PXI_SHIFT) & 0x1FF)
#define VAtoPPI(va) ((((ULONG64)(va)) >> PPI_SHIFT) & 0x1FF)
#define VAtoPDI(va) ((((ULONG64)(va)) >> PDI_SHIFT) & 0x1FF)
#define VAtoPTI(va) ((((ULONG64)(va)) >> PTI_SHIFT) & 0x1FF)


static
BOOLEAN
MempMapSinglePage(ULONG64 VirtualAddress, ULONG64 PhysicalAddress);

void write_ttbr1_el1(UINT64 Tbbr1Addr);
void write_ttbr0_el1(UINT64 Tbbr0Addr);
void SmashTlb();
UINT64 GetSCTLREL1();
void SetSCTLREL1(UINT64 Value);
void SetSCTLREL1_hold(UINT64 Value);\


#define PAGESIZE    4096

// granularity
#define PT_PAGE     0b11        // 4k granule
#define PT_BLOCK    0b01        // 2M granule
// accessibility
#define PT_KERNEL   (0<<6)      // privileged, supervisor EL1 access only
#define PT_USER     (1<<6)      // unprivileged, EL0 access allowed
#define PT_RW       (0<<7)      // read-write
#define PT_RO       (1<<7)      // read-only
#define PT_AF       (1<<10)     // accessed flag
#define PT_NX       (1UL<<54)   // no execute
// shareability
#define PT_OSH      (2<<8)      // outter shareable
#define PT_ISH      (3<<8)      // inner shareable
// defined in MAIR register
#define PT_MEM      (0<<2)      // normal memory
#define PT_DEV      (1<<2)      // device MMIO
#define PT_NC       (2<<2)      // non-cachable

#define TTBR_CNP    1
void TriggerIsb();
void HoldForArmTests();
void  write_tcr(UINT64 tcr);
UINT64  read_tcr();
void set_mair_el1(UINT64 mair);
void DisalbeMMU();
static
BOOLEAN
MempAllocatePageTables(VOID)
{
    TRACE(">>> MempAllocatePageTables\n");
  /* Allocate a page for the PML4 */
    PxeBase = (PVOID)MmAllocateMemoryWithType(PAGE_SIZE * 1024, LoaderMemoryData);

    if (!PxeBase)
    {
        ERR("failed to allocate PML4\n");
        return FALSE;
    }




    /* Zero the PML4 */
    RtlZeroMemory(PxeBase, PAGE_SIZE);

    /* The page tables are located at 0xfffff68000000000
*/
    (ULONG_PTR)PxeBase[VAtoPXI(PXE_BASE)].Valid = 1;
                 PxeBase[VAtoPXI(PXE_BASE)].Valid = 1;
     PxeBase[VAtoPXI(PXE_BASE)].Valid = 1;
        PxeBase[VAtoPXI(PXE_BASE)].Valid = 1;
    PxeBase[VAtoPXI(PXE_BASE)].PageFrameNumber = PtrToPfn(PxeBase);

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
        PdeBase[Index].Accessed = 1;
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

    PpeBase = MempGetOrCreatePageDir(PxeBase, VAtoPXI(VirtualAddress));
    PdeBase = MempGetOrCreatePageDir(PpeBase, VAtoPPI(VirtualAddress));
    PteBase = MempGetOrCreatePageDir(PdeBase, VAtoPDI(VirtualAddress));

    if (!PteBase)
    {
        ERR("!!!No Dir %p, %p, %p, %p\n", PxeBase, PpeBase, PdeBase, PteBase);
        return FALSE;
    }

    Index = VAtoPTI(VirtualAddress);
    if (PteBase[Index].Valid)
    {
        ERR("!!!Already mapped %ld\n", Index);
        return FALSE;
    }

     PteBase[Index].PageFrameNumber = (PhysicalAddress / PAGE_SIZE);
     PteBase[Index].Valid = 1;
     PteBase[Index].Write = 1;
        PteBase[Index].Accessed = 1;
    return TRUE;
}

BOOLEAN
MempIsPageMapped(PVOID VirtualAddress)
{
    PHARDWARE_PTE PpeBase, PdeBase, PteBase;
    ULONG Index;

    Index = VAtoPXI(VirtualAddress);
    if (!PxeBase[Index].Valid)
        return FALSE;

    PpeBase = (PVOID)((ULONG64)(PxeBase[Index].PageFrameNumber) * PAGE_SIZE);
    Index = VAtoPPI(VirtualAddress);
    if (!PpeBase[Index].Valid)
        return FALSE;

    PdeBase = (PVOID)((ULONG64)(PpeBase[Index].PageFrameNumber) * PAGE_SIZE);
    Index = VAtoPDI(VirtualAddress);
    if (!PdeBase[Index].Valid)
        return FALSE;

    PteBase = (PVOID)((ULONG64)(PdeBase[Index].PageFrameNumber) * PAGE_SIZE);
    Index = VAtoPTI(VirtualAddress);
    if (!PteBase[Index].Valid)
        return FALSE;

    return TRUE;
}

static
PFN_NUMBER
MempMapRangeOfPages(ULONG64 VirtualAddress, ULONG64 PhysicalAddress, PFN_NUMBER cPages)
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

    /* Identity mapping */
    if (MempMapRangeOfPages(StartPage * PAGE_SIZE,
                            StartPage * PAGE_SIZE,
                            NumberOfPages) != NumberOfPages)
    {
        ERR("Failed to map pages %ld, %ld\n",
                StartPage, NumberOfPages);
        return FALSE;
    }

    /* Kernel mapping */
    if (KernelMapping)
    {
        if (MempMapRangeOfPages(StartPage * PAGE_SIZE + KSEG0_BASE,
                                StartPage * PAGE_SIZE,
                                NumberOfPages) != NumberOfPages)
        {
            ERR("Failed to map pages %ld, %ld\n",
                    StartPage, NumberOfPages);
            return FALSE;
        }
    }

    return TRUE;
}


void WinLdrSetupMachineDependent(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    TRACE("IMage Base Addr %p\n",OsLoaderBase);
    // Before we start mapping pages, create a block of memory, which will contain
    // PDE and PTEs
    if (MempAllocatePageTables() == FALSE)
    {
        // FIXME: bugcheck
    }
}

/* ************************************************************************/
#define MMUBIT 1;
#define DISABLE_MMU_BIT 0
VOID
WinLdrSetProcessorContext(VOID)
{
    
     DisalbeMMU();
    UINT64 r, b;
        b=0x90000000>>21;
    TRACE("WinLdrSetProcessorContext: Entry\n");

   // TRACE("Paging enabled\n");
       // first, set Memory Attributes array, indexed by PT_MEM, PT_DEV, PT_NC in our example
    r=  (0xFF << 0) |    // AttrIdx=0: normal, IWBWA, OWBWA, NTR
        (0x04 << 8) |    // AttrIdx=1: device, nGnRE (must be OSH too)
        (0x44 <<16);     // AttrIdx=2: non cacheable
   // asm volatile ("msr mair_el1, %0" : : "r" (r));
    set_mair_el1(r);
    // next, specify mapping characteristics in translate control register
    r = read_tcr();
    r &=~ (0b00LL << 37) | // TBI=0, no tagging
        (b << 32) |      // IPS=autodetected
        (0b10LL << 30) | // TG1=4k
        (0b11LL << 28) | // SH1=3 inner
        (0b01LL << 26) | // ORGN1=1 write back
        (0b01LL << 24) | // IRGN1=1 write back
        (0b0LL  << 23) | // EPD1 enable higher half
        (25LL   << 16) | // T1SZ=25, 3 levels (512G)
        (0b00LL << 14) | // TG0=4k
        (0b11LL << 12) | // SH0=3 inner
        (0b01LL << 10) | // ORGN0=1 write back
        (0b01LL << 8) |  // IRGN0=1 write back
        (0b0LL  << 7) |  // EPD0 enable lower half
        (25LL   << 0);   // T0SZ=25, 3 levels (512G)
    write_tcr(r);
    // tell the MMU where our translation tables are. TTBR_CNP bit not documented, but required
    // lower half, user space
   // asm volatile ("msr ttbr0_el1, %0" : : "r" ((unsigned long)&PageTablesBase + TTBR_CNP));
    // upper half, kernel space
   // asm volatile ("msr ttbr1_el1, %0" : : "r" ((unsigned long)&PageTablesBase + TTBR_CNP + PAGESIZE));
   write_ttbr0_el1((UINT64)((ULONG_PTR)PxeBase) + TTBR_CNP);
   // write_ttbr1_el1((UINT64)&PageTablesBase + TTBR_CNP + PAGESIZE);
    // finally, toggle some bits in system control register to enable page translation
    r = GetSCTLREL1();
    r|=0xC00800;     // set mandatory reserved bits
    r&=~((1<<25) |   // clear EE, little endian translation tables
         (1<<24) |   // clear E0E
         (1<<19) |   // clear WXN
         (1<<12) |   // clear I, no instruction cache
         (1<<4) |    // clear SA0
         (1<<3) |    // clear SA
         (1<<2) |    // clear C, no cache at all
         (1<<1));    // clear A, no aligment check
    r|=  (1<<0);     // set M, enable MMU
    SetSCTLREL1(r);
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
