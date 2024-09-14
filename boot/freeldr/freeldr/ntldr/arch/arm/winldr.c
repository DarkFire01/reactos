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

FORCEINLINE
ARM_STATUS_REGISTER
ArmStatusRegisterGet(VOID)
{
    ARM_STATUS_REGISTER Value;
#ifdef _MSC_VER
    Value.AsUlong = _ReadStatusReg(0);
#else
    __asm__ __volatile__ ("mrs %0, cpsr" : "=r"(Value.AsUlong) : : "cc");
#endif
    return Value;
}

pArmControlRegisterSet(ULONG Ttb);
FORCEINLINE
VOID
ArmControlRegisterSet(IN ARM_CONTROL_REGISTER ControlRegister)
{
#ifdef _MSC_VER
    pArmControlRegisterSet(ControlRegister.AsUlong);
#else
    __asm__ __volatile__ ("mcr p15, 0, %0, c1, c0, 0" : : "r"(ControlRegister.AsUlong) : "cc");
#endif
}

pArmControlRegisterSetTwo(ULONG Ttb);
FORCEINLINE
VOID
ArmControlRegisterSetTwo(IN ARM_CONTROL_REGISTER ControlRegister)
{
#ifdef _MSC_VER
    pArmControlRegisterSetTwo(ControlRegister.AsUlong);
#else
    __asm__ __volatile__ ("mcr p15, 0, %0, c1, c0, 0" : : "r"(ControlRegister.AsUlong) : "cc");
#endif
}
void pArmControlRegisterGet(ULONG* value);
FORCEINLINE
ARM_CONTROL_REGISTER
ArmControlRegisterGet(VOID)
{
    ARM_CONTROL_REGISTER Value;
#ifdef _MSC_VER
    Value.AsUlong = 0;
    pArmControlRegisterGet(&Value.AsUlong);
#else
    __asm__ __volatile__ ("mrc p15, 0, %0, c1, c0, 0" : "=r"(Value.AsUlong) : : "cc");
#endif
    return Value;
}

void pArmTranslationTableRegisterSet(ULONG Ttb);
FORCEINLINE
VOID
ArmTranslationTableRegisterSet(IN ARM_TTB_REGISTER Ttb)
{
#ifdef _MSC_VER
    pArmTranslationTableRegisterSet(Ttb.AsUlong);
#else
    __asm__ __volatile__ ("mcr p15, 0, %0, c2, c0, 0" : : "r"(Ttb.AsUlong) : "cc");
#endif
}

void pArmDomainRegisterSet(ULONG Ttb);
FORCEINLINE
VOID
ArmDomainRegisterSet(IN ARM_DOMAIN_REGISTER DomainRegister)
{
#ifdef _MSC_VER
    pArmDomainRegisterSet(DomainRegister.AsUlong);
#else
    __asm__ __volatile__ ("mcr p15, 0, %0, c3, c0, 0" : : "r"(DomainRegister.AsUlong) : "cc");
#endif
}











































#if 0
Invalud PTE
Valid = 0
largePage = 0 
ETC  = 0
#endif







#define TTBR1_THRESHOLD 0x40000000 // 4KB (REACTOS PAGE_SIZE)



PHARDWARE_PDE_ARMV6 PDE; //TTBR1?
PHARDWARE_PDE_ARMV6 HalPageTable;
//HARDWARE_PDE_ARMV6
//HARDWARE_PTE_ARMV6
PUCHAR PhysicalPageTablesBuffer;
PUCHAR KernelPageTablesBuffer;
ULONG PhysicalPageTables;
ULONG KernelPageTables;



/* FUNCTIONS **************************************************************/

static
VOID
MempAllocatePTE(ULONG Entry, PHARDWARE_PTE_ARMV6 *PhysicalPT, PHARDWARE_PTE_ARMV6 *KernelPT)
{
    //TRACE("Creating PDE Entry %X\n", Entry);

    // Identity mapping
    *PhysicalPT = (PHARDWARE_PTE_ARMV6)&PhysicalPageTablesBuffer[PhysicalPageTables*MM_PAGE_SIZE];
    PhysicalPageTables++;

    PDE[Entry].PageFrameNumber = (ULONG)*PhysicalPT >> MM_PAGE_SHIFT;
    PDE[Entry].Valid = 1;
    PDE[Entry].LargePage = 0;

    if (Entry+(KSEG0_BASE >> 22) > 1023)
    {
        TRACE("WARNING! Entry: %X > 1023\n", Entry+(KSEG0_BASE >> 22));
    }

    // Kernel-mode mapping
    *KernelPT = (PHARDWARE_PTE_ARMV6)&KernelPageTablesBuffer[KernelPageTables*MM_PAGE_SIZE];
    KernelPageTables++;

    PDE[Entry+(KSEG0_BASE >> 22)].PageFrameNumber = ((ULONG)*KernelPT >> MM_PAGE_SHIFT);
    PDE[Entry+(KSEG0_BASE >> 22)].Valid = 1;
    PDE[Entry+(KSEG0_BASE >> 22)].LargePage = 0;
}

BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage,
                IN PFN_COUNT NumberOfPages,
                IN BOOLEAN KernelMapping)
{
    PHARDWARE_PTE_ARMV6 PhysicalPT;
    PHARDWARE_PTE_ARMV6 KernelPT;
    PFN_COUNT Entry, Page;

    TRACE("MempSetupPaging: SP 0x%X, Number: 0x%X, Kernel: %s\n",
       StartPage, NumberOfPages, KernelMapping ? "yes" : "no");


    //
    // Now actually set up the page tables for identity mapping
    //
    for (Page = StartPage; Page < StartPage + NumberOfPages; Page++)
    {
        Entry = Page >> 10;

        if (((PULONG)PDE)[Entry] == 0)
        {
            MempAllocatePTE(Entry, &PhysicalPT, &KernelPT);
        }
        else
        {
            PhysicalPT = (PHARDWARE_PTE_ARMV6)(PDE[Entry].PageFrameNumber << MM_PAGE_SHIFT);
            KernelPT = (PHARDWARE_PTE_ARMV6)(PDE[Entry+(KSEG0_BASE >> 22)].PageFrameNumber << MM_PAGE_SHIFT);
        }

        PhysicalPT[Page & 0x3ff].PageFrameNumber = Page;
        PhysicalPT[Page & 0x3ff].Valid = 1;
        if (KernelMapping)
        {
            if (KernelPT[Page & 0x3ff].Valid) WARN("KernelPT already mapped\n");
            KernelPT[Page & 0x3ff].PageFrameNumber = Page;
            KernelPT[Page & 0x3ff].Valid = (Page != 0);
        }
    }

    return TRUE;
}
#define SELFMAP_ENTRY       0x300
VOID
MempUnmapPage(IN PFN_NUMBER Page)
{
    TRACE("MempUnmapPage\n");
    return;
}

VOID
MempDump(VOID)
{
    return;
}


static
BOOLEAN
MempAllocatePageTables(VOID)
{
    ULONG NumPageTables, TotalSize;
    PUCHAR Buffer;
    // It's better to allocate PDE + PTEs contiguous

    // Max number of entries = MaxPageNum >> 10
    // FIXME: This is a number to describe ALL physical memory
    // and windows doesn't expect ALL memory mapped...
    NumPageTables = TotalPagesInLookupTable >> 10;

    TRACE("NumPageTables = %d\n", NumPageTables);

    // Allocate memory block for all these things:
    // PDE, HAL mapping page table, physical mapping, kernel mapping
    TotalSize = (1 + 1 + NumPageTables * 2) * MM_PAGE_SIZE;

    // PDE+HAL+KernelPTEs == MemoryData
    Buffer = MmAllocateMemoryWithType(TotalSize, LoaderMemoryData);

    // Physical PTEs = FirmwareTemporary
    PhysicalPageTablesBuffer = (PUCHAR)Buffer + TotalSize - NumPageTables*MM_PAGE_SIZE;
    MmSetMemoryType(PhysicalPageTablesBuffer,
                    NumPageTables*MM_PAGE_SIZE,
                    LoaderFirmwareTemporary);

    // This check is now redundant
    if (Buffer + (TotalSize - NumPageTables*MM_PAGE_SIZE) !=
        PhysicalPageTablesBuffer)
    {
        TRACE("There was a problem allocating two adjacent blocks of memory!\n");
    }

    if (Buffer == NULL || PhysicalPageTablesBuffer == NULL)
    {
        UiMessageBox("Impossible to allocate memory block for page tables!");
        return FALSE;
    }

    // Zero all this memory block
    RtlZeroMemory(Buffer, TotalSize);

    // Set up pointers correctly now
    PDE = (PHARDWARE_PDE_ARMV6)Buffer;

    PDE[SELFMAP_ENTRY].PageFrameNumber = (ULONG)PDE >> MM_PAGE_SHIFT;
    PDE[SELFMAP_ENTRY].Valid = 1;
    PDE[SELFMAP_ENTRY].LargePage = 0;

    // The last PDE slot is allocated for HAL's memory mapping (Virtual Addresses 0xFFC00000 - 0xFFFFFFFF)
    HalPageTable = (PHARDWARE_PDE_ARMV6)&Buffer[MM_PAGE_SIZE*1];

    // Map it
    PDE[1023].PageFrameNumber = (ULONG)HalPageTable >> MM_PAGE_SHIFT;
    PDE[1023].Valid = 1;
    PDE[1023].LargePage = 0;

    // Store pointer to the table for easier access
    KernelPageTablesBuffer = &Buffer[MM_PAGE_SIZE*2];

    // Zero counters of page tables used
    PhysicalPageTables = 0;
    KernelPageTables = 0;

    /* Done */
    return TRUE;
}

stackfun[2048];
stackfuntwo[2048];

#define QEMUUART 0x09000000
extern unsigned int * UART0DR;
void
FuncTest()
{
    *UART0DR = 'C';
    for(;;)
    {

    }
}
  extern  KERNEL_ENTRY_POINT PubKiSystemStartup;
  extern  PLOADER_PARAMETER_BLOCK PubLoaderBlockVA;
VOID
WinLdrSetProcessorContext(
    _In_ USHORT OperatingSystemVersion)
{

    TRACE("WinLdrSetProcessorContext: Entry\n");


    ARM_CONTROL_REGISTER ControlRegister;
    ARM_TTB_REGISTER TtbRegister;
    ARM_DOMAIN_REGISTER DomainRegister;

    /* Set the TTBR */
    TtbRegister.AsUlong = (ULONG_PTR)PDE; //I have no idea if this is right
    if (PDE == NULL)
    {
        TRACE("The page tables are null\n");
    }

        ControlRegister = ArmControlRegisterGet();
    ControlRegister.MmuEnabled = FALSE;
    ControlRegister.ICacheEnabled = TRUE;
    ControlRegister.DCacheEnabled = TRUE;
    ControlRegister.ForceAp = TRUE;
    ControlRegister.ExtendedPageTables = TRUE;
    ArmControlRegisterSet(ControlRegister);

    ArmTranslationTableRegisterSet(TtbRegister);

    /* Disable domains and simply use access bits on PTEs */
    DomainRegister.AsUlong = 0;
    DomainRegister.Domain0 = ClientDomain;
    ArmDomainRegisterSet(DomainRegister);

    TRACE("Enabling paging\n");
    /* Enable ARMv6+ paging (MMU), caches and the access bit */
    ControlRegister = ArmControlRegisterGet();
    ControlRegister.MmuEnabled = TRUE;
    ControlRegister.ICacheEnabled = TRUE;
    ControlRegister.DCacheEnabled = TRUE;
    ControlRegister.ForceAp = TRUE;
    ControlRegister.ExtendedPageTables = TRUE;
    ArmControlRegisterSetTwo(ControlRegister);

    TRACE("Jumping to kernel %p\n", PubKiSystemStartup);

    (*PubKiSystemStartup)(PubLoaderBlockVA);
}

VOID
WinLdrSetupMachineDependent(
    PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    TRACE("WinLdrSetupMachineDependent: Entry\n");
    // Before we start mapping pages, create a block of memory, which will contain
    // PDE and PTEs
    if (MempAllocatePageTables() == FALSE)
    {
        BugCheck("MempAllocatePageTables failed!\n");
    }

}
