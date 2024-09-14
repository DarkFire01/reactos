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
WinLdrSetProcessorContext(
    _In_ USHORT OperatingSystemVersion)
{

}

VOID
WinLdrSetupMachineDependent(
    PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    for(;;)
    {

    }
}
