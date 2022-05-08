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

#define PFN_SHIFT                   12
#define LARGE_PFN_SHIFT             20

#define PTE_BASE                    0xC0000000
#define PDE_BASE                    0xC0400000
#define PDR_BASE                    0xFFD00000
#define VECTOR_BASE                 0xFFFF0000

#ifdef _ZOOM2_
#define IDMAP_BASE                  0x81000000
#define MMIO_BASE                   0x10000000
#else
#define IDMAP_BASE                  0x00000000
#define MMIO_BASE                   0x10000000
#endif

#define LowMemPageTableIndex        (IDMAP_BASE >> PDE_SHIFT)
#define MmioPageTableIndex          (MMIO_BASE >> PDE_SHIFT)
#define KernelPageTableIndex        (KSEG0_BASE >> PDE_SHIFT)
#define StartupPtePageTableIndex    (PTE_BASE >> PDE_SHIFT)
#define StartupPdePageTableIndex    (PDE_BASE >> PDE_SHIFT)
#define PdrPageTableIndex           (PDR_BASE >> PDE_SHIFT)
#define VectorPageTableIndex        (VECTOR_BASE >> PDE_SHIFT)

#ifndef _ZOOM2_
PVOID MempPdrBaseAddress = (PVOID)0x70000;
PVOID MempKernelBaseAddress = (PVOID)0;
#else
PVOID MempPdrBaseAddress = (PVOID)0x81100000;
PVOID MempKernelBaseAddress = (PVOID)0x80000000;
#endif

/* Converts a Physical Address into a Page Frame Number */
#define PaToPfn(p)                  ((p) >> PFN_SHIFT)
#define PaToLargePfn(p)             ((p) >> LARGE_PFN_SHIFT)
#define PaPtrToPfn(p)               (((ULONG_PTR)(p)) >> PFN_SHIFT)

/* Converts a Physical Address into a Coarse Page Table PFN */
#define PaPtrToPdePfn(p)            (((ULONG_PTR)(p)) >> CPT_SHIFT)

/* FUNCTIONS **************************************************************/

BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage,
                IN PFN_NUMBER NumberOfPages,
                IN BOOLEAN KernelMapping)
{
    return TRUE;
}

VOID
MempUnmapPage(IN PFN_NUMBER Page)
{
    return;
}

VOID
MempDump(VOID)
{
    return;
}

static
BOOLEAN
WinLdrMapSpecialPages(ULONG PcrBasePage)
{

    /* TODO: Map in the kernel CPTs */
    return TRUE;
}

VOID
WinLdrSetupForNt(IN PLOADER_PARAMETER_BLOCK LoaderBlock,
                 IN PVOID *GdtIdt,
                 IN PULONG PcrBasePage,
                 IN PULONG TssBasePage)
{

}

static
BOOLEAN
MempAllocatePageTables(VOID)
{
    /* Done */
    return TRUE;
}

VOID
WinLdrSetProcessorContext(VOID)
{

}

VOID
WinLdrSetupMachineDependent(
    PLOADER_PARAMETER_BLOCK LoaderBlock)
{
}
