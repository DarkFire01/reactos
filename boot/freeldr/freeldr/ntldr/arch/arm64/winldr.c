
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
void write_tcr(UINT64 tcr);
UINT64 read_tcr();
void set_mair_el1(UINT64 mair);
void DisalbeMMU();

BOOLEAN
MempIsPageMapped(PVOID VirtualAddress)
{
    return TRUE;
}

BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage,
                IN PFN_NUMBER NumberOfPages,
                IN BOOLEAN KernelMapping)
{
    return TRUE;
}


void WinLdrSetupMachineDependent(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    TRACE("IMage Base Addr %p\n",OsLoaderBase);
}

/* ************************************************************************/
#define MMUBIT 1;
#define DISABLE_MMU_BIT 0
VOID
WinLdrSetProcessorContext(VOID)
{
    DisalbeMMU();
    UINT64 r, b;
    b=0x90000000>>21; //UART_BA >> 21
    TRACE("WinLdrSetProcessorContext: Entry\n");

       // first, set Memory Attributes array, indexed by PT_MEM, PT_DEV, PT_NC in our example
    r=  (0xFF << 0) |    // AttrIdx=0: normal, IWBWA, OWBWA, NTR
        (0x04 << 8) |    // AttrIdx=1: device, nGnRE (must be OSH too)
        (0x44 <<16);     // AttrIdx=2: non cacheable

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
  //  write_ttbr0_el1((UINT64)((ULONG_PTR)PxeBase) + TTBR_CNP);
    // upper half, kernel space
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
